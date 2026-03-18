/*
 * r8e_gc.c - Reference Counting + Epoch Cycle Detection Garbage Collector
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 8 for design rationale.
 *
 * Architecture:
 *   - Reference counting with compile-time elision (53-61% base, ~70-75%
 *     with chain superinstructions)
 *   - Deferred decrement batching at function exit boundaries
 *   - Suspect-list-driven cycle detection (default threshold 4096)
 *   - Adaptive threshold: halve on cycles found, double on none (256..65536)
 *   - Inline refcount bits[31:16] covers 99.99% of objects (0-65535)
 *   - External overflow counter hash table for refcount > 65535
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* NaN-boxing type checks */
#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

/* NaN-boxing extraction */
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* =========================================================================
 * GC header flags layout (32 bits) - shared with object model
 *
 *   Bits [1:0]   = tier (0-3) for objects, or subtype for other heap types
 *   Bit  [2]     = GC mark (used during cycle scan)
 *   Bit  [3]     = on suspect list
 *   Bit  [4]     = frozen (Object.freeze)
 *   Bits [7:5]   = GC kind (object=0, string=1, array=2, closure=3,
 *                           envframe=4, function=5)
 *   Bits [15:8]  = refcount overflow flag (nonzero = use external counter)
 *   Bits [31:16] = inline refcount (0-65535)
 * ========================================================================= */

#define R8E_GC_TIER_MASK         0x00000003u
#define R8E_GC_MARK              0x00000004u
#define R8E_GC_ON_SUSPECT        0x00000008u
#define R8E_GC_FROZEN            0x00000010u

#define R8E_GC_KIND_MASK         0x000000E0u
#define R8E_GC_KIND_SHIFT        5
#define R8E_GC_KIND_OBJECT       0u
#define R8E_GC_KIND_STRING       1u
#define R8E_GC_KIND_ARRAY        2u
#define R8E_GC_KIND_CLOSURE      3u
#define R8E_GC_KIND_ENVFRAME     4u
#define R8E_GC_KIND_FUNCTION     5u

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u  /* Bit 8: overflow flag */
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)
#define R8E_GC_SET_KIND(flags, kind) \
    (((flags) & ~R8E_GC_KIND_MASK) | \
     (((kind) << R8E_GC_KIND_SHIFT) & R8E_GC_KIND_MASK))

/* =========================================================================
 * GC header - first 8 bytes of every heap-allocated object
 * ========================================================================= */

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* =========================================================================
 * Forward declarations for heap object types
 *
 * These mirror the definitions in their respective modules. When shared
 * headers exist, these will be replaced by #include directives.
 * ========================================================================= */

/* Property slot (used by object tiers) */
typedef struct {
    uint32_t key;      /* atom index */
    uint8_t  desc;     /* property descriptor flags */
    uint8_t  pad[3];
    R8EValue val;
} R8EPropSlot;

/* Tier 0: Micro Object (0-1 properties, 24 bytes) */
typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint64_t  key0;     /* atom index of single property (0 = empty) */
    uint64_t  val0;     /* NaN-boxed value */
} R8EObjTier0;

/* Tier 1: Compact Object (2-4 properties) */
typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint8_t     count;
    uint8_t     pad[7];
    R8EPropSlot props[4];
} R8EObjTier1;

/* Tier 2: Compact Array (5-16 properties, heap-allocated) */
typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint8_t     count;
    uint8_t     capacity;
    uint8_t     pad[6];
    R8EPropSlot *props;   /* heap array */
} R8EObjTier2;

/* Tier 3: Hash Table (17+ properties, Robin Hood) */
typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint16_t    count;
    uint16_t    capacity;
    uint32_t    pad;
    R8EPropSlot *buckets;  /* heap array */
} R8EObjTier3;

/* Array */
typedef struct R8EArray {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *elements;    /* dense NaN-boxed array */
    void     *named;       /* CDOL object for named properties, or NULL */
} R8EArray;

/* String */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    /* char data[] follows (flexible array member) */
} R8EString;

/* Function */
typedef struct R8EFunction {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t *bytecode;
    uint32_t bytecode_len;
    uint16_t param_count;
    uint16_t local_count;
    uint16_t capture_count;
    uint16_t reserved;
    uint32_t name_atom;
} R8EFunction;

/* Environment frame (shared mutable captures) */
typedef struct R8EEnvFrame {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  refcount;    /* number of closures sharing this frame */
    uint16_t  slot_count;
    uint16_t  reserved;
    R8EValue *slots;
} R8EEnvFrame;

/* Capture metadata */
#define R8E_CAP_META_DIRECT   0  /* immutable: value stored directly */
#define R8E_CAP_META_INDIRECT 1  /* mutable: value in env_frame->slots[index] */

typedef struct {
    uint8_t  kind;    /* R8E_CAP_META_DIRECT or R8E_CAP_META_INDIRECT */
    uint8_t  index;   /* frame slot index (only for INDIRECT) */
    uint16_t pad;
} R8ECaptureMeta;

/* Closure */
#define R8E_CAPTURE_MODE_INLINE 0
#define R8E_CAPTURE_MODE_ARRAY  1
#define R8E_CAPTURE_MODE_FRAME  2

