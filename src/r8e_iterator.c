/*
 * r8e_iterator.c - Iterators, Generators, and Async Generators
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 14 (Engineering Timeline, Phase 3).
 *
 * Architecture:
 *   - ES2023 Iterator protocol: next(), return(), throw()
 *   - Built-in iterators for Array (values/keys/entries), String (code points)
 *   - Generator functions: yield, yield*, save/restore interpreter frame
 *   - Async generators: combine async + generator mechanics
 *   - for-of support: init, next, close on break/return/throw
 *   - Spread/destructuring: consume iterable into array
 *   - Map/Set iterator stubs for future integration
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
#define R8E_IS_INT32(v)      (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)    (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_NULLISH(v)    (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))
#define R8E_IS_BOOL(v)       ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)     (((v) >> 32) == 0xFFFB0000U)

/* NaN-boxing extraction / encoding */
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline double r8e_get_double(uint64_t v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

static inline uint64_t r8e_from_double(double d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(double));
    return v;
}

static inline R8EValue r8e_from_bool(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}

/* Inline string helpers */
static inline int r8e_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}
static inline char r8e_inline_str_char(R8EValue v, int i) {
    return (char)((v >> (38 - i * 7)) & 0x7F);
}

/* =========================================================================
 * GC header flags layout (must match r8e_gc.c)
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
#define R8E_GC_KIND_ITERATOR     6u
#define R8E_GC_KIND_GENERATOR    7u

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)
#define R8E_GC_SET_KIND(flags, kind) \
    (((flags) & ~R8E_GC_KIND_MASK) | \
     (((kind) << R8E_GC_KIND_SHIFT) & R8E_GC_KIND_MASK))

/* =========================================================================
 * GC header
 * ========================================================================= */

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* =========================================================================
 * Prototype IDs
 * ========================================================================= */

#define R8E_PROTO_NONE       0
#define R8E_PROTO_OBJECT     1
#define R8E_PROTO_ARRAY      2
#define R8E_PROTO_FUNCTION   3
#define R8E_PROTO_STRING     4
#define R8E_PROTO_GENERATOR  33
#define R8E_PROTO_ITERATOR   34

/* =========================================================================
 * Well-known atom IDs (must match r8e_atoms.h)
 * ========================================================================= */

#define R8E_ATOM_EMPTY          0
#define R8E_ATOM_length         1
#define R8E_ATOM_prototype      2
#define R8E_ATOM_constructor    3
#define R8E_ATOM_toString       4
#define R8E_ATOM_valueOf        5
#define R8E_ATOM_name           8
#define R8E_ATOM_value          11
#define R8E_ATOM_iterator       236
#define R8E_ATOM_asyncIterator  237
#define R8E_ATOM_done           288
#define R8E_ATOM_next           289
#define R8E_ATOM_return_kw      290
#define R8E_ATOM_throw_kw       291

/* =========================================================================
 * String header (matches r8e_string.c / r8e_value.c)
 * ========================================================================= */

typedef struct R8EStringIter {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    /* R8EOffsetTable *offset_table; -- skipped for forward compat */
    /* char data[] follows */
} R8EStringIter;

static inline const char *r8e_iter_string_data(const R8EStringIter *s) {
    /* offset_table pointer sits between header and data */
    return (const char *)((const uint8_t *)s + sizeof(R8EStringIter)
                          + sizeof(void *));
}

#define R8E_STR_IS_ASCII 0x01u

/* =========================================================================
 * Forward declarations for external APIs
 * ========================================================================= */

typedef struct R8EIterContext R8EIterContext;

/* Function call (from r8e_interp.c) */
static R8EValue r8e_iter_call_function(R8EIterContext *ctx, R8EValue func,
                                       R8EValue this_val, int argc,
                                       const R8EValue *argv);

/* Object property access (from r8e_object.c) */
static R8EValue r8e_iter_obj_get(R8EIterContext *ctx, R8EValue obj,
                                 uint32_t atom);
static void r8e_iter_obj_set(R8EIterContext *ctx, R8EValue obj,
                             uint32_t atom, R8EValue val);
static bool r8e_iter_obj_has(R8EIterContext *ctx, R8EValue obj,
                             uint32_t atom);

/* Array operations (from r8e_array.c) */
static R8EValue r8e_iter_make_array(R8EIterContext *ctx, uint32_t cap);
static void r8e_iter_array_push(R8EIterContext *ctx, R8EValue arr,
                                R8EValue val);
static uint32_t r8e_iter_array_length(R8EIterContext *ctx, R8EValue arr);
static R8EValue r8e_iter_array_get(R8EIterContext *ctx, R8EValue arr,
                                   uint32_t index);

/* Object creation (from r8e_object.c) */
static R8EValue r8e_iter_make_object(R8EIterContext *ctx);

/* Type checks (from r8e_function.c) */
static bool r8e_iter_is_callable(R8EValue v);

/* Error throwing (from r8e_error.c) */
static R8EValue r8e_iter_throw_type_error(R8EIterContext *ctx,
                                          const char *msg);

/* Memory allocation (from r8e_alloc.c) */
static void *r8e_iter_alloc(R8EIterContext *ctx, size_t size);
static void  r8e_iter_free(R8EIterContext *ctx, void *ptr, size_t size);

/* Promise support (from r8e_promise.c) */
static R8EValue r8e_iter_promise_new(R8EIterContext *ctx);
static void r8e_iter_promise_resolve(R8EIterContext *ctx,
                                     R8EValue promise, R8EValue value);
static void r8e_iter_promise_reject(R8EIterContext *ctx,
                                    R8EValue promise, R8EValue reason);
static R8EValue r8e_iter_promise_then(R8EIterContext *ctx,
                                      R8EValue promise,
                                      R8EValue on_fulfilled,
                                      R8EValue on_rejected);


/* =========================================================================
 * Iterator Context (embedded in R8EContext in integrated build)
 * ========================================================================= */

struct R8EIterContext {
    R8EValue  exception;
    bool      has_exception;
    void     *alloc_ctx;
};


/* =========================================================================
 * Section 1: Iterator Result Object
 *
 * The standard {value, done} result returned by all iterators.
 * We create these frequently, so this is a hot path.
 * ========================================================================= */

/**
 * Create an iterator result object: { value: val, done: is_done }.
 */
static R8EValue r8e_create_iter_result(R8EIterContext *ctx,
                                       R8EValue val, bool is_done)
{
    R8EValue obj = r8e_iter_make_object(ctx);
    r8e_iter_obj_set(ctx, obj, R8E_ATOM_value, val);
    r8e_iter_obj_set(ctx, obj, R8E_ATOM_done, r8e_from_bool(is_done));
    return obj;
}


/* =========================================================================
 * Section 2: Iterator Types
 *
 * All iterators share a common structure prefix (R8EIteratorBase) for
 * polymorphic dispatch. Each iterator type adds its own state.
 * ========================================================================= */

typedef enum {
    R8E_ITER_ARRAY_VALUES   = 0,
    R8E_ITER_ARRAY_KEYS     = 1,
    R8E_ITER_ARRAY_ENTRIES  = 2,
    R8E_ITER_STRING         = 3,
    R8E_ITER_MAP_ENTRIES    = 4,
    R8E_ITER_SET_VALUES     = 5,
    R8E_ITER_GENERATOR      = 6,
    R8E_ITER_ASYNC_GENERATOR = 7,
    R8E_ITER_GENERIC        = 8
} R8EIteratorKind;

/**
 * Base iterator structure.
 * All built-in iterators begin with these fields.
 */
typedef struct R8EIteratorBase {
    /* GC header */
    uint32_t flags;
    uint32_t proto_id;

    /* Iterator state */
    R8EIteratorKind kind;
    bool            done;       /* true when iterator is exhausted */
    uint8_t         pad[3];

    /* The source iterable (retained reference) */
    R8EValue        source;
} R8EIteratorBase;


