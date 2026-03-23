/*
 * r8e_weakref.c - WeakRef, FinalizationRegistry, WeakMap, WeakSet, Map, Set
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 13.1 (r8e_weakref.c).
 *
 * Architecture:
 *   - WeakRef: weak pointer to object, cleared when target is GC'd
 *   - FinalizationRegistry: cleanup callbacks when registered targets collected
 *   - WeakMap: hash table keyed by object identity (weak keys, strong values)
 *   - WeakSet: hash set of weak object references
 *   - Map: ordered hash table preserving insertion order (strong keys+values)
 *   - Set: ordered hash set preserving insertion order
 *   - Weak reference table: global registry in R8EContext for GC integration
 *   - SameValueZero comparison for Map/Set (NaN === NaN, +0 === -0)
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
#include <stdio.h>
#include <math.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_TAG_INT32       0xFFF8U
#define R8E_TAG_POINTER     0xFFF9U
#define R8E_TAG_SPECIAL     0xFFFAU
#define R8E_TAG_SYMBOL      0xFFFBU
#define R8E_TAG_ATOM        0xFFFCU
#define R8E_TAG_INLINE_STR  0xFFFDU

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 48) == R8E_TAG_INT32)
#define R8E_IS_POINTER(v)     (((v) >> 48) == R8E_TAG_POINTER)
#define R8E_IS_UNDEFINED(v)   ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)        ((v) == R8E_NULL)
#define R8E_IS_BOOLEAN(v)     ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)      (((v) >> 48) == 0xFFFBU)
#define R8E_IS_ATOM(v)        (((v) >> 48) == 0xFFFCU)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == R8E_TAG_INLINE_STR)
#define R8E_IS_NUMBER(v)      (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))
#define R8E_IS_NULLISH(v)     (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))

static inline double r8e_get_double(R8EValue v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}
static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline R8EValue r8e_from_double(double d) {
    R8EValue v; memcpy(&v, &d, 8);
    if (v >= 0xFFF8000000000000ULL) v = 0x7FF8000000000000ULL;
    return v;
}
static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}
static inline R8EValue r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline R8EValue r8e_from_boolean(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}

/* =========================================================================
 * GC and object infrastructure
 * ========================================================================= */

#define R8E_GC_KIND_OBJECT     0x02
#define R8E_GC_KIND_ARRAY      0x03
#define R8E_GC_KIND_FUNC       0x04
#define R8E_GC_KIND_STRING     0x01
#define R8E_GC_KIND_WEAKREF    0x08
#define R8E_GC_KIND_FINREG     0x09
#define R8E_GC_KIND_WEAKMAP    0x0A
#define R8E_GC_KIND_WEAKSET    0x0B
#define R8E_GC_KIND_MAP        0x0C
#define R8E_GC_KIND_SET        0x0D

#define R8E_GC_FROZEN_BIT      0x00000010U

#define R8E_PROTO_NONE         0
#define R8E_PROTO_OBJECT       1
#define R8E_PROTO_ARRAY        2
#define R8E_PROTO_FUNCTION     3
#define R8E_PROTO_MAP         16
#define R8E_PROTO_SET         17
#define R8E_PROTO_WEAKMAP     18
#define R8E_PROTO_WEAKSET     19
#define R8E_PROTO_WEAKREF     38
#define R8E_PROTO_FINALIZATION 39
#define R8E_PROTO_COUNT       40

/* Minimal object header */
typedef struct R8EObjHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjHeader;

/* Tier 0 object */
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} R8EObjTier0;

/* Array structure */
typedef struct R8EArray {
    uint32_t   flags;
    uint32_t   proto_id;
    uint32_t   length;
    uint32_t   capacity;
    R8EValue  *elements;
    void      *named;
} R8EArray;

/* Forward declaration so R8ENativeFunc sees the typedef name */
/* Native function callback type */
typedef struct R8EContext R8EContext;

typedef R8EValue (*R8ENativeFunc)(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv);

/* Wrapped native function object */
typedef struct {
    uint32_t      flags;
    uint32_t      proto_id;
    R8ENativeFunc func;
    uint32_t      name_atom;
    int16_t       arity;
    uint16_t      pad;
} R8ENativeFuncObj;

/* Context structure stub (forward-declared above for R8ENativeFunc) */
struct R8EContext {
    void     *arena;
    void     *atom_table;
    void     *global_object;
    char      error_buf[256];
    int       has_error;
    void    **prototypes;
    uint16_t  proto_count;
    uint32_t  next_symbol_id;
    /* Weak reference registry for GC integration */
    struct R8EWeakTable *weak_table;
};

/* Heap string header */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
} R8EString;

#define R8E_STR_FLAG_ASCII    0x01

/* =========================================================================
 * Atom IDs
 * ========================================================================= */

#define R8E_ATOM_length             1
#define R8E_ATOM_prototype          2
#define R8E_ATOM_constructor        3
#define R8E_ATOM_toString           4
#define R8E_ATOM_valueOf            5
#define R8E_ATOM_name               8
#define R8E_ATOM_value             11
#define R8E_ATOM_get               15
#define R8E_ATOM_set               16
#define R8E_ATOM_Map               61
#define R8E_ATOM_Set               62
#define R8E_ATOM_WeakMap           63
#define R8E_ATOM_WeakSet           64
#define R8E_ATOM_WeakRef           65
#define R8E_ATOM_FinalizationRegistry 66
#define R8E_ATOM_keys              93
#define R8E_ATOM_values            94
#define R8E_ATOM_entries           95
#define R8E_ATOM_forEach          135
#define R8E_ATOM_size             292
#define R8E_ATOM_has              293
#define R8E_ATOM_add              294
#define R8E_ATOM_clear            295
#define R8E_ATOM_delete_method    296
#define R8E_ATOM_iterator         236
#define R8E_ATOM_done             288
#define R8E_ATOM_next             289
#define R8E_ATOM_deref            311
#define R8E_ATOM_register         312
#define R8E_ATOM_unregister       313
#define R8E_ATOM_done             288

/* =========================================================================
 * External declarations
 * ========================================================================= */

extern R8EObjTier0 *r8e_obj_new(R8EContext *ctx);
extern R8EObjTier0 *r8e_obj_new_with_proto(R8EContext *ctx, uint32_t proto_id);
extern R8EValue     r8e_obj_get(R8EContext *ctx, void *obj, uint32_t atom);
extern void         r8e_obj_set(R8EContext *ctx, void *obj, uint32_t atom,
                                R8EValue val);
extern bool         r8e_obj_has(R8EContext *ctx, void *obj, uint32_t atom);
extern bool         r8e_obj_delete(R8EContext *ctx, void *obj, uint32_t atom);

/* Call a function */
extern R8EValue r8e_call_function(R8EContext *ctx, R8EValue func,
                                   R8EValue this_val, int argc,
                                   const R8EValue *argv);

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static R8EValue throw_type_error(R8EContext *ctx, const char *msg) {
    if (ctx) {
        snprintf(ctx->error_buf, sizeof(ctx->error_buf), "TypeError: %s", msg);
        ctx->has_error = 1;
    }
    return R8E_UNDEFINED;
}

static inline R8EValue arg_or_undef(int argc, const R8EValue *argv, int idx) {
    return (idx < argc) ? argv[idx] : R8E_UNDEFINED;
}

static inline bool is_object(R8EValue v) {
    return R8E_IS_POINTER(v) && !R8E_IS_NULL(v);
}

static bool is_callable(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(v);
    if (!h) return false;
    return (h->flags & 0x0F) == R8E_GC_KIND_FUNC;
}

static R8EValue make_native_func(R8EContext *ctx, R8ENativeFunc func,
                                  uint32_t name_atom, int arity) {
    (void)ctx;
    R8ENativeFuncObj *f = (R8ENativeFuncObj *)malloc(sizeof(R8ENativeFuncObj));
    if (!f) return R8E_UNDEFINED;
    f->flags = R8E_GC_KIND_FUNC;
    f->proto_id = 3; /* PROTO_FUNCTION */
    f->func = func;
    f->name_atom = name_atom;
    f->arity = (int16_t)arity;
    f->pad = 0;
    return r8e_from_pointer(f);
}

static void install_method(R8EContext *ctx, void *obj, uint32_t name_atom,
                           R8ENativeFunc func, int arity) {
    R8EValue fval = make_native_func(ctx, func, name_atom, arity);
    if (!R8E_IS_UNDEFINED(fval)) {
        r8e_obj_set(ctx, obj, name_atom, fval);
    }
}

static void install_value(R8EContext *ctx, void *obj, uint32_t name_atom,
                          R8EValue val) {
    r8e_obj_set(ctx, obj, name_atom, val);
}


/* *************************************************************************
 * SECTION A: SameValueZero Comparison
 *
 * Used by Map and Set for key equality.
 * - NaN === NaN  (unlike ==, ===)
 * - +0 === -0    (unlike Object.is)
 * ************************************************************************* */