typedef struct R8EClosure {
    uint32_t      flags;
    uint32_t      proto_id;
    R8EFunction  *func;
    uint8_t       capture_count;
    uint8_t       capture_mode;
    uint16_t      reserved;
    union {
        struct {
            R8EValue inline_captures[2];
        } inl;
        struct {
            R8EValue       *values;
            R8ECaptureMeta *meta;
            R8EEnvFrame    *env;
        } arr;
        struct {
            R8EEnvFrame *env_frame;
        } frm;
    } storage;
} R8EClosure;

/* =========================================================================
 * RC elision classifications (Section 8.2)
 * ========================================================================= */

#define R8E_RC_OWNED    0  /* Must do RC: value escapes scope */
#define R8E_RC_BORROWED 1  /* Skip RC: value owned elsewhere in same scope */
#define R8E_RC_TEMP     2  /* Skip RC: intermediate consumed immediately */

/* =========================================================================
 * External refcount overflow table
 *
 * When an object's inline refcount would exceed 65535, we set the overflow
 * flag and store the actual count in this hash table, keyed by pointer.
 * Open-addressing with Fibonacci hashing.
 * ========================================================================= */

typedef struct {
    R8EGCHeader *obj;
    uint32_t     refcount;
} R8EExtRCEntry;

#define R8E_EXT_RC_INIT_CAP  64

typedef struct {
    R8EExtRCEntry *entries;
    uint32_t       count;
    uint32_t       capacity;
} R8EExtRCTable;

/* =========================================================================
 * Suspect list for cycle detection (Section 8.3)
 * ========================================================================= */

#define R8E_SUSPECT_INIT_CAP  256

typedef struct {
    R8EGCHeader **items;
    uint32_t      count;
    uint32_t      capacity;
} R8ESuspectList;

/* =========================================================================
 * Deferred decrement list (Section 8.2)
 *
 * Stack-allocated inline buffer for common case; overflows to heap.
 * One per function activation frame.
 * ========================================================================= */

#define R8E_DEFER_INLINE_CAP  32

typedef struct R8EDeferList {
    R8EValue  inline_buf[R8E_DEFER_INLINE_CAP];
    R8EValue *overflow;
    uint32_t  inline_count;
    uint32_t  overflow_count;
    uint32_t  overflow_capacity;
} R8EDeferList;

/* =========================================================================
 * GC state - embedded inside the engine context
 * ========================================================================= */

typedef struct R8EGCState {
    R8ESuspectList  suspects;
    R8EExtRCTable   ext_rc;
    uint32_t        epoch_threshold;    /* default 4096 */
    uint32_t        total_allocated;    /* total heap objects created */
    uint32_t        total_freed;        /* total heap objects freed */
    uint32_t        cycles_collected;   /* total cycle-collected objects */
} R8EGCState;

/* =========================================================================
 * R8EContext - minimal definition for GC purposes
 * ========================================================================= */

typedef struct R8EContext {
    R8EGCState gc;
    /* Other fields defined by the engine context module */
} R8EContext;

/* =========================================================================
 * GC visitor callback type
 * ========================================================================= */

typedef void (*R8EGCVisitorFn)(R8EGCHeader *ref, void *user_data);

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

void r8e_cycle_scan(R8EContext *ctx);
void r8e_release(R8EContext *ctx, R8EValue val);

static void r8e_free_by_kind(R8EContext *ctx, R8EGCHeader *hdr);
static void r8e_release_children(R8EContext *ctx, R8EGCHeader *hdr);

/* =========================================================================
 * Inline refcount helpers
 * ========================================================================= */

static inline uint32_t r8e_gc_get_rc(const R8EGCHeader *hdr) {
    return (hdr->flags & R8E_GC_RC_INLINE_MASK) >> R8E_GC_RC_INLINE_SHIFT;
}

static inline void r8e_gc_set_rc(R8EGCHeader *hdr, uint32_t rc) {
    hdr->flags = (hdr->flags & ~R8E_GC_RC_INLINE_MASK)
               | ((rc << R8E_GC_RC_INLINE_SHIFT) & R8E_GC_RC_INLINE_MASK);
}

static inline bool r8e_gc_has_overflow(const R8EGCHeader *hdr) {
    return (hdr->flags & R8E_GC_RC_OVERFLOW_BIT) != 0;
}

/* =========================================================================
 * External refcount overflow table implementation
 * ========================================================================= */

static void r8e_ext_rc_init(R8EExtRCTable *tbl) {
    tbl->capacity = R8E_EXT_RC_INIT_CAP;
    tbl->count = 0;
    tbl->entries = (R8EExtRCEntry *)calloc(tbl->capacity,
                                            sizeof(R8EExtRCEntry));
}

static void r8e_ext_rc_destroy(R8EExtRCTable *tbl) {
    free(tbl->entries);
    tbl->entries = NULL;
    tbl->count = 0;
    tbl->capacity = 0;
}

static uint32_t r8e_ext_rc_hash(const void *ptr, uint32_t cap) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    uint64_t h = addr * 11400714819323198485ULL; /* Fibonacci hashing */
    return (uint32_t)(h >> 32) & (cap - 1);
}