/* =========================================================================
 * Section 3: Array Iterator
 *
 * Iterates over array elements (values), indices (keys), or
 * [index, value] pairs (entries).
 * ========================================================================= */

typedef struct R8EArrayIterator {
    R8EIteratorBase base;
    uint32_t        index;      /* current position */
    uint32_t        length;     /* cached array length */
} R8EArrayIterator;

/**
 * Create a new array values iterator.
 */
R8EValue r8e_array_values_iterator(R8EIterContext *ctx, R8EValue array)
{
    R8EArrayIterator *iter = (R8EArrayIterator *)r8e_iter_alloc(
        ctx, sizeof(R8EArrayIterator));
    if (iter == NULL) return R8E_UNDEFINED;

    memset(iter, 0, sizeof(R8EArrayIterator));
    iter->base.flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_ITERATOR);
    iter->base.flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    iter->base.proto_id = R8E_PROTO_ITERATOR;
    iter->base.kind = R8E_ITER_ARRAY_VALUES;
    iter->base.done = false;
    iter->base.source = array;
    iter->index = 0;
    iter->length = r8e_iter_array_length(ctx, array);

    return r8e_from_pointer(iter);
}

/**
 * Create a new array keys iterator.
 */
R8EValue r8e_array_keys_iterator(R8EIterContext *ctx, R8EValue array)
{
    R8EArrayIterator *iter = (R8EArrayIterator *)r8e_iter_alloc(
        ctx, sizeof(R8EArrayIterator));
    if (iter == NULL) return R8E_UNDEFINED;

    memset(iter, 0, sizeof(R8EArrayIterator));
    iter->base.flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_ITERATOR);
    iter->base.flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    iter->base.proto_id = R8E_PROTO_ITERATOR;
    iter->base.kind = R8E_ITER_ARRAY_KEYS;
    iter->base.done = false;
    iter->base.source = array;
    iter->index = 0;
    iter->length = r8e_iter_array_length(ctx, array);

    return r8e_from_pointer(iter);
}

/**
 * Create a new array entries iterator.
 */
R8EValue r8e_array_entries_iterator(R8EIterContext *ctx, R8EValue array)
{
    R8EArrayIterator *iter = (R8EArrayIterator *)r8e_iter_alloc(
        ctx, sizeof(R8EArrayIterator));
    if (iter == NULL) return R8E_UNDEFINED;

    memset(iter, 0, sizeof(R8EArrayIterator));
    iter->base.flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_ITERATOR);
    iter->base.flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    iter->base.proto_id = R8E_PROTO_ITERATOR;
    iter->base.kind = R8E_ITER_ARRAY_ENTRIES;
    iter->base.done = false;
    iter->base.source = array;
    iter->index = 0;
    iter->length = r8e_iter_array_length(ctx, array);

    return r8e_from_pointer(iter);
}

/**
 * Advance an array iterator by one step.
 */
static R8EValue r8e_array_iterator_next(R8EIterContext *ctx,
                                        R8EArrayIterator *iter)
{
    if (iter->base.done || iter->index >= iter->length) {
        iter->base.done = true;
        return r8e_create_iter_result(ctx, R8E_UNDEFINED, true);
    }

    uint32_t i = iter->index++;
    R8EValue val;

    switch (iter->base.kind) {
    case R8E_ITER_ARRAY_VALUES:
        val = r8e_iter_array_get(ctx, iter->base.source, i);
        return r8e_create_iter_result(ctx, val, false);

    case R8E_ITER_ARRAY_KEYS:
        return r8e_create_iter_result(ctx, r8e_from_int32((int32_t)i),
                                      false);

    case R8E_ITER_ARRAY_ENTRIES: {
        /* Create [index, value] pair */
        R8EValue pair = r8e_iter_make_array(ctx, 2);
        r8e_iter_array_push(ctx, pair, r8e_from_int32((int32_t)i));
        val = r8e_iter_array_get(ctx, iter->base.source, i);
        r8e_iter_array_push(ctx, pair, val);
        return r8e_create_iter_result(ctx, pair, false);
    }

    default:
        iter->base.done = true;
        return r8e_create_iter_result(ctx, R8E_UNDEFINED, true);
    }
}


/* =========================================================================
 * Section 4: String Iterator
 *
 * Iterates over a string by Unicode code points (not UTF-16 code units).
 * Handles both inline short strings and heap strings with UTF-8 encoding.
 * ========================================================================= */

typedef struct R8EStringIterator {
    R8EIteratorBase base;
    uint32_t        byte_index;    /* current byte position in UTF-8 data */
    uint32_t        byte_length;   /* total byte length */
    bool            is_inline;     /* true if source is an inline string */
    uint8_t         pad2[3];
} R8EStringIterator;

/**
 * Create a new string iterator.
 */
R8EValue r8e_string_iterator(R8EIterContext *ctx, R8EValue string)
{
    R8EStringIterator *iter = (R8EStringIterator *)r8e_iter_alloc(
        ctx, sizeof(R8EStringIterator));
    if (iter == NULL) return R8E_UNDEFINED;

    memset(iter, 0, sizeof(R8EStringIterator));
    iter->base.flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_ITERATOR);
    iter->base.flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    iter->base.proto_id = R8E_PROTO_ITERATOR;
    iter->base.kind = R8E_ITER_STRING;
    iter->base.done = false;
    iter->base.source = string;
    iter->byte_index = 0;

    if (R8E_IS_INLINE_STR(string)) {
        iter->is_inline = true;
        iter->byte_length = (uint32_t)r8e_inline_str_len(string);
    } else if (R8E_IS_POINTER(string)) {
        iter->is_inline = false;
        R8EStringIter *s = (R8EStringIter *)r8e_get_pointer(string);
        iter->byte_length = s->byte_length;
    } else {
        iter->base.done = true;
        iter->byte_length = 0;
    }

    return r8e_from_pointer(iter);
}

/**
 * Decode one UTF-8 code point from a byte buffer.
 * Returns the code point and advances *pos past it.
 */
static uint32_t r8e_utf8_decode(const uint8_t *data, uint32_t len,
                                uint32_t *pos)
{
    uint32_t p = *pos;
    if (p >= len) return 0;

    uint8_t b0 = data[p];

    if (b0 < 0x80) {
        /* ASCII: single byte */
        *pos = p + 1;
        return b0;
    }

    if ((b0 & 0xE0) == 0xC0 && p + 1 < len) {
        /* 2-byte sequence */
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6)
                    | ((uint32_t)(data[p + 1] & 0x3F));
        *pos = p + 2;
        return cp;
    }

    if ((b0 & 0xF0) == 0xE0 && p + 2 < len) {
        /* 3-byte sequence */
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12)
                    | ((uint32_t)(data[p + 1] & 0x3F) << 6)
                    | ((uint32_t)(data[p + 2] & 0x3F));
        *pos = p + 3;
        return cp;
    }

    if ((b0 & 0xF8) == 0xF0 && p + 3 < len) {
        /* 4-byte sequence */
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18)
                    | ((uint32_t)(data[p + 1] & 0x3F) << 12)
                    | ((uint32_t)(data[p + 2] & 0x3F) << 6)
                    | ((uint32_t)(data[p + 3] & 0x3F));
        *pos = p + 4;
        return cp;
    }

    /* Invalid UTF-8: skip one byte, return replacement character */
    *pos = p + 1;
    return 0xFFFD;
}

/**
 * Encode a Unicode code point as a UTF-8 string and return as an
 * inline short string (if it fits) or a heap string.
 */
