/*
 * r8e_array.c - Array Fast Path + Sparse Fallback + Array.prototype Methods
 *
 * Dense arrays use a contiguous C array of NaN-boxed R8EValues indexed by
 * integer. No key storage needed. Named properties (arr.customProp) are
 * stored in a separate CDOL object, only allocated if needed.
 *
 * Sparse fallback: when length > 2 * element_count, switch to a hash table
 * of index-to-value pairs for memory efficiency.
 *
 * Reference: CLAUDE.md Section 3.3 (Array Optimization)
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

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

static inline double r8e_get_double(uint64_t v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline uint64_t r8e_from_double(double d) {
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* =========================================================================
 * Object model flags (duplicated from r8e_object.c until headers exist)
 * ========================================================================= */

#define R8E_OBJ_TIER_MASK       0x00000003u
#define R8E_OBJ_EXTENSIBLE      0x00000040u
#define R8E_OBJ_IS_ARRAY        0x00000080u
#define R8E_OBJ_RC_INLINE_SHIFT 16

#define R8E_PROTO_ARRAY  1
#define R8E_PROTO_NULL   0xFFFFFFFFu

#define R8E_PROP_WRITABLE     0x01u
#define R8E_PROP_ENUMERABLE   0x02u
#define R8E_PROP_CONFIGURABLE 0x04u
#define R8E_PROP_DEFAULT      (R8E_PROP_WRITABLE | R8E_PROP_ENUMERABLE | R8E_PROP_CONFIGURABLE)

/* =========================================================================
 * Context stub (mirrors r8e_object.c definition)
 * ========================================================================= */

#define R8E_PROTO_MAX 64

typedef struct R8EContext {
    R8EValue  proto_table[R8E_PROTO_MAX];
    uint32_t  proto_count;
    void *(*alloc)(size_t size, void *opaque);
    void *(*realloc_fn)(void *ptr, size_t size, void *opaque);
    void  (*free_fn)(void *ptr, void *opaque);
    void  *alloc_opaque;
    R8EValue last_error;
} R8EContext;

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
 * Forward declarations for the CDOL object model (from r8e_object.c)
 * These are the public functions we call for named property storage.
 * ========================================================================= */

/* Opaque object type for named properties on arrays */
typedef struct R8EObjHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjHeader;

/* External object functions (linked from r8e_object.c) */
extern void    *r8e_obj_new(R8EContext *ctx);
extern R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key);
extern void    *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key, R8EValue val);
extern bool     r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key);
extern int      r8e_obj_delete(R8EContext *ctx, void *obj, uint32_t key);
extern uint32_t r8e_obj_keys(R8EContext *ctx, void *obj,
                              uint32_t *out_keys, uint32_t max_keys);
extern void     r8e_obj_destroy(R8EContext *ctx, void *obj);

/* =========================================================================
 * Sparse hash table for sparse array fallback
 *
 * When length > 2 * element_count, we switch from a dense contiguous
 * buffer to a hash table mapping uint32_t index -> R8EValue.
 * Open addressing with linear probing.
 * ========================================================================= */

#define R8E_SPARSE_EMPTY_KEY  0xFFFFFFFFu
#define R8E_SPARSE_INIT_CAP   32
#define R8E_SPARSE_LOAD_NUM   3
#define R8E_SPARSE_LOAD_DEN   4

typedef struct {
    uint32_t index;   /* array index, 0xFFFFFFFF = empty */
    R8EValue val;
} R8ESparseEntry;

typedef struct {
    R8ESparseEntry *entries;
    uint32_t        count;
    uint32_t        capacity;  /* power of 2 */
} R8ESparseTable;

/* Count trailing zeros (portable fallback for MSVC) */
static inline uint32_t r8e_ctz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(x);
#else
    /* De Bruijn sequence method */
    static const uint32_t debruijn[32] = {
        0,1,28,2,29,14,24,3,30,22,20,15,25,17,4,8,
        31,27,13,23,21,19,16,7,26,12,18,6,11,5,10,9
    };
    return debruijn[((x & -(int32_t)x) * 0x077CB531u) >> 27];
#endif
}

/* Hash a uint32_t index for the sparse table */
static inline uint32_t r8e_sparse_hash(uint32_t idx, uint32_t cap) {
    /* Fibonacci hashing for better distribution */
    return (idx * 2654435769u) >> (32 - r8e_ctz32(cap));
}

static R8ESparseTable *r8e_sparse_new(R8EContext *ctx, uint32_t capacity) {
    R8ESparseTable *st = (R8ESparseTable *)r8e_alloc(ctx,
                                                      sizeof(R8ESparseTable));
    if (!st) return NULL;

    /* Round up to power of 2 */
    uint32_t cap = R8E_SPARSE_INIT_CAP;
    while (cap < capacity) cap *= 2;

    st->entries = (R8ESparseEntry *)r8e_alloc(ctx,
                                               sizeof(R8ESparseEntry) * cap);
    if (!st->entries) {
        r8e_free(ctx, st);
        return NULL;
    }
    for (uint32_t i = 0; i < cap; i++) {
        st->entries[i].index = R8E_SPARSE_EMPTY_KEY;
        st->entries[i].val   = R8E_UNDEFINED;
    }
    st->count    = 0;
    st->capacity = cap;
    return st;
}

static int r8e_sparse_resize(R8EContext *ctx, R8ESparseTable *st,
                               uint32_t new_cap) {
    R8ESparseEntry *old = st->entries;
    uint32_t old_cap = st->capacity;

    R8ESparseEntry *ne = (R8ESparseEntry *)r8e_alloc(ctx,
                                                      sizeof(R8ESparseEntry) * new_cap);
    if (!ne) return -1;
    for (uint32_t i = 0; i < new_cap; i++) {
        ne[i].index = R8E_SPARSE_EMPTY_KEY;
        ne[i].val   = R8E_UNDEFINED;
    }

    st->entries  = ne;
    st->capacity = new_cap;
    st->count    = 0;

    /* Re-insert */
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].index != R8E_SPARSE_EMPTY_KEY) {
            uint32_t slot = r8e_sparse_hash(old[i].index, new_cap);
            while (ne[slot].index != R8E_SPARSE_EMPTY_KEY)
                slot = (slot + 1) & (new_cap - 1);
            ne[slot] = old[i];
            st->count++;
        }
    }

    r8e_free(ctx, old);
    return 0;
}

static int r8e_sparse_find(const R8ESparseTable *st, uint32_t idx) {
    uint32_t slot = r8e_sparse_hash(idx, st->capacity);
    uint32_t probes = 0;
    while (probes < st->capacity) {
        if (st->entries[slot].index == R8E_SPARSE_EMPTY_KEY)
            return -1;
        if (st->entries[slot].index == idx)
            return (int)slot;
        slot = (slot + 1) & (st->capacity - 1);
        probes++;
    }
    return -1;
}