static bool same_value_zero(R8EValue a, R8EValue b) {
    /* Fast path: bitwise equality */
    if (a == b) return true;

    /* NaN === NaN: both are doubles and both are NaN */
    if (R8E_IS_DOUBLE(a) && R8E_IS_DOUBLE(b)) {
        double da = r8e_get_double(a);
        double db = r8e_get_double(b);
        if (isnan(da) && isnan(db)) return true;
        /* +0 === -0 */
        if (da == 0.0 && db == 0.0) return true;
    }

    /* Int32 zero and double zero */
    if (R8E_IS_INT32(a) && R8E_IS_DOUBLE(b)) {
        int32_t ia = r8e_get_int32(a);
        double db = r8e_get_double(b);
        if (ia == 0 && db == 0.0) return true;
    }
    if (R8E_IS_DOUBLE(a) && R8E_IS_INT32(b)) {
        double da = r8e_get_double(a);
        int32_t ib = r8e_get_int32(b);
        if (da == 0.0 && ib == 0) return true;
    }

    /* Int32 and double numeric equality */
    if (R8E_IS_INT32(a) && R8E_IS_DOUBLE(b)) {
        return (double)r8e_get_int32(a) == r8e_get_double(b);
    }
    if (R8E_IS_DOUBLE(a) && R8E_IS_INT32(b)) {
        return r8e_get_double(a) == (double)r8e_get_int32(b);
    }

    return false;
}

/**
 * Hash a value for use in Map/Set/WeakMap hash tables.
 * Must be consistent with same_value_zero: equal values have equal hashes.
 */
static uint32_t value_hash(R8EValue v) {
    if (R8E_IS_INT32(v)) {
        int32_t i = r8e_get_int32(v);
        if (i == 0) return 0; /* +0 and -0 hash the same */
        uint32_t h = (uint32_t)i;
        h = ((h >> 16) ^ h) * 0x45d9f3b;
        h = ((h >> 16) ^ h) * 0x45d9f3b;
        h = (h >> 16) ^ h;
        return h;
    }
    if (R8E_IS_DOUBLE(v)) {
        double d = r8e_get_double(v);
        if (isnan(d)) return 0x7FC00000U; /* canonical NaN hash */
        if (d == 0.0) return 0; /* +0 and -0 hash the same */
        /* If the double is an integer, hash it the same as int32 */
        if (d == (double)(int32_t)d && d >= (double)INT32_MIN &&
            d <= (double)INT32_MAX) {
            uint32_t h = (uint32_t)(int32_t)d;
            h = ((h >> 16) ^ h) * 0x45d9f3b;
            h = ((h >> 16) ^ h) * 0x45d9f3b;
            h = (h >> 16) ^ h;
            return h;
        }
        /* Hash the raw bits */
        uint64_t bits = v;
        return (uint32_t)(bits ^ (bits >> 32));
    }
    if (R8E_IS_POINTER(v)) {
        /* Hash the pointer value */
        uintptr_t p = (uintptr_t)r8e_get_pointer(v);
        uint32_t h = (uint32_t)(p ^ (p >> 16));
        h = ((h >> 16) ^ h) * 0x45d9f3b;
        h = ((h >> 16) ^ h) * 0x45d9f3b;
        return (h >> 16) ^ h;
    }
    /* For other types (undefined, null, bool, symbol, atom, inline str),
     * use the raw bits. */
    return (uint32_t)(v ^ (v >> 32));
}


/* *************************************************************************
 * SECTION B: Weak Reference Table (GC Integration)
 *
 * Global registry tracking all weak references. When GC frees an object,
 * it calls r8e_weak_table_notify_freed() to clear weak references and
 * enqueue finalization callbacks.
 * ************************************************************************* */

/**
 * Entry in the weak reference table.
 * Tracks a weak reference to a target object.
 */
typedef struct R8EWeakEntry {
    void  *target_ptr;          /* raw pointer to the target (NOT ref-counted) */
    uint8_t kind;               /* 0=WeakRef, 1=WeakMap, 2=WeakSet, 3=FinReg */
    uint8_t pad[3];
    void  *owner;               /* pointer to the WeakRef/WeakMap/etc object */
    struct R8EWeakEntry *next;  /* linked list for same target */
} R8EWeakEntry;

/**
 * Weak reference table: hash table mapping target pointers to weak entries.
 */
typedef struct R8EWeakTable {
    R8EWeakEntry **buckets;
    uint32_t       capacity;
    uint32_t       count;
} R8EWeakTable;

#define R8E_WEAK_TABLE_INIT_CAP 64

#define R8E_WEAK_KIND_WEAKREF   0
#define R8E_WEAK_KIND_WEAKMAP   1
#define R8E_WEAK_KIND_WEAKSET   2
#define R8E_WEAK_KIND_FINREG    3

/**
 * Create a new weak table.
 */
static R8EWeakTable *r8e_weak_table_new(void) {
    R8EWeakTable *wt = (R8EWeakTable *)calloc(1, sizeof(R8EWeakTable));
    if (!wt) return NULL;
    wt->capacity = R8E_WEAK_TABLE_INIT_CAP;
    wt->count = 0;
    wt->buckets = (R8EWeakEntry **)calloc(wt->capacity,
                                           sizeof(R8EWeakEntry *));
    if (!wt->buckets) { free(wt); return NULL; }
    return wt;
}

/**
 * Hash a pointer for the weak table.
 */
static uint32_t ptr_hash(const void *ptr, uint32_t capacity) {
    uintptr_t p = (uintptr_t)ptr;
    uint32_t h = (uint32_t)(p ^ (p >> 16));
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return h & (capacity - 1);
}

/**
 * Register a weak reference in the table.
 */
static void r8e_weak_table_register(R8EWeakTable *wt, void *target_ptr,
                                    void *owner, uint8_t kind) {
    if (!wt || !target_ptr) return;

    /* Grow if load factor > 0.75 */
    if (wt->count * 4 > wt->capacity * 3) {
        uint32_t new_cap = wt->capacity * 2;
        R8EWeakEntry **new_buckets = (R8EWeakEntry **)calloc(
            new_cap, sizeof(R8EWeakEntry *));
        if (new_buckets) {
            for (uint32_t i = 0; i < wt->capacity; i++) {
                R8EWeakEntry *e = wt->buckets[i];
                while (e) {
                    R8EWeakEntry *next = e->next;
                    uint32_t idx = ptr_hash(e->target_ptr, new_cap);
                    e->next = new_buckets[idx];
                    new_buckets[idx] = e;
                    e = next;
                }
            }
            free(wt->buckets);
            wt->buckets = new_buckets;
            wt->capacity = new_cap;
        }
    }

    R8EWeakEntry *entry = (R8EWeakEntry *)calloc(1, sizeof(R8EWeakEntry));
    if (!entry) return;
    entry->target_ptr = target_ptr;
    entry->owner = owner;
    entry->kind = kind;

    uint32_t idx = ptr_hash(target_ptr, wt->capacity);
    entry->next = wt->buckets[idx];
    wt->buckets[idx] = entry;
    wt->count++;
}

/**
 * Unregister a specific weak entry.
 */
static void r8e_weak_table_unregister(R8EWeakTable *wt, void *target_ptr,
                                      void *owner) {
    if (!wt || !target_ptr) return;
    uint32_t idx = ptr_hash(target_ptr, wt->capacity);
    R8EWeakEntry **pp = &wt->buckets[idx];
    while (*pp) {
        R8EWeakEntry *e = *pp;
        if (e->target_ptr == target_ptr && e->owner == owner) {
            *pp = e->next;
            free(e);
            wt->count--;
            return;
        }
        pp = &e->next;
    }
}

/**
 * Free the entire weak table.
 */