static void r8e_ext_rc_grow(R8EExtRCTable *tbl) {
    uint32_t new_cap = tbl->capacity * 2;
    R8EExtRCEntry *new_ent = (R8EExtRCEntry *)calloc(new_cap,
                                                      sizeof(R8EExtRCEntry));
    if (!new_ent) return;

    for (uint32_t i = 0; i < tbl->capacity; i++) {
        if (tbl->entries[i].obj != NULL) {
            uint32_t idx = r8e_ext_rc_hash(tbl->entries[i].obj, new_cap);
            while (new_ent[idx].obj != NULL) {
                idx = (idx + 1) & (new_cap - 1);
            }
            new_ent[idx] = tbl->entries[i];
        }
    }

    free(tbl->entries);
    tbl->entries = new_ent;
    tbl->capacity = new_cap;
}

static uint32_t *r8e_ext_rc_find(R8EExtRCTable *tbl, R8EGCHeader *obj) {
    if (!tbl->entries || tbl->capacity == 0) return NULL;

    uint32_t idx = r8e_ext_rc_hash(obj, tbl->capacity);
    uint32_t probes = 0;

    while (probes < tbl->capacity) {
        if (tbl->entries[idx].obj == obj) {
            return &tbl->entries[idx].refcount;
        }
        if (tbl->entries[idx].obj == NULL) {
            return NULL;
        }
        idx = (idx + 1) & (tbl->capacity - 1);
        probes++;
    }
    return NULL;
}

static uint32_t *r8e_ext_rc_insert(R8EExtRCTable *tbl, R8EGCHeader *obj,
                                    uint32_t initial_rc) {
    if (!tbl->entries) {
        r8e_ext_rc_init(tbl);
    }
    if (tbl->count * 4 >= tbl->capacity * 3) {
        r8e_ext_rc_grow(tbl);
    }

    uint32_t idx = r8e_ext_rc_hash(obj, tbl->capacity);
    while (tbl->entries[idx].obj != NULL) {
        if (tbl->entries[idx].obj == obj) {
            return &tbl->entries[idx].refcount;
        }
        idx = (idx + 1) & (tbl->capacity - 1);
    }

    tbl->entries[idx].obj = obj;
    tbl->entries[idx].refcount = initial_rc;
    tbl->count++;
    return &tbl->entries[idx].refcount;
}

static void r8e_ext_rc_remove(R8EExtRCTable *tbl, R8EGCHeader *obj) {
    if (!tbl->entries || tbl->capacity == 0) return;

    uint32_t idx = r8e_ext_rc_hash(obj, tbl->capacity);
    uint32_t probes = 0;

    while (probes < tbl->capacity) {
        if (tbl->entries[idx].obj == obj) {
            tbl->entries[idx].obj = NULL;
            tbl->entries[idx].refcount = 0;
            tbl->count--;

            /* Rehash displaced entries after deletion */
            uint32_t next = (idx + 1) & (tbl->capacity - 1);
            while (tbl->entries[next].obj != NULL) {
                R8EExtRCEntry tmp = tbl->entries[next];
                tbl->entries[next].obj = NULL;
                tbl->entries[next].refcount = 0;
                tbl->count--;
                r8e_ext_rc_insert(tbl, tmp.obj, tmp.refcount);
                next = (next + 1) & (tbl->capacity - 1);
            }
            return;
        }
        if (tbl->entries[idx].obj == NULL) return;
        idx = (idx + 1) & (tbl->capacity - 1);
        probes++;
    }
}

/* =========================================================================
 * Full refcount management (inline + overflow)
 * ========================================================================= */

static uint32_t r8e_gc_full_refcount(R8EContext *ctx, R8EGCHeader *hdr) {
    if (!r8e_gc_has_overflow(hdr)) {
        return r8e_gc_get_rc(hdr);
    }
    uint32_t *ext = r8e_ext_rc_find(&ctx->gc.ext_rc, hdr);
    return ext ? *ext : r8e_gc_get_rc(hdr);
}

static void r8e_gc_inc_refcount(R8EContext *ctx, R8EGCHeader *hdr) {
    if (r8e_gc_has_overflow(hdr)) {
        uint32_t *ext = r8e_ext_rc_find(&ctx->gc.ext_rc, hdr);
        if (ext) (*ext)++;
        return;
    }

    uint32_t rc = r8e_gc_get_rc(hdr);
    if (rc < 65535) {
        r8e_gc_set_rc(hdr, rc + 1);
    } else {
        /* Overflow to external table */
        hdr->flags |= R8E_GC_RC_OVERFLOW_BIT;
        r8e_gc_set_rc(hdr, 65535);
        r8e_ext_rc_insert(&ctx->gc.ext_rc, hdr, 65536);
    }
}

/**
 * Decrement refcount. Returns true if it reached zero (object is dead).
 */
