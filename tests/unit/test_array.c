/*
 * test_array.c - Unit tests for r8e_array.c (Dense/Sparse Array + Methods)
 *
 * Tests cover:
 *   - Array creation and length
 *   - push/pop
 *   - get/set by index
 *   - Growth to 1000 elements
 *   - shift/unshift
 *   - splice (insert, remove, replace)
 *   - slice (positive, negative)
 *   - indexOf, lastIndexOf, includes
 *   - sort (numbers, sorted, reversed)
 *   - reverse
 *   - join
 *   - map, filter, reduce (C callbacks)
 *   - find, findIndex
 *   - some, every
 *   - fill, copyWithin
 *   - Array.isArray
 *   - at() (positive and negative)
 *   - Sparse fallback
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

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  %-60s ", #name); \
    name(); \
    printf("[PASS]\n"); \
    tests_passed++; \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    int64_t _a = (int64_t)(a); \
    int64_t _b = (int64_t)(b); \
    if (_a != _b) { \
        printf("[FAIL]\n    Expected %lld == %lld\n    at %s:%d\n", \
               (long long)_a, (long long)_b, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

/* =========================================================================
 * Types matching r8e_array.c internal definitions
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_INT32(v)  (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v) (((v) >> 32) == 0xFFF90000U)

static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}

#define R8E_PROTO_MAX 64

typedef struct R8EContext {
    R8EValue  proto_table[R8E_PROTO_MAX];
    uint32_t  proto_count;
    void *(*alloc)(size_t size, void *opaque);
    void *(*realloc_fn)(void *ptr, size_t size, void *opaque);
    void  (*free_fn)(void *ptr, void *opaque);
    void  *alloc_opaque;
    R8EValue  last_error;
} R8EContext;

/* Forward declarations matching r8e_array.c public API */
typedef struct R8EArray R8EArray;

extern R8EArray *r8e_array_new(R8EContext *ctx, uint32_t initial_capacity);
extern void r8e_array_destroy(R8EContext *ctx, R8EArray *arr);
extern uint32_t r8e_array_length(const R8EArray *arr);
extern R8EValue r8e_array_get(R8EContext *ctx, const R8EArray *arr, uint32_t index);
extern int r8e_array_set(R8EContext *ctx, R8EArray *arr, uint32_t index, R8EValue val);
extern uint32_t r8e_array_push(R8EContext *ctx, R8EArray *arr, R8EValue val);
extern R8EValue r8e_array_pop(R8EContext *ctx, R8EArray *arr);
extern R8EValue r8e_array_shift(R8EContext *ctx, R8EArray *arr);
extern uint32_t r8e_array_unshift(R8EContext *ctx, R8EArray *arr,
                                   const R8EValue *vals, uint32_t count);
extern int r8e_array_splice(R8EContext *ctx, R8EArray *arr,
                             int32_t start, int32_t delete_count,
                             const R8EValue *insert_vals, uint32_t insert_count,
                             R8EArray *removed);
extern R8EArray *r8e_array_slice(R8EContext *ctx, const R8EArray *arr,
                                  int32_t start, int32_t end);
extern int32_t r8e_array_index_of(R8EContext *ctx, const R8EArray *arr,
                                   R8EValue search, int32_t from_index);
extern int32_t r8e_array_last_index_of(R8EContext *ctx, const R8EArray *arr,
                                        R8EValue search, int32_t from_index);
extern bool r8e_array_includes(R8EContext *ctx, const R8EArray *arr,
                                R8EValue search, int32_t from_index);
extern R8EValue r8e_array_at(R8EContext *ctx, const R8EArray *arr, int32_t index);
extern void r8e_array_reverse(R8EContext *ctx, R8EArray *arr);
extern void r8e_array_fill(R8EContext *ctx, R8EArray *arr, R8EValue val,
                            int32_t start, int32_t end);
extern void r8e_array_copy_within(R8EContext *ctx, R8EArray *arr,
                                   int32_t target, int32_t start, int32_t end);
extern bool r8e_array_is_array(R8EValue val);

/* Callback types matching r8e_array.c */
typedef R8EValue (*R8EArrayCallback)(R8EContext *ctx, R8EValue this_val,
                                      R8EValue val, uint32_t index,
                                      R8EArray *arr);