static void r8e_weak_table_free(R8EWeakTable *wt) {
    if (!wt) return;
    for (uint32_t i = 0; i < wt->capacity; i++) {
        R8EWeakEntry *e = wt->buckets[i];
        while (e) {
            R8EWeakEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    free(wt->buckets);
    free(wt);
}


/* Forward declarations for notification handlers */
static void weakref_on_target_freed(void *owner);
static void weakmap_on_target_freed(void *owner, void *target_ptr);
static void weakset_on_target_freed(void *owner, void *target_ptr);
static void finreg_on_target_freed(R8EContext *ctx, void *owner,
                                   void *target_ptr);

/**
 * Called by GC when an object is freed.
 * Scans the weak table for entries pointing to target_ptr and clears them.
 */
void r8e_weak_table_notify_freed(R8EContext *ctx, void *target_ptr) {
    R8EWeakTable *wt = ctx ? ctx->weak_table : NULL;
    if (!wt || !target_ptr) return;

    uint32_t idx = ptr_hash(target_ptr, wt->capacity);
    R8EWeakEntry **pp = &wt->buckets[idx];

    while (*pp) {
        R8EWeakEntry *e = *pp;
        if (e->target_ptr == target_ptr) {
            /* Notify the owner based on kind */
            switch (e->kind) {
            case R8E_WEAK_KIND_WEAKREF:
                weakref_on_target_freed(e->owner);
                break;
            case R8E_WEAK_KIND_WEAKMAP:
                weakmap_on_target_freed(e->owner, target_ptr);
                break;
            case R8E_WEAK_KIND_WEAKSET:
                weakset_on_target_freed(e->owner, target_ptr);
                break;
            case R8E_WEAK_KIND_FINREG:
                finreg_on_target_freed(ctx, e->owner, target_ptr);
                break;
            }

            /* Remove entry from table */
            *pp = e->next;
            free(e);
            wt->count--;
        } else {
            pp = &e->next;
        }
    }
}

/**
 * Ensure the weak table exists on the context.
 */
static R8EWeakTable *ensure_weak_table(R8EContext *ctx) {
    if (!ctx->weak_table) {
        ctx->weak_table = r8e_weak_table_new();
    }
    return ctx->weak_table;
}


/* *************************************************************************
 * SECTION C: WeakRef
 * ************************************************************************* */

/**
 * R8EWeakRef: weak reference to a target object.
 * When the target is collected by GC, the target_ptr is set to NULL.
 */
typedef struct R8EWeakRef {
    uint32_t flags;        /* GC kind = R8E_GC_KIND_WEAKREF */
    uint32_t proto_id;
    void    *target_ptr;   /* raw pointer to target (NOT ref-counted) */
} R8EWeakRef;

/**
 * Called by weak table when the target is freed.
 */
static void weakref_on_target_freed(void *owner) {
    R8EWeakRef *wr = (R8EWeakRef *)owner;
    if (wr) wr->target_ptr = NULL;
}

/**
 * Create a new WeakRef.
 */
R8EValue r8e_weakref_new(R8EContext *ctx, R8EValue target) {
    if (!is_object(target)) {
        return throw_type_error(ctx,
            "WeakRef: target must be an object or symbol");
    }

    R8EWeakRef *wr = (R8EWeakRef *)calloc(1, sizeof(R8EWeakRef));
    if (!wr) return R8E_UNDEFINED;

    wr->flags = R8E_GC_KIND_WEAKREF;
    wr->proto_id = R8E_PROTO_WEAKREF;
    wr->target_ptr = r8e_get_pointer(target);

    /* Register in the weak table */
    R8EWeakTable *wt = ensure_weak_table(ctx);
    if (wt) {
        r8e_weak_table_register(wt, wr->target_ptr, wr,
                                R8E_WEAK_KIND_WEAKREF);
    }

    return r8e_from_pointer(wr);
}

/**
 * WeakRef.prototype.deref() -> target or undefined
 */
R8EValue r8e_weakref_deref(R8EContext *ctx, R8EValue weakref_val) {
    (void)ctx;
    if (!R8E_IS_POINTER(weakref_val)) return R8E_UNDEFINED;
    R8EWeakRef *wr = (R8EWeakRef *)r8e_get_pointer(weakref_val);
    if (!wr || (wr->flags & 0x0F) != R8E_GC_KIND_WEAKREF)
        return R8E_UNDEFINED;
    if (!wr->target_ptr) return R8E_UNDEFINED;
    return r8e_from_pointer(wr->target_ptr);
}

/* Built-in: new WeakRef(target) */
static R8EValue builtin_weakref_constructor(R8EContext *ctx, R8EValue this_val,
                                            int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    return r8e_weakref_new(ctx, target);
}

/* Built-in: WeakRef.prototype.deref() */
static R8EValue builtin_weakref_deref(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return r8e_weakref_deref(ctx, this_val);
}


/* *************************************************************************
 * SECTION D: FinalizationRegistry
 * ************************************************************************* */

/**
 * Entry in a finalization registry: associates a target with held value
 * and optional unregister token.
 */
typedef struct R8EFinRegEntry {
    void    *target_ptr;          /* weak ref to target */
    R8EValue held_value;          /* strong ref: value passed to callback */
    R8EValue unregister_token;    /* optional: for unregistering */
    bool     active;              /* false if unregistered or already fired */
    struct R8EFinRegEntry *next;
} R8EFinRegEntry;

/**
 * R8EFinalizationRegistry: cleanup callbacks when targets are collected.
 */
typedef struct R8EFinalizationRegistry {
    uint32_t         flags;      /* GC kind = R8E_GC_KIND_FINREG */
    uint32_t         proto_id;
    R8EValue         cleanup_fn; /* the callback function */
    R8EFinRegEntry  *entries;    /* linked list of registered entries */
    uint32_t         entry_count;
} R8EFinalizationRegistry;

/**
 * Called by weak table when a registered target is freed.
 * Enqueues the cleanup callback (as a microtask in a real implementation).
 */
static void finreg_on_target_freed(R8EContext *ctx, void *owner,
                                   void *target_ptr) {
    R8EFinalizationRegistry *fr = (R8EFinalizationRegistry *)owner;
    if (!fr) return;

    R8EFinRegEntry *e = fr->entries;
    while (e) {
        if (e->target_ptr == target_ptr && e->active) {
            e->active = false;
            e->target_ptr = NULL;

            /*
             * Call the cleanup callback with the held_value.
             * In a full implementation, this would be enqueued as a microtask.
             * For now, call it synchronously.
             */
            if (ctx && is_callable(fr->cleanup_fn)) {
                R8EValue args[1] = { e->held_value };
                r8e_call_function(ctx, fr->cleanup_fn, R8E_UNDEFINED, 1, args);
            }
        }
        e = e->next;
    }
}

/**
 * Create a new FinalizationRegistry.
 */
R8EValue r8e_finalization_registry_new(R8EContext *ctx, R8EValue cleanup_fn) {
    if (!is_callable(cleanup_fn)) {
        return throw_type_error(ctx,
            "FinalizationRegistry: cleanup must be callable");
    }

    R8EFinalizationRegistry *fr = (R8EFinalizationRegistry *)calloc(
        1, sizeof(R8EFinalizationRegistry));
    if (!fr) return R8E_UNDEFINED;

    fr->flags = R8E_GC_KIND_FINREG;
    fr->proto_id = R8E_PROTO_FINALIZATION;
    fr->cleanup_fn = cleanup_fn;
    fr->entries = NULL;
    fr->entry_count = 0;

    return r8e_from_pointer(fr);
}

/**
 * FinalizationRegistry.prototype.register(target, heldValue [, unregisterToken])
 */
void r8e_finalization_registry_register(R8EContext *ctx,
                                        R8EValue registry_val,
                                        R8EValue target,
                                        R8EValue held_value,
                                        R8EValue unregister_token) {
    if (!R8E_IS_POINTER(registry_val)) {
        throw_type_error(ctx, "FinalizationRegistry.register: not a registry");
        return;
    }
    R8EFinalizationRegistry *fr =
        (R8EFinalizationRegistry *)r8e_get_pointer(registry_val);
    if (!fr || (fr->flags & 0x0F) != R8E_GC_KIND_FINREG) {
        throw_type_error(ctx, "FinalizationRegistry.register: not a registry");
        return;
    }
    if (!is_object(target)) {
        throw_type_error(ctx,
            "FinalizationRegistry.register: target must be an object");
        return;
    }
    /* target and heldValue must not be the same object */
    if (target == held_value) {
        throw_type_error(ctx,
            "FinalizationRegistry.register: target and heldValue must differ");
        return;
    }

    R8EFinRegEntry *entry = (R8EFinRegEntry *)calloc(1,
                                                      sizeof(R8EFinRegEntry));
    if (!entry) return;

    entry->target_ptr = r8e_get_pointer(target);
    entry->held_value = held_value;
    entry->unregister_token = unregister_token;
    entry->active = true;
    entry->next = fr->entries;
    fr->entries = entry;
    fr->entry_count++;

    /* Register in weak table */
    R8EWeakTable *wt = ensure_weak_table(ctx);
    if (wt) {
        r8e_weak_table_register(wt, entry->target_ptr, fr,
                                R8E_WEAK_KIND_FINREG);
    }
}

/**
 * FinalizationRegistry.prototype.unregister(token)
 * Removes all entries with matching unregister token.
 * Returns true if any entries were removed.
 */
bool r8e_finalization_registry_unregister(R8EContext *ctx,
                                          R8EValue registry_val,
                                          R8EValue token) {
    if (!R8E_IS_POINTER(registry_val)) return false;
    R8EFinalizationRegistry *fr =
        (R8EFinalizationRegistry *)r8e_get_pointer(registry_val);
    if (!fr || (fr->flags & 0x0F) != R8E_GC_KIND_FINREG) return false;

    bool removed_any = false;
    R8EFinRegEntry *e = fr->entries;
    while (e) {
        if (e->active && same_value_zero(e->unregister_token, token)) {
            /* Unregister from weak table */
            R8EWeakTable *wt = ctx->weak_table;
            if (wt && e->target_ptr) {
                r8e_weak_table_unregister(wt, e->target_ptr, fr);
            }
            e->active = false;
            removed_any = true;
        }
        e = e->next;
    }

    return removed_any;
}

/**
 * Run pending cleanups. Called by GC after collection.
 */
void r8e_finalization_registry_cleanup(R8EContext *ctx, R8EValue registry_val) {
    if (!R8E_IS_POINTER(registry_val)) return;
    R8EFinalizationRegistry *fr =
        (R8EFinalizationRegistry *)r8e_get_pointer(registry_val);
    if (!fr || (fr->flags & 0x0F) != R8E_GC_KIND_FINREG) return;

    R8EFinRegEntry *e = fr->entries;
    while (e) {
        if (!e->active && e->held_value != R8E_UNDEFINED) {
            /* Already fired. Nothing to do here; cleanup happened on notify. */
        }
        e = e->next;
    }
}

/* Built-in: new FinalizationRegistry(cleanup) */
static R8EValue builtin_finreg_constructor(R8EContext *ctx, R8EValue this_val,
                                           int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue cleanup = arg_or_undef(argc, argv, 0);
    return r8e_finalization_registry_new(ctx, cleanup);
}

/* Built-in: FinalizationRegistry.prototype.register(target, held, token) */
static R8EValue builtin_finreg_register(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue held = arg_or_undef(argc, argv, 1);
    R8EValue token = arg_or_undef(argc, argv, 2);
    r8e_finalization_registry_register(ctx, this_val, target, held, token);
    return R8E_UNDEFINED;
}

/* Built-in: FinalizationRegistry.prototype.unregister(token) */
static R8EValue builtin_finreg_unregister(R8EContext *ctx, R8EValue this_val,
                                          int argc, const R8EValue *argv) {
    R8EValue token = arg_or_undef(argc, argv, 0);
    return r8e_from_boolean(
        r8e_finalization_registry_unregister(ctx, this_val, token));
}


/* *************************************************************************
 * SECTION E: WeakMap
 *
 * Hash table keyed by object identity (pointer). Keys are NOT ref-counted.
 * When a key is collected by GC, its entry is automatically removed.
 * ************************************************************************* */

/**
 * Entry in a WeakMap bucket.
 */
typedef struct R8EWeakMapEntry {
    void    *key_ptr;     /* raw pointer to key object (weak) */
    R8EValue value;       /* strongly held value */
    struct R8EWeakMapEntry *next;
} R8EWeakMapEntry;

/**
 * R8EWeakMap: hash table of weak-key -> strong-value.
 */
typedef struct R8EWeakMap {
    uint32_t          flags;       /* GC kind = R8E_GC_KIND_WEAKMAP */
    uint32_t          proto_id;
    R8EWeakMapEntry **buckets;
    uint32_t          capacity;
    uint32_t          count;
} R8EWeakMap;

#define R8E_WEAKMAP_INIT_CAP 16

/**
 * Called by weak table when a key is freed.
 */
static void weakmap_on_target_freed(void *owner, void *target_ptr) {
    R8EWeakMap *wm = (R8EWeakMap *)owner;
    if (!wm || !wm->buckets) return;

    uint32_t idx = ptr_hash(target_ptr, wm->capacity);
    R8EWeakMapEntry **pp = &wm->buckets[idx];
    while (*pp) {
        R8EWeakMapEntry *e = *pp;
        if (e->key_ptr == target_ptr) {
            *pp = e->next;
            free(e);
            wm->count--;
            return;
        }
        pp = &e->next;
    }
}

R8EValue r8e_weakmap_new(R8EContext *ctx) {
    (void)ctx;
    R8EWeakMap *wm = (R8EWeakMap *)calloc(1, sizeof(R8EWeakMap));
    if (!wm) return R8E_UNDEFINED;

    wm->flags = R8E_GC_KIND_WEAKMAP;
    wm->proto_id = R8E_PROTO_WEAKMAP;
    wm->capacity = R8E_WEAKMAP_INIT_CAP;
    wm->count = 0;
    wm->buckets = (R8EWeakMapEntry **)calloc(wm->capacity,
                                              sizeof(R8EWeakMapEntry *));
    if (!wm->buckets) { free(wm); return R8E_UNDEFINED; }

    return r8e_from_pointer(wm);
}

static R8EWeakMap *get_weakmap(R8EContext *ctx, R8EValue this_val,
                               const char *method) {
    if (!R8E_IS_POINTER(this_val)) {
        throw_type_error(ctx, method);
        return NULL;
    }
    R8EWeakMap *wm = (R8EWeakMap *)r8e_get_pointer(this_val);
    if (!wm || (wm->flags & 0x0F) != R8E_GC_KIND_WEAKMAP) {
        throw_type_error(ctx, method);
        return NULL;
    }
    return wm;
}

static void weakmap_grow(R8EWeakMap *wm) {
    uint32_t new_cap = wm->capacity * 2;
    R8EWeakMapEntry **new_buckets = (R8EWeakMapEntry **)calloc(
        new_cap, sizeof(R8EWeakMapEntry *));
    if (!new_buckets) return;

    for (uint32_t i = 0; i < wm->capacity; i++) {
        R8EWeakMapEntry *e = wm->buckets[i];
        while (e) {
            R8EWeakMapEntry *next = e->next;
            uint32_t idx = ptr_hash(e->key_ptr, new_cap);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }
    free(wm->buckets);
    wm->buckets = new_buckets;
    wm->capacity = new_cap;
}

R8EValue r8e_weakmap_get(R8EContext *ctx, R8EValue map_val, R8EValue key) {
    R8EWeakMap *wm = get_weakmap(ctx, map_val, "WeakMap.get: not a WeakMap");
    if (!wm) return R8E_UNDEFINED;
    if (!is_object(key)) return R8E_UNDEFINED;

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, wm->capacity);
    R8EWeakMapEntry *e = wm->buckets[idx];
    while (e) {
        if (e->key_ptr == key_ptr) return e->value;
        e = e->next;
    }
    return R8E_UNDEFINED;
}

R8EValue r8e_weakmap_set(R8EContext *ctx, R8EValue map_val, R8EValue key,
                         R8EValue value) {
    R8EWeakMap *wm = get_weakmap(ctx, map_val, "WeakMap.set: not a WeakMap");
    if (!wm) return R8E_UNDEFINED;
    if (!is_object(key)) {
        return throw_type_error(ctx,
            "WeakMap.set: key must be an object or symbol");
    }

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, wm->capacity);

    /* Check for existing entry */
    R8EWeakMapEntry *e = wm->buckets[idx];
    while (e) {
        if (e->key_ptr == key_ptr) {
            e->value = value;
            return map_val;
        }
        e = e->next;
    }

    /* Grow if needed */
    if (wm->count * 4 > wm->capacity * 3) {
        weakmap_grow(wm);
        idx = ptr_hash(key_ptr, wm->capacity);
    }

    /* Insert new entry */
    R8EWeakMapEntry *entry = (R8EWeakMapEntry *)calloc(1,
                                                        sizeof(R8EWeakMapEntry));
    if (!entry) return R8E_UNDEFINED;
    entry->key_ptr = key_ptr;
    entry->value = value;
    entry->next = wm->buckets[idx];
    wm->buckets[idx] = entry;
    wm->count++;

    /* Register in weak table */
    R8EWeakTable *wt = ensure_weak_table(ctx);
    if (wt) {
        r8e_weak_table_register(wt, key_ptr, wm, R8E_WEAK_KIND_WEAKMAP);
    }

    return map_val;
}

bool r8e_weakmap_has(R8EContext *ctx, R8EValue map_val, R8EValue key) {
    R8EWeakMap *wm = get_weakmap(ctx, map_val, "WeakMap.has: not a WeakMap");
    if (!wm) return false;
    if (!is_object(key)) return false;

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, wm->capacity);
    R8EWeakMapEntry *e = wm->buckets[idx];
    while (e) {
        if (e->key_ptr == key_ptr) return true;
        e = e->next;
    }
    return false;
}