static R8EValue r8e_sparse_get(const R8ESparseTable *st, uint32_t idx) {
    int slot = r8e_sparse_find(st, idx);
    if (slot >= 0)
        return st->entries[slot].val;
    return R8E_UNDEFINED;
}

static bool r8e_sparse_has(const R8ESparseTable *st, uint32_t idx) {
    return r8e_sparse_find(st, idx) >= 0;
}

static int r8e_sparse_set(R8EContext *ctx, R8ESparseTable *st,
                            uint32_t idx, R8EValue val) {
    /* Update existing */
    int existing = r8e_sparse_find(st, idx);
    if (existing >= 0) {
        st->entries[existing].val = val;
        return 0;
    }

    /* Check load factor */
    if (st->count * R8E_SPARSE_LOAD_DEN >= st->capacity * R8E_SPARSE_LOAD_NUM) {
        if (r8e_sparse_resize(ctx, st, st->capacity * 2) != 0)
            return -1;
    }

    /* Insert */
    uint32_t slot = r8e_sparse_hash(idx, st->capacity);
    while (st->entries[slot].index != R8E_SPARSE_EMPTY_KEY)
        slot = (slot + 1) & (st->capacity - 1);
    st->entries[slot].index = idx;
    st->entries[slot].val   = val;
    st->count++;
    return 0;
}

static int r8e_sparse_delete(R8ESparseTable *st, uint32_t idx) {
    int slot_int = r8e_sparse_find(st, idx);
    if (slot_int < 0)
        return 0;  /* not found */

    uint32_t slot = (uint32_t)slot_int;
    st->entries[slot].index = R8E_SPARSE_EMPTY_KEY;
    st->entries[slot].val   = R8E_UNDEFINED;
    st->count--;

    /* Backward shift to maintain linear probing invariant */
    uint32_t next = (slot + 1) & (st->capacity - 1);
    while (st->entries[next].index != R8E_SPARSE_EMPTY_KEY) {
        uint32_t desired = r8e_sparse_hash(st->entries[next].index,
                                            st->capacity);
        /* Check if 'next' is displaced past 'slot' */
        uint32_t d_next = (next + st->capacity - desired) & (st->capacity - 1);
        if (d_next == 0) break;
        st->entries[slot] = st->entries[next];
        st->entries[next].index = R8E_SPARSE_EMPTY_KEY;
        st->entries[next].val   = R8E_UNDEFINED;
        slot = next;
        next = (next + 1) & (st->capacity - 1);
    }
    return 0;
}

static void r8e_sparse_destroy(R8EContext *ctx, R8ESparseTable *st) {
    if (st) {
        r8e_free(ctx, st->entries);
        r8e_free(ctx, st);
    }
}

/* =========================================================================
 * R8EArray - the main array struct
 *
 * Dense mode: elements is a contiguous R8EValue array, indexed 0..length-1.
 * Sparse mode: sparse points to a hash table. elements is NULL.
 * Named properties: named is a CDOL object pointer, NULL if no named props.
 * ========================================================================= */

#define R8E_ARRAY_INIT_CAP      8
#define R8E_ARRAY_MAX_LENGTH    0xFFFFFFFFu
#define R8E_ARRAY_SPARSE_RATIO  2  /* go sparse when length > ratio * count */

struct R8EArray {
    uint32_t         flags;
    uint32_t         proto_id;
    uint32_t         length;
    uint32_t         capacity;       /* dense capacity (0 if sparse) */
    uint32_t         dense_count;    /* number of non-undefined elements in dense */
    uint32_t         pad;
    R8EValue        *elements;       /* dense array (NULL if sparse) */
    R8ESparseTable  *sparse;         /* sparse hash table (NULL if dense) */
    void            *named;          /* R8EObject* for named props, NULL if none */
};

typedef struct R8EArray R8EArray;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static inline uint32_t r8e_array_init_flags(void) {
    uint32_t flags = 0;
    flags |= R8E_OBJ_IS_ARRAY;
    flags |= R8E_OBJ_EXTENSIBLE;
    flags |= (1u << R8E_OBJ_RC_INLINE_SHIFT);  /* refcount = 1 */
    return flags;
}

/* Check if the array is in sparse mode */
static inline bool r8e_array_is_sparse(const R8EArray *arr) {
    return arr->sparse != NULL;
}

/* Grow the dense elements buffer to at least new_cap */
static int r8e_array_grow(R8EContext *ctx, R8EArray *arr, uint32_t new_cap) {
    if (new_cap <= arr->capacity)
        return 0;

    /* Growth policy: double until we reach the target */
    uint32_t cap = arr->capacity;
    if (cap == 0) cap = R8E_ARRAY_INIT_CAP;
    while (cap < new_cap) {
        uint32_t next = cap * 2;
        if (next < cap) {
            /* overflow: just use new_cap */
            cap = new_cap;
            break;
        }
        cap = next;
    }

    R8EValue *new_elems = (R8EValue *)r8e_realloc(ctx, arr->elements,
                                                    sizeof(R8EValue) * cap);
    if (!new_elems) return -1;

    /* Initialize new slots to undefined */
    for (uint32_t i = arr->capacity; i < cap; i++)
        new_elems[i] = R8E_UNDEFINED;

    arr->elements = new_elems;
    arr->capacity = cap;
    return 0;
}

/* Convert dense array to sparse when length >> count */
static int r8e_array_to_sparse(R8EContext *ctx, R8EArray *arr) {
    if (r8e_array_is_sparse(arr))
        return 0;  /* already sparse */

    uint32_t init_cap = R8E_SPARSE_INIT_CAP;
    while (init_cap < arr->dense_count * 2) init_cap *= 2;

    R8ESparseTable *st = r8e_sparse_new(ctx, init_cap);
    if (!st) return -1;

    /* Migrate non-undefined elements to sparse table */
    for (uint32_t i = 0; i < arr->length && i < arr->capacity; i++) {
        if (arr->elements[i] != R8E_UNDEFINED) {
            if (r8e_sparse_set(ctx, st, i, arr->elements[i]) != 0) {
                r8e_sparse_destroy(ctx, st);
                return -1;
            }
        }
    }

    r8e_free(ctx, arr->elements);
    arr->elements = NULL;
    arr->capacity = 0;
    arr->sparse   = st;
    return 0;
}

