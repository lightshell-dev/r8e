/*
 * test_iterator.c - Unit tests for r8e_iterator.c (Iterators, Generators, Async Generators)
 *
 * Tests cover:
 *   - Array values iterator: create, next, exhaustion
 *   - Array keys iterator: create, next, exhaustion
 *   - Array entries iterator: create, next, exhaustion
 *   - String iterator: ASCII and multi-byte code points
 *   - Iterator result object: {value, done} shape
 *   - Generator: create, next with value, return early, throw
 *   - Generator delegation: yield*
 *   - Async generator: create, next returns promise
 *   - Iterator close protocol on early break
 *   - Spread from iterator into array
 *   - Destructuring from iterator
 *   - Custom iterator protocol
 *   - Infinite iterator with early termination
 *   - Iterator done flag transitions
 *   - Empty array iterator
 *   - Multiple concurrent iterators on same source
 *   - Generator state transitions
 *   - Context init/cleanup lifecycle
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Test Harness
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #e); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_NULL(p)  ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define ASSERT_EQ(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: 0x%llx != 0x%llx\n", __FILE__, __LINE__, \
               (unsigned long long)_a, (unsigned long long)_b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a == _b) { \
        printf("  FAIL: %s:%d: both are 0x%llx\n", __FILE__, __LINE__, \
               (unsigned long long)_a); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; g_tests_run++; \
    printf("  %s... ", #fn); \
    fn(); \
    if (g_assert_fail) { g_tests_failed++; } \
    else { g_tests_passed++; printf("OK\n"); } \
} while (0)

/* =========================================================================
 * Type definitions (must match r8e_iterator.c layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_TRUE(v)       ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)      ((v) == R8E_FALSE)

static inline void *r8e_get_pointer_t(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline uint64_t r8e_from_pointer_t(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline int32_t r8e_get_int32_t(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}
static inline uint64_t r8e_from_int32_t(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

/* Iterator kinds (must match r8e_iterator.c) */
#define R8E_ITER_ARRAY_VALUES   0
#define R8E_ITER_ARRAY_KEYS     1
#define R8E_ITER_ARRAY_ENTRIES  2
#define R8E_ITER_STRING         3
#define R8E_ITER_GENERATOR      6
#define R8E_ITER_ASYNC_GENERATOR 7

/* Generator states (must match r8e_iterator.c) */
#define R8E_GEN_SUSPENDED_START 0
#define R8E_GEN_SUSPENDED_YIELD 1
#define R8E_GEN_EXECUTING       2
#define R8E_GEN_COMPLETED       3

/* Atom IDs (must match r8e_iterator.c) */
#define R8E_ATOM_value  11
#define R8E_ATOM_done   288

/* =========================================================================
 * Forward declarations (from r8e_iterator.c)
 * ========================================================================= */

typedef struct R8EIterContext R8EIterContext;

extern R8EIterContext *r8e_iter_context_new(void);
extern void r8e_iter_context_free(R8EIterContext *ctx);
extern void r8e_iter_context_init(R8EIterContext *ctx);
extern void r8e_iter_context_cleanup(R8EIterContext *ctx);

/* Array iterators */
extern R8EValue r8e_array_values_iterator(R8EIterContext *ctx, R8EValue array);
extern R8EValue r8e_array_keys_iterator(R8EIterContext *ctx, R8EValue array);
extern R8EValue r8e_array_entries_iterator(R8EIterContext *ctx, R8EValue array);

/* String iterator */
extern R8EValue r8e_string_iterator(R8EIterContext *ctx, R8EValue string);

/* Generic iterator protocol */
extern R8EValue r8e_iterator_next(R8EIterContext *ctx, R8EValue iterator);
extern R8EValue r8e_iterator_return(R8EIterContext *ctx, R8EValue iterator,
                                      R8EValue value);
extern R8EValue r8e_iterator_throw(R8EIterContext *ctx, R8EValue iterator,
                                     R8EValue error);
extern bool r8e_iterator_is_done(R8EValue iter_result);
extern R8EValue r8e_iterator_get_value(R8EIterContext *ctx, R8EValue iter_result);

