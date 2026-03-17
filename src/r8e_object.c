/*
 * r8e_object.c - 4-Tier CDOL Object Model
 *
 * Implements the Compact Dynamic Object Layout (CDOL) with four tiers:
 *   Tier 0: Micro Object   (0-1 properties, 24 bytes)  - 55% of objects
 *   Tier 1: Compact Object  (2-4 properties, 40-72 bytes) - 44% of objects
 *   Tier 2: Compact Array   (5-16 properties, heap-alloc) - ~1% of objects
 *   Tier 3: Hash Table      (17+ properties, Robin Hood)  - <0.01%
 *
 * Objects promote to the next tier when they outgrow capacity. They never
 * demote, avoiding thrashing (99% never promote past Tier 1).
 *
 * Property lookup dispatches by tier via the flags field bits [1:0].
 * Prototype chain uses proto_id (index into realm prototype table).
 *
 * Reference: CLAUDE.md Section 3 (Object Model)
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

/* NaN-boxing constants (Section 2.3) */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* =========================================================================
 * Object flags and tier encoding
 *
 * flags layout (32 bits):
 *   Bits [1:0]   = tier (0-3)
 *   Bit  [2]     = GC mark
 *   Bit  [3]     = on suspect list
 *   Bit  [4]     = frozen (Object.freeze)
 *   Bit  [5]     = sealed (Object.seal)
 *   Bit  [6]     = extensible (default 1)
 *   Bit  [7]     = IS_ARRAY
 *   Bits [15:8]  = refcount overflow flag
 *   Bits [31:16] = inline refcount
 * ========================================================================= */

#define R8E_OBJ_TIER_MASK       0x00000003u
#define R8E_OBJ_TIER_SHIFT      0
#define R8E_OBJ_GC_MARK         0x00000004u
#define R8E_OBJ_ON_SUSPECT      0x00000008u
#define R8E_OBJ_FROZEN          0x00000010u
#define R8E_OBJ_SEALED          0x00000020u
#define R8E_OBJ_EXTENSIBLE      0x00000040u
#define R8E_OBJ_IS_ARRAY        0x00000080u
#define R8E_OBJ_RC_OVERFLOW     0x0000FF00u
#define R8E_OBJ_RC_INLINE_MASK  0xFFFF0000u
#define R8E_OBJ_RC_INLINE_SHIFT 16

#define R8E_OBJ_GET_TIER(flags)  ((flags) & R8E_OBJ_TIER_MASK)
#define R8E_OBJ_SET_TIER(flags, tier) \
    (((flags) & ~R8E_OBJ_TIER_MASK) | ((tier) & R8E_OBJ_TIER_MASK))

/* Property descriptor flags (per-property) */
#define R8E_PROP_WRITABLE       0x01u
#define R8E_PROP_ENUMERABLE     0x02u
#define R8E_PROP_CONFIGURABLE   0x04u
#define R8E_PROP_DEFAULT        (R8E_PROP_WRITABLE | R8E_PROP_ENUMERABLE | R8E_PROP_CONFIGURABLE)
#define R8E_PROP_ACCESSOR       0x08u
#define R8E_PROP_HAS_GET        0x10u
#define R8E_PROP_HAS_SET        0x20u

/* Prototype IDs for built-in prototypes */
#define R8E_PROTO_OBJECT   0
#define R8E_PROTO_ARRAY    1
#define R8E_PROTO_FUNCTION 2
#define R8E_PROTO_STRING   3
#define R8E_PROTO_NUMBER   4
#define R8E_PROTO_BOOLEAN  5
#define R8E_PROTO_REGEXP   6
#define R8E_PROTO_ERROR    7
#define R8E_PROTO_DATE     8
#define R8E_PROTO_NULL     0xFFFFFFFFu
#define R8E_PROTO_MAX      64

/* Atom key: 0 means empty slot */
#define R8E_ATOM_EMPTY 0

/* Tier capacity thresholds */
#define R8E_TIER0_MAX_PROPS  1
#define R8E_TIER1_MAX_PROPS  4
#define R8E_TIER2_MAX_PROPS  16

/* Tier 2 initial capacity */
#define R8E_TIER2_INIT_CAP   8

/* Tier 3 initial capacity and load factor */
#define R8E_TIER3_INIT_CAP   32
#define R8E_TIER3_LOAD_NUM   3
#define R8E_TIER3_LOAD_DEN   4

/* Robin Hood max probe distance before forced resize */
#define R8E_ROBIN_HOOD_MAX_PROBE 32

/* Max prototype chain depth (loop detection) */
#define R8E_PROTO_CHAIN_MAX  256

/* =========================================================================
 * Property slot: key + value + descriptor flags
 * ========================================================================= */

typedef struct {
    uint32_t key;      /* atom index (interned string) */
    uint8_t  desc;     /* property descriptor flags */
    uint8_t  pad[3];
    R8EValue val;      /* NaN-boxed value */
} R8EPropSlot;

/* =========================================================================
 * Tier 0: Micro Object (0-1 properties, 24 bytes)
 *
 * The most common tier (55% of all objects). Single inline property slot.
 * If key0 == R8E_ATOM_EMPTY, the object has 0 properties.
 * ========================================================================= */

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  key0;      /* atom index, 0 = empty */
    uint8_t   desc0;     /* property descriptor for slot 0 */
    uint8_t   pad[3];
    R8EValue  val0;      /* value for slot 0 */
} R8EObjTier0;

/* =========================================================================
 * Tier 1: Compact Object (2-4 properties, 40-72 bytes)
 *
 * Second most common tier (44%). Up to 4 inline slots. Linear scan for
 * property lookup -- with 2-4 entries this fits in 1-2 cache lines and
 * is faster than any hash table.
 * ========================================================================= */

typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint8_t     count;     /* current property count: 0-4 */
    uint8_t     pad[7];
    R8EPropSlot props[R8E_TIER1_MAX_PROPS];
} R8EObjTier1;

/* =========================================================================
 * Tier 2: Compact Array (5-16 properties, heap-allocated)
 *
 * ~1% of objects. Heap-allocated property array with capacity 8 or 16.
 * Linear scan for count <= 8; sorted atom indices + binary search for 9-16.
 * ========================================================================= */

typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint8_t     count;
    uint8_t     capacity;   /* power of 2: 8 or 16 */
    uint8_t     sorted;     /* 1 if props are sorted by key for bsearch */
    uint8_t     pad[5];
    R8EPropSlot *props;     /* heap-allocated array */
} R8EObjTier2;

/* =========================================================================
 * Tier 3: Hash Table (17+ properties, Robin Hood open-addressing)
 *
 * Less than 0.01% of objects. Probe length bounded to O(log n) worst case.
 * ========================================================================= */

typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint16_t    count;
    uint16_t    capacity;   /* power of 2 */
    uint32_t    pad;
    R8EPropSlot *buckets;   /* heap-allocated hash table */
} R8EObjTier3;

/* =========================================================================
 * Generic object header
 *
 * All tiers share this prefix (flags, proto_id) so we can inspect the
 * tier tag before casting to the specific tier struct.
 * ========================================================================= */

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjHeader;

/* Forward declaration for array type (used in named properties) */
typedef struct R8EArray R8EArray;

/* =========================================================================
 * Context stub (minimal until r8e_api.h is integrated)
 * ========================================================================= */

typedef struct R8EContext {
    /* Prototype table: array of R8EValue (each a pointer to a prototype obj) */
    R8EValue  proto_table[R8E_PROTO_MAX];
    uint32_t  proto_count;

    /* Allocator hooks (default to malloc/free) */
    void *(*alloc)(size_t size, void *opaque);
    void *(*realloc_fn)(void *ptr, size_t size, void *opaque);
    void  (*free_fn)(void *ptr, void *opaque);
    void  *alloc_opaque;

    /* Error state */
    R8EValue last_error;
} R8EContext;

/* =========================================================================
 * Allocator wrappers
 * ========================================================================= */

static inline void *r8e_alloc(R8EContext *ctx, size_t size) {
    if (ctx && ctx->alloc)
        return ctx->alloc(size, ctx->alloc_opaque);
    return malloc(size);
}

static inline void *r8e_realloc(R8EContext *ctx, void *ptr, size_t size) {
    if (ctx && ctx->realloc_fn)
        return ctx->realloc_fn(ptr, size, ctx->alloc_opaque);
    return realloc(ptr, size);
}