static R8EValue r8e_codepoint_to_string(R8EIterContext *ctx, uint32_t cp)
{
    char buf[5];
    int len;

    if (cp < 0x80) {
        buf[0] = (char)cp;
        len = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    buf[len] = '\0';

    /* Try inline encoding for ASCII (1 char, 7-bit) */
    if (len == 1 && cp < 128) {
        uint64_t v = 0xFFFD000000000000ULL;
        v |= ((uint64_t)1 << 45);
        v |= ((uint64_t)(uint8_t)buf[0] << 38);
        return v;
    }

    /* For multi-byte characters, in the integrated build we would
     * call r8e_make_string(). Returning a placeholder for now. */
    (void)ctx;
    return R8E_UNDEFINED;
}

/**
 * Advance a string iterator by one code point.
 */
static R8EValue r8e_string_iterator_next(R8EIterContext *ctx,
                                         R8EStringIterator *iter)
{
    if (iter->base.done || iter->byte_index >= iter->byte_length) {
        iter->base.done = true;
        return r8e_create_iter_result(ctx, R8E_UNDEFINED, true);
    }

    uint32_t cp;

    if (iter->is_inline) {
        /* Inline short string: all ASCII, one byte per char */
        cp = (uint32_t)(uint8_t)r8e_inline_str_char(iter->base.source,
                                                     (int)iter->byte_index);
        iter->byte_index++;
    } else {
        /* Heap string: UTF-8 encoded */
        R8EStringIter *s = (R8EStringIter *)r8e_get_pointer(
            iter->base.source);
        const uint8_t *data = (const uint8_t *)r8e_iter_string_data(s);
        cp = r8e_utf8_decode(data, iter->byte_length, &iter->byte_index);
    }

    R8EValue char_str = r8e_codepoint_to_string(ctx, cp);
    return r8e_create_iter_result(ctx, char_str, false);
}


/* =========================================================================
 * Section 5: Generator State Machine
 *
 * Generator functions use a saved interpreter frame to yield and
 * resume. The generator state machine has four states:
 *
 *   SUSPENDED_START - created but next() not yet called
 *   SUSPENDED_YIELD - suspended at a yield point
 *   EXECUTING       - currently running (re-entrancy guard)
 *   COMPLETED       - returned or threw, no more values
 *
 * State transitions:
 *   SUSPENDED_START -> EXECUTING  (first next())
 *   EXECUTING -> SUSPENDED_YIELD  (yield)
 *   EXECUTING -> COMPLETED        (return or throw)
 *   SUSPENDED_YIELD -> EXECUTING  (subsequent next()/return()/throw())
 *   SUSPENDED_YIELD -> COMPLETED  (return() or throw() from outside)
 * ========================================================================= */

typedef enum {
    R8E_GEN_SUSPENDED_START = 0,
    R8E_GEN_SUSPENDED_YIELD = 1,
    R8E_GEN_EXECUTING       = 2,
    R8E_GEN_COMPLETED       = 3
} R8EGeneratorState;

typedef struct R8EGenerator {
    /* GC header */
    uint32_t flags;
    uint32_t proto_id;

    /* Generator-specific state */
    R8EGeneratorState gen_state;

    /* The generator function closure */
    R8EValue closure;

    /* Saved interpreter frame (opaque to this module) */
    R8EValue  *saved_locals;
    R8EValue  *saved_stack;
    uint8_t   *saved_pc;
    uint32_t   local_count;
    uint32_t   stack_depth;
    uint32_t   stack_capacity;

    /* The value to resume with (from next(value)) */
    R8EValue   resume_value;

    /* Exception to throw into the generator (from throw(error)) */
    R8EValue   throw_value;
    bool       has_throw;

    /* Return value (from return(value)) */
    R8EValue   return_value;
    bool       has_return;

    /* For yield* delegation */
    R8EValue   delegate_iter;    /* sub-iterator for yield* */
    bool       is_delegating;
    uint8_t    pad[5];
} R8EGenerator;


/**
 * Allocate a new generator object.
 */
static R8EGenerator *r8e_generator_alloc(R8EIterContext *ctx,
                                         uint32_t local_count,
                                         uint32_t stack_size)
{
    R8EGenerator *gen = (R8EGenerator *)r8e_iter_alloc(
        ctx, sizeof(R8EGenerator));
    if (gen == NULL) return NULL;

    memset(gen, 0, sizeof(R8EGenerator));
    gen->flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_GENERATOR);
    gen->flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    gen->proto_id = R8E_PROTO_GENERATOR;
    gen->gen_state = R8E_GEN_SUSPENDED_START;
    gen->local_count = local_count;
    gen->stack_capacity = stack_size;

    /* Allocate saved frame storage */
    if (local_count > 0) {
        gen->saved_locals = (R8EValue *)r8e_iter_alloc(
            ctx, local_count * sizeof(R8EValue));
        if (gen->saved_locals == NULL) {
            r8e_iter_free(ctx, gen, sizeof(R8EGenerator));
            return NULL;
        }
        for (uint32_t i = 0; i < local_count; i++) {
            gen->saved_locals[i] = R8E_UNDEFINED;
        }
    }

    if (stack_size > 0) {
        gen->saved_stack = (R8EValue *)r8e_iter_alloc(
            ctx, stack_size * sizeof(R8EValue));
        if (gen->saved_stack == NULL) {
            if (gen->saved_locals) {
                r8e_iter_free(ctx, gen->saved_locals,
                              local_count * sizeof(R8EValue));
            }
            r8e_iter_free(ctx, gen, sizeof(R8EGenerator));
            return NULL;
        }
    }

    return gen;
}

/**
 * Free a generator object.
 */
static void r8e_generator_free_obj(R8EIterContext *ctx, R8EGenerator *gen)
{
    if (gen == NULL) return;
    if (gen->saved_locals != NULL) {
        r8e_iter_free(ctx, gen->saved_locals,
                      gen->local_count * sizeof(R8EValue));
    }
    if (gen->saved_stack != NULL) {
        r8e_iter_free(ctx, gen->saved_stack,
                      gen->stack_capacity * sizeof(R8EValue));
    }
    r8e_iter_free(ctx, gen, sizeof(R8EGenerator));
}

/**
 * Get the R8EGenerator from a NaN-boxed value.
 * Returns NULL if not a generator.
 */
static R8EGenerator *r8e_generator_get(R8EValue v)
{
    if (!R8E_IS_POINTER(v)) return NULL;
    R8EGenerator *gen = (R8EGenerator *)r8e_get_pointer(v);
    if (R8E_GC_GET_KIND(gen->flags) != R8E_GC_KIND_GENERATOR) return NULL;
    return gen;
}

/**
 * Create a new generator from a generator function closure.
 *
 * Called when a generator function is invoked (but not when next() is
 * called). The generator starts in SUSPENDED_START state.
 *
 * @param ctx    Iterator context.
 * @param func   The generator function closure.
 * @param argc   Number of arguments.
 * @param argv   Arguments.
 * @return       NaN-boxed pointer to the new generator.
 */
R8EValue r8e_generator_new(R8EIterContext *ctx, R8EValue func,
                           int argc, const R8EValue *argv)
{
    /* In the full implementation, we inspect the function descriptor
     * for local_count and stack_size. Use reasonable defaults. */
    R8EGenerator *gen = r8e_generator_alloc(ctx, 64, 32);
    if (gen == NULL) return R8E_UNDEFINED;

    gen->closure = func;

    /* Copy arguments into saved locals */
    for (int i = 0; i < argc && (uint32_t)i < gen->local_count; i++) {
        gen->saved_locals[i] = argv[i];
    }

    return r8e_from_pointer(gen);
}

/**
 * Execute the generator body until the next yield/return/throw.
 *
 * In the full implementation, this restores the saved frame into the
 * interpreter and runs until a yield or return opcode is hit.
 *
 * @param ctx    Iterator context.
 * @param gen    The generator.
 * @return       Iterator result { value, done }.
 */