static bool r8e_gc_dec_refcount(R8EContext *ctx, R8EGCHeader *hdr) {
    if (r8e_gc_has_overflow(hdr)) {
        uint32_t *ext = r8e_ext_rc_find(&ctx->gc.ext_rc, hdr);
        if (ext) {
            (*ext)--;
            if (*ext <= 65535) {
                uint32_t rc = *ext;
                r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
                hdr->flags &= ~R8E_GC_RC_OVERFLOW_BIT;
                r8e_gc_set_rc(hdr, rc);
                return (rc == 0);
            }
            return false;
        }
        return false;
    }

    uint32_t rc = r8e_gc_get_rc(hdr);
    if (rc == 0) return true;
    rc--;
    r8e_gc_set_rc(hdr, rc);
    return (rc == 0);
}

/* =========================================================================
 * Suspect list management (Section 8.3)
 * ========================================================================= */

static void r8e_suspect_list_init(R8ESuspectList *sl) {
    sl->capacity = R8E_SUSPECT_INIT_CAP;
    sl->count = 0;
    sl->items = (R8EGCHeader **)malloc(sl->capacity * sizeof(R8EGCHeader *));
}

static void r8e_suspect_list_destroy(R8ESuspectList *sl) {
    free(sl->items);
    sl->items = NULL;
    sl->count = 0;
    sl->capacity = 0;
}

static void r8e_suspect_list_grow(R8ESuspectList *sl) {
    uint32_t new_cap = sl->capacity * 2;
    R8EGCHeader **new_items = (R8EGCHeader **)realloc(
        sl->items, new_cap * sizeof(R8EGCHeader *));
    if (!new_items) return;
    sl->items = new_items;
    sl->capacity = new_cap;
}

void r8e_suspect_add(R8EContext *ctx, R8EGCHeader *obj) {
    if (obj->flags & R8E_GC_ON_SUSPECT) return;

    R8ESuspectList *sl = &ctx->gc.suspects;
    if (sl->count >= sl->capacity) {
        r8e_suspect_list_grow(sl);
        if (sl->count >= sl->capacity) return;
    }

    obj->flags |= R8E_GC_ON_SUSPECT;
    sl->items[sl->count++] = obj;
}

static void r8e_suspect_remove(R8EContext *ctx, R8EGCHeader *obj) {
    R8ESuspectList *sl = &ctx->gc.suspects;
    for (uint32_t i = 0; i < sl->count; i++) {
        if (sl->items[i] == obj) {
            sl->items[i] = sl->items[--sl->count];
            return;
        }
    }
}

/* =========================================================================
 * Object reference traversal
 *
 * Enumerates all outgoing heap references from a heap object. The visitor
 * is called once per referenced heap object.
 * ========================================================================= */

static void r8e_visit_value(R8EValue val, R8EGCVisitorFn visitor, void *ud) {
    if (R8E_IS_POINTER(val)) {
        R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(val);
        if (ref) visitor(ref, ud);
    }
}

void r8e_gc_visit_refs(R8EGCHeader *hdr, R8EGCVisitorFn visitor, void *ud) {
    if (!hdr) return;

    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);

    switch (kind) {
    case R8E_GC_KIND_OBJECT: {
        uint32_t tier = hdr->flags & R8E_GC_TIER_MASK;
        switch (tier) {
        case 0: {
            R8EObjTier0 *obj = (R8EObjTier0 *)hdr;
            if (obj->key0 != 0) {
                r8e_visit_value(obj->val0, visitor, ud);
            }
            break;
        }
        case 1: {
            R8EObjTier1 *obj = (R8EObjTier1 *)hdr;
            for (uint8_t i = 0; i < obj->count; i++) {
                r8e_visit_value(obj->props[i].val, visitor, ud);
            }
            break;
        }
        case 2: {
            R8EObjTier2 *obj = (R8EObjTier2 *)hdr;
            if (obj->props) {
                for (uint8_t i = 0; i < obj->count; i++) {
                    r8e_visit_value(obj->props[i].val, visitor, ud);
                }
            }
            break;
        }
        case 3: {
            R8EObjTier3 *obj = (R8EObjTier3 *)hdr;
            if (obj->buckets) {
                for (uint16_t i = 0; i < obj->capacity; i++) {
                    if (obj->buckets[i].key != 0) {
                        r8e_visit_value(obj->buckets[i].val, visitor, ud);
                    }
                }
            }
            break;
        }
        }
        break;
    }

    case R8E_GC_KIND_ARRAY: {
        R8EArray *arr = (R8EArray *)hdr;
        if (arr->elements) {
            for (uint32_t i = 0; i < arr->length; i++) {
                r8e_visit_value(arr->elements[i], visitor, ud);
            }
        }
        if (arr->named) {
            visitor((R8EGCHeader *)arr->named, ud);
        }
        break;
    }

    case R8E_GC_KIND_CLOSURE: {
        R8EClosure *cl = (R8EClosure *)hdr;
        if (cl->func) {
            visitor((R8EGCHeader *)cl->func, ud);
        }
        switch (cl->capture_mode) {
        case R8E_CAPTURE_MODE_INLINE:
            for (uint8_t i = 0; i < cl->capture_count && i < 2; i++) {
                r8e_visit_value(cl->storage.inl.inline_captures[i],
                                visitor, ud);
            }
            break;
        case R8E_CAPTURE_MODE_ARRAY:
            if (cl->storage.arr.values) {
                for (uint8_t i = 0; i < cl->capture_count; i++) {
                    if (cl->storage.arr.meta &&
                        cl->storage.arr.meta[i].kind == R8E_CAP_META_DIRECT) {
                        r8e_visit_value(cl->storage.arr.values[i],
                                        visitor, ud);
                    }
                }
            }
            if (cl->storage.arr.env) {
                visitor((R8EGCHeader *)cl->storage.arr.env, ud);
            }
            break;
        case R8E_CAPTURE_MODE_FRAME:
            if (cl->storage.frm.env_frame) {
                visitor((R8EGCHeader *)cl->storage.frm.env_frame, ud);
            }
            break;
        }
        break;
    }

    case R8E_GC_KIND_ENVFRAME: {
        R8EEnvFrame *frame = (R8EEnvFrame *)hdr;
        if (frame->slots) {
            for (uint16_t i = 0; i < frame->slot_count; i++) {
                r8e_visit_value(frame->slots[i], visitor, ud);
            }
        }
        break;
    }

    case R8E_GC_KIND_STRING:
        /* Strings hold no outgoing references */
        break;

    case R8E_GC_KIND_FUNCTION:
        /* Functions: bytecode is raw bytes, not GC-managed */
        break;

    default:
        break;
    }
}