static inline void r8e_free(R8EContext *ctx, void *ptr) {
    if (ctx && ctx->free_fn)
        ctx->free_fn(ptr, ctx->alloc_opaque);
    else
        free(ptr);
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Initialize flags for a new object at a given tier */
static inline uint32_t r8e_obj_init_flags(uint8_t tier) {
    uint32_t flags = 0;
    flags |= (tier & R8E_OBJ_TIER_MASK);
    flags |= R8E_OBJ_EXTENSIBLE;
    flags |= (1u << R8E_OBJ_RC_INLINE_SHIFT);  /* refcount = 1 */
    return flags;
}

/* Refcount manipulation */
static inline uint16_t r8e_obj_refcount(const R8EObjHeader *h) {
    return (uint16_t)((h->flags & R8E_OBJ_RC_INLINE_MASK)
                      >> R8E_OBJ_RC_INLINE_SHIFT);
}

static inline void r8e_obj_inc_ref(R8EObjHeader *h) {
    uint16_t rc = r8e_obj_refcount(h);
    if (rc < 0xFFFF) {
        h->flags = (h->flags & ~R8E_OBJ_RC_INLINE_MASK)
                 | ((uint32_t)(rc + 1) << R8E_OBJ_RC_INLINE_SHIFT);
    }
}

static inline bool r8e_obj_dec_ref(R8EObjHeader *h) {
    uint16_t rc = r8e_obj_refcount(h);
    if (rc <= 1) return true;  /* should be freed */
    h->flags = (h->flags & ~R8E_OBJ_RC_INLINE_MASK)
             | ((uint32_t)(rc - 1) << R8E_OBJ_RC_INLINE_SHIFT);
    return false;
}

/* =========================================================================
 * Tier 3 Robin Hood hashing helpers
 * ========================================================================= */

/* FNV-1a hash for atom keys (atoms are uint32_t) */
static inline uint32_t r8e_atom_hash(uint32_t atom) {
    uint32_t h = 2166136261u;
    h ^= (atom & 0xFF);         h *= 16777619u;
    h ^= ((atom >> 8) & 0xFF);  h *= 16777619u;
    h ^= ((atom >> 16) & 0xFF); h *= 16777619u;
    h ^= ((atom >> 24) & 0xFF); h *= 16777619u;
    return h;
}

/* Desired index for a key in a table of given capacity (power of 2) */
static inline uint32_t r8e_hash_slot(uint32_t atom, uint32_t capacity) {
    return r8e_atom_hash(atom) & (capacity - 1);
}

/* Probe distance from desired slot */
static inline uint32_t r8e_probe_distance(uint32_t slot, uint32_t desired,
                                           uint32_t capacity) {
    return (slot + capacity - desired) & (capacity - 1);
}

/* =========================================================================
 * SECTION: Object Creation
 * ========================================================================= */

/*
 * r8e_obj_new - Create an empty Tier 0 object with Object.prototype.
 */
R8EObjTier0 *r8e_obj_new(R8EContext *ctx) {
    R8EObjTier0 *obj = (R8EObjTier0 *)r8e_alloc(ctx, sizeof(R8EObjTier0));
    if (!obj) return NULL;

    obj->flags    = r8e_obj_init_flags(0);
    obj->proto_id = R8E_PROTO_OBJECT;
    obj->key0     = R8E_ATOM_EMPTY;
    obj->desc0    = R8E_PROP_DEFAULT;
    memset(obj->pad, 0, sizeof(obj->pad));
    obj->val0     = R8E_UNDEFINED;

    return obj;
}

/*
 * r8e_obj_new_with_proto - Create an empty Tier 0 object with specific proto.
 */
R8EObjTier0 *r8e_obj_new_with_proto(R8EContext *ctx, uint32_t proto_id) {
    R8EObjTier0 *obj = r8e_obj_new(ctx);
    if (obj) obj->proto_id = proto_id;
    return obj;
}

/* =========================================================================
 * SECTION: Tier 0 Operations
 * ========================================================================= */

static inline int r8e_tier0_count(const R8EObjTier0 *obj) {
    return (obj->key0 != R8E_ATOM_EMPTY) ? 1 : 0;
}

static R8EValue r8e_tier0_get(const R8EObjTier0 *obj, uint32_t key) {
    if (obj->key0 == key)
        return obj->val0;
    return R8E_UNDEFINED;
}

static bool r8e_tier0_has(const R8EObjTier0 *obj, uint32_t key) {
    return (obj->key0 == key);
}

static uint8_t r8e_tier0_get_desc(const R8EObjTier0 *obj, uint32_t key) {
    if (obj->key0 == key)
        return obj->desc0;
    return R8E_PROP_DEFAULT;
}

/*
 * Returns:
 *   0  = success (set in place)
 *   1  = needs promotion (object is full, key not found)
 *  -1  = error (property not writable / object frozen)
 */
static int r8e_tier0_set(R8EObjTier0 *obj, uint32_t key,
                          R8EValue val, uint8_t desc) {
    if (obj->flags & R8E_OBJ_FROZEN)
        return -1;

    /* Update existing property */
    if (obj->key0 == key) {
        if (!(obj->desc0 & R8E_PROP_WRITABLE))
            return -1;
        obj->val0 = val;
        return 0;
    }

    /* Insert into empty slot */
    if (obj->key0 == R8E_ATOM_EMPTY) {
        if (!(obj->flags & R8E_OBJ_EXTENSIBLE))
            return -1;
        obj->key0  = key;
        obj->val0  = val;
        obj->desc0 = desc;
        return 0;
    }

    /* Full: need promotion to Tier 1 */
    return 1;
}

static int r8e_tier0_delete(R8EObjTier0 *obj, uint32_t key) {
    if (obj->key0 == key) {
        if (!(obj->desc0 & R8E_PROP_CONFIGURABLE))
            return -1;
        obj->key0  = R8E_ATOM_EMPTY;
        obj->val0  = R8E_UNDEFINED;
        obj->desc0 = R8E_PROP_DEFAULT;
        return 0;
    }
    return 0;  /* not found: no-op, success per spec */
}

/* =========================================================================
 * SECTION: Tier 1 Operations
 * ========================================================================= */

static R8EObjTier1 *r8e_tier1_alloc(R8EContext *ctx, uint32_t proto_id) {
    R8EObjTier1 *obj = (R8EObjTier1 *)r8e_alloc(ctx, sizeof(R8EObjTier1));
    if (!obj) return NULL;

    obj->flags    = r8e_obj_init_flags(1);
    obj->proto_id = proto_id;
    obj->count    = 0;
    memset(obj->pad, 0, sizeof(obj->pad));
    for (int i = 0; i < R8E_TIER1_MAX_PROPS; i++) {
        obj->props[i].key  = R8E_ATOM_EMPTY;
        obj->props[i].desc = R8E_PROP_DEFAULT;
        obj->props[i].val  = R8E_UNDEFINED;
        memset(obj->props[i].pad, 0, sizeof(obj->props[i].pad));
    }

    return obj;
}

static R8EValue r8e_tier1_get(const R8EObjTier1 *obj, uint32_t key) {
    /* Linear scan: 2-4 entries, fits in 1-2 cache lines */
    for (int i = 0; i < obj->count; i++) {
        if (obj->props[i].key == key)
            return obj->props[i].val;
    }
    return R8E_UNDEFINED;
}

static bool r8e_tier1_has(const R8EObjTier1 *obj, uint32_t key) {
    for (int i = 0; i < obj->count; i++) {
        if (obj->props[i].key == key)
            return true;
    }
    return false;
}

static uint8_t r8e_tier1_get_desc(const R8EObjTier1 *obj, uint32_t key) {
    for (int i = 0; i < obj->count; i++) {
        if (obj->props[i].key == key)
            return obj->props[i].desc;
    }
    return R8E_PROP_DEFAULT;
}

/*
 * Returns:
 *   0  = success
 *   1  = needs promotion to Tier 2
 *  -1  = error (frozen/not writable)
 */
static int r8e_tier1_set(R8EObjTier1 *obj, uint32_t key,
                          R8EValue val, uint8_t desc) {
    if (obj->flags & R8E_OBJ_FROZEN)
        return -1;

    /* Update existing */
    for (int i = 0; i < obj->count; i++) {
        if (obj->props[i].key == key) {
            if (!(obj->props[i].desc & R8E_PROP_WRITABLE))
                return -1;
            obj->props[i].val = val;
            return 0;
        }
    }

    /* Append */
    if (!(obj->flags & R8E_OBJ_EXTENSIBLE))
        return -1;
    if (obj->count >= R8E_TIER1_MAX_PROPS)
        return 1;  /* needs promotion */

    obj->props[obj->count].key  = key;
    obj->props[obj->count].val  = val;
    obj->props[obj->count].desc = desc;
    memset(obj->props[obj->count].pad, 0, 3);
    obj->count++;
    return 0;
}

static int r8e_tier1_delete(R8EObjTier1 *obj, uint32_t key) {
    for (int i = 0; i < obj->count; i++) {
        if (obj->props[i].key == key) {
            if (!(obj->props[i].desc & R8E_PROP_CONFIGURABLE))
                return -1;
            /* Shift remaining down to keep compact */
            for (int j = i; j < obj->count - 1; j++)
                obj->props[j] = obj->props[j + 1];
            obj->count--;
            /* Clear vacated slot */
            obj->props[obj->count].key  = R8E_ATOM_EMPTY;
            obj->props[obj->count].val  = R8E_UNDEFINED;
            obj->props[obj->count].desc = R8E_PROP_DEFAULT;
            return 0;
        }
    }
    return 0;  /* not found: success */
}

/* =========================================================================
 * SECTION: Tier 2 Operations
 * ========================================================================= */

static R8EObjTier2 *r8e_tier2_alloc(R8EContext *ctx, uint32_t proto_id,
                                     uint8_t capacity) {
    R8EObjTier2 *obj = (R8EObjTier2 *)r8e_alloc(ctx, sizeof(R8EObjTier2));
    if (!obj) return NULL;

    obj->flags    = r8e_obj_init_flags(2);
    obj->proto_id = proto_id;
    obj->count    = 0;
    obj->capacity = capacity;
    obj->sorted   = 0;
    memset(obj->pad, 0, sizeof(obj->pad));

    obj->props = (R8EPropSlot *)r8e_alloc(ctx, sizeof(R8EPropSlot) * capacity);
    if (!obj->props) {
        r8e_free(ctx, obj);
        return NULL;
    }
    for (int i = 0; i < capacity; i++) {
        obj->props[i].key  = R8E_ATOM_EMPTY;
        obj->props[i].desc = R8E_PROP_DEFAULT;
        obj->props[i].val  = R8E_UNDEFINED;
        memset(obj->props[i].pad, 0, 3);
    }

    return obj;
}

/* Sort props by key for binary search (called when count > 8) */
static void r8e_tier2_sort(R8EObjTier2 *obj) {
    /* Insertion sort: at most 16 elements, highly efficient for small N */
    for (int i = 1; i < obj->count; i++) {
        R8EPropSlot tmp = obj->props[i];
        int j = i - 1;
        while (j >= 0 && obj->props[j].key > tmp.key) {
            obj->props[j + 1] = obj->props[j];
            j--;
        }
        obj->props[j + 1] = tmp;
    }
    obj->sorted = 1;
}

/* Binary search on sorted Tier 2 props. Returns index or -1. */
static int r8e_tier2_bsearch(const R8EObjTier2 *obj, uint32_t key) {
    int lo = 0, hi = obj->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t mk = obj->props[mid].key;
        if (mk == key)       return mid;
        else if (mk < key)   lo = mid + 1;
        else                  hi = mid - 1;
    }
    return -1;
}