/* Convert sparse array back to dense (when it becomes dense enough) */
static int r8e_array_to_dense(R8EContext *ctx, R8EArray *arr) {
    if (!r8e_array_is_sparse(arr))
        return 0;  /* already dense */

    uint32_t cap = R8E_ARRAY_INIT_CAP;
    while (cap < arr->length) {
        uint32_t next = cap * 2;
        if (next < cap) { cap = arr->length; break; }
        cap = next;
    }

    R8EValue *elems = (R8EValue *)r8e_alloc(ctx, sizeof(R8EValue) * cap);
    if (!elems) return -1;

    for (uint32_t i = 0; i < cap; i++)
        elems[i] = R8E_UNDEFINED;

    /* Copy from sparse to dense */
    R8ESparseTable *st = arr->sparse;
    uint32_t count = 0;
    for (uint32_t i = 0; i < st->capacity; i++) {
        if (st->entries[i].index != R8E_SPARSE_EMPTY_KEY &&
            st->entries[i].index < cap) {
            elems[st->entries[i].index] = st->entries[i].val;
            count++;
        }
    }

    r8e_sparse_destroy(ctx, st);
    arr->elements    = elems;
    arr->capacity    = cap;
    arr->sparse      = NULL;
    arr->dense_count = count;
    return 0;
}

/* Check if we should switch to sparse mode */
static inline bool r8e_should_go_sparse(const R8EArray *arr, uint32_t new_len) {
    if (r8e_array_is_sparse(arr))
        return false;  /* already sparse */
    /* Go sparse when the requested length greatly exceeds actual element count */
    return (new_len > 1024 &&
            new_len > R8E_ARRAY_SPARSE_RATIO * arr->dense_count);
}

/* =========================================================================
 * SECTION: Array Creation
 * ========================================================================= */

/*
 * r8e_array_new - Create a new dense array with given initial capacity.
 */
R8EArray *r8e_array_new(R8EContext *ctx, uint32_t initial_capacity) {
    R8EArray *arr = (R8EArray *)r8e_alloc(ctx, sizeof(R8EArray));
    if (!arr) return NULL;

    arr->flags       = r8e_array_init_flags();
    arr->proto_id    = R8E_PROTO_ARRAY;
    arr->length      = 0;
    arr->dense_count = 0;
    arr->pad         = 0;
    arr->sparse      = NULL;
    arr->named       = NULL;

    uint32_t cap = R8E_ARRAY_INIT_CAP;
    if (initial_capacity > cap) {
        while (cap < initial_capacity) {
            uint32_t next = cap * 2;
            if (next < cap) { cap = initial_capacity; break; }
            cap = next;
        }
    }

    arr->elements = (R8EValue *)r8e_alloc(ctx, sizeof(R8EValue) * cap);
    if (!arr->elements) {
        r8e_free(ctx, arr);
        return NULL;
    }
    for (uint32_t i = 0; i < cap; i++)
        arr->elements[i] = R8E_UNDEFINED;
    arr->capacity = cap;

    return arr;
}

/*
 * r8e_array_destroy - Free all memory associated with an array.
 */
void r8e_array_destroy(R8EContext *ctx, R8EArray *arr) {
    if (!arr) return;

    if (arr->elements)
        r8e_free(ctx, arr->elements);
    if (arr->sparse)
        r8e_sparse_destroy(ctx, arr->sparse);
    if (arr->named)
        r8e_obj_destroy(ctx, arr->named);

    r8e_free(ctx, arr);
}

/* =========================================================================
 * SECTION: Core Dense Operations
 * ========================================================================= */

/*
 * r8e_array_length - Return the array length.
 */
uint32_t r8e_array_length(const R8EArray *arr) {
    return arr->length;
}

/*
 * r8e_array_get - Get element at index. O(1) for dense arrays.
 * Returns R8E_UNDEFINED for out-of-range indices.
 */
R8EValue r8e_array_get(R8EContext *ctx, const R8EArray *arr, uint32_t index) {
    (void)ctx;
    if (index >= arr->length)
        return R8E_UNDEFINED;

    if (r8e_array_is_sparse(arr))
        return r8e_sparse_get(arr->sparse, index);

    if (index < arr->capacity)
        return arr->elements[index];

    return R8E_UNDEFINED;
}

/*
 * r8e_array_set - Set element at index. Auto-grows as needed.
 * Returns 0 on success, -1 on allocation failure.
 */
int r8e_array_set(R8EContext *ctx, R8EArray *arr, uint32_t index, R8EValue val) {
    /* Update length if setting beyond current */
    uint32_t new_len = (index >= arr->length) ? index + 1 : arr->length;

    /* Check if we should go sparse */
    if (r8e_should_go_sparse(arr, new_len)) {
        if (r8e_array_to_sparse(ctx, arr) != 0)
            return -1;
    }

    if (r8e_array_is_sparse(arr)) {
        if (r8e_sparse_set(ctx, arr->sparse, index, val) != 0)
            return -1;
        arr->length = new_len;
        return 0;
    }

    /* Dense mode: grow if needed */
    if (index >= arr->capacity) {
        if (r8e_array_grow(ctx, arr, index + 1) != 0)
            return -1;
    }

    /* Track dense element count */
    if (index < arr->capacity) {
        bool was_defined = (index < arr->length && arr->elements[index] != R8E_UNDEFINED);
        bool is_defined  = (val != R8E_UNDEFINED);

        if (!was_defined && is_defined)
            arr->dense_count++;
        else if (was_defined && !is_defined)
            arr->dense_count--;
    }

    arr->elements[index] = val;
    arr->length = new_len;
    return 0;
}

/*
 * r8e_array_push - Append a value to the end. Returns new length.
 */
uint32_t r8e_array_push(R8EContext *ctx, R8EArray *arr, R8EValue val) {
    uint32_t idx = arr->length;
    if (r8e_array_set(ctx, arr, idx, val) != 0)
        return arr->length;  /* return old length on failure */
    return arr->length;
}

/*
 * r8e_array_pop - Remove and return the last element.
 * Returns R8E_UNDEFINED if empty.
 */
R8EValue r8e_array_pop(R8EContext *ctx, R8EArray *arr) {
    if (arr->length == 0)
        return R8E_UNDEFINED;

    uint32_t idx = arr->length - 1;
    R8EValue val = r8e_array_get(ctx, arr, idx);

    if (r8e_array_is_sparse(arr)) {
        r8e_sparse_delete(arr->sparse, idx);
    } else if (idx < arr->capacity) {
        if (arr->elements[idx] != R8E_UNDEFINED)
            arr->dense_count--;
        arr->elements[idx] = R8E_UNDEFINED;
    }

    arr->length--;
    return val;
}