/* =========================================================================
 * Free functions - release structural memory for each heap type
 *
 * These free the object's own allocations. Outgoing references are released
 * by r8e_release_children() which is called before the free.
 * ========================================================================= */

void r8e_free_object(R8EContext *ctx, void *obj) {
    R8EGCHeader *hdr = (R8EGCHeader *)obj;
    uint32_t tier = hdr->flags & R8E_GC_TIER_MASK;

    switch (tier) {
    case 0:
    case 1:
        /* All inline, no extra allocations */
        break;
    case 2: {
        R8EObjTier2 *t2 = (R8EObjTier2 *)obj;
        free(t2->props);
        break;
    }
    case 3: {
        R8EObjTier3 *t3 = (R8EObjTier3 *)obj;
        free(t3->buckets);
        break;
    }
    }

    if (r8e_gc_has_overflow(hdr)) {
        r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
    }
    ctx->gc.total_freed++;
    free(obj);
}

void r8e_free_string(R8EContext *ctx, R8EString *str) {
    if (!str) return;
    R8EGCHeader *hdr = (R8EGCHeader *)str;
    if (r8e_gc_has_overflow(hdr)) {
        r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
    }
    ctx->gc.total_freed++;
    free(str);
}

void r8e_free_array(R8EContext *ctx, R8EArray *arr) {
    if (!arr) return;
    free(arr->elements);
    if (arr->named) {
        r8e_free_object(ctx, arr->named);
    }
    R8EGCHeader *hdr = (R8EGCHeader *)arr;
    if (r8e_gc_has_overflow(hdr)) {
        r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
    }
    ctx->gc.total_freed++;
    free(arr);
}

void r8e_free_closure(R8EContext *ctx, R8EClosure *closure) {
    if (!closure) return;
    switch (closure->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        break;
    case R8E_CAPTURE_MODE_ARRAY:
        free(closure->storage.arr.values);
        free(closure->storage.arr.meta);
        /* env frame is released via its own refcount (r8e_release_children) */
        break;
    case R8E_CAPTURE_MODE_FRAME:
        /* env_frame released via refcount */
        break;
    }
    R8EGCHeader *hdr = (R8EGCHeader *)closure;
    if (r8e_gc_has_overflow(hdr)) {
        r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
    }
    ctx->gc.total_freed++;
    free(closure);
}

static void r8e_free_envframe(R8EContext *ctx, R8EEnvFrame *frame) {
    if (!frame) return;
    free(frame->slots);
    R8EGCHeader *hdr = (R8EGCHeader *)frame;
    if (r8e_gc_has_overflow(hdr)) {
        r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
    }
    ctx->gc.total_freed++;
    free(frame);
}

static void r8e_free_function(R8EContext *ctx, R8EFunction *func) {
    if (!func) return;
    free(func->bytecode);
    R8EGCHeader *hdr = (R8EGCHeader *)func;
    if (r8e_gc_has_overflow(hdr)) {
        r8e_ext_rc_remove(&ctx->gc.ext_rc, hdr);
    }
    ctx->gc.total_freed++;
    free(func);
}

/**
 * Dispatch to the correct free function based on GC kind.
 */
static void r8e_free_by_kind(R8EContext *ctx, R8EGCHeader *hdr) {
    switch (R8E_GC_GET_KIND(hdr->flags)) {
    case R8E_GC_KIND_OBJECT:
        r8e_free_object(ctx, hdr);
        break;
    case R8E_GC_KIND_STRING:
        r8e_free_string(ctx, (R8EString *)hdr);
        break;
    case R8E_GC_KIND_ARRAY:
        r8e_free_array(ctx, (R8EArray *)hdr);
        break;
    case R8E_GC_KIND_CLOSURE:
        r8e_free_closure(ctx, (R8EClosure *)hdr);
        break;
    case R8E_GC_KIND_ENVFRAME:
        r8e_free_envframe(ctx, (R8EEnvFrame *)hdr);
        break;
    case R8E_GC_KIND_FUNCTION:
        r8e_free_function(ctx, (R8EFunction *)hdr);
        break;
    default:
        free(hdr);
        break;
    }
}