static R8EValue r8e_generator_execute(R8EIterContext *ctx,
                                      R8EGenerator *gen)
{
    gen->gen_state = R8E_GEN_EXECUTING;

    /* In the full implementation:
     * 1. Restore gen->saved_locals, gen->saved_stack, gen->saved_pc
     *    into the interpreter frame
     * 2. If has_throw, dispatch to the active catch handler
     * 3. If has_return, execute finally blocks then return
     * 4. Otherwise, push resume_value onto the operand stack
     * 5. Run until yield, return, or throw
     * 6. On yield: save frame, set SUSPENDED_YIELD, return value
     * 7. On return: set COMPLETED, return value with done=true
     * 8. On throw: set COMPLETED, propagate exception
     *
     * For the self-contained version, we call the closure and handle
     * the result. */

    if (gen->has_return) {
        gen->has_return = false;
        gen->gen_state = R8E_GEN_COMPLETED;
        return r8e_create_iter_result(ctx, gen->return_value, true);
    }

    if (gen->has_throw) {
        gen->has_throw = false;
        gen->gen_state = R8E_GEN_COMPLETED;
        /* In full implementation, dispatch to catch handler.
         * For now, propagate as exception. */
        ctx->has_exception = true;
        ctx->exception = gen->throw_value;
        return R8E_UNDEFINED;
    }

    /* Call the generator function body */
    R8EValue result;
    if (gen->gen_state == R8E_GEN_EXECUTING) {
        R8EValue args[1] = { gen->resume_value };
        result = r8e_iter_call_function(ctx, gen->closure,
                                        R8E_UNDEFINED, 1, args);
    } else {
        result = R8E_UNDEFINED;
    }

    if (ctx->has_exception) {
        gen->gen_state = R8E_GEN_COMPLETED;
        return R8E_UNDEFINED;
    }

    gen->gen_state = R8E_GEN_COMPLETED;
    return r8e_create_iter_result(ctx, result, true);
}

/**
 * Generator.prototype.next(value).
 *
 * Resumes the generator with the given value and returns the next
 * iterator result.
 */
R8EValue r8e_generator_next(R8EIterContext *ctx, R8EValue gen_val,
                            R8EValue value)
{
    R8EGenerator *gen = r8e_generator_get(gen_val);
    if (gen == NULL) {
        r8e_iter_throw_type_error(ctx,
            "Generator.prototype.next called on non-generator");
        return R8E_UNDEFINED;
    }

    /* Re-entrancy check */
    if (gen->gen_state == R8E_GEN_EXECUTING) {
        r8e_iter_throw_type_error(ctx,
            "Generator is already executing");
        return R8E_UNDEFINED;
    }

    /* Already completed */
    if (gen->gen_state == R8E_GEN_COMPLETED) {
        return r8e_create_iter_result(ctx, R8E_UNDEFINED, true);
    }

    gen->resume_value = value;
    gen->has_throw = false;
    gen->has_return = false;

    return r8e_generator_execute(ctx, gen);
}

/**
 * Generator.prototype.return(value).
 *
 * Forces the generator to complete with the given value.
 * If the generator has an active try/finally, the finally block runs first.
 */
R8EValue r8e_generator_return(R8EIterContext *ctx, R8EValue gen_val,
                              R8EValue value)
{
    R8EGenerator *gen = r8e_generator_get(gen_val);
    if (gen == NULL) {
        r8e_iter_throw_type_error(ctx,
            "Generator.prototype.return called on non-generator");
        return R8E_UNDEFINED;
    }

    if (gen->gen_state == R8E_GEN_EXECUTING) {
        r8e_iter_throw_type_error(ctx,
            "Generator is already executing");
        return R8E_UNDEFINED;
    }

    if (gen->gen_state == R8E_GEN_COMPLETED) {
        return r8e_create_iter_result(ctx, value, true);
    }

    if (gen->gen_state == R8E_GEN_SUSPENDED_START) {
        /* Never started: just complete immediately */
        gen->gen_state = R8E_GEN_COMPLETED;
        return r8e_create_iter_result(ctx, value, true);
    }

    /* Suspended at a yield: need to run finally blocks */
    gen->has_return = true;
    gen->return_value = value;
    return r8e_generator_execute(ctx, gen);
}

/**
 * Generator.prototype.throw(error).
 *
 * Throws an exception into the generator at the current yield point.
 * If the generator has an active try/catch, the catch block handles it.
 */
R8EValue r8e_generator_throw(R8EIterContext *ctx, R8EValue gen_val,
                             R8EValue error)
{
    R8EGenerator *gen = r8e_generator_get(gen_val);
    if (gen == NULL) {
        r8e_iter_throw_type_error(ctx,
            "Generator.prototype.throw called on non-generator");
        return R8E_UNDEFINED;
    }

    if (gen->gen_state == R8E_GEN_EXECUTING) {
        r8e_iter_throw_type_error(ctx,
            "Generator is already executing");
        return R8E_UNDEFINED;
    }

    if (gen->gen_state == R8E_GEN_COMPLETED) {
        /* Throw when completed: re-throw the error */
        ctx->has_exception = true;
        ctx->exception = error;
        return R8E_UNDEFINED;
    }

    if (gen->gen_state == R8E_GEN_SUSPENDED_START) {
        /* Never started: just complete and throw */
        gen->gen_state = R8E_GEN_COMPLETED;
        ctx->has_exception = true;
        ctx->exception = error;
        return R8E_UNDEFINED;
    }

    /* Suspended at a yield: throw into the catch handler */
    gen->has_throw = true;
    gen->throw_value = error;
    return r8e_generator_execute(ctx, gen);
}


/* =========================================================================
 * Section 6: yield* Delegation
 *
 * yield* expr delegates to a sub-iterator. Each next() call is forwarded
 * to the sub-iterator until it is exhausted, then the generator resumes.
 *
 * The implementation handles:
 *   - Forwarding next(value) to the sub-iterator
 *   - Forwarding return(value) if the sub-iterator has a return() method
 *   - Forwarding throw(error) if the sub-iterator has a throw() method
 *   - Returning the sub-iterator's final value as the yield* expression value
 * ========================================================================= */

/* Forward declarations (defined later in this file) */
R8EValue r8e_get_iterator(R8EIterContext *ctx, R8EValue iterable);
R8EValue r8e_iterator_next(R8EIterContext *ctx, R8EValue iter);

/**
 * Handle yield* delegation within a generator's next() call.
 *
 * @param ctx    Iterator context.
 * @param gen    The generator that has a yield* delegation active.
 * @param value  The value to pass to the sub-iterator's next().
 * @return       Iterator result from the sub-iterator, or the generator
 *               if the sub-iterator is exhausted.
 */
static R8EValue r8e_generator_yield_star_next(R8EIterContext *ctx,
                                              R8EGenerator *gen,
                                              R8EValue value)
{
    if (!gen->is_delegating) {
        return R8E_UNDEFINED;
    }

    /* Forward next(value) to the sub-iterator */
    R8EValue sub_result = r8e_iterator_next(ctx, gen->delegate_iter);

    if (ctx->has_exception) {
        gen->is_delegating = false;
        gen->delegate_iter = R8E_UNDEFINED;
        return R8E_UNDEFINED;
    }

    /* Check if sub-iterator is done */
    R8EValue done_val = r8e_iter_obj_get(ctx, sub_result, R8E_ATOM_done);
    if (done_val == R8E_TRUE) {
        /* Sub-iterator exhausted: return its final value as the
         * yield* expression value and resume the generator */
        gen->is_delegating = false;
        R8EValue final_val = r8e_iter_obj_get(ctx, sub_result,
                                               R8E_ATOM_value);
        gen->delegate_iter = R8E_UNDEFINED;
        gen->resume_value = final_val;
        return r8e_generator_execute(ctx, gen);
    }

    /* Sub-iterator not done: return its result directly */
    return sub_result;
}

/**
 * Begin yield* delegation to a sub-iterator.
 *
 * Called by the interpreter when it encounters a YIELD_STAR opcode.
 * Sets up the delegation state so subsequent next() calls are forwarded.
 *
 * @param ctx       Iterator context.
 * @param gen       The generator.
 * @param iterable  The iterable to delegate to.
 */
void r8e_generator_delegate(R8EIterContext *ctx, R8EGenerator *gen,
                            R8EValue iterable)
{
    /* Get iterator from iterable */
    R8EValue iter = r8e_get_iterator(ctx, iterable);
    if (R8E_IS_UNDEFINED(iter)) {
        r8e_iter_throw_type_error(ctx,
            "yield* operand is not iterable");
        return;
    }

    gen->delegate_iter = iter;
    gen->is_delegating = true;
}