/* =========================================================================
 * SECTION: Array.prototype methods
 * ========================================================================= */

/*
 * r8e_array_shift - Remove the first element, shift all others left.
 * Returns the removed element, or R8E_UNDEFINED if empty.
 */
R8EValue r8e_array_shift(R8EContext *ctx, R8EArray *arr) {
    if (arr->length == 0)
        return R8E_UNDEFINED;

    R8EValue val = r8e_array_get(ctx, arr, 0);

    if (r8e_array_is_sparse(arr)) {
        r8e_sparse_delete(arr->sparse, 0);
        /* Re-index: shift all entries down by 1 */
        R8ESparseTable *st = arr->sparse;
        /* We need a temporary copy since modifying in place is complex */
        R8ESparseTable *new_st = r8e_sparse_new(ctx, st->capacity);
        if (new_st) {
            for (uint32_t i = 0; i < st->capacity; i++) {
                if (st->entries[i].index != R8E_SPARSE_EMPTY_KEY &&
                    st->entries[i].index > 0) {
                    r8e_sparse_set(ctx, new_st, st->entries[i].index - 1,
                                   st->entries[i].val);
                }
            }
            r8e_sparse_destroy(ctx, st);
            arr->sparse = new_st;
        }
    } else {
        if (arr->elements[0] != R8E_UNDEFINED)
            arr->dense_count--;
        /* Shift elements left by one */
        uint32_t copy_len = (arr->length - 1 < arr->capacity)
                            ? arr->length - 1 : arr->capacity - 1;
        if (copy_len > 0)
            memmove(arr->elements, arr->elements + 1,
                    sizeof(R8EValue) * copy_len);
        if (arr->length - 1 < arr->capacity)
            arr->elements[arr->length - 1] = R8E_UNDEFINED;
    }

    arr->length--;
    return val;
}

/*
 * r8e_array_unshift - Insert elements at the beginning.
 * Returns the new length.
 */
uint32_t r8e_array_unshift(R8EContext *ctx, R8EArray *arr,
                            const R8EValue *vals, uint32_t count) {
    if (count == 0)
        return arr->length;

    uint32_t new_len = arr->length + count;

    if (r8e_array_is_sparse(arr)) {
        /* Re-index existing entries upward */
        R8ESparseTable *st = arr->sparse;
        R8ESparseTable *new_st = r8e_sparse_new(ctx, st->capacity);
        if (!new_st) return arr->length;

        for (uint32_t i = 0; i < st->capacity; i++) {
            if (st->entries[i].index != R8E_SPARSE_EMPTY_KEY) {
                r8e_sparse_set(ctx, new_st, st->entries[i].index + count,
                               st->entries[i].val);
            }
        }
        /* Insert new elements */
        for (uint32_t i = 0; i < count; i++)
            r8e_sparse_set(ctx, new_st, i, vals[i]);

        r8e_sparse_destroy(ctx, st);
        arr->sparse = new_st;
        arr->length = new_len;
    } else {
        /* Dense: grow if needed, shift right, insert */
        if (new_len > arr->capacity) {
            if (r8e_array_grow(ctx, arr, new_len) != 0)
                return arr->length;
        }
        /* Shift existing elements right by count */
        if (arr->length > 0) {
            memmove(arr->elements + count, arr->elements,
                    sizeof(R8EValue) * arr->length);
        }
        /* Insert new elements */
        for (uint32_t i = 0; i < count; i++) {
            arr->elements[i] = vals[i];
            if (vals[i] != R8E_UNDEFINED)
                arr->dense_count++;
        }
        arr->length = new_len;
    }

    return arr->length;
}

/*
 * r8e_array_splice - Remove/insert elements at a position.
 *
 * start: index to begin changes
 * delete_count: number of elements to remove
 * items: elements to insert (can be NULL if insert_count is 0)
 * insert_count: number of elements to insert
 * out_deleted: if non-NULL, receives the deleted elements (caller allocates)
 *
 * Returns 0 on success, -1 on failure.
 */
int r8e_array_splice(R8EContext *ctx, R8EArray *arr,
                      uint32_t start, uint32_t delete_count,
                      const R8EValue *items, uint32_t insert_count,
                      R8EValue *out_deleted) {
    /* Clamp start */
    if (start > arr->length)
        start = arr->length;

    /* Clamp delete_count */
    if (delete_count > arr->length - start)
        delete_count = arr->length - start;

    /* Save deleted elements if requested */
    if (out_deleted) {
        for (uint32_t i = 0; i < delete_count; i++)
            out_deleted[i] = r8e_array_get(ctx, arr, start + i);
    }

    int32_t delta = (int32_t)insert_count - (int32_t)delete_count;
    uint32_t new_len = (uint32_t)((int32_t)arr->length + delta);

    if (r8e_array_is_sparse(arr)) {
        /* Sparse splice: delete old, shift, insert new */
        R8ESparseTable *st = arr->sparse;
        R8ESparseTable *new_st = r8e_sparse_new(ctx, st->capacity);
        if (!new_st) return -1;

        /* Copy elements before start */
        for (uint32_t i = 0; i < st->capacity; i++) {
            uint32_t idx = st->entries[i].index;
            if (idx == R8E_SPARSE_EMPTY_KEY) continue;

            if (idx < start) {
                r8e_sparse_set(ctx, new_st, idx, st->entries[i].val);
            } else if (idx >= start + delete_count) {
                /* Shift by delta */
                r8e_sparse_set(ctx, new_st, (uint32_t)((int32_t)idx + delta),
                               st->entries[i].val);
            }
            /* Elements in [start, start+delete_count) are removed */
        }

        /* Insert new elements */
        for (uint32_t i = 0; i < insert_count; i++)
            r8e_sparse_set(ctx, new_st, start + i, items[i]);

        r8e_sparse_destroy(ctx, st);
        arr->sparse = new_st;
        arr->length = new_len;
    } else {
        /* Dense splice */
        if (new_len > arr->capacity) {
            if (r8e_array_grow(ctx, arr, new_len) != 0)
                return -1;
        }

        /* Update dense_count for deleted elements */
        for (uint32_t i = start; i < start + delete_count && i < arr->capacity; i++) {
            if (arr->elements[i] != R8E_UNDEFINED)
                arr->dense_count--;
        }

        /* Shift tail */
        uint32_t tail_start = start + delete_count;
        uint32_t tail_len = (arr->length > tail_start) ? arr->length - tail_start : 0;
        if (tail_len > 0 && delta != 0) {
            memmove(arr->elements + start + insert_count,
                    arr->elements + tail_start,
                    sizeof(R8EValue) * tail_len);
        }

        /* Clear newly vacated slots if shrinking */
        if (delta < 0) {
            for (uint32_t i = new_len; i < arr->length && i < arr->capacity; i++)
                arr->elements[i] = R8E_UNDEFINED;
        }

        /* Insert new elements */
        for (uint32_t i = 0; i < insert_count; i++) {
            arr->elements[start + i] = items[i];
            if (items[i] != R8E_UNDEFINED)
                arr->dense_count++;
        }

        arr->length = new_len;
    }

    return 0;
}