bool r8e_weakmap_delete(R8EContext *ctx, R8EValue map_val, R8EValue key) {
    R8EWeakMap *wm = get_weakmap(ctx, map_val, "WeakMap.delete: not a WeakMap");
    if (!wm) return false;
    if (!is_object(key)) return false;

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, wm->capacity);
    R8EWeakMapEntry **pp = &wm->buckets[idx];
    while (*pp) {
        R8EWeakMapEntry *e = *pp;
        if (e->key_ptr == key_ptr) {
            *pp = e->next;
            free(e);
            wm->count--;
            /* Unregister from weak table */
            R8EWeakTable *wt = ctx->weak_table;
            if (wt) r8e_weak_table_unregister(wt, key_ptr, wm);
            return true;
        }
        pp = &e->next;
    }
    return false;
}

/* Built-in wrappers */
static R8EValue builtin_weakmap_constructor(R8EContext *ctx, R8EValue this_val,
                                            int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return r8e_weakmap_new(ctx);
}
static R8EValue builtin_weakmap_get(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    return r8e_weakmap_get(ctx, this_val, arg_or_undef(argc, argv, 0));
}
static R8EValue builtin_weakmap_set(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    return r8e_weakmap_set(ctx, this_val, arg_or_undef(argc, argv, 0),
                           arg_or_undef(argc, argv, 1));
}
static R8EValue builtin_weakmap_has(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_weakmap_has(ctx, this_val, arg_or_undef(argc, argv, 0)));
}
static R8EValue builtin_weakmap_delete(R8EContext *ctx, R8EValue this_val,
                                       int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_weakmap_delete(ctx, this_val, arg_or_undef(argc, argv, 0)));
}


/* *************************************************************************
 * SECTION F: WeakSet
 *
 * Like WeakMap but without values. Stores a set of weak object references.
 * ************************************************************************* */