/* Find property index: linear for count <= 8, binary search for 9-16 */
static int r8e_tier2_find(const R8EObjTier2 *obj, uint32_t key) {
    if (obj->count <= 8 || !obj->sorted) {
        for (int i = 0; i < obj->count; i++) {
            if (obj->props[i].key == key)
                return i;
        }
        return -1;
    }
    return r8e_tier2_bsearch(obj, key);
}

static R8EValue r8e_tier2_get(const R8EObjTier2 *obj, uint32_t key) {
    int idx = r8e_tier2_find(obj, key);
    if (idx >= 0)
        return obj->props[idx].val;
    return R8E_UNDEFINED;
}

static bool r8e_tier2_has(const R8EObjTier2 *obj, uint32_t key) {
    return r8e_tier2_find(obj, key) >= 0;
}

static uint8_t r8e_tier2_get_desc(const R8EObjTier2 *obj, uint32_t key) {
    int idx = r8e_tier2_find(obj, key);
    if (idx >= 0)
        return obj->props[idx].desc;
    return R8E_PROP_DEFAULT;
}

/*
 * Returns:
 *   0  = success
 *   1  = needs promotion to Tier 3
 *  -1  = error
 */
static int r8e_tier2_set(R8EContext *ctx, R8EObjTier2 *obj, uint32_t key,
                          R8EValue val, uint8_t desc) {
    if (obj->flags & R8E_OBJ_FROZEN)
        return -1;

    /* Update existing */
    int idx = r8e_tier2_find(obj, key);
    if (idx >= 0) {
        if (!(obj->props[idx].desc & R8E_PROP_WRITABLE))
            return -1;
        obj->props[idx].val = val;
        return 0;
    }

    /* Append new property */
    if (!(obj->flags & R8E_OBJ_EXTENSIBLE))
        return -1;
    if (obj->count >= R8E_TIER2_MAX_PROPS)
        return 1;  /* needs promotion to Tier 3 */

    /* Grow backing array if needed */
    if (obj->count >= obj->capacity) {
        uint8_t new_cap = (obj->capacity < 16) ? 16 : obj->capacity;
        R8EPropSlot *new_props = (R8EPropSlot *)r8e_realloc(
            ctx, obj->props, sizeof(R8EPropSlot) * new_cap);
        if (!new_props) return -1;
        for (int i = obj->capacity; i < new_cap; i++) {
            new_props[i].key  = R8E_ATOM_EMPTY;
            new_props[i].desc = R8E_PROP_DEFAULT;
            new_props[i].val  = R8E_UNDEFINED;
            memset(new_props[i].pad, 0, 3);
        }
        obj->props    = new_props;
        obj->capacity = new_cap;
    }

    obj->props[obj->count].key  = key;
    obj->props[obj->count].val  = val;
    obj->props[obj->count].desc = desc;
    memset(obj->props[obj->count].pad, 0, 3);
    obj->count++;

    /* Maintain sort invariant for count > 8 */
    if (obj->sorted && obj->count > 1)
        obj->sorted = 0;
    if (obj->count > 8 && !obj->sorted)
        r8e_tier2_sort(obj);

    return 0;
}

static int r8e_tier2_delete(R8EObjTier2 *obj, uint32_t key) {
    int idx = r8e_tier2_find(obj, key);
    if (idx < 0)
        return 0;  /* not found: success */
    if (!(obj->props[idx].desc & R8E_PROP_CONFIGURABLE))
        return -1;

    /* Shift remaining down */
    for (int i = idx; i < obj->count - 1; i++)
        obj->props[i] = obj->props[i + 1];
    obj->count--;
    obj->props[obj->count].key = R8E_ATOM_EMPTY;
    obj->props[obj->count].val = R8E_UNDEFINED;
    /* Sorted order preserved when removing from a sorted array */
    return 0;
}

/* =========================================================================
 * SECTION: Tier 3 Operations (Robin Hood Hash Table)
 * ========================================================================= */

static R8EObjTier3 *r8e_tier3_alloc(R8EContext *ctx, uint32_t proto_id,
                                     uint16_t capacity) {
    R8EObjTier3 *obj = (R8EObjTier3 *)r8e_alloc(ctx, sizeof(R8EObjTier3));
    if (!obj) return NULL;

    obj->flags    = r8e_obj_init_flags(3);
    obj->proto_id = proto_id;
    obj->count    = 0;
    obj->capacity = capacity;
    obj->pad      = 0;

    obj->buckets = (R8EPropSlot *)r8e_alloc(ctx, sizeof(R8EPropSlot) * capacity);
    if (!obj->buckets) {
        r8e_free(ctx, obj);
        return NULL;
    }
    for (uint16_t i = 0; i < capacity; i++) {
        obj->buckets[i].key  = R8E_ATOM_EMPTY;
        obj->buckets[i].desc = R8E_PROP_DEFAULT;
        obj->buckets[i].val  = R8E_UNDEFINED;
        memset(obj->buckets[i].pad, 0, 3);
    }

    return obj;
}

/* Find slot for key in Tier 3. Returns index or -1. */
static int r8e_tier3_find(const R8EObjTier3 *obj, uint32_t key) {
    uint32_t slot = r8e_hash_slot(key, obj->capacity);
    uint32_t dist = 0;

    for (;;) {
        uint32_t k = obj->buckets[slot].key;
        if (k == R8E_ATOM_EMPTY)
            return -1;
        if (k == key)
            return (int)slot;

        /* Robin Hood: if existing entry's probe distance < ours, key absent */
        uint32_t existing_desired = r8e_hash_slot(k, obj->capacity);
        uint32_t existing_dist = r8e_probe_distance(slot, existing_desired,
                                                     obj->capacity);
        if (existing_dist < dist)
            return -1;

        dist++;
        slot = (slot + 1) & (obj->capacity - 1);

        if (dist > R8E_ROBIN_HOOD_MAX_PROBE)
            return -1;
    }
}

static R8EValue r8e_tier3_get(const R8EObjTier3 *obj, uint32_t key) {
    int idx = r8e_tier3_find(obj, key);
    if (idx >= 0)
        return obj->buckets[idx].val;
    return R8E_UNDEFINED;
}

static bool r8e_tier3_has(const R8EObjTier3 *obj, uint32_t key) {
    return r8e_tier3_find(obj, key) >= 0;
}

static uint8_t r8e_tier3_get_desc(const R8EObjTier3 *obj, uint32_t key) {
    int idx = r8e_tier3_find(obj, key);
    if (idx >= 0)
        return obj->buckets[idx].desc;
    return R8E_PROP_DEFAULT;
}