/*
 * r8e_array_slice - Return a new array containing a shallow copy of
 * elements from start to end (exclusive).
 */
R8EArray *r8e_array_slice(R8EContext *ctx, const R8EArray *arr,
                           int32_t start, int32_t end) {
    /* Resolve negative indices */
    int32_t len = (int32_t)arr->length;
    if (start < 0) start = (start + len > 0) ? start + len : 0;
    if (start > len) start = len;
    if (end < 0) end = (end + len > 0) ? end + len : 0;
    if (end > len) end = len;
    if (end < start) end = start;

    uint32_t count = (uint32_t)(end - start);
    R8EArray *result = r8e_array_new(ctx, count);
    if (!result) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        R8EValue val = r8e_array_get(ctx, arr, (uint32_t)start + i);
        r8e_array_set(ctx, result, i, val);
    }

    return result;
}

/*
 * r8e_array_index_of - Find the first occurrence of a value.
 * Returns the index, or -1 if not found.
 * Uses strict equality (===) semantics (bitwise comparison of NaN-boxed values).
 */
int32_t r8e_array_index_of(R8EContext *ctx, const R8EArray *arr,
                            R8EValue search, uint32_t from_index) {
    for (uint32_t i = from_index; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        if (val == search)
            return (int32_t)i;
    }
    return -1;
}

/*
 * r8e_array_last_index_of - Find the last occurrence of a value.
 */
int32_t r8e_array_last_index_of(R8EContext *ctx, const R8EArray *arr,
                                 R8EValue search, int32_t from_index) {
    if (arr->length == 0)
        return -1;

    int32_t start = from_index;
    if (start < 0)
        start = (int32_t)arr->length + start;
    if (start >= (int32_t)arr->length)
        start = (int32_t)arr->length - 1;
    if (start < 0)
        return -1;

    for (int32_t i = start; i >= 0; i--) {
        R8EValue val = r8e_array_get(ctx, arr, (uint32_t)i);
        if (val == search)
            return i;
    }
    return -1;
}

/*
 * r8e_array_includes - Check if a value exists in the array.
 */
bool r8e_array_includes(R8EContext *ctx, const R8EArray *arr,
                         R8EValue search, uint32_t from_index) {
    return r8e_array_index_of(ctx, arr, search, from_index) >= 0;
}

/*
 * r8e_array_at - Access element by index (supports negative indices).
 * Returns R8E_UNDEFINED if out of range.
 */
R8EValue r8e_array_at(R8EContext *ctx, const R8EArray *arr, int32_t index) {
    int32_t len = (int32_t)arr->length;
    if (index < 0) index = index + len;
    if (index < 0 || index >= len)
        return R8E_UNDEFINED;
    return r8e_array_get(ctx, arr, (uint32_t)index);
}

/* =========================================================================
 * SECTION: Callback-based methods
 *
 * These methods accept a C function pointer as the callback since we
 * do not yet have the interpreter to call JS functions. The callback
 * signature matches the JS semantics: (value, index, array) -> result.
 *
 * The "this_val" parameter corresponds to the thisArg in JS.
 * ========================================================================= */

typedef R8EValue (*R8EArrayCallback)(R8EContext *ctx, R8EValue this_val,
                                      R8EValue val, uint32_t index,
                                      R8EArray *arr);

typedef R8EValue (*R8EArrayReduceCallback)(R8EContext *ctx, R8EValue accumulator,
                                            R8EValue val, uint32_t index,
                                            R8EArray *arr);

/*
 * r8e_array_for_each - Call fn for each element.
 */
void r8e_array_for_each(R8EContext *ctx, R8EArray *arr,
                         R8EArrayCallback fn, R8EValue this_val) {
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        /*
         * In JS, forEach skips holes (not-present indices) but visits
         * indices set to undefined. In dense mode, all indices within
         * capacity are "present". In sparse mode, only indices in the
         * sparse table are present.
         */
        bool present;
        if (r8e_array_is_sparse(arr))
            present = r8e_sparse_has(arr->sparse, i);
        else
            present = (i < arr->capacity);

        if (present)
            fn(ctx, this_val, val, i, arr);
    }
}

/*
 * r8e_array_map - Create a new array with results of calling fn on each element.
 */
R8EArray *r8e_array_map(R8EContext *ctx, const R8EArray *arr,
                         R8EArrayCallback fn, R8EValue this_val) {
    R8EArray *result = r8e_array_new(ctx, arr->length);
    if (!result) return NULL;

    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        R8EValue mapped = fn(ctx, this_val, val, i, (R8EArray *)arr);
        r8e_array_set(ctx, result, i, mapped);
    }

    return result;
}

/*
 * r8e_array_filter - Create a new array with elements passing the test.
 */
R8EArray *r8e_array_filter(R8EContext *ctx, const R8EArray *arr,
                            R8EArrayCallback fn, R8EValue this_val) {
    R8EArray *result = r8e_array_new(ctx, 0);
    if (!result) return NULL;

    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        R8EValue test = fn(ctx, this_val, val, i, (R8EArray *)arr);
        /* Truthy: anything other than undefined, null, false, 0, NaN */
        if (test != R8E_UNDEFINED && test != R8E_NULL &&
            test != R8E_FALSE && test != r8e_from_int32(0)) {
            r8e_array_push(ctx, result, val);
        }
    }

    return result;
}

/*
 * r8e_array_reduce - Reduce the array to a single value (left-to-right).
 */
R8EValue r8e_array_reduce(R8EContext *ctx, R8EArray *arr,
                           R8EArrayReduceCallback fn, R8EValue initial,
                           bool has_initial) {
    R8EValue acc;
    uint32_t start_idx;

    if (has_initial) {
        acc = initial;
        start_idx = 0;
    } else {
        if (arr->length == 0)
            return R8E_UNDEFINED;  /* TypeError in real JS */
        acc = r8e_array_get(ctx, arr, 0);
        start_idx = 1;
    }

    for (uint32_t i = start_idx; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        acc = fn(ctx, acc, val, i, arr);
    }

    return acc;
}

/*
 * r8e_array_reduce_right - Reduce right-to-left.
 */