typedef struct R8EWeakSetEntry {
    void *key_ptr;
    struct R8EWeakSetEntry *next;
} R8EWeakSetEntry;

typedef struct R8EWeakSet {
    uint32_t          flags;     /* GC kind = R8E_GC_KIND_WEAKSET */
    uint32_t          proto_id;
    R8EWeakSetEntry **buckets;
    uint32_t          capacity;
    uint32_t          count;
} R8EWeakSet;

#define R8E_WEAKSET_INIT_CAP 16

static void weakset_on_target_freed(void *owner, void *target_ptr) {
    R8EWeakSet *ws = (R8EWeakSet *)owner;
    if (!ws || !ws->buckets) return;

    uint32_t idx = ptr_hash(target_ptr, ws->capacity);
    R8EWeakSetEntry **pp = &ws->buckets[idx];
    while (*pp) {
        R8EWeakSetEntry *e = *pp;
        if (e->key_ptr == target_ptr) {
            *pp = e->next;
            free(e);
            ws->count--;
            return;
        }
        pp = &e->next;
    }
}

static R8EWeakSet *get_weakset(R8EContext *ctx, R8EValue this_val,
                               const char *method) {
    if (!R8E_IS_POINTER(this_val)) {
        throw_type_error(ctx, method);
        return NULL;
    }
    R8EWeakSet *ws = (R8EWeakSet *)r8e_get_pointer(this_val);
    if (!ws || (ws->flags & 0x0F) != R8E_GC_KIND_WEAKSET) {
        throw_type_error(ctx, method);
        return NULL;
    }
    return ws;
}

R8EValue r8e_weakset_new(R8EContext *ctx) {
    (void)ctx;
    R8EWeakSet *ws = (R8EWeakSet *)calloc(1, sizeof(R8EWeakSet));
    if (!ws) return R8E_UNDEFINED;
    ws->flags = R8E_GC_KIND_WEAKSET;
    ws->proto_id = R8E_PROTO_WEAKSET;
    ws->capacity = R8E_WEAKSET_INIT_CAP;
    ws->count = 0;
    ws->buckets = (R8EWeakSetEntry **)calloc(ws->capacity,
                                              sizeof(R8EWeakSetEntry *));
    if (!ws->buckets) { free(ws); return R8E_UNDEFINED; }
    return r8e_from_pointer(ws);
}

R8EValue r8e_weakset_add(R8EContext *ctx, R8EValue set_val, R8EValue key) {
    R8EWeakSet *ws = get_weakset(ctx, set_val, "WeakSet.add: not a WeakSet");
    if (!ws) return R8E_UNDEFINED;
    if (!is_object(key)) {
        return throw_type_error(ctx,
            "WeakSet.add: key must be an object or symbol");
    }

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, ws->capacity);

    /* Check if already present */
    R8EWeakSetEntry *e = ws->buckets[idx];
    while (e) {
        if (e->key_ptr == key_ptr) return set_val;
        e = e->next;
    }

    /* Grow if needed */
    if (ws->count * 4 > ws->capacity * 3) {
        uint32_t new_cap = ws->capacity * 2;
        R8EWeakSetEntry **new_buckets = (R8EWeakSetEntry **)calloc(
            new_cap, sizeof(R8EWeakSetEntry *));
        if (new_buckets) {
            for (uint32_t i = 0; i < ws->capacity; i++) {
                R8EWeakSetEntry *we = ws->buckets[i];
                while (we) {
                    R8EWeakSetEntry *wnext = we->next;
                    uint32_t wi = ptr_hash(we->key_ptr, new_cap);
                    we->next = new_buckets[wi];
                    new_buckets[wi] = we;
                    we = wnext;
                }
            }
            free(ws->buckets);
            ws->buckets = new_buckets;
            ws->capacity = new_cap;
        }
        idx = ptr_hash(key_ptr, ws->capacity);
    }

    R8EWeakSetEntry *entry = (R8EWeakSetEntry *)calloc(1,
                                                        sizeof(R8EWeakSetEntry));
    if (!entry) return R8E_UNDEFINED;
    entry->key_ptr = key_ptr;
    entry->next = ws->buckets[idx];
    ws->buckets[idx] = entry;
    ws->count++;

    R8EWeakTable *wt = ensure_weak_table(ctx);
    if (wt) {
        r8e_weak_table_register(wt, key_ptr, ws, R8E_WEAK_KIND_WEAKSET);
    }

    return set_val;
}

bool r8e_weakset_has(R8EContext *ctx, R8EValue set_val, R8EValue key) {
    R8EWeakSet *ws = get_weakset(ctx, set_val, "WeakSet.has: not a WeakSet");
    if (!ws) return false;
    if (!is_object(key)) return false;

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, ws->capacity);
    R8EWeakSetEntry *e = ws->buckets[idx];
    while (e) {
        if (e->key_ptr == key_ptr) return true;
        e = e->next;
    }
    return false;
}

bool r8e_weakset_delete(R8EContext *ctx, R8EValue set_val, R8EValue key) {
    R8EWeakSet *ws = get_weakset(ctx, set_val, "WeakSet.delete: not a WeakSet");
    if (!ws) return false;
    if (!is_object(key)) return false;

    void *key_ptr = r8e_get_pointer(key);
    uint32_t idx = ptr_hash(key_ptr, ws->capacity);
    R8EWeakSetEntry **pp = &ws->buckets[idx];
    while (*pp) {
        R8EWeakSetEntry *e = *pp;
        if (e->key_ptr == key_ptr) {
            *pp = e->next;
            free(e);
            ws->count--;
            R8EWeakTable *wt = ctx->weak_table;
            if (wt) r8e_weak_table_unregister(wt, key_ptr, ws);
            return true;
        }
        pp = &e->next;
    }
    return false;
}

/* Built-in wrappers */
static R8EValue builtin_weakset_constructor(R8EContext *ctx, R8EValue this_val,
                                            int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return r8e_weakset_new(ctx);
}
static R8EValue builtin_weakset_add(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    return r8e_weakset_add(ctx, this_val, arg_or_undef(argc, argv, 0));
}
static R8EValue builtin_weakset_has(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_weakset_has(ctx, this_val, arg_or_undef(argc, argv, 0)));
}
static R8EValue builtin_weakset_delete(R8EContext *ctx, R8EValue this_val,
                                       int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_weakset_delete(ctx, this_val, arg_or_undef(argc, argv, 0)));
}


/* *************************************************************************
 * SECTION G: Map (Strong, Ordered)
 *
 * Ordered hash table that preserves insertion order.
 * Uses a doubly-linked list of entries threaded through the hash table.
 * Key comparison uses SameValueZero.
 * ************************************************************************* */

typedef struct R8EMapEntry {
    R8EValue key;
    R8EValue value;
    uint32_t hash;
    struct R8EMapEntry *hash_next;   /* hash chain */
    struct R8EMapEntry *order_next;  /* insertion order: next */
    struct R8EMapEntry *order_prev;  /* insertion order: prev */
} R8EMapEntry;

typedef struct R8EMap {
    uint32_t       flags;      /* GC kind = R8E_GC_KIND_MAP */
    uint32_t       proto_id;
    R8EMapEntry  **buckets;    /* hash table */
    uint32_t       capacity;
    uint32_t       count;
    R8EMapEntry   *order_first;  /* head of insertion-order list */
    R8EMapEntry   *order_last;   /* tail of insertion-order list */
} R8EMap;

#define R8E_MAP_INIT_CAP 16

static R8EMap *get_map(R8EContext *ctx, R8EValue this_val,
                       const char *method) {
    if (!R8E_IS_POINTER(this_val)) {
        throw_type_error(ctx, method);
        return NULL;
    }
    R8EMap *m = (R8EMap *)r8e_get_pointer(this_val);
    if (!m || (m->flags & 0x0F) != R8E_GC_KIND_MAP) {
        throw_type_error(ctx, method);
        return NULL;
    }
    return m;
}

static void map_grow(R8EMap *m) {
    uint32_t new_cap = m->capacity * 2;
    R8EMapEntry **new_buckets = (R8EMapEntry **)calloc(new_cap,
                                                        sizeof(R8EMapEntry *));
    if (!new_buckets) return;

    /* Re-insert all entries using the order list (preserves order) */
    R8EMapEntry *e = m->order_first;
    while (e) {
        uint32_t idx = e->hash & (new_cap - 1);
        e->hash_next = new_buckets[idx];
        new_buckets[idx] = e;
        e = e->order_next;
    }
    free(m->buckets);
    m->buckets = new_buckets;
    m->capacity = new_cap;
}

R8EValue r8e_map_new(R8EContext *ctx) {
    (void)ctx;
    R8EMap *m = (R8EMap *)calloc(1, sizeof(R8EMap));
    if (!m) return R8E_UNDEFINED;
    m->flags = R8E_GC_KIND_MAP;
    m->proto_id = R8E_PROTO_MAP;
    m->capacity = R8E_MAP_INIT_CAP;
    m->count = 0;
    m->buckets = (R8EMapEntry **)calloc(m->capacity,
                                         sizeof(R8EMapEntry *));
    if (!m->buckets) { free(m); return R8E_UNDEFINED; }
    m->order_first = NULL;
    m->order_last = NULL;
    return r8e_from_pointer(m);
}