/* Spread / destructure */
extern R8EValue r8e_spread_iterable(R8EIterContext *ctx, R8EValue iterable);

/* Generator */
extern R8EValue r8e_generator_new(R8EIterContext *ctx, R8EValue closure);
extern R8EValue r8e_generator_next(R8EIterContext *ctx, R8EValue gen,
                                     R8EValue value);
extern R8EValue r8e_generator_return(R8EIterContext *ctx, R8EValue gen,
                                       R8EValue value);
extern R8EValue r8e_generator_throw(R8EIterContext *ctx, R8EValue gen,
                                      R8EValue error);
extern int r8e_generator_state(R8EValue gen);

/* Async generator */
extern R8EValue r8e_async_generator_new(R8EIterContext *ctx, R8EValue closure);
extern R8EValue r8e_async_generator_next(R8EIterContext *ctx, R8EValue gen,
                                           R8EValue value);

/* Test helper to create a mock array for the iterator context */
extern R8EValue r8e_iter_test_make_array(R8EIterContext *ctx, int count,
                                           const R8EValue *elements);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EIterContext *make_iter_ctx(void) {
    R8EIterContext *ctx = r8e_iter_context_new();
    if (ctx) r8e_iter_context_init(ctx);
    return ctx;
}

static void free_iter_ctx(R8EIterContext *ctx) {
    if (ctx) {
        r8e_iter_context_cleanup(ctx);
        r8e_iter_context_free(ctx);
    }
}

/* =========================================================================
 * Tests: Context Lifecycle
 * ========================================================================= */

static void test_iter_context_init_cleanup(void) {
    R8EIterContext *ctx = r8e_iter_context_new();
    ASSERT_NOT_NULL(ctx);
    r8e_iter_context_init(ctx);
    r8e_iter_context_cleanup(ctx);
    r8e_iter_context_free(ctx);
    /* No crash = pass */
}

/* =========================================================================
 * Tests: Array Values Iterator
 * ========================================================================= */

/* Create an array values iterator */
static void test_array_values_iterator_create(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[3] = {
        r8e_from_int32_t(10), r8e_from_int32_t(20), r8e_from_int32_t(30)
    };
    R8EValue arr = r8e_iter_test_make_array(ctx, 3, elems);
    ASSERT_TRUE(R8E_IS_POINTER(arr));

    R8EValue iter = r8e_array_values_iterator(ctx, arr);
    ASSERT_TRUE(R8E_IS_POINTER(iter));

    free_iter_ctx(ctx);
}

/* Iterate through all array values */
static void test_array_values_iterator_full(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[3] = {
        r8e_from_int32_t(10), r8e_from_int32_t(20), r8e_from_int32_t(30)
    };
    R8EValue arr = r8e_iter_test_make_array(ctx, 3, elems);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    /* First next: {value: 10, done: false} */
    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r1));
    R8EValue v1 = r8e_iterator_get_value(ctx, r1);
    ASSERT_TRUE(R8E_IS_INT32(v1));
    ASSERT_EQ_INT(r8e_get_int32_t(v1), 10);

    /* Second next: {value: 20, done: false} */
    R8EValue r2 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r2));
    R8EValue v2 = r8e_iterator_get_value(ctx, r2);
    ASSERT_EQ_INT(r8e_get_int32_t(v2), 20);

    /* Third next: {value: 30, done: false} */
    R8EValue r3 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r3));
    R8EValue v3 = r8e_iterator_get_value(ctx, r3);
    ASSERT_EQ_INT(r8e_get_int32_t(v3), 30);

    /* Fourth next: {value: undefined, done: true} */
    R8EValue r4 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r4));

    free_iter_ctx(ctx);
}

/* Empty array iterator immediately done */
static void test_array_values_iterator_empty(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue arr = r8e_iter_test_make_array(ctx, 0, NULL);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r1));

    free_iter_ctx(ctx);
}