/* =========================================================================
 * Release children: decrement refcounts on all outgoing references
 *
 * Called just before freeing an object so that referenced objects can be
 * collected if they become unreachable. Triggers recursive release.
 * ========================================================================= */

static void r8e_release_child_visitor(R8EGCHeader *ref, void *ud) {
    R8EContext *ctx = (R8EContext *)ud;
    R8EValue val = r8e_from_pointer(ref);
    r8e_release(ctx, val);
}

static void r8e_release_children(R8EContext *ctx, R8EGCHeader *hdr) {
    r8e_gc_visit_refs(hdr, r8e_release_child_visitor, ctx);
}

/* =========================================================================
 * Core RC operations: r8e_retain / r8e_release
 * ========================================================================= */

/**
 * r8e_retain - increment refcount if val is a heap pointer.
 * No-op for immediate values (doubles, int32, inline strings, etc.)
 */
void r8e_retain(R8EContext *ctx, R8EValue val) {
    if (!R8E_IS_POINTER(val)) return;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);
    if (!hdr) return;
    r8e_gc_inc_refcount(ctx, hdr);
}

/**
 * r8e_release - decrement refcount if val is a heap pointer.
 * If refcount reaches zero: release children, then free.
 * If refcount > 0 after decrement: add to suspect list (possible cycle).
 */
void r8e_release(R8EContext *ctx, R8EValue val) {
    if (!R8E_IS_POINTER(val)) return;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);
    if (!hdr) return;

    bool is_dead = r8e_gc_dec_refcount(ctx, hdr);
    if (is_dead) {
        if (hdr->flags & R8E_GC_ON_SUSPECT) {
            r8e_suspect_remove(ctx, hdr);
        }
        hdr->flags &= ~R8E_GC_ON_SUSPECT;
        r8e_release_children(ctx, hdr);
        r8e_free_by_kind(ctx, hdr);
    } else {
        r8e_suspect_add(ctx, hdr);
        if (ctx->gc.suspects.count >= ctx->gc.epoch_threshold) {
            r8e_cycle_scan(ctx);
        }
    }
}

/* =========================================================================
 * Conditional RC based on elision classification (Section 8.2)
 * ========================================================================= */

void r8e_retain_if_owned(R8EContext *ctx, R8EValue val,
                          uint8_t classification) {
    if (classification == R8E_RC_OWNED) {
        r8e_retain(ctx, val);
    }
}

void r8e_release_if_owned(R8EContext *ctx, R8EValue val,
                           uint8_t classification) {
    if (classification == R8E_RC_OWNED) {
        r8e_release(ctx, val);
    }
}

/* =========================================================================
 * Deferred decrement batching (Section 8.2)
 * ========================================================================= */

void r8e_defer_list_init(R8EDeferList *dl) {
    dl->inline_count = 0;
    dl->overflow = NULL;
    dl->overflow_count = 0;
    dl->overflow_capacity = 0;
}

void r8e_defer_list_destroy(R8EDeferList *dl) {
    free(dl->overflow);
    dl->overflow = NULL;
    dl->overflow_count = 0;
    dl->overflow_capacity = 0;
    dl->inline_count = 0;
}

void r8e_defer_release(R8EDeferList *dl, R8EValue val) {
    if (!R8E_IS_POINTER(val)) return;

    if (dl->inline_count < R8E_DEFER_INLINE_CAP) {
        dl->inline_buf[dl->inline_count++] = val;
        return;
    }

    if (dl->overflow_count >= dl->overflow_capacity) {
        uint32_t new_cap = (dl->overflow_capacity == 0)
                         ? 64
                         : dl->overflow_capacity * 2;
        R8EValue *buf = (R8EValue *)realloc(dl->overflow,
                                             new_cap * sizeof(R8EValue));
        if (!buf) return;
        dl->overflow = buf;
        dl->overflow_capacity = new_cap;
    }
    dl->overflow[dl->overflow_count++] = val;
}