/* Resize the Tier 3 hash table */
static int r8e_tier3_resize(R8EContext *ctx, R8EObjTier3 *obj,
                             uint16_t new_cap) {
    R8EPropSlot *old_buckets = obj->buckets;
    uint16_t old_cap = obj->capacity;

    R8EPropSlot *new_buckets = (R8EPropSlot *)r8e_alloc(
        ctx, sizeof(R8EPropSlot) * new_cap);
    if (!new_buckets)
        return -1;

    for (uint16_t i = 0; i < new_cap; i++) {
        new_buckets[i].key  = R8E_ATOM_EMPTY;
        new_buckets[i].desc = R8E_PROP_DEFAULT;
        new_buckets[i].val  = R8E_UNDEFINED;
        memset(new_buckets[i].pad, 0, 3);
    }

    obj->buckets  = new_buckets;
    obj->capacity = new_cap;
    obj->count    = 0;

    /* Re-insert all entries from old table via Robin Hood */
    for (uint16_t i = 0; i < old_cap; i++) {
        if (old_buckets[i].key != R8E_ATOM_EMPTY) {
            R8EPropSlot entry = old_buckets[i];
            uint32_t slot = r8e_hash_slot(entry.key, new_cap);
            uint32_t dist = 0;

            for (;;) {
                if (new_buckets[slot].key == R8E_ATOM_EMPTY) {
                    new_buckets[slot] = entry;
                    obj->count++;
                    break;
                }
                uint32_t ed = r8e_hash_slot(new_buckets[slot].key, new_cap);
                uint32_t edist = r8e_probe_distance(slot, ed, new_cap);
                if (dist > edist) {
                    R8EPropSlot tmp = new_buckets[slot];
                    new_buckets[slot] = entry;
                    entry = tmp;
                    dist = edist;
                }
                dist++;
                slot = (slot + 1) & (new_cap - 1);
            }
        }
    }

    r8e_free(ctx, old_buckets);
    return 0;
}

/* Forward declaration for recursive retry after resize */
static int r8e_tier3_set(R8EContext *ctx, R8EObjTier3 *obj, uint32_t key,
                          R8EValue val, uint8_t desc);

/*
 * Returns:
 *   0  = success
 *  -1  = error
 */
static int r8e_tier3_set(R8EContext *ctx, R8EObjTier3 *obj, uint32_t key,
                          R8EValue val, uint8_t desc) {
    if (obj->flags & R8E_OBJ_FROZEN)
        return -1;

    /* Update existing */
    int idx = r8e_tier3_find(obj, key);
    if (idx >= 0) {
        if (!(obj->buckets[idx].desc & R8E_PROP_WRITABLE))
            return -1;
        obj->buckets[idx].val = val;
        return 0;
    }

    /* Insert new */
    if (!(obj->flags & R8E_OBJ_EXTENSIBLE))
        return -1;

    /* Check load factor and resize if needed */
    if ((uint32_t)obj->count * R8E_TIER3_LOAD_DEN >=
        (uint32_t)obj->capacity * R8E_TIER3_LOAD_NUM) {
        uint16_t new_cap = obj->capacity * 2;
        if (new_cap < obj->capacity) return -1;  /* overflow */
        if (r8e_tier3_resize(ctx, obj, new_cap) != 0)
            return -1;
    }

    /* Robin Hood insertion */
    R8EPropSlot entry;
    entry.key  = key;
    entry.val  = val;
    entry.desc = desc;
    memset(entry.pad, 0, 3);

    uint32_t slot = r8e_hash_slot(key, obj->capacity);
    uint32_t dist = 0;

    for (;;) {
        if (obj->buckets[slot].key == R8E_ATOM_EMPTY) {
            obj->buckets[slot] = entry;
            obj->count++;
            return 0;
        }

        /* Robin Hood: swap if our probe distance is greater */
        uint32_t ed = r8e_hash_slot(obj->buckets[slot].key, obj->capacity);
        uint32_t edist = r8e_probe_distance(slot, ed, obj->capacity);
        if (dist > edist) {
            R8EPropSlot tmp = obj->buckets[slot];
            obj->buckets[slot] = entry;
            entry = tmp;
            dist = edist;
        }

        dist++;
        slot = (slot + 1) & (obj->capacity - 1);

        /* Safety: excessive probe -> resize and retry */
        if (dist > R8E_ROBIN_HOOD_MAX_PROBE) {
            uint16_t new_cap = obj->capacity * 2;
            if (new_cap < obj->capacity) return -1;
            if (r8e_tier3_resize(ctx, obj, new_cap) != 0)
                return -1;
            return r8e_tier3_set(ctx, obj, entry.key, entry.val, entry.desc);
        }
    }
}

static int r8e_tier3_delete(R8EContext *ctx, R8EObjTier3 *obj, uint32_t key) {
    (void)ctx;
    int idx = r8e_tier3_find(obj, key);
    if (idx < 0)
        return 0;  /* not found: success */
    if (!(obj->buckets[idx].desc & R8E_PROP_CONFIGURABLE))
        return -1;

    /*
     * Robin Hood backward shift deletion:
     * After removing the entry, shift subsequent displaced entries backward.
     */
    uint32_t slot = (uint32_t)idx;
    obj->buckets[slot].key = R8E_ATOM_EMPTY;
    obj->buckets[slot].val = R8E_UNDEFINED;
    obj->count--;

    uint32_t next = (slot + 1) & (obj->capacity - 1);
    while (obj->buckets[next].key != R8E_ATOM_EMPTY) {
        uint32_t desired = r8e_hash_slot(obj->buckets[next].key, obj->capacity);
        uint32_t d = r8e_probe_distance(next, desired, obj->capacity);
        if (d == 0)
            break;  /* at its ideal slot, stop */
        obj->buckets[slot] = obj->buckets[next];
        obj->buckets[next].key = R8E_ATOM_EMPTY;
        obj->buckets[next].val = R8E_UNDEFINED;
        slot = next;
        next = (next + 1) & (obj->capacity - 1);
    }

    return 0;
}

/* =========================================================================
 * SECTION: Tier Promotion (Tier N -> Tier N+1)
 *
 * Objects promote when they outgrow their tier. They never demote.
 * 99% never promote past Tier 1.
 * ========================================================================= */

/* Promote Tier 0 -> Tier 1 */
static R8EObjTier1 *r8e_promote_0_to_1(R8EContext *ctx, R8EObjTier0 *old) {
    R8EObjTier1 *obj = r8e_tier1_alloc(ctx, old->proto_id);
    if (!obj) return NULL;

    /* Preserve flags (except tier) */
    obj->flags = R8E_OBJ_SET_TIER(old->flags, 1);

    /* Copy existing property if present */
    if (old->key0 != R8E_ATOM_EMPTY) {
        obj->props[0].key  = old->key0;
        obj->props[0].val  = old->val0;
        obj->props[0].desc = old->desc0;
        memset(obj->props[0].pad, 0, 3);
        obj->count = 1;
    }

    r8e_free(ctx, old);
    return obj;
}