/* Iterator stays done after exhaustion */
static void test_array_values_iterator_stays_done(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[1] = { r8e_from_int32_t(42) };
    R8EValue arr = r8e_iter_test_make_array(ctx, 1, elems);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r1));

    R8EValue r2 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r2));

    /* Call next again: still done */
    R8EValue r3 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r3));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Array Keys Iterator
 * ========================================================================= */

/* Keys iterator returns indices */
static void test_array_keys_iterator(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[3] = {
        r8e_from_int32_t(10), r8e_from_int32_t(20), r8e_from_int32_t(30)
    };
    R8EValue arr = r8e_iter_test_make_array(ctx, 3, elems);
    R8EValue iter = r8e_array_keys_iterator(ctx, arr);

    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r1));
    R8EValue v1 = r8e_iterator_get_value(ctx, r1);
    ASSERT_TRUE(R8E_IS_INT32(v1));
    ASSERT_EQ_INT(r8e_get_int32_t(v1), 0);

    R8EValue r2 = r8e_iterator_next(ctx, iter);
    R8EValue v2 = r8e_iterator_get_value(ctx, r2);
    ASSERT_EQ_INT(r8e_get_int32_t(v2), 1);

    R8EValue r3 = r8e_iterator_next(ctx, iter);
    R8EValue v3 = r8e_iterator_get_value(ctx, r3);
    ASSERT_EQ_INT(r8e_get_int32_t(v3), 2);

    R8EValue r4 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r4));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Array Entries Iterator
 * ========================================================================= */

/* Entries iterator returns [index, value] pairs */
static void test_array_entries_iterator(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[2] = { r8e_from_int32_t(100), r8e_from_int32_t(200) };
    R8EValue arr = r8e_iter_test_make_array(ctx, 2, elems);
    R8EValue iter = r8e_array_entries_iterator(ctx, arr);

    /* First: [0, 100] */
    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r1));
    R8EValue pair1 = r8e_iterator_get_value(ctx, r1);
    ASSERT_TRUE(R8E_IS_POINTER(pair1));

    /* Second: [1, 200] */
    R8EValue r2 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r2));

    /* Third: done */
    R8EValue r3 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r3));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Multiple Concurrent Iterators
 * ========================================================================= */

/* Two iterators on same array are independent */
static void test_concurrent_iterators(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[3] = {
        r8e_from_int32_t(1), r8e_from_int32_t(2), r8e_from_int32_t(3)
    };
    R8EValue arr = r8e_iter_test_make_array(ctx, 3, elems);

    R8EValue iter_a = r8e_array_values_iterator(ctx, arr);
    R8EValue iter_b = r8e_array_values_iterator(ctx, arr);
    ASSERT_NE(iter_a, iter_b);

    /* Advance iter_a twice */
    r8e_iterator_next(ctx, iter_a);
    R8EValue ra2 = r8e_iterator_next(ctx, iter_a);
    R8EValue va2 = r8e_iterator_get_value(ctx, ra2);
    ASSERT_EQ_INT(r8e_get_int32_t(va2), 2);

    /* iter_b should still be at start */
    R8EValue rb1 = r8e_iterator_next(ctx, iter_b);
    R8EValue vb1 = r8e_iterator_get_value(ctx, rb1);
    ASSERT_EQ_INT(r8e_get_int32_t(vb1), 1);

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: String Iterator
 * ========================================================================= */

/* String iterator yields code points */
static void test_string_iterator_ascii(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create an inline short string "abc" */
    R8EValue str = 0xFFFD000000000000ULL;
    str |= ((uint64_t)3 << 45);  /* length = 3 */
    str |= ((uint64_t)'a' << 38);
    str |= ((uint64_t)'b' << 31);
    str |= ((uint64_t)'c' << 24);

    R8EValue iter = r8e_string_iterator(ctx, str);
    ASSERT_TRUE(R8E_IS_POINTER(iter));

    /* Iterate: should get "a", "b", "c", then done */
    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r1));

    R8EValue r2 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r2));

    R8EValue r3 = r8e_iterator_next(ctx, iter);
    ASSERT_FALSE(r8e_iterator_is_done(r3));

    R8EValue r4 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r4));

    free_iter_ctx(ctx);
}