typedef R8EValue (*R8EArrayReduceCallback)(R8EContext *ctx, R8EValue accumulator,
                                            R8EValue val, uint32_t index,
                                            R8EArray *arr);
typedef int (*R8EArrayCompareFn)(R8EContext *ctx, R8EValue a, R8EValue b,
                                  void *user_data);
typedef int (*R8EToStringFn)(R8EContext *ctx, R8EValue val,
                              char *buf, size_t buf_size);

extern void r8e_array_for_each(R8EContext *ctx, R8EArray *arr,
                                R8EArrayCallback cb, R8EValue this_val);
extern R8EArray *r8e_array_map(R8EContext *ctx, const R8EArray *arr,
                                R8EArrayCallback cb, R8EValue this_val);
extern R8EArray *r8e_array_filter(R8EContext *ctx, const R8EArray *arr,
                                   R8EArrayCallback cb, R8EValue this_val);
extern R8EValue r8e_array_reduce(R8EContext *ctx, R8EArray *arr,
                                  R8EArrayReduceCallback cb, R8EValue initial);
extern R8EValue r8e_array_find(R8EContext *ctx, const R8EArray *arr,
                                R8EArrayCallback cb, R8EValue this_val);
extern int32_t r8e_array_find_index(R8EContext *ctx, const R8EArray *arr,
                                     R8EArrayCallback cb, R8EValue this_val);
extern bool r8e_array_some(R8EContext *ctx, const R8EArray *arr,
                            R8EArrayCallback cb, R8EValue this_val);
extern bool r8e_array_every(R8EContext *ctx, const R8EArray *arr,
                             R8EArrayCallback cb, R8EValue this_val);
extern void r8e_array_sort(R8EContext *ctx, R8EArray *arr,
                             R8EArrayCompareFn cmp, void *user_data);
extern int r8e_array_join(R8EContext *ctx, const R8EArray *arr,
                           const char *separator, R8EToStringFn to_str,
                           char *out_buf, size_t buf_size);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EContext *make_ctx(void) {
    return (R8EContext *)calloc(1, sizeof(R8EContext));
}

static void free_ctx(R8EContext *ctx) {
    free(ctx);
}

/* =========================================================================
 * Callback helpers for map/filter/reduce/find/some/every
 * ========================================================================= */

/* Double each value */
static R8EValue cb_double(R8EContext *ctx, R8EValue this_val,
                           R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)this_val; (void)index; (void)arr;
    if (R8E_IS_INT32(val))
        return r8e_from_int32(r8e_get_int32(val) * 2);
    return val;
}

/* Filter: keep values > 5 */
static R8EValue cb_gt5(R8EContext *ctx, R8EValue this_val,
                        R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)this_val; (void)index; (void)arr;
    if (R8E_IS_INT32(val) && r8e_get_int32(val) > 5)
        return R8E_TRUE;
    return R8E_FALSE;
}

/* Reduce: sum */
static R8EValue cb_sum(R8EContext *ctx, R8EValue acc,
                        R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)index; (void)arr;
    int32_t a = R8E_IS_INT32(acc) ? r8e_get_int32(acc) : 0;
    int32_t b = R8E_IS_INT32(val) ? r8e_get_int32(val) : 0;
    return r8e_from_int32(a + b);
}

/* Find: value == 7 */
static R8EValue cb_eq7(R8EContext *ctx, R8EValue this_val,
                        R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)this_val; (void)index; (void)arr;
    if (R8E_IS_INT32(val) && r8e_get_int32(val) == 7)
        return R8E_TRUE;
    return R8E_FALSE;
}

/* some/every: all positive */
static R8EValue cb_positive(R8EContext *ctx, R8EValue this_val,
                              R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)this_val; (void)index; (void)arr;
    if (R8E_IS_INT32(val) && r8e_get_int32(val) > 0)
        return R8E_TRUE;
    return R8E_FALSE;
}

/* Numeric compare for sort */
static int cmp_numeric(R8EContext *ctx, R8EValue a, R8EValue b, void *ud) {
    (void)ctx; (void)ud;
    int32_t ia = R8E_IS_INT32(a) ? r8e_get_int32(a) : 0;
    int32_t ib = R8E_IS_INT32(b) ? r8e_get_int32(b) : 0;
    return (ia > ib) - (ia < ib);
}