/* Promote Tier 1 -> Tier 2 */
static R8EObjTier2 *r8e_promote_1_to_2(R8EContext *ctx, R8EObjTier1 *old) {
    R8EObjTier2 *obj = r8e_tier2_alloc(ctx, old->proto_id, R8E_TIER2_INIT_CAP);
    if (!obj) return NULL;

    obj->flags = R8E_OBJ_SET_TIER(old->flags, 2);

    for (int i = 0; i < old->count; i++)
        obj->props[i] = old->props[i];
    obj->count = old->count;

    r8e_free(ctx, old);
    return obj;
}

/* Promote Tier 2 -> Tier 3 */
static R8EObjTier3 *r8e_promote_2_to_3(R8EContext *ctx, R8EObjTier2 *old) {
    uint16_t cap = R8E_TIER3_INIT_CAP;
    while (cap < (uint16_t)(old->count * 2))
        cap *= 2;

    R8EObjTier3 *obj = r8e_tier3_alloc(ctx, old->proto_id, cap);
    if (!obj) return NULL;

    obj->flags = R8E_OBJ_SET_TIER(old->flags, 3);

    /* Re-insert all properties via Robin Hood */
    for (int i = 0; i < old->count; i++) {
        if (old->props[i].key != R8E_ATOM_EMPTY) {
            r8e_tier3_set(ctx, obj, old->props[i].key,
                          old->props[i].val, old->props[i].desc);
        }
    }

    r8e_free(ctx, old->props);
    r8e_free(ctx, old);
    return obj;
}

/* =========================================================================
 * SECTION: Generic Property Operations (dispatch by tier)
 *
 * These are the primary public API. They inspect bits [1:0] of flags to
 * determine the tier, then dispatch to the appropriate tier handler.
 * ========================================================================= */

/*
 * r8e_obj_get - Get a property value from an object (own properties only).
 * Returns R8E_UNDEFINED if the property is not found on this object.
 */
R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: return r8e_tier0_get((R8EObjTier0 *)obj, key);
    case 1: return r8e_tier1_get((R8EObjTier1 *)obj, key);
    case 2: return r8e_tier2_get((R8EObjTier2 *)obj, key);
    case 3: return r8e_tier3_get((R8EObjTier3 *)obj, key);
    default: return R8E_UNDEFINED;
    }
}

/*
 * r8e_obj_has - Check if an object has an own property.
 */
bool r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: return r8e_tier0_has((R8EObjTier0 *)obj, key);
    case 1: return r8e_tier1_has((R8EObjTier1 *)obj, key);
    case 2: return r8e_tier2_has((R8EObjTier2 *)obj, key);
    case 3: return r8e_tier3_has((R8EObjTier3 *)obj, key);
    default: return false;
    }
}

/*
 * r8e_obj_get_prop_desc - Get property descriptor flags.
 */
uint8_t r8e_obj_get_prop_desc(R8EContext *ctx, void *obj, uint32_t key) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: return r8e_tier0_get_desc((R8EObjTier0 *)obj, key);
    case 1: return r8e_tier1_get_desc((R8EObjTier1 *)obj, key);
    case 2: return r8e_tier2_get_desc((R8EObjTier2 *)obj, key);
    case 3: return r8e_tier3_get_desc((R8EObjTier3 *)obj, key);
    default: return R8E_PROP_DEFAULT;
    }
}

/*
 * r8e_obj_set - Set a property on an object (with auto-promotion).
 * Returns the (possibly new) object pointer. The old pointer is invalid
 * after promotion. Callers MUST use the returned pointer.
 * Returns NULL on allocation failure.
 */
/* Forward declaration */
void *r8e_obj_set_with_desc(R8EContext *ctx, void *obj, uint32_t key,
                             R8EValue val, uint8_t desc);

void *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key, R8EValue val) {
    return r8e_obj_set_with_desc(ctx, obj, key, val, R8E_PROP_DEFAULT);
}

/*
 * r8e_obj_set_with_desc - Set a property with explicit descriptor flags.
 */
void *r8e_obj_set_with_desc(R8EContext *ctx, void *obj, uint32_t key,
                             R8EValue val, uint8_t desc) {
    R8EObjHeader *h = (R8EObjHeader *)obj;
    int rc;

    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: {
        rc = r8e_tier0_set((R8EObjTier0 *)obj, key, val, desc);
        if (rc == 0) return obj;
        if (rc < 0) return obj;  /* error: return unchanged */
        /* rc == 1: promote Tier 0 -> Tier 1 */
        R8EObjTier1 *t1 = r8e_promote_0_to_1(ctx, (R8EObjTier0 *)obj);
        if (!t1) return NULL;
        rc = r8e_tier1_set(t1, key, val, desc);
        (void)rc;
        return t1;
    }
    case 1: {
        rc = r8e_tier1_set((R8EObjTier1 *)obj, key, val, desc);
        if (rc == 0) return obj;
        if (rc < 0) return obj;
        /* promote Tier 1 -> Tier 2 */
        R8EObjTier2 *t2 = r8e_promote_1_to_2(ctx, (R8EObjTier1 *)obj);
        if (!t2) return NULL;
        rc = r8e_tier2_set(ctx, t2, key, val, desc);
        (void)rc;
        return t2;
    }
    case 2: {
        rc = r8e_tier2_set(ctx, (R8EObjTier2 *)obj, key, val, desc);
        if (rc == 0) return obj;
        if (rc < 0) return obj;
        /* promote Tier 2 -> Tier 3 */
        R8EObjTier3 *t3 = r8e_promote_2_to_3(ctx, (R8EObjTier2 *)obj);
        if (!t3) return NULL;
        rc = r8e_tier3_set(ctx, t3, key, val, desc);
        (void)rc;
        return t3;
    }
    case 3: {
        r8e_tier3_set(ctx, (R8EObjTier3 *)obj, key, val, desc);
        return obj;
    }
    default:
        return obj;
    }
}

/*
 * r8e_obj_delete - Delete an own property.
 * Returns 0 on success, -1 if the property is not configurable.
 */
int r8e_obj_delete(R8EContext *ctx, void *obj, uint32_t key) {
    R8EObjHeader *h = (R8EObjHeader *)obj;
    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: return r8e_tier0_delete((R8EObjTier0 *)obj, key);
    case 1: return r8e_tier1_delete((R8EObjTier1 *)obj, key);
    case 2: return r8e_tier2_delete((R8EObjTier2 *)obj, key);
    case 3: return r8e_tier3_delete(ctx, (R8EObjTier3 *)obj, key);
    default: return -1;
    }
}

/*
 * r8e_obj_count - Return the number of own properties.
 */
uint32_t r8e_obj_count(R8EContext *ctx, void *obj) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: return (uint32_t)r8e_tier0_count((R8EObjTier0 *)obj);
    case 1: return ((R8EObjTier1 *)obj)->count;
    case 2: return ((R8EObjTier2 *)obj)->count;
    case 3: return ((R8EObjTier3 *)obj)->count;
    default: return 0;
    }
}