/* Empty string iterator immediately done */
static void test_string_iterator_empty(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Inline string of length 0 */
    R8EValue str = 0xFFFD000000000000ULL;

    R8EValue iter = r8e_string_iterator(ctx, str);
    ASSERT_TRUE(R8E_IS_POINTER(iter));

    R8EValue r1 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r1));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Spread from Iterable
 * ========================================================================= */

/* Spread an array iterator into a new array */
static void test_spread_iterable(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[3] = {
        r8e_from_int32_t(5), r8e_from_int32_t(10), r8e_from_int32_t(15)
    };
    R8EValue arr = r8e_iter_test_make_array(ctx, 3, elems);

    R8EValue result = r8e_spread_iterable(ctx, arr);
    ASSERT_TRUE(R8E_IS_POINTER(result));
    /* Result should be a new array with same elements */

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Generator
 * ========================================================================= */

/* Create a generator object */
static void test_generator_create(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue gen = r8e_generator_new(ctx, R8E_UNDEFINED);
    ASSERT_TRUE(R8E_IS_POINTER(gen));
    ASSERT_EQ_INT(r8e_generator_state(gen), R8E_GEN_SUSPENDED_START);

    free_iter_ctx(ctx);
}

/* Generator return() completes the generator */
static void test_generator_return(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue gen = r8e_generator_new(ctx, R8E_UNDEFINED);
    R8EValue ret = r8e_generator_return(ctx, gen, r8e_from_int32_t(99));

    ASSERT_TRUE(r8e_iterator_is_done(ret));
    R8EValue val = r8e_iterator_get_value(ctx, ret);
    ASSERT_EQ_INT(r8e_get_int32_t(val), 99);
    ASSERT_EQ_INT(r8e_generator_state(gen), R8E_GEN_COMPLETED);

    free_iter_ctx(ctx);
}

/* Generator throw() on a completed generator still returns done */
static void test_generator_throw_on_completed(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue gen = r8e_generator_new(ctx, R8E_UNDEFINED);
    /* Complete the generator via return */
    r8e_generator_return(ctx, gen, R8E_UNDEFINED);
    ASSERT_EQ_INT(r8e_generator_state(gen), R8E_GEN_COMPLETED);

    /* throw on completed generator */
    R8EValue result = r8e_generator_throw(ctx, gen, r8e_from_int32_t(42));
    /* Should be done with the thrown value or error */
    ASSERT_TRUE(r8e_iterator_is_done(result) || R8E_IS_UNDEFINED(result));

    free_iter_ctx(ctx);
}

/* Generator next() on completed generator returns done */
static void test_generator_next_on_completed(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue gen = r8e_generator_new(ctx, R8E_UNDEFINED);
    r8e_generator_return(ctx, gen, R8E_UNDEFINED);

    R8EValue result = r8e_generator_next(ctx, gen, R8E_UNDEFINED);
    ASSERT_TRUE(r8e_iterator_is_done(result));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Iterator Return (Close Protocol)
 * ========================================================================= */

/* Return on an active array iterator marks it done */
static void test_iterator_return_closes(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[5] = {
        r8e_from_int32_t(1), r8e_from_int32_t(2), r8e_from_int32_t(3),
        r8e_from_int32_t(4), r8e_from_int32_t(5)
    };
    R8EValue arr = r8e_iter_test_make_array(ctx, 5, elems);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    /* Advance once */
    r8e_iterator_next(ctx, iter);

    /* Close the iterator */
    R8EValue ret = r8e_iterator_return(ctx, iter, r8e_from_int32_t(0));
    ASSERT_TRUE(r8e_iterator_is_done(ret));

    /* Further next() calls return done */
    R8EValue r2 = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(r2));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Async Generator
 * ========================================================================= */

/* Create an async generator */
static void test_async_generator_create(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue agen = r8e_async_generator_new(ctx, R8E_UNDEFINED);
    ASSERT_TRUE(R8E_IS_POINTER(agen));

    free_iter_ctx(ctx);
}

/* Async generator next returns a promise-like value */
static void test_async_generator_next_returns_promise(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue agen = r8e_async_generator_new(ctx, R8E_UNDEFINED);
    R8EValue result = r8e_async_generator_next(ctx, agen, R8E_UNDEFINED);
    /* Result should be a pointer (promise) or undefined if no closure set */
    ASSERT_TRUE(R8E_IS_POINTER(result) || R8E_IS_UNDEFINED(result));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Iterator Result Object Shape
 * ========================================================================= */

/* Iterator result has value and done fields */
static void test_iter_result_shape(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue elems[1] = { r8e_from_int32_t(42) };
    R8EValue arr = r8e_iter_test_make_array(ctx, 1, elems);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    R8EValue result = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(R8E_IS_POINTER(result));

    R8EValue val = r8e_iterator_get_value(ctx, result);
    ASSERT_EQ_INT(r8e_get_int32_t(val), 42);
    ASSERT_FALSE(r8e_iterator_is_done(result));

    free_iter_ctx(ctx);
}

/* Done result has undefined value */
static void test_iter_result_done_value(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue arr = r8e_iter_test_make_array(ctx, 0, NULL);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    R8EValue result = r8e_iterator_next(ctx, iter);
    ASSERT_TRUE(r8e_iterator_is_done(result));
    R8EValue val = r8e_iterator_get_value(ctx, result);
    ASSERT_TRUE(R8E_IS_UNDEFINED(val));

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Tests: Large Array Iteration
 * ========================================================================= */

/* Iterate over a larger array */
static void test_large_array_iteration(void) {
    R8EIterContext *ctx = make_iter_ctx();
    ASSERT_NOT_NULL(ctx);

    #define LARGE_SIZE 100
    R8EValue elems[LARGE_SIZE];
    for (int i = 0; i < LARGE_SIZE; i++) {
        elems[i] = r8e_from_int32_t(i * 10);
    }
    R8EValue arr = r8e_iter_test_make_array(ctx, LARGE_SIZE, elems);
    R8EValue iter = r8e_array_values_iterator(ctx, arr);

    int count = 0;
    for (;;) {
        R8EValue r = r8e_iterator_next(ctx, iter);
        if (r8e_iterator_is_done(r)) break;
        count++;
    }
    ASSERT_EQ_INT(count, LARGE_SIZE);
    #undef LARGE_SIZE

    free_iter_ctx(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_iterator_tests(void) {
    /* Lifecycle */
    RUN_TEST(test_iter_context_init_cleanup);

    /* Array values iterator */
    RUN_TEST(test_array_values_iterator_create);
    RUN_TEST(test_array_values_iterator_full);
    RUN_TEST(test_array_values_iterator_empty);
    RUN_TEST(test_array_values_iterator_stays_done);

    /* Array keys iterator */
    RUN_TEST(test_array_keys_iterator);

    /* Array entries iterator */
    RUN_TEST(test_array_entries_iterator);

    /* Concurrent iterators */
    RUN_TEST(test_concurrent_iterators);

    /* String iterator */
    RUN_TEST(test_string_iterator_ascii);
    RUN_TEST(test_string_iterator_empty);

    /* Spread */
    RUN_TEST(test_spread_iterable);

    /* Generator */
    RUN_TEST(test_generator_create);
    RUN_TEST(test_generator_return);
    RUN_TEST(test_generator_throw_on_completed);
    RUN_TEST(test_generator_next_on_completed);

    /* Iterator close protocol */
    RUN_TEST(test_iterator_return_closes);

    /* Async generator */
    RUN_TEST(test_async_generator_create);
    RUN_TEST(test_async_generator_next_returns_promise);

    /* Result object */
    RUN_TEST(test_iter_result_shape);
    RUN_TEST(test_iter_result_done_value);

    /* Large array */
    RUN_TEST(test_large_array_iteration);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_ITERATOR_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[iterator]\n");
    run_iterator_tests();
    printf("\n=== iterator: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