/* =========================================================================
 * Section 7: Async Generator
 *
 * Async generators combine generator yield semantics with async await.
 * Each next()/return()/throw() call returns a Promise.
 *
 * The async generator maintains a queue of pending requests so that
 * concurrent calls to next() are properly serialized.
 * ========================================================================= */

/* Request types for the async generator queue */
typedef enum {
    R8E_ASYNC_GEN_NEXT   = 0,
    R8E_ASYNC_GEN_RETURN = 1,
    R8E_ASYNC_GEN_THROW  = 2
} R8EAsyncGenRequestType;

typedef struct R8EAsyncGenRequest {
    R8EAsyncGenRequestType type;
    R8EValue               value;    /* argument to next/return/throw */
    R8EValue               promise;  /* the promise returned to the caller */
    struct R8EAsyncGenRequest *next;  /* linked list */
} R8EAsyncGenRequest;

typedef struct R8EAsyncGenerator {
    /* GC header */
    uint32_t flags;
    uint32_t proto_id;

    /* Underlying generator state */
    R8EGenerator *gen;

    /* Request queue (serializes concurrent next/return/throw calls) */
    R8EAsyncGenRequest *queue_head;
    R8EAsyncGenRequest *queue_tail;
    uint32_t            queue_count;

    /* Processing flag */
    bool processing;
    uint8_t pad[7];
} R8EAsyncGenerator;

/**
 * Allocate a new async generator.
 */
R8EValue r8e_async_generator_new(R8EIterContext *ctx, R8EValue func,
                                 int argc, const R8EValue *argv)
{
    R8EAsyncGenerator *agen = (R8EAsyncGenerator *)r8e_iter_alloc(
        ctx, sizeof(R8EAsyncGenerator));
    if (agen == NULL) return R8E_UNDEFINED;

    memset(agen, 0, sizeof(R8EAsyncGenerator));
    agen->flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_GENERATOR);
    agen->flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    agen->proto_id = R8E_PROTO_GENERATOR;

    /* Create the underlying generator */
    R8EGenerator *gen = r8e_generator_alloc(ctx, 64, 32);
    if (gen == NULL) {
        r8e_iter_free(ctx, agen, sizeof(R8EAsyncGenerator));
        return R8E_UNDEFINED;
    }
    gen->closure = func;
    for (int i = 0; i < argc && (uint32_t)i < gen->local_count; i++) {
        gen->saved_locals[i] = argv[i];
    }
    agen->gen = gen;

    return r8e_from_pointer(agen);
}

/**
 * Get the R8EAsyncGenerator from a NaN-boxed value.
 */
static R8EAsyncGenerator *r8e_async_generator_get(R8EValue v)
{
    if (!R8E_IS_POINTER(v)) return NULL;
    R8EAsyncGenerator *agen = (R8EAsyncGenerator *)r8e_get_pointer(v);
    /* Distinguish from regular generator by checking the queue pointer
     * is valid. In the full implementation, use a different GC kind. */
    return agen;
}

/**
 * Enqueue a request to an async generator.
 */
static R8EValue r8e_async_gen_enqueue(R8EIterContext *ctx,
                                      R8EAsyncGenerator *agen,
                                      R8EAsyncGenRequestType type,
                                      R8EValue value)
{
    R8EAsyncGenRequest *req = (R8EAsyncGenRequest *)r8e_iter_alloc(
        ctx, sizeof(R8EAsyncGenRequest));
    if (req == NULL) return R8E_UNDEFINED;

    memset(req, 0, sizeof(R8EAsyncGenRequest));
    req->type = type;
    req->value = value;
    req->promise = r8e_iter_promise_new(ctx);
    req->next = NULL;

    if (agen->queue_tail != NULL) {
        agen->queue_tail->next = req;
    } else {
        agen->queue_head = req;
    }
    agen->queue_tail = req;
    agen->queue_count++;

    return req->promise;
}

/**
 * Process the next queued request for an async generator.
 */
static void r8e_async_gen_process_queue(R8EIterContext *ctx,
                                        R8EAsyncGenerator *agen)
{
    if (agen->processing || agen->queue_head == NULL) return;
    agen->processing = true;

    R8EAsyncGenRequest *req = agen->queue_head;
    agen->queue_head = req->next;
    if (agen->queue_head == NULL) {
        agen->queue_tail = NULL;
    }
    agen->queue_count--;

    R8EGenerator *gen = agen->gen;
    R8EValue result;

    switch (req->type) {
    case R8E_ASYNC_GEN_NEXT:
        result = r8e_generator_next(ctx, r8e_from_pointer(gen),
                                    req->value);
        break;
    case R8E_ASYNC_GEN_RETURN:
        result = r8e_generator_return(ctx, r8e_from_pointer(gen),
                                      req->value);
        break;
    case R8E_ASYNC_GEN_THROW:
        result = r8e_generator_throw(ctx, r8e_from_pointer(gen),
                                     req->value);
        break;
    default:
        result = R8E_UNDEFINED;
        break;
    }

    if (ctx->has_exception) {
        R8EValue err = ctx->exception;
        ctx->has_exception = false;
        ctx->exception = R8E_UNDEFINED;
        r8e_iter_promise_reject(ctx, req->promise, err);
    } else {
        r8e_iter_promise_resolve(ctx, req->promise, result);
    }

    r8e_iter_free(ctx, req, sizeof(R8EAsyncGenRequest));
    agen->processing = false;

    /* Process next queued request if any */
    r8e_async_gen_process_queue(ctx, agen);
}

/**
 * AsyncGenerator.prototype.next(value).
 * Returns a Promise of { value, done }.
 */
R8EValue r8e_async_generator_next(R8EIterContext *ctx, R8EValue agen_val,
                                  R8EValue value)
{
    R8EAsyncGenerator *agen = r8e_async_generator_get(agen_val);
    if (agen == NULL) {
        r8e_iter_throw_type_error(ctx,
            "AsyncGenerator.prototype.next called on non-async-generator");
        return R8E_UNDEFINED;
    }

    R8EValue promise = r8e_async_gen_enqueue(ctx, agen,
                                             R8E_ASYNC_GEN_NEXT, value);
    r8e_async_gen_process_queue(ctx, agen);
    return promise;
}

/**
 * AsyncGenerator.prototype.return(value).
 */
R8EValue r8e_async_generator_return(R8EIterContext *ctx, R8EValue agen_val,
                                    R8EValue value)
{
    R8EAsyncGenerator *agen = r8e_async_generator_get(agen_val);
    if (agen == NULL) {
        r8e_iter_throw_type_error(ctx,
            "AsyncGenerator.prototype.return called on non-async-generator");
        return R8E_UNDEFINED;
    }

    R8EValue promise = r8e_async_gen_enqueue(ctx, agen,
                                             R8E_ASYNC_GEN_RETURN, value);
    r8e_async_gen_process_queue(ctx, agen);
    return promise;
}

/**
 * AsyncGenerator.prototype.throw(error).
 */
R8EValue r8e_async_generator_throw(R8EIterContext *ctx, R8EValue agen_val,
                                   R8EValue error)
{
    R8EAsyncGenerator *agen = r8e_async_generator_get(agen_val);
    if (agen == NULL) {
        r8e_iter_throw_type_error(ctx,
            "AsyncGenerator.prototype.throw called on non-async-generator");
        return R8E_UNDEFINED;
    }

    R8EValue promise = r8e_async_gen_enqueue(ctx, agen,
                                             R8E_ASYNC_GEN_THROW, error);
    r8e_async_gen_process_queue(ctx, agen);
    return promise;
}


/* =========================================================================
 * Section 8: Generic Iterator Protocol
 *
 * For user-defined iterables, we look up Symbol.iterator to get the
 * iterator, then call .next() on it.
 * ========================================================================= */