R8EValue r8e_array_reduce_right(R8EContext *ctx, R8EArray *arr,
                                 R8EArrayReduceCallback fn, R8EValue initial,
                                 bool has_initial) {
    R8EValue acc;
    int32_t start_idx;

    if (has_initial) {
        acc = initial;
        start_idx = (int32_t)arr->length - 1;
    } else {
        if (arr->length == 0)
            return R8E_UNDEFINED;
        acc = r8e_array_get(ctx, arr, arr->length - 1);
        start_idx = (int32_t)arr->length - 2;
    }

    for (int32_t i = start_idx; i >= 0; i--) {
        R8EValue val = r8e_array_get(ctx, arr, (uint32_t)i);
        acc = fn(ctx, acc, val, (uint32_t)i, arr);
    }

    return acc;
}

/*
 * r8e_array_find - Return the first element for which fn returns truthy.
 */
R8EValue r8e_array_find(R8EContext *ctx, const R8EArray *arr,
                         R8EArrayCallback fn, R8EValue this_val) {
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        R8EValue test = fn(ctx, this_val, val, i, (R8EArray *)arr);
        if (test != R8E_UNDEFINED && test != R8E_NULL &&
            test != R8E_FALSE && test != r8e_from_int32(0))
            return val;
    }
    return R8E_UNDEFINED;
}

/*
 * r8e_array_find_index - Return the index of the first element for which
 * fn returns truthy. Returns -1 if not found.
 */
int32_t r8e_array_find_index(R8EContext *ctx, const R8EArray *arr,
                              R8EArrayCallback fn, R8EValue this_val) {
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        R8EValue test = fn(ctx, this_val, val, i, (R8EArray *)arr);
        if (test != R8E_UNDEFINED && test != R8E_NULL &&
            test != R8E_FALSE && test != r8e_from_int32(0))
            return (int32_t)i;
    }
    return -1;
}

/*
 * r8e_array_find_last - Return the last element for which fn returns truthy.
 */
R8EValue r8e_array_find_last(R8EContext *ctx, const R8EArray *arr,
                              R8EArrayCallback fn, R8EValue this_val) {
    if (arr->length == 0)
        return R8E_UNDEFINED;

    for (int32_t i = (int32_t)arr->length - 1; i >= 0; i--) {
        R8EValue val = r8e_array_get(ctx, arr, (uint32_t)i);
        R8EValue test = fn(ctx, this_val, val, (uint32_t)i, (R8EArray *)arr);
        if (test != R8E_UNDEFINED && test != R8E_NULL &&
            test != R8E_FALSE && test != r8e_from_int32(0))
            return val;
    }
    return R8E_UNDEFINED;
}

/*
 * r8e_array_find_last_index - Return the index of the last element for which
 * fn returns truthy.
 */
int32_t r8e_array_find_last_index(R8EContext *ctx, const R8EArray *arr,
                                   R8EArrayCallback fn, R8EValue this_val) {
    if (arr->length == 0)
        return -1;

    for (int32_t i = (int32_t)arr->length - 1; i >= 0; i--) {
        R8EValue val = r8e_array_get(ctx, arr, (uint32_t)i);
        R8EValue test = fn(ctx, this_val, val, (uint32_t)i, (R8EArray *)arr);
        if (test != R8E_UNDEFINED && test != R8E_NULL &&
            test != R8E_FALSE && test != r8e_from_int32(0))
            return i;
    }
    return -1;
}

/*
 * r8e_array_some - Return true if fn returns truthy for at least one element.
 */
bool r8e_array_some(R8EContext *ctx, const R8EArray *arr,
                     R8EArrayCallback fn, R8EValue this_val) {
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        R8EValue test = fn(ctx, this_val, val, i, (R8EArray *)arr);
        if (test != R8E_UNDEFINED && test != R8E_NULL &&
            test != R8E_FALSE && test != r8e_from_int32(0))
            return true;
    }
    return false;
}

/*
 * r8e_array_every - Return true if fn returns truthy for ALL elements.
 */
bool r8e_array_every(R8EContext *ctx, const R8EArray *arr,
                      R8EArrayCallback fn, R8EValue this_val) {
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        R8EValue test = fn(ctx, this_val, val, i, (R8EArray *)arr);
        if (test == R8E_UNDEFINED || test == R8E_NULL ||
            test == R8E_FALSE || test == r8e_from_int32(0))
            return false;
    }
    return true;
}

/*
 * r8e_array_flat - Flatten the array by one level.
 * depth specifies how many levels to flatten.
 *
 * Since we do not have a type system to distinguish nested arrays from
 * plain values at this level, this function checks if a value is a pointer
 * (i.e., could be an array object). The caller must provide a function
 * to check if a value is an array and to get sub-elements.
 *
 * For the simple case (depth=1, values are R8EArray pointers), this works
 * with our NaN-boxing: R8E_IS_POINTER(val) and cast.
 */
R8EArray *r8e_array_flat(R8EContext *ctx, const R8EArray *arr, uint32_t depth) {
    R8EArray *result = r8e_array_new(ctx, arr->length);
    if (!result) return NULL;

    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);

        if (depth > 0 && R8E_IS_POINTER(val)) {
            void *ptr = r8e_get_pointer(val);
            /* Check IS_ARRAY flag on the pointed-to object */
            R8EObjHeader *hdr = (R8EObjHeader *)ptr;
            if (hdr->flags & R8E_OBJ_IS_ARRAY) {
                R8EArray *sub = (R8EArray *)ptr;
                R8EArray *flat_sub = (depth > 1)
                    ? r8e_array_flat(ctx, sub, depth - 1) : NULL;
                const R8EArray *src = flat_sub ? flat_sub : sub;

                for (uint32_t j = 0; j < src->length; j++) {
                    R8EValue sv = r8e_array_get(ctx, src, j);
                    r8e_array_push(ctx, result, sv);
                }

                if (flat_sub)
                    r8e_array_destroy(ctx, flat_sub);
                continue;
            }
        }
        r8e_array_push(ctx, result, val);
    }

    return result;
}

/*
 * r8e_array_flat_map - Map each element using fn, then flatten one level.
 */
R8EArray *r8e_array_flat_map(R8EContext *ctx, const R8EArray *arr,
                              R8EArrayCallback fn, R8EValue this_val) {
    R8EArray *mapped = r8e_array_map(ctx, arr, fn, this_val);
    if (!mapped) return NULL;

    R8EArray *result = r8e_array_flat(ctx, mapped, 1);
    r8e_array_destroy(ctx, mapped);
    return result;
}

/* =========================================================================
 * SECTION: In-place mutation methods
 * ========================================================================= */