static int r8e_value_cmp(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/**
 * r8e_flush_deferred - batch-release all deferred values.
 *
 * Deduplicates repeated values: if the same pointer appears N times,
 * we perform N decrements but stop as soon as the object is freed.
 */
void r8e_flush_deferred(R8EContext *ctx, R8EDeferList *dl) {
    uint32_t total = dl->inline_count + dl->overflow_count;
    if (total == 0) return;

    /* Fast path: few entries, release individually */
    if (total <= 4) {
        for (uint32_t i = 0; i < dl->inline_count; i++) {
            r8e_release(ctx, dl->inline_buf[i]);
        }
        for (uint32_t i = 0; i < dl->overflow_count; i++) {
            r8e_release(ctx, dl->overflow[i]);
        }
        dl->inline_count = 0;
        dl->overflow_count = 0;
        return;
    }

    /* Merge inline + overflow into a sorted buffer for dedup */
    R8EValue stack_buf[R8E_DEFER_INLINE_CAP + 64];
    R8EValue *merged;
    bool heap_merged = false;

    if (total <= (uint32_t)(R8E_DEFER_INLINE_CAP + 64)) {
        merged = stack_buf;
    } else {
        merged = (R8EValue *)malloc(total * sizeof(R8EValue));
        if (!merged) {
            /* OOM fallback: release individually */
            for (uint32_t i = 0; i < dl->inline_count; i++) {
                r8e_release(ctx, dl->inline_buf[i]);
            }
            for (uint32_t i = 0; i < dl->overflow_count; i++) {
                r8e_release(ctx, dl->overflow[i]);
            }
            dl->inline_count = 0;
            dl->overflow_count = 0;
            return;
        }
        heap_merged = true;
    }

    memcpy(merged, dl->inline_buf, dl->inline_count * sizeof(R8EValue));
    if (dl->overflow_count > 0) {
        memcpy(merged + dl->inline_count, dl->overflow,
               dl->overflow_count * sizeof(R8EValue));
    }

    qsort(merged, total, sizeof(R8EValue), r8e_value_cmp);

    /* Release with deduplication */
    uint32_t i = 0;
    while (i < total) {
        R8EValue val = merged[i];
        uint32_t run_len = 1;
        while (i + run_len < total && merged[i + run_len] == val) {
            run_len++;
        }

        if (R8E_IS_POINTER(val)) {
            R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);
            if (hdr) {
                bool freed = false;
                for (uint32_t j = 0; j < run_len; j++) {
                    bool is_dead = r8e_gc_dec_refcount(ctx, hdr);
                    if (is_dead) {
                        hdr->flags &= ~R8E_GC_ON_SUSPECT;
                        r8e_release_children(ctx, hdr);
                        r8e_free_by_kind(ctx, hdr);
                        freed = true;
                        break;
                    }
                }
                if (!freed) {
                    r8e_suspect_add(ctx, hdr);
                }
            }
        }

        i += run_len;
    }

    dl->inline_count = 0;
    dl->overflow_count = 0;
    if (heap_merged) free(merged);

    if (ctx->gc.suspects.count >= ctx->gc.epoch_threshold) {
        r8e_cycle_scan(ctx);
    }
}

/* =========================================================================
 * Cycle detection (Section 8.3)
 *
 * Trial decrement algorithm on the suspect list:
 *
 *   1. Snapshot suspects, mark them with GC_MARK
 *   2. Init trial_rc = actual refcount for each suspect
 *   3. For each suspect, visit children; if child is also a suspect,
 *      decrement its trial_rc (removes internal edges)
 *   4. trial_rc <= 0 => candidate is garbage (no external refs)
 *   5. Propagate liveness from non-garbage to garbage children
 *   6. Free confirmed garbage, rebuild suspect list with survivors
 *   7. Adapt threshold: halve on cycles found, double otherwise
 *
 * Complexity: O(S + E) where S = suspect count, E = edges among them.
 * S is bounded by epoch_threshold (default 4096).
 * ========================================================================= */

typedef struct {
    R8EGCHeader **suspects;
    int32_t      *trial_rc;
    uint32_t      count;
} R8ETrialCtx;

static void r8e_trial_dec_visitor(R8EGCHeader *ref, void *ud) {
    R8ETrialCtx *tc = (R8ETrialCtx *)ud;
    if (!(ref->flags & R8E_GC_MARK)) return;

    for (uint32_t i = 0; i < tc->count; i++) {
        if (tc->suspects[i] == ref) {
            tc->trial_rc[i]--;
            return;
        }
    }
}

typedef struct {
    R8EGCHeader **suspects;
    bool         *is_garbage;
    uint32_t      count;
    bool         *changed;
} R8EPropagateCtx;

static void r8e_propagate_visitor(R8EGCHeader *ref, void *ud) {
    R8EPropagateCtx *pc = (R8EPropagateCtx *)ud;
    if (!(ref->flags & R8E_GC_MARK)) return;

    for (uint32_t i = 0; i < pc->count; i++) {
        if (pc->suspects[i] == ref && pc->is_garbage[i]) {
            pc->is_garbage[i] = false;
            *pc->changed = true;
            return;
        }
    }
}