R8EValue r8e_map_get(R8EContext *ctx, R8EValue map_val, R8EValue key) {
    R8EMap *m = get_map(ctx, map_val, "Map.get: not a Map");
    if (!m) return R8E_UNDEFINED;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (m->capacity - 1);
    R8EMapEntry *e = m->buckets[idx];
    while (e) {
        if (e->hash == h && same_value_zero(e->key, key)) {
            return e->value;
        }
        e = e->hash_next;
    }
    return R8E_UNDEFINED;
}

R8EValue r8e_map_set(R8EContext *ctx, R8EValue map_val, R8EValue key,
                     R8EValue value) {
    R8EMap *m = get_map(ctx, map_val, "Map.set: not a Map");
    if (!m) return R8E_UNDEFINED;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (m->capacity - 1);

    /* Check for existing key */
    R8EMapEntry *e = m->buckets[idx];
    while (e) {
        if (e->hash == h && same_value_zero(e->key, key)) {
            e->value = value;
            return map_val;
        }
        e = e->hash_next;
    }

    /* Grow if needed */
    if (m->count * 4 > m->capacity * 3) {
        map_grow(m);
        idx = h & (m->capacity - 1);
    }

    /* Insert new entry */
    R8EMapEntry *entry = (R8EMapEntry *)calloc(1, sizeof(R8EMapEntry));
    if (!entry) return R8E_UNDEFINED;

    entry->key = key;
    entry->value = value;
    entry->hash = h;
    entry->hash_next = m->buckets[idx];
    m->buckets[idx] = entry;

    /* Append to insertion order list */
    entry->order_prev = m->order_last;
    entry->order_next = NULL;
    if (m->order_last) {
        m->order_last->order_next = entry;
    } else {
        m->order_first = entry;
    }
    m->order_last = entry;

    m->count++;
    return map_val;
}

bool r8e_map_has(R8EContext *ctx, R8EValue map_val, R8EValue key) {
    R8EMap *m = get_map(ctx, map_val, "Map.has: not a Map");
    if (!m) return false;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (m->capacity - 1);
    R8EMapEntry *e = m->buckets[idx];
    while (e) {
        if (e->hash == h && same_value_zero(e->key, key)) return true;
        e = e->hash_next;
    }
    return false;
}

bool r8e_map_delete(R8EContext *ctx, R8EValue map_val, R8EValue key) {
    R8EMap *m = get_map(ctx, map_val, "Map.delete: not a Map");
    if (!m) return false;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (m->capacity - 1);
    R8EMapEntry **pp = &m->buckets[idx];
    while (*pp) {
        R8EMapEntry *e = *pp;
        if (e->hash == h && same_value_zero(e->key, key)) {
            /* Remove from hash chain */
            *pp = e->hash_next;

            /* Remove from insertion order list */
            if (e->order_prev) {
                e->order_prev->order_next = e->order_next;
            } else {
                m->order_first = e->order_next;
            }
            if (e->order_next) {
                e->order_next->order_prev = e->order_prev;
            } else {
                m->order_last = e->order_prev;
            }

            free(e);
            m->count--;
            return true;
        }
        pp = &e->hash_next;
    }
    return false;
}

void r8e_map_clear(R8EContext *ctx, R8EValue map_val) {
    R8EMap *m = get_map(ctx, map_val, "Map.clear: not a Map");
    if (!m) return;

    R8EMapEntry *e = m->order_first;
    while (e) {
        R8EMapEntry *next = e->order_next;
        free(e);
        e = next;
    }
    memset(m->buckets, 0, m->capacity * sizeof(R8EMapEntry *));
    m->count = 0;
    m->order_first = NULL;
    m->order_last = NULL;
}

uint32_t r8e_map_size(R8EContext *ctx, R8EValue map_val) {
    R8EMap *m = get_map(ctx, map_val, "Map.size: not a Map");
    if (!m) return 0;
    return m->count;
}

/**
 * Map.prototype.forEach(callback [, thisArg])
 */
void r8e_map_for_each(R8EContext *ctx, R8EValue map_val, R8EValue callback,
                      R8EValue this_arg) {
    R8EMap *m = get_map(ctx, map_val, "Map.forEach: not a Map");
    if (!m) return;
    if (!is_callable(callback)) {
        throw_type_error(ctx, "Map.forEach: callback is not callable");
        return;
    }

    R8EMapEntry *e = m->order_first;
    while (e) {
        R8EValue args[3] = { e->value, e->key, map_val };
        r8e_call_function(ctx, callback, this_arg, 3, args);
        if (ctx->has_error) return;
        e = e->order_next;
    }
}

/* Built-in wrappers for Map */
static R8EValue builtin_map_constructor(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return r8e_map_new(ctx);
}
static R8EValue builtin_map_get(R8EContext *ctx, R8EValue this_val,
                                int argc, const R8EValue *argv) {
    return r8e_map_get(ctx, this_val, arg_or_undef(argc, argv, 0));
}
static R8EValue builtin_map_set(R8EContext *ctx, R8EValue this_val,
                                int argc, const R8EValue *argv) {
    return r8e_map_set(ctx, this_val, arg_or_undef(argc, argv, 0),
                       arg_or_undef(argc, argv, 1));
}
static R8EValue builtin_map_has(R8EContext *ctx, R8EValue this_val,
                                int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_map_has(ctx, this_val, arg_or_undef(argc, argv, 0)));
}
static R8EValue builtin_map_delete(R8EContext *ctx, R8EValue this_val,
                                   int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_map_delete(ctx, this_val, arg_or_undef(argc, argv, 0)));
}
static R8EValue builtin_map_clear(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    r8e_map_clear(ctx, this_val);
    return R8E_UNDEFINED;
}
static R8EValue builtin_map_size_getter(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return r8e_from_int32((int32_t)r8e_map_size(ctx, this_val));
}
static R8EValue builtin_map_forEach(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    R8EValue cb = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    r8e_map_for_each(ctx, this_val, cb, this_arg);
    return R8E_UNDEFINED;
}

/* Map iterators: keys(), values(), entries() */
typedef struct R8EMapIterator {
    uint32_t     flags;
    uint32_t     proto_id;
    R8EMap      *map;
    R8EMapEntry *current;
    uint8_t      mode; /* 0=keys, 1=values, 2=entries */
} R8EMapIterator;

#define R8E_GC_KIND_MAP_ITER  0x0E

static R8EValue map_iter_next(R8EContext *ctx, R8EValue this_val,
                              int argc, const R8EValue *argv) {
    (void)ctx; (void)argc; (void)argv;
    if (!R8E_IS_POINTER(this_val)) return R8E_UNDEFINED;
    R8EMapIterator *it = (R8EMapIterator *)r8e_get_pointer(this_val);
    if (!it) return R8E_UNDEFINED;

    R8EObjTier0 *result = r8e_obj_new(ctx);
    if (!result) return R8E_UNDEFINED;

    if (!it->current) {
        r8e_obj_set(ctx, result, R8E_ATOM_done, R8E_TRUE);
        r8e_obj_set(ctx, result, R8E_ATOM_value, R8E_UNDEFINED);
    } else {
        r8e_obj_set(ctx, result, R8E_ATOM_done, R8E_FALSE);
        if (it->mode == 0) {
            r8e_obj_set(ctx, result, R8E_ATOM_value, it->current->key);
        } else if (it->mode == 1) {
            r8e_obj_set(ctx, result, R8E_ATOM_value, it->current->value);
        } else {
            /* entries: return [key, value] as a 2-element array */
            R8EArray *pair = (R8EArray *)calloc(1, sizeof(R8EArray));
            if (pair) {
                pair->flags = R8E_GC_KIND_ARRAY;
                pair->proto_id = R8E_PROTO_ARRAY;
                pair->length = 2;
                pair->capacity = 2;
                pair->elements = (R8EValue *)calloc(2, sizeof(R8EValue));
                if (pair->elements) {
                    pair->elements[0] = it->current->key;
                    pair->elements[1] = it->current->value;
                }
                r8e_obj_set(ctx, result, R8E_ATOM_value,
                            r8e_from_pointer(pair));
            }
        }
        it->current = it->current->order_next;
    }
    return r8e_from_pointer(result);
}

static R8EValue make_map_iterator(R8EContext *ctx, R8EValue map_val,
                                  uint8_t mode) {
    R8EMap *m = get_map(ctx, map_val, "Map iterator: not a Map");
    if (!m) return R8E_UNDEFINED;

    R8EMapIterator *it = (R8EMapIterator *)calloc(1, sizeof(R8EMapIterator));
    if (!it) return R8E_UNDEFINED;
    it->flags = R8E_GC_KIND_MAP_ITER;
    it->proto_id = R8E_PROTO_OBJECT;
    it->map = m;
    it->current = m->order_first;
    it->mode = mode;

    /* Create an iterator object with a next() method */
    R8EObjTier0 *iter_obj = r8e_obj_new(ctx);
    if (!iter_obj) { free(it); return R8E_UNDEFINED; }

    install_method(ctx, iter_obj, R8E_ATOM_next, map_iter_next, 0);
    /* Store the iterator state as a hidden property (simplified: use val0) */
    /* In a full impl, the next function would close over the iterator state.
     * For now we return the iterator struct directly as the this binding. */
    return r8e_from_pointer(it);
}