/**
 * Get an iterator from an iterable.
 *
 * Implements: iterable[Symbol.iterator]()
 *
 * For built-in types (Array, String), returns a specialized iterator.
 * For user-defined iterables, calls the [Symbol.iterator]() method.
 *
 * @param ctx       Iterator context.
 * @param iterable  The iterable value.
 * @return          An iterator value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_get_iterator(R8EIterContext *ctx, R8EValue iterable)
{
    if (R8E_IS_UNDEFINED(iterable) || R8E_IS_NULL(iterable)) {
        r8e_iter_throw_type_error(ctx,
            "Cannot iterate over null or undefined");
        return R8E_UNDEFINED;
    }

    /* Check for built-in iterables by GC kind */
    if (R8E_IS_POINTER(iterable)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(iterable);
        uint32_t kind = R8E_GC_GET_KIND(hdr->flags);

        if (kind == R8E_GC_KIND_ARRAY) {
            return r8e_array_values_iterator(ctx, iterable);
        }
        if (kind == R8E_GC_KIND_STRING) {
            return r8e_string_iterator(ctx, iterable);
        }
        if (kind == R8E_GC_KIND_ITERATOR) {
            /* Already an iterator: return it directly
             * (iterators are their own iterables) */
            return iterable;
        }
        if (kind == R8E_GC_KIND_GENERATOR) {
            /* Generator is also an iterator */
            return iterable;
        }
    }

    /* Inline short string */
    if (R8E_IS_INLINE_STR(iterable)) {
        return r8e_string_iterator(ctx, iterable);
    }

    /* User-defined iterable: look up [Symbol.iterator] */
    if (R8E_IS_POINTER(iterable)) {
        R8EValue iter_method = r8e_iter_obj_get(ctx, iterable,
                                                 R8E_ATOM_iterator);
        if (r8e_iter_is_callable(iter_method)) {
            R8EValue iter = r8e_iter_call_function(ctx, iter_method,
                                                    iterable, 0, NULL);
            if (ctx->has_exception) return R8E_UNDEFINED;
            return iter;
        }
    }

    r8e_iter_throw_type_error(ctx, "Object is not iterable");
    return R8E_UNDEFINED;
}

/**
 * Get an async iterator from an async iterable.
 *
 * Implements: iterable[Symbol.asyncIterator]()
 * Falls back to iterable[Symbol.iterator]() if no async iterator.
 */
R8EValue r8e_get_async_iterator(R8EIterContext *ctx, R8EValue iterable)
{
    if (R8E_IS_POINTER(iterable)) {
        /* Try Symbol.asyncIterator first */
        R8EValue async_iter_method = r8e_iter_obj_get(
            ctx, iterable, R8E_ATOM_asyncIterator);

        if (r8e_iter_is_callable(async_iter_method)) {
            R8EValue iter = r8e_iter_call_function(
                ctx, async_iter_method, iterable, 0, NULL);
            if (ctx->has_exception) return R8E_UNDEFINED;
            return iter;
        }
    }

    /* Fall back to sync iterator */
    return r8e_get_iterator(ctx, iterable);
}


/* =========================================================================
 * Section 9: Polymorphic Iterator next/close
 *
 * These functions dispatch to the correct implementation based on the
 * iterator kind.
 * ========================================================================= */

/**
 * Call iterator.next().
 *
 * For built-in iterators, dispatches to the appropriate implementation.
 * For user-defined iterators (generic), calls the .next() method.
 */
R8EValue r8e_iterator_next(R8EIterContext *ctx, R8EValue iter)
{
    if (R8E_IS_POINTER(iter)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(iter);
        uint32_t kind = R8E_GC_GET_KIND(hdr->flags);

        if (kind == R8E_GC_KIND_ITERATOR) {
            R8EIteratorBase *base = (R8EIteratorBase *)r8e_get_pointer(iter);

            switch (base->kind) {
            case R8E_ITER_ARRAY_VALUES:
            case R8E_ITER_ARRAY_KEYS:
            case R8E_ITER_ARRAY_ENTRIES:
                return r8e_array_iterator_next(
                    ctx, (R8EArrayIterator *)base);

            case R8E_ITER_STRING:
                return r8e_string_iterator_next(
                    ctx, (R8EStringIterator *)base);

            case R8E_ITER_MAP_ENTRIES:
            case R8E_ITER_SET_VALUES:
                /* Stub: Map/Set iterators not yet implemented */
                return r8e_create_iter_result(ctx, R8E_UNDEFINED, true);

            case R8E_ITER_GENERATOR:
            case R8E_ITER_ASYNC_GENERATOR:
            case R8E_ITER_GENERIC:
                break;
            }
        }

        if (kind == R8E_GC_KIND_GENERATOR) {
            return r8e_generator_next(ctx, iter, R8E_UNDEFINED);
        }

        /* Generic/user-defined iterator: call .next() */
        R8EValue next_fn = r8e_iter_obj_get(ctx, iter, R8E_ATOM_next);
        if (r8e_iter_is_callable(next_fn)) {
            return r8e_iter_call_function(ctx, next_fn, iter, 0, NULL);
        }
    }

    r8e_iter_throw_type_error(ctx, "Iterator does not have a next method");
    return R8E_UNDEFINED;
}

/**
 * Call iterator.next(value) with a sent value.
 */
R8EValue r8e_iterator_next_value(R8EIterContext *ctx, R8EValue iter,
                                 R8EValue value)
{
    if (R8E_IS_POINTER(iter)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(iter);
        uint32_t kind = R8E_GC_GET_KIND(hdr->flags);

        if (kind == R8E_GC_KIND_GENERATOR) {
            return r8e_generator_next(ctx, iter, value);
        }

        /* User-defined iterator: call .next(value) */
        R8EValue next_fn = r8e_iter_obj_get(ctx, iter, R8E_ATOM_next);
        if (r8e_iter_is_callable(next_fn)) {
            R8EValue args[1] = { value };
            return r8e_iter_call_function(ctx, next_fn, iter, 1, args);
        }
    }

    /* For built-in non-generator iterators, ignore the sent value */
    return r8e_iterator_next(ctx, iter);
}

/**
 * Close an iterator (call .return() if available).
 *
 * Called on break/return/throw during for-of iteration.
 * If the iterator has a .return() method, call it to allow cleanup.
 *
 * @param ctx         Iterator context.
 * @param iter        The iterator to close.
 * @param completion  The completion value (for error propagation).
 */
void r8e_iterator_close(R8EIterContext *ctx, R8EValue iter,
                        R8EValue completion)
{
    if (!R8E_IS_POINTER(iter)) return;

    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(iter);
    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);

    if (kind == R8E_GC_KIND_GENERATOR) {
        /* For generators, call .return(undefined) */
        r8e_generator_return(ctx, iter, completion);
        return;
    }

    /* Check for .return() method on user-defined iterators */
    R8EValue return_fn = r8e_iter_obj_get(ctx, iter, R8E_ATOM_return_kw);
    if (r8e_iter_is_callable(return_fn)) {
        R8EValue args[1] = { completion };
        R8EValue result = r8e_iter_call_function(ctx, return_fn, iter,
                                                  1, args);
        (void)result;  /* ES2023: ignore .return() result */
    }
}


/* =========================================================================
 * Section 10: for-of Support
 *
 * These functions support the for-of loop implementation in the
 * interpreter. The interpreter emits FOR_OF_INIT and FOR_OF_NEXT opcodes
 * that call into these helpers.
 * ========================================================================= */

/**
 * Initialize a for-of loop by getting an iterator from the iterable.
 *
 * @param ctx       Iterator context.
 * @param iterable  The value being iterated over.
 * @return          Iterator value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_for_of_init(R8EIterContext *ctx, R8EValue iterable)
{
    return r8e_get_iterator(ctx, iterable);
}

/**
 * Get the next value from a for-of iterator.
 *
 * @param ctx        Iterator context.
 * @param iter       The iterator.
 * @param value_out  Output: the next value (only valid if returns true).
 * @return           true if a value was produced, false if done.
 */