/* to_string for join */
static int to_str_int(R8EContext *ctx, R8EValue val, char *buf, size_t sz) {
    (void)ctx;
    if (R8E_IS_INT32(val))
        return snprintf(buf, sz, "%d", r8e_get_int32(val));
    return snprintf(buf, sz, "?");
}

/* =========================================================================
 * Tests
 * ========================================================================= */

static void test_create_empty_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ(r8e_array_length(arr), 0);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_push_pop(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++) {
        r8e_array_push(ctx, arr, r8e_from_int32(i * 10));
    }
    ASSERT_EQ(r8e_array_length(arr), 5);

    /* Pop in reverse order */
    for (int i = 5; i >= 1; i--) {
        R8EValue v = r8e_array_pop(ctx, arr);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), i * 10);
    }
    ASSERT_EQ(r8e_array_length(arr), 0);

    /* Pop from empty returns UNDEFINED */
    R8EValue v = r8e_array_pop(ctx, arr);
    ASSERT_EQ(v, R8E_UNDEFINED);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_get_set_by_index(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 8);

    r8e_array_set(ctx, arr, 0, r8e_from_int32(100));
    r8e_array_set(ctx, arr, 3, r8e_from_int32(300));

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 100);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 3)), 300);

    /* Gaps should be undefined */
    R8EValue gap = r8e_array_get(ctx, arr, 1);
    ASSERT_EQ(gap, R8E_UNDEFINED);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_array_growth_1000(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 4);

    for (int i = 0; i < 1000; i++) {
        r8e_array_push(ctx, arr, r8e_from_int32(i));
    }
    ASSERT_EQ(r8e_array_length(arr), 1000);

    for (int i = 0; i < 1000; i++) {
        R8EValue v = r8e_array_get(ctx, arr, (uint32_t)i);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), i);
    }

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_shift_unshift(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(2));
    r8e_array_push(ctx, arr, r8e_from_int32(3));

    /* Unshift: prepend 0, 1 */
    R8EValue vals[2] = { r8e_from_int32(0), r8e_from_int32(1) };
    r8e_array_unshift(ctx, arr, vals, 2);
    ASSERT_EQ(r8e_array_length(arr), 4);

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 0);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 3)), 3);

    /* Shift: remove from front */
    R8EValue first = r8e_array_shift(ctx, arr);
    ASSERT_EQ(r8e_get_int32(first), 0);
    ASSERT_EQ(r8e_array_length(arr), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_splice_insert(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(4));

    /* Insert 2, 3 at position 1 (delete 0 items) */
    R8EValue ins[2] = { r8e_from_int32(2), r8e_from_int32(3) };
    r8e_array_splice(ctx, arr, 1, 0, ins, 2, NULL);

    ASSERT_EQ(r8e_array_length(arr), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 3)), 4);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_splice_remove(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* Remove 2 items starting at index 1 */
    R8EArray *removed = r8e_array_new(ctx, 2);
    r8e_array_splice(ctx, arr, 1, 2, NULL, 0, removed);

    ASSERT_EQ(r8e_array_length(arr), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 5);

    /* Removed array should contain 2, 3 */
    ASSERT_EQ(r8e_array_length(removed), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, removed, 0)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, removed, 1)), 3);

    r8e_array_destroy(ctx, removed);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_splice_replace(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 4; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* Replace index 1-2 with 20, 30 */
    R8EValue repl[2] = { r8e_from_int32(20), r8e_from_int32(30) };
    r8e_array_splice(ctx, arr, 1, 2, repl, 2, NULL);

    ASSERT_EQ(r8e_array_length(arr), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 20);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 30);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_slice_positive(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i * 10));

    R8EArray *sl = r8e_array_slice(ctx, arr, 1, 4);
    ASSERT_NOT_NULL(sl);
    ASSERT_EQ(r8e_array_length(sl), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, sl, 0)), 10);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, sl, 1)), 20);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, sl, 2)), 30);

    r8e_array_destroy(ctx, sl);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_slice_negative(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* slice(-2, length) = last 2 elements */
    R8EArray *sl = r8e_array_slice(ctx, arr, -2, (int32_t)r8e_array_length(arr));
    ASSERT_NOT_NULL(sl);
    ASSERT_EQ(r8e_array_length(sl), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, sl, 0)), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, sl, 1)), 4);

    r8e_array_destroy(ctx, sl);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_index_of(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i * 10));

    ASSERT_EQ(r8e_array_index_of(ctx, arr, r8e_from_int32(20), 0), 2);
    ASSERT_EQ(r8e_array_index_of(ctx, arr, r8e_from_int32(99), 0), -1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_last_index_of(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));
    r8e_array_push(ctx, arr, r8e_from_int32(1));

    ASSERT_EQ(r8e_array_last_index_of(ctx, arr, r8e_from_int32(1), 2), 2);
    ASSERT_EQ(r8e_array_last_index_of(ctx, arr, r8e_from_int32(2), 2), 1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_includes(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(10));
    r8e_array_push(ctx, arr, r8e_from_int32(20));

    ASSERT(r8e_array_includes(ctx, arr, r8e_from_int32(10), 0));
    ASSERT(!r8e_array_includes(ctx, arr, r8e_from_int32(30), 0));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sort_numbers(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    int32_t vals[] = {5, 3, 8, 1, 4, 2, 7, 6};
    for (int i = 0; i < 8; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(vals[i]));

    r8e_array_sort(ctx, arr, cmp_numeric, NULL);

    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, (uint32_t)i)), i + 1);
    }

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sort_already_sorted(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    r8e_array_sort(ctx, arr, cmp_numeric, NULL);

    for (int i = 0; i < 5; i++)
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, (uint32_t)i)), i);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sort_reverse_sorted(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 9; i >= 0; i--)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    r8e_array_sort(ctx, arr, cmp_numeric, NULL);

    for (int i = 0; i < 10; i++)
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, (uint32_t)i)), i);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_reverse(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    r8e_array_reverse(ctx, arr);

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 5);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 4)), 1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_join(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));
    r8e_array_push(ctx, arr, r8e_from_int32(3));

    char buf[64];
    int n = r8e_array_join(ctx, arr, ",", to_str_int, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(strcmp(buf, "1,2,3") == 0);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_map(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 3; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EArray *mapped = r8e_array_map(ctx, arr, cb_double, R8E_UNDEFINED);
    ASSERT_NOT_NULL(mapped);
    ASSERT_EQ(r8e_array_length(mapped), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, mapped, 0)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, mapped, 1)), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, mapped, 2)), 6);

    r8e_array_destroy(ctx, mapped);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_filter(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 10; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EArray *filtered = r8e_array_filter(ctx, arr, cb_gt5, R8E_UNDEFINED);
    ASSERT_NOT_NULL(filtered);
    ASSERT_EQ(r8e_array_length(filtered), 5); /* 6,7,8,9,10 */

    r8e_array_destroy(ctx, filtered);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_reduce(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 4; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EValue result = r8e_array_reduce(ctx, arr, cb_sum, r8e_from_int32(0));
    ASSERT(R8E_IS_INT32(result));
    ASSERT_EQ(r8e_get_int32(result), 10); /* 1+2+3+4 */

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_find(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 5; i <= 9; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EValue found = r8e_array_find(ctx, arr, cb_eq7, R8E_UNDEFINED);
    ASSERT(R8E_IS_INT32(found));
    ASSERT_EQ(r8e_get_int32(found), 7);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_find_index(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 5; i <= 9; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    int32_t idx = r8e_array_find_index(ctx, arr, cb_eq7, R8E_UNDEFINED);
    ASSERT_EQ(idx, 2); /* 7 is at index 2 (5,6,7,...) */

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_some(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(-1));
    r8e_array_push(ctx, arr, r8e_from_int32(0));
    r8e_array_push(ctx, arr, r8e_from_int32(5));

    ASSERT(r8e_array_some(ctx, arr, cb_positive, R8E_UNDEFINED));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_every(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));
    r8e_array_push(ctx, arr, r8e_from_int32(3));

    ASSERT(r8e_array_every(ctx, arr, cb_positive, R8E_UNDEFINED));

    /* Add a non-positive value */
    r8e_array_push(ctx, arr, r8e_from_int32(-1));
    ASSERT(!r8e_array_every(ctx, arr, cb_positive, R8E_UNDEFINED));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_fill(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(0));

    r8e_array_fill(ctx, arr, r8e_from_int32(99), 1, 4);

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 0);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 99);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 99);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 3)), 99);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 4)), 0);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_copy_within(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* copyWithin(0, 3, 5) -> copies elements at 3,4 to 0,1 */
    r8e_array_copy_within(ctx, arr, 0, 3, 5);

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 5);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 3);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_is_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    R8EValue arr_val = 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)arr;
    ASSERT(r8e_array_is_array(arr_val));

    /* Non-pointer values should not be arrays */
    ASSERT(!r8e_array_is_array(r8e_from_int32(42)));
    ASSERT(!r8e_array_is_array(R8E_UNDEFINED));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_at_positive_and_negative(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 10; i <= 50; i += 10)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    ASSERT_EQ(r8e_get_int32(r8e_array_at(ctx, arr, 0)), 10);
    ASSERT_EQ(r8e_get_int32(r8e_array_at(ctx, arr, 4)), 50);
    ASSERT_EQ(r8e_get_int32(r8e_array_at(ctx, arr, -1)), 50);
    ASSERT_EQ(r8e_get_int32(r8e_array_at(ctx, arr, -2)), 40);
    ASSERT_EQ(r8e_array_at(ctx, arr, 5), R8E_UNDEFINED);
    ASSERT_EQ(r8e_array_at(ctx, arr, -6), R8E_UNDEFINED);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sparse_fallback(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* Setting a very high index should trigger sparse mode */
    r8e_array_set(ctx, arr, 1000000, r8e_from_int32(1));

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1000000)), 1);

    /* Index 0 should be undefined */
    ASSERT_EQ(r8e_array_get(ctx, arr, 0), R8E_UNDEFINED);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Dense array push/pop edge cases
 * ========================================================================= */