static R8EValue builtin_map_keys(R8EContext *ctx, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return make_map_iterator(ctx, this_val, 0);
}
static R8EValue builtin_map_values(R8EContext *ctx, R8EValue this_val,
                                   int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return make_map_iterator(ctx, this_val, 1);
}
static R8EValue builtin_map_entries(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return make_map_iterator(ctx, this_val, 2);
}


/* *************************************************************************
 * SECTION H: Set (Strong, Ordered)
 *
 * Ordered hash set that preserves insertion order.
 * Uses same_value_zero for equality.
 * ************************************************************************* */

typedef struct R8ESetEntry {
    R8EValue key;
    uint32_t hash;
    struct R8ESetEntry *hash_next;
    struct R8ESetEntry *order_next;
    struct R8ESetEntry *order_prev;
} R8ESetEntry;

typedef struct R8ESet {
    uint32_t       flags;     /* GC kind = R8E_GC_KIND_SET */
    uint32_t       proto_id;
    R8ESetEntry  **buckets;
    uint32_t       capacity;
    uint32_t       count;
    R8ESetEntry   *order_first;
    R8ESetEntry   *order_last;
} R8ESet;

#define R8E_SET_INIT_CAP 16

static R8ESet *get_set(R8EContext *ctx, R8EValue this_val,
                       const char *method) {
    if (!R8E_IS_POINTER(this_val)) {
        throw_type_error(ctx, method);
        return NULL;
    }
    R8ESet *s = (R8ESet *)r8e_get_pointer(this_val);
    if (!s || (s->flags & 0x0F) != R8E_GC_KIND_SET) {
        throw_type_error(ctx, method);
        return NULL;
    }
    return s;
}

static void set_grow(R8ESet *s) {
    uint32_t new_cap = s->capacity * 2;
    R8ESetEntry **new_buckets = (R8ESetEntry **)calloc(new_cap,
                                                        sizeof(R8ESetEntry *));
    if (!new_buckets) return;

    R8ESetEntry *e = s->order_first;
    while (e) {
        uint32_t idx = e->hash & (new_cap - 1);
        e->hash_next = new_buckets[idx];
        new_buckets[idx] = e;
        e = e->order_next;
    }
    free(s->buckets);
    s->buckets = new_buckets;
    s->capacity = new_cap;
}

R8EValue r8e_set_new(R8EContext *ctx) {
    (void)ctx;
    R8ESet *s = (R8ESet *)calloc(1, sizeof(R8ESet));
    if (!s) return R8E_UNDEFINED;
    s->flags = R8E_GC_KIND_SET;
    s->proto_id = R8E_PROTO_SET;
    s->capacity = R8E_SET_INIT_CAP;
    s->count = 0;
    s->buckets = (R8ESetEntry **)calloc(s->capacity,
                                         sizeof(R8ESetEntry *));
    if (!s->buckets) { free(s); return R8E_UNDEFINED; }
    s->order_first = NULL;
    s->order_last = NULL;
    return r8e_from_pointer(s);
}

R8EValue r8e_set_add(R8EContext *ctx, R8EValue set_val, R8EValue key) {
    R8ESet *s = get_set(ctx, set_val, "Set.add: not a Set");
    if (!s) return R8E_UNDEFINED;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (s->capacity - 1);

    /* Check if already present */
    R8ESetEntry *e = s->buckets[idx];
    while (e) {
        if (e->hash == h && same_value_zero(e->key, key)) return set_val;
        e = e->hash_next;
    }

    /* Grow if needed */
    if (s->count * 4 > s->capacity * 3) {
        set_grow(s);
        idx = h & (s->capacity - 1);
    }

    R8ESetEntry *entry = (R8ESetEntry *)calloc(1, sizeof(R8ESetEntry));
    if (!entry) return R8E_UNDEFINED;
    entry->key = key;
    entry->hash = h;
    entry->hash_next = s->buckets[idx];
    s->buckets[idx] = entry;

    /* Append to order list */
    entry->order_prev = s->order_last;
    entry->order_next = NULL;
    if (s->order_last) {
        s->order_last->order_next = entry;
    } else {
        s->order_first = entry;
    }
    s->order_last = entry;

    s->count++;
    return set_val;
}

bool r8e_set_has(R8EContext *ctx, R8EValue set_val, R8EValue key) {
    R8ESet *s = get_set(ctx, set_val, "Set.has: not a Set");
    if (!s) return false;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (s->capacity - 1);
    R8ESetEntry *e = s->buckets[idx];
    while (e) {
        if (e->hash == h && same_value_zero(e->key, key)) return true;
        e = e->hash_next;
    }
    return false;
}

bool r8e_set_delete(R8EContext *ctx, R8EValue set_val, R8EValue key) {
    R8ESet *s = get_set(ctx, set_val, "Set.delete: not a Set");
    if (!s) return false;

    uint32_t h = value_hash(key);
    uint32_t idx = h & (s->capacity - 1);
    R8ESetEntry **pp = &s->buckets[idx];
    while (*pp) {
        R8ESetEntry *e = *pp;
        if (e->hash == h && same_value_zero(e->key, key)) {
            *pp = e->hash_next;

            if (e->order_prev) {
                e->order_prev->order_next = e->order_next;
            } else {
                s->order_first = e->order_next;
            }
            if (e->order_next) {
                e->order_next->order_prev = e->order_prev;
            } else {
                s->order_last = e->order_prev;
            }

            free(e);
            s->count--;
            return true;
        }
        pp = &e->hash_next;
    }
    return false;
}

void r8e_set_clear(R8EContext *ctx, R8EValue set_val) {
    R8ESet *s = get_set(ctx, set_val, "Set.clear: not a Set");
    if (!s) return;

    R8ESetEntry *e = s->order_first;
    while (e) {
        R8ESetEntry *next = e->order_next;
        free(e);
        e = next;
    }
    memset(s->buckets, 0, s->capacity * sizeof(R8ESetEntry *));
    s->count = 0;
    s->order_first = NULL;
    s->order_last = NULL;
}

uint32_t r8e_set_size(R8EContext *ctx, R8EValue set_val) {
    R8ESet *s = get_set(ctx, set_val, "Set.size: not a Set");
    if (!s) return 0;
    return s->count;
}

void r8e_set_for_each(R8EContext *ctx, R8EValue set_val, R8EValue callback,
                      R8EValue this_arg) {
    R8ESet *s = get_set(ctx, set_val, "Set.forEach: not a Set");
    if (!s) return;
    if (!is_callable(callback)) {
        throw_type_error(ctx, "Set.forEach: callback is not callable");
        return;
    }

    R8ESetEntry *e = s->order_first;
    while (e) {
        /* Set.forEach calls callback(value, value, set) */
        R8EValue args[3] = { e->key, e->key, set_val };
        r8e_call_function(ctx, callback, this_arg, 3, args);
        if (ctx->has_error) return;
        e = e->order_next;
    }
}

/* Built-in wrappers for Set */
static R8EValue builtin_set_constructor(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return r8e_set_new(ctx);
}
static R8EValue builtin_set_add(R8EContext *ctx, R8EValue this_val,
                                int argc, const R8EValue *argv) {
    return r8e_set_add(ctx, this_val, arg_or_undef(argc, argv, 0));
}
static R8EValue builtin_set_has(R8EContext *ctx, R8EValue this_val,
                                int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_set_has(ctx, this_val, arg_or_undef(argc, argv, 0)));
}
static R8EValue builtin_set_delete(R8EContext *ctx, R8EValue this_val,
                                   int argc, const R8EValue *argv) {
    return r8e_from_boolean(
        r8e_set_delete(ctx, this_val, arg_or_undef(argc, argv, 0)));
}
static R8EValue builtin_set_clear(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    r8e_set_clear(ctx, this_val);
    return R8E_UNDEFINED;
}
static R8EValue builtin_set_size_getter(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return r8e_from_int32((int32_t)r8e_set_size(ctx, this_val));
}
static R8EValue builtin_set_forEach(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    R8EValue cb = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    r8e_set_for_each(ctx, this_val, cb, this_arg);
    return R8E_UNDEFINED;
}

/* Set iterators */
typedef struct R8ESetIterator {
    uint32_t     flags;
    uint32_t     proto_id;
    R8ESet      *set;
    R8ESetEntry *current;
    uint8_t      mode; /* 0=values, 1=entries */
} R8ESetIterator;

#define R8E_GC_KIND_SET_ITER  0x0F