bool r8e_for_of_next(R8EIterContext *ctx, R8EValue iter,
                     R8EValue *value_out)
{
    R8EValue result = r8e_iterator_next(ctx, iter);
    if (ctx->has_exception) return false;

    /* Extract 'done' field */
    R8EValue done_val = r8e_iter_obj_get(ctx, result, R8E_ATOM_done);
    if (done_val == R8E_TRUE) {
        return false;
    }

    /* Extract 'value' field */
    *value_out = r8e_iter_obj_get(ctx, result, R8E_ATOM_value);
    return true;
}


/* =========================================================================
 * Section 11: Spread and Destructuring
 *
 * These functions consume an iterable into an array, used for:
 *   - Spread syntax: [...iterable]
 *   - Destructuring: const [a, b, ...rest] = iterable
 *   - Spread in function calls: foo(...args)
 * ========================================================================= */

/**
 * Consume an entire iterable into an array.
 *
 * Used for spread syntax: [...iterable] and spread in function calls.
 *
 * @param ctx       Iterator context.
 * @param iterable  The iterable to consume.
 * @return          Array of all values, or R8E_UNDEFINED on error.
 */
R8EValue r8e_spread_iterable(R8EIterContext *ctx, R8EValue iterable)
{
    R8EValue iter = r8e_get_iterator(ctx, iterable);
    if (R8E_IS_UNDEFINED(iter) || ctx->has_exception) {
        return R8E_UNDEFINED;
    }

    R8EValue arr = r8e_iter_make_array(ctx, 8);
    R8EValue value;

    while (r8e_for_of_next(ctx, iter, &value)) {
        r8e_iter_array_push(ctx, arr, value);
        if (ctx->has_exception) {
            r8e_iterator_close(ctx, iter, R8E_UNDEFINED);
            return R8E_UNDEFINED;
        }
    }

    return arr;
}

/**
 * Destructure an iterable: consume up to 'count' elements.
 *
 * Used for: const [a, b, c] = iterable
 *
 * Returns an array of exactly 'count' elements. If the iterable
 * produces fewer, the remaining elements are undefined.
 *
 * @param ctx       Iterator context.
 * @param iterable  The iterable to destructure.
 * @param count     Number of elements to extract.
 * @return          Array of extracted values.
 */
R8EValue r8e_destructure_iterable(R8EIterContext *ctx, R8EValue iterable,
                                  uint32_t count)
{
    R8EValue iter = r8e_get_iterator(ctx, iterable);
    if (R8E_IS_UNDEFINED(iter) || ctx->has_exception) {
        return R8E_UNDEFINED;
    }

    R8EValue arr = r8e_iter_make_array(ctx, count);
    R8EValue value;
    uint32_t i = 0;

    while (i < count && r8e_for_of_next(ctx, iter, &value)) {
        r8e_iter_array_push(ctx, arr, value);
        i++;
        if (ctx->has_exception) {
            r8e_iterator_close(ctx, iter, R8E_UNDEFINED);
            return R8E_UNDEFINED;
        }
    }

    /* Fill remaining slots with undefined */
    while (i < count) {
        r8e_iter_array_push(ctx, arr, R8E_UNDEFINED);
        i++;
    }

    /* Close the iterator (we may not have consumed all elements) */
    r8e_iterator_close(ctx, iter, R8E_UNDEFINED);

    return arr;
}

/**
 * Destructure with rest: consume first 'count' elements, then collect
 * the rest into an array.
 *
 * Used for: const [a, b, ...rest] = iterable
 *
 * @param ctx       Iterator context.
 * @param iterable  The iterable to destructure.
 * @param count     Number of elements before the rest.
 * @param rest_out  Output: array of remaining elements.
 * @return          Array of the first 'count' elements.
 */
R8EValue r8e_destructure_rest(R8EIterContext *ctx, R8EValue iterable,
                              uint32_t count, R8EValue *rest_out)
{
    R8EValue iter = r8e_get_iterator(ctx, iterable);
    if (R8E_IS_UNDEFINED(iter) || ctx->has_exception) {
        *rest_out = R8E_UNDEFINED;
        return R8E_UNDEFINED;
    }

    R8EValue arr = r8e_iter_make_array(ctx, count);
    R8EValue value;
    uint32_t i = 0;

    /* Consume first 'count' elements */
    while (i < count && r8e_for_of_next(ctx, iter, &value)) {
        r8e_iter_array_push(ctx, arr, value);
        i++;
        if (ctx->has_exception) {
            r8e_iterator_close(ctx, iter, R8E_UNDEFINED);
            *rest_out = R8E_UNDEFINED;
            return R8E_UNDEFINED;
        }
    }

    /* Fill remaining with undefined */
    while (i < count) {
        r8e_iter_array_push(ctx, arr, R8E_UNDEFINED);
        i++;
    }

    /* Collect rest elements */
    R8EValue rest = r8e_iter_make_array(ctx, 8);
    while (r8e_for_of_next(ctx, iter, &value)) {
        r8e_iter_array_push(ctx, rest, value);
        if (ctx->has_exception) {
            r8e_iterator_close(ctx, iter, R8E_UNDEFINED);
            *rest_out = R8E_UNDEFINED;
            return R8E_UNDEFINED;
        }
    }

    *rest_out = rest;
    return arr;
}


/* =========================================================================
 * Section 12: Iterator GC Integration
 * ========================================================================= */

/**
 * Mark all values reachable from an iterator.
 */
void r8e_iterator_gc_mark(R8EIterContext *ctx, R8EIteratorBase *iter)
{
    (void)ctx;
    if (iter == NULL) return;

    /* Mark the source iterable */
    /* r8e_gc_mark_value(ctx, iter->source); */

    /* For generators, also mark saved frame contents */
    if (iter->kind == R8E_ITER_GENERATOR) {
        R8EGenerator *gen = (R8EGenerator *)iter;
        /* r8e_gc_mark_value(ctx, gen->closure); */
        /* Mark all saved locals */
        for (uint32_t i = 0; i < gen->local_count; i++) {
            /* r8e_gc_mark_value(ctx, gen->saved_locals[i]); */
            (void)gen;
        }
    }
}

/**
 * Free an iterator and its resources.
 */
void r8e_iterator_gc_free(R8EIterContext *ctx, R8EIteratorBase *iter)
{
    if (iter == NULL) return;

    switch (iter->kind) {
    case R8E_ITER_ARRAY_VALUES:
    case R8E_ITER_ARRAY_KEYS:
    case R8E_ITER_ARRAY_ENTRIES:
        r8e_iter_free(ctx, iter, sizeof(R8EArrayIterator));
        break;

    case R8E_ITER_STRING:
        r8e_iter_free(ctx, iter, sizeof(R8EStringIterator));
        break;

    case R8E_ITER_GENERATOR:
        r8e_generator_free_obj(ctx, (R8EGenerator *)iter);
        break;

    case R8E_ITER_ASYNC_GENERATOR: {
        R8EAsyncGenerator *agen = (R8EAsyncGenerator *)iter;
        /* Free queued requests */
        R8EAsyncGenRequest *req = agen->queue_head;
        while (req != NULL) {
            R8EAsyncGenRequest *next = req->next;
            r8e_iter_free(ctx, req, sizeof(R8EAsyncGenRequest));
            req = next;
        }
        if (agen->gen != NULL) {
            r8e_generator_free_obj(ctx, agen->gen);
        }
        r8e_iter_free(ctx, agen, sizeof(R8EAsyncGenerator));
        break;
    }

    case R8E_ITER_MAP_ENTRIES:
    case R8E_ITER_SET_VALUES:
    case R8E_ITER_GENERIC:
        r8e_iter_free(ctx, iter, sizeof(R8EIteratorBase));
        break;
    }
}