/* =========================================================================
 * SECTION: Property Enumeration
 * ========================================================================= */

/*
 * r8e_obj_keys - Enumerate own enumerable property atom keys.
 * Writes atom keys into out_keys[], up to max_keys.
 * Returns the number of keys written. (Object.keys semantics)
 */
uint32_t r8e_obj_keys(R8EContext *ctx, void *obj,
                       uint32_t *out_keys, uint32_t max_keys) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    uint32_t n = 0;

    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: {
        R8EObjTier0 *t0 = (R8EObjTier0 *)obj;
        if (t0->key0 != R8E_ATOM_EMPTY &&
            (t0->desc0 & R8E_PROP_ENUMERABLE) && n < max_keys)
            out_keys[n++] = t0->key0;
        break;
    }
    case 1: {
        R8EObjTier1 *t1 = (R8EObjTier1 *)obj;
        for (int i = 0; i < t1->count && n < max_keys; i++) {
            if (t1->props[i].desc & R8E_PROP_ENUMERABLE)
                out_keys[n++] = t1->props[i].key;
        }
        break;
    }
    case 2: {
        R8EObjTier2 *t2 = (R8EObjTier2 *)obj;
        for (int i = 0; i < t2->count && n < max_keys; i++) {
            if (t2->props[i].desc & R8E_PROP_ENUMERABLE)
                out_keys[n++] = t2->props[i].key;
        }
        break;
    }
    case 3: {
        R8EObjTier3 *t3 = (R8EObjTier3 *)obj;
        for (uint16_t i = 0; i < t3->capacity && n < max_keys; i++) {
            if (t3->buckets[i].key != R8E_ATOM_EMPTY &&
                (t3->buckets[i].desc & R8E_PROP_ENUMERABLE))
                out_keys[n++] = t3->buckets[i].key;
        }
        break;
    }
    default:
        break;
    }

    return n;
}

/*
 * r8e_obj_own_keys - Enumerate ALL own property atom keys
 * (including non-enumerable). Object.getOwnPropertyNames / Reflect.ownKeys.
 */
uint32_t r8e_obj_own_keys(R8EContext *ctx, void *obj,
                           uint32_t *out_keys, uint32_t max_keys) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    uint32_t n = 0;

    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: {
        R8EObjTier0 *t0 = (R8EObjTier0 *)obj;
        if (t0->key0 != R8E_ATOM_EMPTY && n < max_keys)
            out_keys[n++] = t0->key0;
        break;
    }
    case 1: {
        R8EObjTier1 *t1 = (R8EObjTier1 *)obj;
        for (int i = 0; i < t1->count && n < max_keys; i++)
            out_keys[n++] = t1->props[i].key;
        break;
    }
    case 2: {
        R8EObjTier2 *t2 = (R8EObjTier2 *)obj;
        for (int i = 0; i < t2->count && n < max_keys; i++)
            out_keys[n++] = t2->props[i].key;
        break;
    }
    case 3: {
        R8EObjTier3 *t3 = (R8EObjTier3 *)obj;
        for (uint16_t i = 0; i < t3->capacity && n < max_keys; i++) {
            if (t3->buckets[i].key != R8E_ATOM_EMPTY)
                out_keys[n++] = t3->buckets[i].key;
        }
        break;
    }
    default:
        break;
    }

    return n;
}

/* =========================================================================
 * SECTION: Prototype Chain
 * ========================================================================= */

/*
 * r8e_obj_get_prototype - Get the prototype R8EValue for an object.
 * Returns R8E_NULL if proto_id is R8E_PROTO_NULL or out of range.
 */
R8EValue r8e_obj_get_prototype(R8EContext *ctx, void *obj) {
    R8EObjHeader *h = (R8EObjHeader *)obj;
    uint32_t pid = h->proto_id;

    if (pid == R8E_PROTO_NULL || pid >= ctx->proto_count)
        return R8E_NULL;

    return ctx->proto_table[pid];
}

/*
 * r8e_obj_set_prototype - Set the prototype of an object.
 */
void r8e_obj_set_prototype(R8EContext *ctx, void *obj, uint32_t proto_id) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    h->proto_id = proto_id;
}

/*
 * r8e_obj_get_with_proto - Get a property, walking the prototype chain.
 * First checks own properties, then walks proto chain until found or
 * chain ends. Returns R8E_UNDEFINED if not found anywhere.
 */
R8EValue r8e_obj_get_with_proto(R8EContext *ctx, void *obj, uint32_t key) {
    void *current = obj;

    for (int depth = 0; depth < R8E_PROTO_CHAIN_MAX && current != NULL;
         depth++) {
        if (r8e_obj_has(ctx, current, key))
            return r8e_obj_get(ctx, current, key);

        R8EObjHeader *h = (R8EObjHeader *)current;
        uint32_t pid = h->proto_id;

        if (pid == R8E_PROTO_NULL || pid >= ctx->proto_count)
            break;

        R8EValue proto_val = ctx->proto_table[pid];
        if (!R8E_IS_POINTER(proto_val))
            break;

        current = r8e_get_pointer(proto_val);
    }

    return R8E_UNDEFINED;
}

/*
 * r8e_obj_has_with_proto - Check property existence along prototype chain.
 */
bool r8e_obj_has_with_proto(R8EContext *ctx, void *obj, uint32_t key) {
    void *current = obj;

    for (int depth = 0; depth < R8E_PROTO_CHAIN_MAX && current != NULL;
         depth++) {
        if (r8e_obj_has(ctx, current, key))
            return true;

        R8EObjHeader *h = (R8EObjHeader *)current;
        uint32_t pid = h->proto_id;

        if (pid == R8E_PROTO_NULL || pid >= ctx->proto_count)
            break;

        R8EValue proto_val = ctx->proto_table[pid];
        if (!R8E_IS_POINTER(proto_val))
            break;

        current = r8e_get_pointer(proto_val);
    }

    return false;
}

/* =========================================================================
 * SECTION: Object Freeze / Seal / Prevent Extensions
 * ========================================================================= */

/*
 * r8e_obj_freeze - Object.freeze: make all properties non-writable and
 * non-configurable, make the object non-extensible.
 */