static void test_pop_empty_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* Pop from empty should return UNDEFINED */
    R8EValue v1 = r8e_array_pop(ctx, arr);
    ASSERT_EQ(v1, R8E_UNDEFINED);
    ASSERT_EQ(r8e_array_length(arr), 0);

    /* Multiple pops from empty should all return UNDEFINED */
    R8EValue v2 = r8e_array_pop(ctx, arr);
    ASSERT_EQ(v2, R8E_UNDEFINED);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_push_pop_interleaved(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* Interleave push and pop operations */
    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));
    ASSERT_EQ(r8e_get_int32(r8e_array_pop(ctx, arr)), 2);

    r8e_array_push(ctx, arr, r8e_from_int32(3));
    r8e_array_push(ctx, arr, r8e_from_int32(4));
    ASSERT_EQ(r8e_array_length(arr), 3);

    ASSERT_EQ(r8e_get_int32(r8e_array_pop(ctx, arr)), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_pop(ctx, arr)), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_pop(ctx, arr)), 1);
    ASSERT_EQ(r8e_array_length(arr), 0);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Shift/unshift edge cases
 * ========================================================================= */

static void test_shift_empty_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    R8EValue v = r8e_array_shift(ctx, arr);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_EQ(r8e_array_length(arr), 0);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_unshift_single_element(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(2));
    r8e_array_push(ctx, arr, r8e_from_int32(3));

    R8EValue val = r8e_from_int32(1);
    r8e_array_unshift(ctx, arr, &val, 1);

    ASSERT_EQ(r8e_array_length(arr), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 3);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Splice edge cases
 * ========================================================================= */