/*
 * r8e_array_fill - Fill elements with a static value from start to end.
 */
void r8e_array_fill(R8EContext *ctx, R8EArray *arr, R8EValue val,
                     uint32_t start, uint32_t end) {
    if (start >= arr->length) return;
    if (end > arr->length) end = arr->length;
    if (end <= start) return;

    for (uint32_t i = start; i < end; i++)
        r8e_array_set(ctx, arr, i, val);
}

/*
 * r8e_array_copy_within - Copy elements within the array.
 * target: index to copy to
 * start: index to copy from
 * end: index to copy to (exclusive)
 */
void r8e_array_copy_within(R8EContext *ctx, R8EArray *arr,
                            int32_t target, int32_t start, int32_t end) {
    int32_t len = (int32_t)arr->length;

    /* Resolve negative indices */
    if (target < 0) target = (target + len > 0) ? target + len : 0;
    if (target >= len) return;
    if (start < 0) start = (start + len > 0) ? start + len : 0;
    if (start >= len) return;
    if (end < 0) end = (end + len > 0) ? end + len : 0;
    if (end > len) end = len;
    if (end <= start) return;

    int32_t count = end - start;
    if (target + count > len) count = len - target;

    if (!r8e_array_is_sparse(arr) && (uint32_t)(start + count) <= arr->capacity
        && (uint32_t)(target + count) <= arr->capacity) {
        /* Dense fast path: use memmove for overlapping regions */
        memmove(arr->elements + target, arr->elements + start,
                sizeof(R8EValue) * (uint32_t)count);
    } else {
        /* Generic path */
        if (target < start) {
            for (int32_t i = 0; i < count; i++) {
                R8EValue val = r8e_array_get(ctx, arr, (uint32_t)(start + i));
                r8e_array_set(ctx, arr, (uint32_t)(target + i), val);
            }
        } else {
            for (int32_t i = count - 1; i >= 0; i--) {
                R8EValue val = r8e_array_get(ctx, arr, (uint32_t)(start + i));
                r8e_array_set(ctx, arr, (uint32_t)(target + i), val);
            }
        }
    }
}

/*
 * r8e_array_reverse - Reverse the array in place.
 */
void r8e_array_reverse(R8EContext *ctx, R8EArray *arr) {
    if (arr->length <= 1) return;

    uint32_t lo = 0;
    uint32_t hi = arr->length - 1;

    while (lo < hi) {
        R8EValue a = r8e_array_get(ctx, arr, lo);
        R8EValue b = r8e_array_get(ctx, arr, hi);
        r8e_array_set(ctx, arr, lo, b);
        r8e_array_set(ctx, arr, hi, a);
        lo++;
        hi--;
    }
}

/*
 * r8e_array_sort - Sort the array in place using a comparison function.
 *
 * The compare function returns:
 *   < 0 if a < b
 *   0   if a == b
 *   > 0 if a > b
 *
 * Uses insertion sort for small arrays, quicksort for larger ones.
 */
typedef int (*R8EArrayCompareFn)(R8EContext *ctx, R8EValue a, R8EValue b,
                                  void *user_data);

/* Internal: insertion sort for small sub-arrays */
static void r8e_array_insertion_sort(R8EContext *ctx, R8EArray *arr,
                                      uint32_t lo, uint32_t hi,
                                      R8EArrayCompareFn cmp, void *ud) {
    for (uint32_t i = lo + 1; i <= hi; i++) {
        R8EValue key = r8e_array_get(ctx, arr, i);
        int32_t j = (int32_t)i - 1;
        while (j >= (int32_t)lo) {
            R8EValue jv = r8e_array_get(ctx, arr, (uint32_t)j);
            if (cmp(ctx, jv, key, ud) <= 0)
                break;
            r8e_array_set(ctx, arr, (uint32_t)(j + 1), jv);
            j--;
        }
        r8e_array_set(ctx, arr, (uint32_t)(j + 1), key);
    }
}

/* Internal: partition for quicksort */
static uint32_t r8e_array_partition(R8EContext *ctx, R8EArray *arr,
                                     uint32_t lo, uint32_t hi,
                                     R8EArrayCompareFn cmp, void *ud) {
    /* Median-of-three pivot selection */
    uint32_t mid = lo + (hi - lo) / 2;
    R8EValue vlo = r8e_array_get(ctx, arr, lo);
    R8EValue vmid = r8e_array_get(ctx, arr, mid);
    R8EValue vhi = r8e_array_get(ctx, arr, hi);

    /* Sort lo, mid, hi and use mid as pivot */
    if (cmp(ctx, vlo, vmid, ud) > 0) {
        r8e_array_set(ctx, arr, lo, vmid);
        r8e_array_set(ctx, arr, mid, vlo);
        R8EValue tmp = vlo; vlo = vmid; vmid = tmp;
    }
    if (cmp(ctx, vmid, vhi, ud) > 0) {
        r8e_array_set(ctx, arr, mid, vhi);
        r8e_array_set(ctx, arr, hi, vmid);
        vmid = vhi; /* pivot = former vhi */
        if (cmp(ctx, vlo, vmid, ud) > 0) {
            r8e_array_set(ctx, arr, lo, vmid);
            r8e_array_set(ctx, arr, mid, vlo);
            vmid = vlo;
        }
    }

    /* Use vmid (at index mid) as pivot */
    R8EValue pivot = r8e_array_get(ctx, arr, mid);
    /* Move pivot to hi-1 */
    R8EValue tmp = r8e_array_get(ctx, arr, hi - 1);
    r8e_array_set(ctx, arr, hi - 1, pivot);
    r8e_array_set(ctx, arr, mid, tmp);

    uint32_t i = lo;
    uint32_t j = hi - 1;

    for (;;) {
        do { i++; } while (i < j && cmp(ctx, r8e_array_get(ctx, arr, i), pivot, ud) < 0);
        do { if (j == 0) break; j--; } while (j > i && cmp(ctx, r8e_array_get(ctx, arr, j), pivot, ud) > 0);
        if (i >= j) break;
        R8EValue vi = r8e_array_get(ctx, arr, i);
        R8EValue vj = r8e_array_get(ctx, arr, j);
        r8e_array_set(ctx, arr, i, vj);
        r8e_array_set(ctx, arr, j, vi);
    }

    /* Move pivot to final position */
    R8EValue vi = r8e_array_get(ctx, arr, i);
    r8e_array_set(ctx, arr, i, r8e_array_get(ctx, arr, hi - 1));
    r8e_array_set(ctx, arr, hi - 1, vi);

    return i;
}