void r8e_obj_freeze(R8EContext *ctx, void *obj) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    h->flags |= R8E_OBJ_FROZEN;
    h->flags &= ~R8E_OBJ_EXTENSIBLE;

    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: {
        R8EObjTier0 *t0 = (R8EObjTier0 *)obj;
        if (t0->key0 != R8E_ATOM_EMPTY)
            t0->desc0 &= ~(R8E_PROP_WRITABLE | R8E_PROP_CONFIGURABLE);
        break;
    }
    case 1: {
        R8EObjTier1 *t1 = (R8EObjTier1 *)obj;
        for (int i = 0; i < t1->count; i++)
            t1->props[i].desc &= ~(R8E_PROP_WRITABLE | R8E_PROP_CONFIGURABLE);
        break;
    }
    case 2: {
        R8EObjTier2 *t2 = (R8EObjTier2 *)obj;
        for (int i = 0; i < t2->count; i++)
            t2->props[i].desc &= ~(R8E_PROP_WRITABLE | R8E_PROP_CONFIGURABLE);
        break;
    }
    case 3: {
        R8EObjTier3 *t3 = (R8EObjTier3 *)obj;
        for (uint16_t i = 0; i < t3->capacity; i++) {
            if (t3->buckets[i].key != R8E_ATOM_EMPTY)
                t3->buckets[i].desc &= ~(R8E_PROP_WRITABLE |
                                          R8E_PROP_CONFIGURABLE);
        }
        break;
    }
    default:
        break;
    }
}

/*
 * r8e_obj_seal - Object.seal: make all properties non-configurable,
 * leave writability unchanged, make object non-extensible.
 */
void r8e_obj_seal(R8EContext *ctx, void *obj) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    h->flags |= R8E_OBJ_SEALED;
    h->flags &= ~R8E_OBJ_EXTENSIBLE;

    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0: {
        R8EObjTier0 *t0 = (R8EObjTier0 *)obj;
        if (t0->key0 != R8E_ATOM_EMPTY)
            t0->desc0 &= ~R8E_PROP_CONFIGURABLE;
        break;
    }
    case 1: {
        R8EObjTier1 *t1 = (R8EObjTier1 *)obj;
        for (int i = 0; i < t1->count; i++)
            t1->props[i].desc &= ~R8E_PROP_CONFIGURABLE;
        break;
    }
    case 2: {
        R8EObjTier2 *t2 = (R8EObjTier2 *)obj;
        for (int i = 0; i < t2->count; i++)
            t2->props[i].desc &= ~R8E_PROP_CONFIGURABLE;
        break;
    }
    case 3: {
        R8EObjTier3 *t3 = (R8EObjTier3 *)obj;
        for (uint16_t i = 0; i < t3->capacity; i++) {
            if (t3->buckets[i].key != R8E_ATOM_EMPTY)
                t3->buckets[i].desc &= ~R8E_PROP_CONFIGURABLE;
        }
        break;
    }
    default:
        break;
    }
}

/*
 * r8e_obj_prevent_extensions - Prevent new properties from being added.
 */
void r8e_obj_prevent_extensions(R8EContext *ctx, void *obj) {
    (void)ctx;
    R8EObjHeader *h = (R8EObjHeader *)obj;
    h->flags &= ~R8E_OBJ_EXTENSIBLE;
}

bool r8e_obj_is_frozen(R8EContext *ctx, void *obj) {
    (void)ctx;
    return (((R8EObjHeader *)obj)->flags & R8E_OBJ_FROZEN) != 0;
}

bool r8e_obj_is_sealed(R8EContext *ctx, void *obj) {
    (void)ctx;
    return (((R8EObjHeader *)obj)->flags & R8E_OBJ_SEALED) != 0;
}

bool r8e_obj_is_extensible(R8EContext *ctx, void *obj) {
    (void)ctx;
    return (((R8EObjHeader *)obj)->flags & R8E_OBJ_EXTENSIBLE) != 0;
}

/* =========================================================================
 * SECTION: Define Property (Object.defineProperty semantics)
 * ========================================================================= */

/*
 * r8e_obj_define_property - Define or redefine a property with full
 * descriptor control. Returns the (possibly promoted) object pointer,
 * or NULL on failure.
 */
void *r8e_obj_define_property(R8EContext *ctx, void *obj, uint32_t key,
                               R8EValue val, uint8_t desc) {
    R8EObjHeader *h = (R8EObjHeader *)obj;

    if (r8e_obj_has(ctx, obj, key)) {
        uint8_t existing_desc = r8e_obj_get_prop_desc(ctx, obj, key);

        if (!(existing_desc & R8E_PROP_CONFIGURABLE)) {
            if ((desc & ~R8E_PROP_WRITABLE) !=
                (existing_desc & ~R8E_PROP_WRITABLE))
                return obj;  /* TypeError in strict mode */
            if (!(existing_desc & R8E_PROP_WRITABLE))
                return obj;
        }

        r8e_obj_delete(ctx, obj, key);
    } else {
        if (!(h->flags & R8E_OBJ_EXTENSIBLE))
            return obj;
    }

    return r8e_obj_set_with_desc(ctx, obj, key, val, desc);
}

/* =========================================================================
 * SECTION: Object Destruction
 * ========================================================================= */

/*
 * r8e_obj_destroy - Free all memory associated with an object.
 * Caller must ensure refcount is zero and no external references remain.
 */
void r8e_obj_destroy(R8EContext *ctx, void *obj) {
    R8EObjHeader *h = (R8EObjHeader *)obj;
    switch (R8E_OBJ_GET_TIER(h->flags)) {
    case 0:
    case 1:
        r8e_free(ctx, obj);
        break;
    case 2: {
        R8EObjTier2 *t2 = (R8EObjTier2 *)obj;
        r8e_free(ctx, t2->props);
        r8e_free(ctx, t2);
        break;
    }
    case 3: {
        R8EObjTier3 *t3 = (R8EObjTier3 *)obj;
        r8e_free(ctx, t3->buckets);
        r8e_free(ctx, t3);
        break;
    }
    default:
        r8e_free(ctx, obj);
        break;
    }
}

/* =========================================================================
 * SECTION: Utility Functions
 * ========================================================================= */

uint32_t r8e_obj_get_flags(void *obj) {
    return ((R8EObjHeader *)obj)->flags;
}

uint8_t r8e_obj_get_tier(void *obj) {
    return R8E_OBJ_GET_TIER(((R8EObjHeader *)obj)->flags);
}

uint32_t r8e_obj_get_proto_id(void *obj) {
    return ((R8EObjHeader *)obj)->proto_id;
}

/*
 * r8e_obj_assign - Copy all enumerable own properties from src to dst.
 * Implements Object.assign() semantics.
 * Returns the (possibly promoted) dst pointer.
 */
void *r8e_obj_assign(R8EContext *ctx, void *dst, void *src) {
    uint32_t keys[256];
    uint32_t n = r8e_obj_keys(ctx, src, keys, 256);

    for (uint32_t i = 0; i < n; i++) {
        R8EValue val = r8e_obj_get(ctx, src, keys[i]);
        dst = r8e_obj_set(ctx, dst, keys[i], val);
        if (!dst) return NULL;
    }

    return dst;
}