static void test_splice_at_start(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 4; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* Delete 2 items at start */
    R8EArray *removed = r8e_array_new(ctx, 2);
    r8e_array_splice(ctx, arr, 0, 2, NULL, 0, removed);

    ASSERT_EQ(r8e_array_length(arr), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 4);

    ASSERT_EQ(r8e_array_length(removed), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, removed, 0)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, removed, 1)), 2);

    r8e_array_destroy(ctx, removed);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_splice_at_end(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 4; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* Insert at end */
    R8EValue ins[2] = { r8e_from_int32(5), r8e_from_int32(6) };
    r8e_array_splice(ctx, arr, 4, 0, ins, 2, NULL);

    ASSERT_EQ(r8e_array_length(arr), 6);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 4)), 5);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 5)), 6);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_splice_replace_more_than_deleted(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 3; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* Delete 1, insert 3: net growth of 2 */
    R8EValue ins[3] = { r8e_from_int32(20), r8e_from_int32(21), r8e_from_int32(22) };
    r8e_array_splice(ctx, arr, 1, 1, ins, 3, NULL);

    ASSERT_EQ(r8e_array_length(arr), 5);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 20);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 21);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 3)), 22);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 4)), 3);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Slice edge cases
 * ========================================================================= */

static void test_slice_empty_range(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* slice(2, 2) = empty */
    R8EArray *sl = r8e_array_slice(ctx, arr, 2, 2);
    ASSERT_NOT_NULL(sl);
    ASSERT_EQ(r8e_array_length(sl), 0);

    r8e_array_destroy(ctx, sl);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_slice_full_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i * 10));

    /* slice(0, length) = copy of entire array */
    R8EArray *sl = r8e_array_slice(ctx, arr, 0, 5);
    ASSERT_NOT_NULL(sl);
    ASSERT_EQ(r8e_array_length(sl), 5);

    for (int i = 0; i < 5; i++)
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, sl, (uint32_t)i)), i * 10);

    r8e_array_destroy(ctx, sl);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: indexOf/lastIndexOf/includes edge cases
 * ========================================================================= */