void r8e_cycle_scan(R8EContext *ctx) {
    R8ESuspectList *sl = &ctx->gc.suspects;
    if (sl->count == 0) return;

    uint32_t snap_count = sl->count;

    R8EGCHeader **snap = (R8EGCHeader **)malloc(
        snap_count * sizeof(R8EGCHeader *));
    int32_t *trial_rc = (int32_t *)malloc(snap_count * sizeof(int32_t));
    bool *is_garbage = (bool *)calloc(snap_count, sizeof(bool));

    if (!snap || !trial_rc || !is_garbage) {
        free(snap);
        free(trial_rc);
        free(is_garbage);
        return;
    }

    /* Phase 1: Snapshot and initialize */
    memcpy(snap, sl->items, snap_count * sizeof(R8EGCHeader *));
    for (uint32_t i = 0; i < snap_count; i++) {
        snap[i]->flags |= R8E_GC_MARK;
        trial_rc[i] = (int32_t)r8e_gc_full_refcount(ctx, snap[i]);
    }

    /* Phase 2: Trial decrement */
    R8ETrialCtx tc;
    tc.suspects = snap;
    tc.trial_rc = trial_rc;
    tc.count = snap_count;

    for (uint32_t i = 0; i < snap_count; i++) {
        r8e_gc_visit_refs(snap[i], r8e_trial_dec_visitor, &tc);
    }

    /* Phase 3: Mark garbage candidates */
    uint32_t garbage_count = 0;
    for (uint32_t i = 0; i < snap_count; i++) {
        if (trial_rc[i] <= 0) {
            is_garbage[i] = true;
            garbage_count++;
        }
    }

    /* Phase 4: Propagate liveness from survivors to their children */
    if (garbage_count > 0 && garbage_count < snap_count) {
        R8EPropagateCtx pc;
        pc.suspects = snap;
        pc.is_garbage = is_garbage;
        pc.count = snap_count;

        bool changed = true;
        while (changed) {
            changed = false;
            pc.changed = &changed;
            for (uint32_t i = 0; i < snap_count; i++) {
                if (!is_garbage[i]) {
                    r8e_gc_visit_refs(snap[i], r8e_propagate_visitor, &pc);
                }
            }
        }

        /* Recount */
        garbage_count = 0;
        for (uint32_t i = 0; i < snap_count; i++) {
            if (is_garbage[i]) garbage_count++;
        }
    }

    /* Phase 5: Clear marks */
    for (uint32_t i = 0; i < snap_count; i++) {
        snap[i]->flags &= ~R8E_GC_MARK;
        snap[i]->flags &= ~R8E_GC_ON_SUSPECT;
    }

    /* Phase 6: Free garbage
     * Zero refcounts first to prevent recursive release within the cycle
     * from causing double-free. */
    if (garbage_count > 0) {
        for (uint32_t i = 0; i < snap_count; i++) {
            if (is_garbage[i]) {
                r8e_gc_set_rc(snap[i], 0);
                if (r8e_gc_has_overflow(snap[i])) {
                    r8e_ext_rc_remove(&ctx->gc.ext_rc, snap[i]);
                    snap[i]->flags &= ~R8E_GC_RC_OVERFLOW_BIT;
                }
            }
        }

        for (uint32_t i = 0; i < snap_count; i++) {
            if (is_garbage[i]) {
                ctx->gc.cycles_collected++;
                r8e_free_by_kind(ctx, snap[i]);
            }
        }
    }

    /* Phase 7: Rebuild suspect list with survivors */
    sl->count = 0;
    for (uint32_t i = 0; i < snap_count; i++) {
        if (!is_garbage[i]) {
            if (sl->count < sl->capacity) {
                sl->items[sl->count] = snap[i];
                snap[i]->flags |= R8E_GC_ON_SUSPECT;
                sl->count++;
            }
        }
    }

    /* Phase 8: Adapt threshold */
    if (garbage_count > 0) {
        if (ctx->gc.epoch_threshold > 256) {
            ctx->gc.epoch_threshold /= 2;
        }
    } else {
        if (ctx->gc.epoch_threshold < 65536) {
            ctx->gc.epoch_threshold *= 2;
        }
    }

    free(snap);
    free(trial_rc);
    free(is_garbage);
}

/* =========================================================================
 * GC initialization and teardown
 * ========================================================================= */

void r8e_gc_init(R8EContext *ctx) {
    memset(&ctx->gc, 0, sizeof(R8EGCState));
    ctx->gc.epoch_threshold = 4096;
    r8e_suspect_list_init(&ctx->gc.suspects);
    r8e_ext_rc_init(&ctx->gc.ext_rc);
}

void r8e_gc_destroy(R8EContext *ctx) {
    r8e_suspect_list_destroy(&ctx->gc.suspects);
    r8e_ext_rc_destroy(&ctx->gc.ext_rc);
}

void r8e_gc_collect(R8EContext *ctx) {
    r8e_cycle_scan(ctx);
}

/* =========================================================================
 * GC statistics queries
 * ========================================================================= */

uint32_t r8e_gc_alive_count(const R8EContext *ctx) {
    return ctx->gc.total_allocated - ctx->gc.total_freed;
}

uint32_t r8e_gc_suspect_count(const R8EContext *ctx) {
    return ctx->gc.suspects.count;
}

uint32_t r8e_gc_cycles_collected(const R8EContext *ctx) {
    return ctx->gc.cycles_collected;
}

uint32_t r8e_gc_epoch_threshold(const R8EContext *ctx) {
    return ctx->gc.epoch_threshold;
}

/* =========================================================================
 * GC header initialization helper
 *
 * Called by allocators to set up the GC header with initial refcount = 1
 * and the correct kind tag.
 * ========================================================================= */

void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind, uint32_t proto_id) {
    hdr->flags = ((kind << R8E_GC_KIND_SHIFT) & R8E_GC_KIND_MASK)
               | (1u << R8E_GC_RC_INLINE_SHIFT); /* refcount = 1 */
    hdr->proto_id = proto_id;
}