/* Internal: quicksort with insertion sort fallback */
static void r8e_array_qsort(R8EContext *ctx, R8EArray *arr,
                              uint32_t lo, uint32_t hi,
                              R8EArrayCompareFn cmp, void *ud) {
    if (hi <= lo) return;
    if (hi - lo < 16) {
        r8e_array_insertion_sort(ctx, arr, lo, hi, cmp, ud);
        return;
    }
    uint32_t pivot = r8e_array_partition(ctx, arr, lo, hi, cmp, ud);
    if (pivot > 0)
        r8e_array_qsort(ctx, arr, lo, pivot - 1, cmp, ud);
    r8e_array_qsort(ctx, arr, pivot + 1, hi, cmp, ud);
}

void r8e_array_sort(R8EContext *ctx, R8EArray *arr,
                     R8EArrayCompareFn cmp, void *user_data) {
    if (arr->length <= 1) return;

    /* Convert sparse to dense for sorting */
    if (r8e_array_is_sparse(arr)) {
        if (r8e_array_to_dense(ctx, arr) != 0)
            return;
    }

    r8e_array_qsort(ctx, arr, 0, arr->length - 1, cmp, user_data);
}

/* =========================================================================
 * SECTION: String-like methods (join, toString)
 * ========================================================================= */

/*
 * r8e_array_join - Join array elements into a string.
 *
 * Since we don't have the string engine yet, this writes to a caller-provided
 * buffer. Each element is converted via to_string_fn callback.
 *
 * Returns the number of bytes written (excluding null terminator), or -1 on
 * truncation.
 */
typedef int (*R8EToStringFn)(R8EContext *ctx, R8EValue val,
                              char *buf, size_t buf_size);

int r8e_array_join(R8EContext *ctx, const R8EArray *arr,
                    const char *separator, R8EToStringFn to_str,
                    char *out_buf, size_t buf_size) {
    if (!separator) separator = ",";
    size_t sep_len = strlen(separator);
    size_t pos = 0;

    for (uint32_t i = 0; i < arr->length; i++) {
        /* Add separator between elements */
        if (i > 0 && sep_len > 0) {
            if (pos + sep_len >= buf_size) return -1;
            memcpy(out_buf + pos, separator, sep_len);
            pos += sep_len;
        }

        R8EValue val = r8e_array_get(ctx, arr, i);

        /* undefined and null produce empty string in join */
        if (val == R8E_UNDEFINED || val == R8E_NULL)
            continue;

        int written = to_str(ctx, val, out_buf + pos, buf_size - pos);
        if (written < 0) return -1;
        pos += (size_t)written;
    }

    if (pos < buf_size)
        out_buf[pos] = '\0';
    return (int)pos;
}

/* =========================================================================
 * SECTION: Array.concat
 * ========================================================================= */

/*
 * r8e_array_concat - Concatenate multiple arrays into a new array.
 * sources: array of R8EArray pointers
 * count: number of source arrays
 */
R8EArray *r8e_array_concat(R8EContext *ctx, const R8EArray *arr,
                            const R8EArray **sources, uint32_t count) {
    /* Calculate total length */
    uint32_t total = arr->length;
    for (uint32_t i = 0; i < count; i++)
        total += sources[i]->length;

    R8EArray *result = r8e_array_new(ctx, total);
    if (!result) return NULL;

    /* Copy from original */
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue val = r8e_array_get(ctx, arr, i);
        r8e_array_set(ctx, result, i, val);
    }

    /* Copy from each source */
    uint32_t offset = arr->length;
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t i = 0; i < sources[s]->length; i++) {
            R8EValue val = r8e_array_get(ctx, sources[s], i);
            r8e_array_set(ctx, result, offset + i, val);
        }
        offset += sources[s]->length;
    }

    return result;
}

/* =========================================================================
 * SECTION: Static Array methods
 * ========================================================================= */

/*
 * r8e_array_is_array - Check if a value is an array.
 * Checks the IS_ARRAY flag in the object header.
 */
bool r8e_array_is_array(R8EValue val) {
    if (!R8E_IS_POINTER(val))
        return false;
    void *ptr = r8e_get_pointer(val);
    R8EObjHeader *h = (R8EObjHeader *)ptr;
    return (h->flags & R8E_OBJ_IS_ARRAY) != 0;
}

/*
 * r8e_array_from - Create an array from a set of values.
 * This is the simple form (no mapFn).
 */
R8EArray *r8e_array_from(R8EContext *ctx, const R8EValue *values,
                          uint32_t count) {
    R8EArray *arr = r8e_array_new(ctx, count);
    if (!arr) return NULL;

    for (uint32_t i = 0; i < count; i++)
        r8e_array_set(ctx, arr, i, values[i]);

    return arr;
}

/*
 * r8e_array_of - Create an array from variadic-style arguments.
 * Same as r8e_array_from but named to match Array.of().
 */
R8EArray *r8e_array_of(R8EContext *ctx, const R8EValue *values,
                        uint32_t count) {
    return r8e_array_from(ctx, values, count);
}

/* =========================================================================
 * SECTION: Named property access (arr.customProp)
 *
 * Arrays can have named (non-index) properties like any JS object.
 * These are stored in a lazy-allocated R8EObject (CDOL Tier 0/1).
 * Only allocated on first use (most arrays never have named properties).
 * ========================================================================= */

/*
 * r8e_array_get_named - Get a named property from the array.
 */
R8EValue r8e_array_get_named(R8EContext *ctx, R8EArray *arr, uint32_t key) {
    if (!arr->named)
        return R8E_UNDEFINED;
    return r8e_obj_get(ctx, arr->named, key);
}

/*
 * r8e_array_set_named - Set a named property on the array.
 * Lazily allocates the named property object if needed.
 * Returns 0 on success, -1 on failure.
 */
int r8e_array_set_named(R8EContext *ctx, R8EArray *arr,
                         uint32_t key, R8EValue val) {
    if (!arr->named) {
        arr->named = r8e_obj_new(ctx);
        if (!arr->named) return -1;
    }

    void *result = r8e_obj_set(ctx, arr->named, key, val);
    if (!result) return -1;
    arr->named = result;
    return 0;
}

/*
 * r8e_array_has_named - Check if the array has a named property.
 */
bool r8e_array_has_named(R8EContext *ctx, R8EArray *arr, uint32_t key) {
    if (!arr->named)
        return false;
    return r8e_obj_has(ctx, arr->named, key);
}

/*
 * r8e_array_delete_named - Delete a named property.
 */
int r8e_array_delete_named(R8EContext *ctx, R8EArray *arr, uint32_t key) {
    if (!arr->named)
        return 0;
    return r8e_obj_delete(ctx, arr->named, key);
}