static void test_index_of_from_offset(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));
    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));

    /* Start search from index 2 */
    ASSERT_EQ(r8e_array_index_of(ctx, arr, r8e_from_int32(1), 2), 2);
    ASSERT_EQ(r8e_array_index_of(ctx, arr, r8e_from_int32(2), 2), 3);
    ASSERT_EQ(r8e_array_index_of(ctx, arr, r8e_from_int32(1), 3), -1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_includes_from_offset(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(10));
    r8e_array_push(ctx, arr, r8e_from_int32(20));
    r8e_array_push(ctx, arr, r8e_from_int32(30));

    ASSERT(r8e_array_includes(ctx, arr, r8e_from_int32(20), 0));
    ASSERT(r8e_array_includes(ctx, arr, r8e_from_int32(20), 1));
    ASSERT(!r8e_array_includes(ctx, arr, r8e_from_int32(20), 2));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Sort edge cases
 * ========================================================================= */

static void test_sort_single_element(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(42));
    r8e_array_sort(ctx, arr, cmp_numeric, NULL);

    ASSERT_EQ(r8e_array_length(arr), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 42);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sort_empty_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_sort(ctx, arr, cmp_numeric, NULL);
    ASSERT_EQ(r8e_array_length(arr), 0);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sort_duplicates(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    int32_t vals[] = {3, 1, 2, 1, 3, 2};
    for (int i = 0; i < 6; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(vals[i]));

    r8e_array_sort(ctx, arr, cmp_numeric, NULL);

    int32_t expected[] = {1, 1, 2, 2, 3, 3};
    for (int i = 0; i < 6; i++)
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, (uint32_t)i)), expected[i]);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* Reverse numeric compare for sort */
static int cmp_reverse(R8EContext *ctx, R8EValue a, R8EValue b, void *ud) {
    (void)ctx; (void)ud;
    int32_t ia = R8E_IS_INT32(a) ? r8e_get_int32(a) : 0;
    int32_t ib = R8E_IS_INT32(b) ? r8e_get_int32(b) : 0;
    return (ib > ia) - (ib < ia);
}

static void test_sort_custom_comparator_reverse(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    r8e_array_sort(ctx, arr, cmp_reverse, NULL);

    for (int i = 0; i < 5; i++)
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, (uint32_t)i)), 5 - i);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Reverse edge cases
 * ========================================================================= */

static void test_reverse_single_element(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(42));
    r8e_array_reverse(ctx, arr);

    ASSERT_EQ(r8e_array_length(arr), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 42);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_reverse_even_count(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 4; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    r8e_array_reverse(ctx, arr);

    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 4);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 3);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 2)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 3)), 1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Array length semantics
 * ========================================================================= */