static R8EValue set_iter_next(R8EContext *ctx, R8EValue this_val,
                              int argc, const R8EValue *argv) {
    (void)ctx; (void)argc; (void)argv;
    if (!R8E_IS_POINTER(this_val)) return R8E_UNDEFINED;
    R8ESetIterator *it = (R8ESetIterator *)r8e_get_pointer(this_val);
    if (!it) return R8E_UNDEFINED;

    R8EObjTier0 *result = r8e_obj_new(ctx);
    if (!result) return R8E_UNDEFINED;

    if (!it->current) {
        r8e_obj_set(ctx, result, R8E_ATOM_done, R8E_TRUE);
        r8e_obj_set(ctx, result, R8E_ATOM_value, R8E_UNDEFINED);
    } else {
        r8e_obj_set(ctx, result, R8E_ATOM_done, R8E_FALSE);
        if (it->mode == 1) {
            /* entries: [value, value] */
            R8EArray *pair = (R8EArray *)calloc(1, sizeof(R8EArray));
            if (pair) {
                pair->flags = R8E_GC_KIND_ARRAY;
                pair->proto_id = R8E_PROTO_ARRAY;
                pair->length = 2;
                pair->capacity = 2;
                pair->elements = (R8EValue *)calloc(2, sizeof(R8EValue));
                if (pair->elements) {
                    pair->elements[0] = it->current->key;
                    pair->elements[1] = it->current->key;
                }
                r8e_obj_set(ctx, result, R8E_ATOM_value,
                            r8e_from_pointer(pair));
            }
        } else {
            r8e_obj_set(ctx, result, R8E_ATOM_value, it->current->key);
        }
        it->current = it->current->order_next;
    }
    return r8e_from_pointer(result);
}

static R8EValue make_set_iterator(R8EContext *ctx, R8EValue set_val,
                                  uint8_t mode) {
    R8ESet *s = get_set(ctx, set_val, "Set iterator: not a Set");
    if (!s) return R8E_UNDEFINED;

    R8ESetIterator *it = (R8ESetIterator *)calloc(1, sizeof(R8ESetIterator));
    if (!it) return R8E_UNDEFINED;
    it->flags = R8E_GC_KIND_SET_ITER;
    it->proto_id = R8E_PROTO_OBJECT;
    it->set = s;
    it->current = s->order_first;
    it->mode = mode;

    return r8e_from_pointer(it);
}

static R8EValue builtin_set_values(R8EContext *ctx, R8EValue this_val,
                                   int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return make_set_iterator(ctx, this_val, 0);
}
static R8EValue builtin_set_entries(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return make_set_iterator(ctx, this_val, 1);
}
/* Set.keys is an alias for Set.values */
static R8EValue builtin_set_keys(R8EContext *ctx, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    return make_set_iterator(ctx, this_val, 0);
}


/* *************************************************************************
 * SECTION I: Initialization
 *
 * Register WeakRef, FinalizationRegistry, WeakMap, WeakSet, Map, Set
 * prototypes and constructors on the global object.
 * ************************************************************************* */

void r8e_init_weakref_collections(R8EContext *ctx)
{
    if (!ctx) return;

    /* Ensure weak table exists */
    ensure_weak_table(ctx);

    /* ---- WeakRef ---- */
    R8EObjTier0 *wr_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    if (ctx->prototypes && R8E_PROTO_WEAKREF < ctx->proto_count) {
        ctx->prototypes[R8E_PROTO_WEAKREF] = wr_proto;
    }
    install_method(ctx, wr_proto, R8E_ATOM_deref, builtin_weakref_deref, 0);

    R8EObjTier0 *wr_ctor = r8e_obj_new(ctx);
    install_value(ctx, wr_ctor, R8E_ATOM_prototype, r8e_from_pointer(wr_proto));
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_WeakRef,
                    r8e_from_pointer(wr_ctor));
    }

    /* ---- FinalizationRegistry ---- */
    R8EObjTier0 *fr_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    if (ctx->prototypes && R8E_PROTO_FINALIZATION < ctx->proto_count) {
        ctx->prototypes[R8E_PROTO_FINALIZATION] = fr_proto;
    }
    install_method(ctx, fr_proto, R8E_ATOM_register,
                   builtin_finreg_register, 2);
    install_method(ctx, fr_proto, R8E_ATOM_unregister,
                   builtin_finreg_unregister, 1);

    R8EObjTier0 *fr_ctor = r8e_obj_new(ctx);
    install_value(ctx, fr_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(fr_proto));
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_FinalizationRegistry,
                    r8e_from_pointer(fr_ctor));
    }

    /* ---- WeakMap ---- */
    R8EObjTier0 *wm_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    if (ctx->prototypes && R8E_PROTO_WEAKMAP < ctx->proto_count) {
        ctx->prototypes[R8E_PROTO_WEAKMAP] = wm_proto;
    }
    install_method(ctx, wm_proto, R8E_ATOM_get, builtin_weakmap_get, 1);
    install_method(ctx, wm_proto, R8E_ATOM_set, builtin_weakmap_set, 2);
    install_method(ctx, wm_proto, R8E_ATOM_has, builtin_weakmap_has, 1);
    install_method(ctx, wm_proto, R8E_ATOM_delete_method,
                   builtin_weakmap_delete, 1);

    R8EObjTier0 *wm_ctor = r8e_obj_new(ctx);
    install_value(ctx, wm_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(wm_proto));
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_WeakMap,
                    r8e_from_pointer(wm_ctor));
    }

    /* ---- WeakSet ---- */
    R8EObjTier0 *ws_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    if (ctx->prototypes && R8E_PROTO_WEAKSET < ctx->proto_count) {
        ctx->prototypes[R8E_PROTO_WEAKSET] = ws_proto;
    }
    install_method(ctx, ws_proto, R8E_ATOM_add, builtin_weakset_add, 1);
    install_method(ctx, ws_proto, R8E_ATOM_has, builtin_weakset_has, 1);
    install_method(ctx, ws_proto, R8E_ATOM_delete_method,
                   builtin_weakset_delete, 1);

    R8EObjTier0 *ws_ctor = r8e_obj_new(ctx);
    install_value(ctx, ws_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(ws_proto));
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_WeakSet,
                    r8e_from_pointer(ws_ctor));
    }

    /* ---- Map ---- */
    R8EObjTier0 *map_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    if (ctx->prototypes && R8E_PROTO_MAP < ctx->proto_count) {
        ctx->prototypes[R8E_PROTO_MAP] = map_proto;
    }
    install_method(ctx, map_proto, R8E_ATOM_get, builtin_map_get, 1);
    install_method(ctx, map_proto, R8E_ATOM_set, builtin_map_set, 2);
    install_method(ctx, map_proto, R8E_ATOM_has, builtin_map_has, 1);
    install_method(ctx, map_proto, R8E_ATOM_delete_method,
                   builtin_map_delete, 1);
    install_method(ctx, map_proto, R8E_ATOM_clear, builtin_map_clear, 0);
    install_method(ctx, map_proto, R8E_ATOM_forEach, builtin_map_forEach, 1);
    install_method(ctx, map_proto, R8E_ATOM_keys, builtin_map_keys, 0);
    install_method(ctx, map_proto, R8E_ATOM_values, builtin_map_values, 0);
    install_method(ctx, map_proto, R8E_ATOM_entries, builtin_map_entries, 0);
    install_method(ctx, map_proto, R8E_ATOM_size, builtin_map_size_getter, 0);
    /* Map[Symbol.iterator] = Map.prototype.entries */
    install_method(ctx, map_proto, R8E_ATOM_iterator, builtin_map_entries, 0);

    R8EObjTier0 *map_ctor = r8e_obj_new(ctx);
    install_value(ctx, map_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(map_proto));
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_Map,
                    r8e_from_pointer(map_ctor));
    }

    /* ---- Set ---- */
    R8EObjTier0 *set_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    if (ctx->prototypes && R8E_PROTO_SET < ctx->proto_count) {
        ctx->prototypes[R8E_PROTO_SET] = set_proto;
    }
    install_method(ctx, set_proto, R8E_ATOM_add, builtin_set_add, 1);
    install_method(ctx, set_proto, R8E_ATOM_has, builtin_set_has, 1);
    install_method(ctx, set_proto, R8E_ATOM_delete_method,
                   builtin_set_delete, 1);
    install_method(ctx, set_proto, R8E_ATOM_clear, builtin_set_clear, 0);
    install_method(ctx, set_proto, R8E_ATOM_forEach, builtin_set_forEach, 1);
    install_method(ctx, set_proto, R8E_ATOM_keys, builtin_set_keys, 0);
    install_method(ctx, set_proto, R8E_ATOM_values, builtin_set_values, 0);
    install_method(ctx, set_proto, R8E_ATOM_entries, builtin_set_entries, 0);
    install_method(ctx, set_proto, R8E_ATOM_size, builtin_set_size_getter, 0);
    /* Set[Symbol.iterator] = Set.prototype.values */
    install_method(ctx, set_proto, R8E_ATOM_iterator, builtin_set_values, 0);

    R8EObjTier0 *set_ctor = r8e_obj_new(ctx);
    install_value(ctx, set_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(set_proto));
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_Set,
                    r8e_from_pointer(set_ctor));
    }
}


/* *************************************************************************
 * SECTION J: Cleanup
 *
 * Free resources for the weak reference subsystem.
 * Called during context destruction.
 * ************************************************************************* */

void r8e_weakref_cleanup(R8EContext *ctx) {
    if (!ctx) return;
    if (ctx->weak_table) {
        r8e_weak_table_free(ctx->weak_table);
        ctx->weak_table = NULL;
    }
}