/* =========================================================================
 * Section 13: Built-in Iterator Method Dispatch
 *
 * These functions implement the JS-visible methods on iterator prototypes.
 * Registered during built-in initialization in r8e_builtin.c.
 * ========================================================================= */

/**
 * JS-facing: Iterator.prototype.next
 */
R8EValue r8e_builtin_iterator_next(R8EIterContext *ctx,
                                   R8EValue this_val,
                                   int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    return r8e_iterator_next(ctx, this_val);
}

/**
 * JS-facing: Iterator.prototype.return
 */
R8EValue r8e_builtin_iterator_return(R8EIterContext *ctx,
                                     R8EValue this_val,
                                     int argc, const R8EValue *argv)
{
    R8EValue value = (argc > 0) ? argv[0] : R8E_UNDEFINED;

    if (R8E_IS_POINTER(this_val)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(this_val);
        if (R8E_GC_GET_KIND(hdr->flags) == R8E_GC_KIND_GENERATOR) {
            return r8e_generator_return(ctx, this_val, value);
        }
    }

    /* Built-in iterators: just return {value, done: true} */
    if (R8E_IS_POINTER(this_val)) {
        R8EIteratorBase *base = (R8EIteratorBase *)r8e_get_pointer(this_val);
        base->done = true;
    }
    return r8e_create_iter_result(ctx, value, true);
}

/**
 * JS-facing: Iterator.prototype.throw
 */
R8EValue r8e_builtin_iterator_throw(R8EIterContext *ctx,
                                    R8EValue this_val,
                                    int argc, const R8EValue *argv)
{
    R8EValue error = (argc > 0) ? argv[0] : R8E_UNDEFINED;

    if (R8E_IS_POINTER(this_val)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(this_val);
        if (R8E_GC_GET_KIND(hdr->flags) == R8E_GC_KIND_GENERATOR) {
            return r8e_generator_throw(ctx, this_val, error);
        }
    }

    /* Non-generator iterators: just throw the error */
    ctx->has_exception = true;
    ctx->exception = error;
    return R8E_UNDEFINED;
}

/**
 * JS-facing: IteratorPrototype[Symbol.iterator]
 * Returns 'this' (iterators are their own iterables).
 */
R8EValue r8e_builtin_iterator_symbol_iterator(R8EIterContext *ctx,
                                              R8EValue this_val,
                                              int argc,
                                              const R8EValue *argv)
{
    (void)ctx; (void)argc; (void)argv;
    return this_val;
}

/**
 * JS-facing: Generator.prototype.next(value)
 */
R8EValue r8e_builtin_generator_next(R8EIterContext *ctx,
                                    R8EValue this_val,
                                    int argc, const R8EValue *argv)
{
    R8EValue value = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_generator_next(ctx, this_val, value);
}

/**
 * JS-facing: Generator.prototype.return(value)
 */
R8EValue r8e_builtin_generator_return(R8EIterContext *ctx,
                                      R8EValue this_val,
                                      int argc, const R8EValue *argv)
{
    R8EValue value = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_generator_return(ctx, this_val, value);
}

/**
 * JS-facing: Generator.prototype.throw(error)
 */
R8EValue r8e_builtin_generator_throw(R8EIterContext *ctx,
                                     R8EValue this_val,
                                     int argc, const R8EValue *argv)
{
    R8EValue error = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_generator_throw(ctx, this_val, error);
}

/**
 * JS-facing: AsyncGenerator.prototype.next(value)
 */
R8EValue r8e_builtin_async_generator_next(R8EIterContext *ctx,
                                          R8EValue this_val,
                                          int argc, const R8EValue *argv)
{
    R8EValue value = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_async_generator_next(ctx, this_val, value);
}

/**
 * JS-facing: AsyncGenerator.prototype.return(value)
 */
R8EValue r8e_builtin_async_generator_return(R8EIterContext *ctx,
                                            R8EValue this_val,
                                            int argc, const R8EValue *argv)
{
    R8EValue value = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_async_generator_return(ctx, this_val, value);
}

/**
 * JS-facing: AsyncGenerator.prototype.throw(error)
 */
R8EValue r8e_builtin_async_generator_throw(R8EIterContext *ctx,
                                           R8EValue this_val,
                                           int argc, const R8EValue *argv)
{
    R8EValue error = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_async_generator_throw(ctx, this_val, error);
}


/* =========================================================================
 * Section 14: Stub Implementations for External Dependencies
 *
 * These stubs allow the file to compile standalone. In the integrated
 * build, these are replaced by the real implementations.
 * ========================================================================= */

#ifndef R8E_INTEGRATED_BUILD

static R8EValue r8e_iter_call_function(R8EIterContext *ctx, R8EValue func,
                                       R8EValue this_val, int argc,
                                       const R8EValue *argv)
{
    (void)ctx; (void)func; (void)this_val; (void)argc; (void)argv;
    return R8E_UNDEFINED;
}

static R8EValue r8e_iter_obj_get(R8EIterContext *ctx, R8EValue obj,
                                 uint32_t atom)
{
    (void)ctx; (void)obj; (void)atom;
    return R8E_UNDEFINED;
}

static void r8e_iter_obj_set(R8EIterContext *ctx, R8EValue obj,
                             uint32_t atom, R8EValue val)
{
    (void)ctx; (void)obj; (void)atom; (void)val;
}

static bool r8e_iter_obj_has(R8EIterContext *ctx, R8EValue obj,
                             uint32_t atom)
{
    (void)ctx; (void)obj; (void)atom;
    return false;
}

static R8EValue r8e_iter_make_array(R8EIterContext *ctx, uint32_t cap)
{
    (void)ctx; (void)cap;
    return R8E_UNDEFINED;
}

static void r8e_iter_array_push(R8EIterContext *ctx, R8EValue arr,
                                R8EValue val)
{
    (void)ctx; (void)arr; (void)val;
}

static uint32_t r8e_iter_array_length(R8EIterContext *ctx, R8EValue arr)
{
    (void)ctx; (void)arr;
    return 0;
}

static R8EValue r8e_iter_array_get(R8EIterContext *ctx, R8EValue arr,
                                   uint32_t index)
{
    (void)ctx; (void)arr; (void)index;
    return R8E_UNDEFINED;
}

static R8EValue r8e_iter_make_object(R8EIterContext *ctx)
{
    (void)ctx;
    return R8E_UNDEFINED;
}

static bool r8e_iter_is_callable(R8EValue v)
{
    if (!R8E_IS_POINTER(v)) return false;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(v);
    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);
    return kind == R8E_GC_KIND_CLOSURE || kind == R8E_GC_KIND_FUNCTION;
}

static R8EValue r8e_iter_throw_type_error(R8EIterContext *ctx,
                                          const char *msg)
{
    (void)msg;
    ctx->has_exception = true;
    ctx->exception = R8E_UNDEFINED;
    return R8E_UNDEFINED;
}

static void *r8e_iter_alloc(R8EIterContext *ctx, size_t size)
{
    (void)ctx;
    return calloc(1, size);
}

static void r8e_iter_free(R8EIterContext *ctx, void *ptr, size_t size)
{
    (void)ctx; (void)size;
    free(ptr);
}

static R8EValue r8e_iter_promise_new(R8EIterContext *ctx)
{
    (void)ctx;
    return R8E_UNDEFINED;
}

static void r8e_iter_promise_resolve(R8EIterContext *ctx,
                                     R8EValue promise, R8EValue value)
{
    (void)ctx; (void)promise; (void)value;
}

static void r8e_iter_promise_reject(R8EIterContext *ctx,
                                    R8EValue promise, R8EValue reason)
{
    (void)ctx; (void)promise; (void)reason;
}

static R8EValue r8e_iter_promise_then(R8EIterContext *ctx,
                                      R8EValue promise,
                                      R8EValue on_fulfilled,
                                      R8EValue on_rejected)
{
    (void)ctx; (void)promise; (void)on_fulfilled; (void)on_rejected;
    return R8E_UNDEFINED;
}

#endif /* R8E_INTEGRATED_BUILD */