static void test_set_beyond_length_extends(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* Setting index 5 on empty array should set length to 6 */
    r8e_array_set(ctx, arr, 5, r8e_from_int32(50));

    ASSERT_EQ(r8e_array_length(arr), 6);
    ASSERT_EQ(r8e_array_get(ctx, arr, 0), R8E_UNDEFINED);
    ASSERT_EQ(r8e_array_get(ctx, arr, 4), R8E_UNDEFINED);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 5)), 50);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_sparse_then_dense_get(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* Push creates a dense region */
    r8e_array_push(ctx, arr, r8e_from_int32(1));
    r8e_array_push(ctx, arr, r8e_from_int32(2));

    /* Set a very high index to trigger sparse if the impl supports it */
    r8e_array_set(ctx, arr, 1000000, r8e_from_int32(999));

    /* All original values accessible */
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 0)), 1);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1)), 2);
    ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, 1000000)), 999);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: forEach callback
 * ========================================================================= */

static int foreach_accumulator = 0;

static R8EValue cb_accumulate(R8EContext *ctx, R8EValue this_val,
                               R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)this_val; (void)index; (void)arr;
    if (R8E_IS_INT32(val))
        foreach_accumulator += r8e_get_int32(val);
    return R8E_UNDEFINED;
}

static void test_for_each(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    foreach_accumulator = 0;
    r8e_array_for_each(ctx, arr, cb_accumulate, R8E_UNDEFINED);
    ASSERT_EQ(foreach_accumulator, 15); /* 1+2+3+4+5 */

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: find and findIndex edge cases
 * ========================================================================= */

/* Callback: value == 99 (not in array) */
static R8EValue cb_eq99(R8EContext *ctx, R8EValue this_val,
                         R8EValue val, uint32_t index, R8EArray *arr) {
    (void)ctx; (void)this_val; (void)index; (void)arr;
    if (R8E_IS_INT32(val) && r8e_get_int32(val) == 99)
        return R8E_TRUE;
    return R8E_FALSE;
}

static void test_find_not_found(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EValue found = r8e_array_find(ctx, arr, cb_eq99, R8E_UNDEFINED);
    ASSERT_EQ(found, R8E_UNDEFINED);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_find_index_not_found(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    int32_t idx = r8e_array_find_index(ctx, arr, cb_eq99, R8E_UNDEFINED);
    ASSERT_EQ(idx, -1);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: some/every on empty arrays
 * ========================================================================= */

static void test_some_empty_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* some() on empty should return false */
    ASSERT(!r8e_array_some(ctx, arr, cb_positive, R8E_UNDEFINED));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_every_empty_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* every() on empty should return true (vacuous truth) */
    ASSERT(r8e_array_every(ctx, arr, cb_positive, R8E_UNDEFINED));

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: reduce on empty and single-element arrays
 * ========================================================================= */

static void test_reduce_single_element(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    r8e_array_push(ctx, arr, r8e_from_int32(42));

    R8EValue result = r8e_array_reduce(ctx, arr, cb_sum, r8e_from_int32(0));
    ASSERT_EQ(r8e_get_int32(result), 42);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_reduce_with_initial_value(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 1; i <= 3; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    /* sum with initial = 100 */
    R8EValue result = r8e_array_reduce(ctx, arr, cb_sum, r8e_from_int32(100));
    ASSERT_EQ(r8e_get_int32(result), 106); /* 100+1+2+3 */

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: fill edge cases
 * ========================================================================= */

static void test_fill_entire_array(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    for (int i = 0; i < 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(0));

    r8e_array_fill(ctx, arr, r8e_from_int32(7), 0, 5);

    for (int i = 0; i < 5; i++)
        ASSERT_EQ(r8e_get_int32(r8e_array_get(ctx, arr, (uint32_t)i)), 7);

    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Map/filter on empty arrays
 * ========================================================================= */

static void test_map_empty(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    R8EArray *mapped = r8e_array_map(ctx, arr, cb_double, R8E_UNDEFINED);
    ASSERT_NOT_NULL(mapped);
    ASSERT_EQ(r8e_array_length(mapped), 0);

    r8e_array_destroy(ctx, mapped);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_filter_none_pass(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* All values <= 5, filter keeps > 5 */
    for (int i = 1; i <= 5; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EArray *filtered = r8e_array_filter(ctx, arr, cb_gt5, R8E_UNDEFINED);
    ASSERT_NOT_NULL(filtered);
    ASSERT_EQ(r8e_array_length(filtered), 0);

    r8e_array_destroy(ctx, filtered);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

static void test_filter_all_pass(void) {
    R8EContext *ctx = make_ctx();
    R8EArray *arr = r8e_array_new(ctx, 0);

    /* All values > 5, filter keeps > 5 */
    for (int i = 6; i <= 10; i++)
        r8e_array_push(ctx, arr, r8e_from_int32(i));

    R8EArray *filtered = r8e_array_filter(ctx, arr, cb_gt5, R8E_UNDEFINED);
    ASSERT_NOT_NULL(filtered);
    ASSERT_EQ(r8e_array_length(filtered), 5);

    r8e_array_destroy(ctx, filtered);
    r8e_array_destroy(ctx, arr);
    free_ctx(ctx);
}

/* =========================================================================
 * Test Runner
 * ========================================================================= */

void run_array_tests(void) {
    printf("\n=== Array Tests ===\n");

    /* Original tests */
    RUN_TEST(test_create_empty_array);
    RUN_TEST(test_push_pop);
    RUN_TEST(test_get_set_by_index);
    RUN_TEST(test_array_growth_1000);
    RUN_TEST(test_shift_unshift);
    RUN_TEST(test_splice_insert);
    RUN_TEST(test_splice_remove);
    RUN_TEST(test_splice_replace);
    RUN_TEST(test_slice_positive);
    RUN_TEST(test_slice_negative);
    RUN_TEST(test_index_of);
    RUN_TEST(test_last_index_of);
    RUN_TEST(test_includes);
    RUN_TEST(test_sort_numbers);
    RUN_TEST(test_sort_already_sorted);
    RUN_TEST(test_sort_reverse_sorted);
    RUN_TEST(test_reverse);
    RUN_TEST(test_join);
    RUN_TEST(test_map);
    RUN_TEST(test_filter);
    RUN_TEST(test_reduce);
    RUN_TEST(test_find);
    RUN_TEST(test_find_index);
    RUN_TEST(test_some);
    RUN_TEST(test_every);
    RUN_TEST(test_fill);
    RUN_TEST(test_copy_within);
    RUN_TEST(test_is_array);
    RUN_TEST(test_at_positive_and_negative);
    RUN_TEST(test_sparse_fallback);

    /* NEW: Push/pop edge cases */
    RUN_TEST(test_pop_empty_array);
    RUN_TEST(test_push_pop_interleaved);

    /* NEW: Shift/unshift */
    RUN_TEST(test_shift_empty_array);
    RUN_TEST(test_unshift_single_element);

    /* NEW: Splice edge cases */
    RUN_TEST(test_splice_at_start);
    RUN_TEST(test_splice_at_end);
    RUN_TEST(test_splice_replace_more_than_deleted);

    /* NEW: Slice edge cases */
    RUN_TEST(test_slice_empty_range);
    RUN_TEST(test_slice_full_array);

    /* NEW: indexOf/includes with offsets */
    RUN_TEST(test_index_of_from_offset);
    RUN_TEST(test_includes_from_offset);

    /* NEW: Sort edge cases */
    RUN_TEST(test_sort_single_element);
    RUN_TEST(test_sort_empty_array);
    RUN_TEST(test_sort_duplicates);
    RUN_TEST(test_sort_custom_comparator_reverse);

    /* NEW: Reverse edge cases */
    RUN_TEST(test_reverse_single_element);
    RUN_TEST(test_reverse_even_count);

    /* NEW: Length semantics */
    RUN_TEST(test_set_beyond_length_extends);
    RUN_TEST(test_sparse_then_dense_get);

    /* NEW: forEach */
    RUN_TEST(test_for_each);

    /* NEW: find/findIndex not found */
    RUN_TEST(test_find_not_found);
    RUN_TEST(test_find_index_not_found);

    /* NEW: some/every on empty */
    RUN_TEST(test_some_empty_array);
    RUN_TEST(test_every_empty_array);

    /* NEW: reduce edge cases */
    RUN_TEST(test_reduce_single_element);
    RUN_TEST(test_reduce_with_initial_value);

    /* NEW: fill edge cases */
    RUN_TEST(test_fill_entire_array);

    /* NEW: map/filter edge cases */
    RUN_TEST(test_map_empty);
    RUN_TEST(test_filter_none_pass);
    RUN_TEST(test_filter_all_pass);

    printf("\n  Array Tests: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf("\n");
}

#ifdef TEST_ARRAY_MAIN
int main(void) {
    run_array_tests();
    return tests_failed > 0 ? 1 : 0;
}
#endif
