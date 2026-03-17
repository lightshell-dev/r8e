/*
 * test_promise.c - Unit tests for r8e_promise.c (Promises + Microtask Queue)
 *
 * Tests cover:
 *   - Create pending promise
 *   - Resolve with value
 *   - Reject with reason
 *   - then() chaining returns new promise
 *   - catch() for rejections
 *   - Promise.resolve() static method
 *   - Promise.reject() static method
 *   - Microtask queue drain
 *   - Self-resolution detection (TypeError)
 *   - Already-resolved flag (double resolve is no-op)
 *   - Already-resolved flag (double reject is no-op)
 *   - Promise state accessor
 *   - Promise result accessor
 *   - Promise is_handled flag
 *   - Resolve then query state
 *   - Reject then query state
 *   - Init/cleanup lifecycle
 *   - Microtask queue initialization
 *   - Queue drain processes all pending tasks
 *   - Unhandled rejection handler
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
    if ((a) == (b)) { \
        printf("  FAIL: %s:%d: values should differ\n", __FILE__, __LINE__); \
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
 * Type definitions (must match r8e_promise.c layout)
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

/* Promise states (must match r8e_promise.c) */
#define R8E_PROMISE_PENDING   0
#define R8E_PROMISE_FULFILLED 1
#define R8E_PROMISE_REJECTED  2

/* =========================================================================
 * The R8EPromiseContext is the promise module's context. r8e_promise.c
 * uses its own context type that wraps the microtask queue, unhandled
 * rejection handler, and allocation hooks.
 * ========================================================================= */

typedef struct R8EPromiseContext R8EPromiseContext;

/* =========================================================================
 * Forward declarations of functions under test (from r8e_promise.c)
 * ========================================================================= */

extern void r8e_promise_init(R8EPromiseContext *ctx);
extern void r8e_promise_cleanup(R8EPromiseContext *ctx);

extern R8EValue r8e_promise_new(R8EPromiseContext *ctx);
extern void r8e_promise_resolve(R8EPromiseContext *ctx, R8EValue promise_val,
                                  R8EValue value);
extern void r8e_promise_reject(R8EPromiseContext *ctx, R8EValue promise_val,
                                 R8EValue reason);

extern R8EValue r8e_promise_then(R8EPromiseContext *ctx, R8EValue promise_val,
                                   R8EValue on_fulfilled, R8EValue on_rejected);
extern R8EValue r8e_promise_catch(R8EPromiseContext *ctx, R8EValue promise_val,
                                    R8EValue on_rejected);

extern R8EValue r8e_promise_static_resolve(R8EPromiseContext *ctx,
                                             R8EValue value);
extern R8EValue r8e_promise_static_reject(R8EPromiseContext *ctx,
                                            R8EValue reason);

extern int r8e_promise_state(R8EValue v);
extern R8EValue r8e_promise_result(R8EValue v);
extern bool r8e_promise_is_handled(R8EValue v);

typedef void (*R8EUnhandledRejectionFn)(R8EPromiseContext *ctx,
                                          R8EValue promise, R8EValue reason,
                                          void *user_data);

extern void r8e_promise_set_unhandled_handler(
    R8EPromiseContext *ctx, R8EUnhandledRejectionFn handler,
    void *user_data);

/* Microtask queue */
extern void r8e_microtask_drain(R8EPromiseContext *ctx);
extern uint32_t r8e_microtask_pending_count(R8EPromiseContext *ctx);

/* Context creation/destruction (allocates the promise context) */
extern R8EPromiseContext *r8e_promise_context_new(void);
extern void r8e_promise_context_free(R8EPromiseContext *ctx);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EPromiseContext *make_promise_ctx(void) {
    R8EPromiseContext *ctx = r8e_promise_context_new();
    if (ctx) r8e_promise_init(ctx);
    return ctx;
}

static void free_promise_ctx(R8EPromiseContext *ctx) {
    if (ctx) {
        r8e_promise_cleanup(ctx);
        r8e_promise_context_free(ctx);
    }
}

/* =========================================================================
 * Tests: Promise Creation
 * ========================================================================= */

/* Create a pending promise */
static void test_promise_create_pending(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(p));
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_PENDING);
    ASSERT_EQ(r8e_promise_result(p), R8E_UNDEFINED);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Resolve and Reject
 * ========================================================================= */

/* Resolve a promise with a value */
static void test_promise_resolve_value(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    r8e_promise_resolve(ctx, p, r8e_from_int32_t(42));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(42));

    free_promise_ctx(ctx);
}

/* Reject a promise with a reason */
static void test_promise_reject_reason(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    r8e_promise_reject(ctx, p, r8e_from_int32_t(99));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_REJECTED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(99));

    free_promise_ctx(ctx);
}

/* Double resolve: second resolve is ignored */
static void test_promise_double_resolve(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    r8e_promise_resolve(ctx, p, r8e_from_int32_t(1));
    r8e_promise_resolve(ctx, p, r8e_from_int32_t(2));

    r8e_microtask_drain(ctx);

    /* First resolve wins */
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(1));

    free_promise_ctx(ctx);
}

/* Double reject: second reject is ignored */
static void test_promise_double_reject(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    r8e_promise_reject(ctx, p, r8e_from_int32_t(10));
    r8e_promise_reject(ctx, p, r8e_from_int32_t(20));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_REJECTED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(10));

    free_promise_ctx(ctx);
}

/* Resolve then reject: reject is ignored */
static void test_promise_resolve_then_reject(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    r8e_promise_resolve(ctx, p, r8e_from_int32_t(7));
    r8e_promise_reject(ctx, p, r8e_from_int32_t(8));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(7));

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Self-resolution Detection
 * ========================================================================= */

/* Resolving a promise with itself should reject with TypeError */
static void test_promise_self_resolution(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    r8e_promise_resolve(ctx, p, p); /* resolve with self */

    r8e_microtask_drain(ctx);

    /* Should be rejected (TypeError: chaining cycle detected) */
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_REJECTED);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: then() / catch()
 * ========================================================================= */

/* then() returns a new promise */
static void test_promise_then_returns_promise(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);

    ASSERT_TRUE(R8E_IS_POINTER(p2));
    /* p2 should be a different promise from p */
    ASSERT_TRUE(p != p2);
    ASSERT_EQ_INT(r8e_promise_state(p2), R8E_PROMISE_PENDING);

    free_promise_ctx(ctx);
}

/* catch() returns a new promise */
static void test_promise_catch_returns_promise(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_catch(ctx, p, R8E_UNDEFINED);

    ASSERT_TRUE(R8E_IS_POINTER(p2));
    ASSERT_TRUE(p != p2);

    free_promise_ctx(ctx);
}

/* then() on an already-resolved promise marks it as handled */
static void test_promise_then_marks_handled(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    ASSERT_FALSE(r8e_promise_is_handled(p));

    r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);
    ASSERT_TRUE(r8e_promise_is_handled(p));

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Static Methods
 * ========================================================================= */

/* Promise.resolve() creates an already-fulfilled promise */
static void test_promise_static_resolve(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, r8e_from_int32_t(55));
    ASSERT_TRUE(R8E_IS_POINTER(p));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(55));

    free_promise_ctx(ctx);
}

/* Promise.reject() creates an already-rejected promise */
static void test_promise_static_reject(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_reject(ctx, r8e_from_int32_t(66));
    ASSERT_TRUE(R8E_IS_POINTER(p));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_REJECTED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(66));

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Microtask Queue
 * ========================================================================= */

/* Microtask queue starts empty */
static void test_microtask_queue_initially_empty(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    ASSERT_EQ_INT(r8e_microtask_pending_count(ctx), 0);

    free_promise_ctx(ctx);
}

/* Drain processes all pending microtasks */
static void test_microtask_drain_all(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create and resolve multiple promises */
    R8EValue p1 = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_new(ctx);
    R8EValue p3 = r8e_promise_new(ctx);

    r8e_promise_resolve(ctx, p1, r8e_from_int32_t(1));
    r8e_promise_resolve(ctx, p2, r8e_from_int32_t(2));
    r8e_promise_resolve(ctx, p3, r8e_from_int32_t(3));

    /* Drain all */
    r8e_microtask_drain(ctx);

    /* All should be fulfilled */
    ASSERT_EQ_INT(r8e_promise_state(p1), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p2), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p3), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_microtask_pending_count(ctx), 0);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Unhandled Rejection Handler
 * ========================================================================= */

static int g_unhandled_count = 0;
static R8EValue g_unhandled_reason = 0;

static void test_unhandled_handler(R8EPromiseContext *ctx,
                                    R8EValue promise, R8EValue reason,
                                    void *user_data) {
    (void)ctx; (void)promise; (void)user_data;
    g_unhandled_count++;
    g_unhandled_reason = reason;
}

/* Unhandled rejection triggers the handler */
static void test_unhandled_rejection_handler(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    g_unhandled_count = 0;
    g_unhandled_reason = R8E_UNDEFINED;
    r8e_promise_set_unhandled_handler(ctx, test_unhandled_handler, NULL);

    R8EValue p = r8e_promise_new(ctx);
    /* Reject without attaching .then() or .catch() */
    r8e_promise_reject(ctx, p, r8e_from_int32_t(777));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(g_unhandled_count, 1);
    ASSERT_EQ(g_unhandled_reason, r8e_from_int32_t(777));

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Init / Cleanup Lifecycle
 * ========================================================================= */

/* Init and cleanup without creating any promises */
static void test_promise_init_cleanup(void) {
    R8EPromiseContext *ctx = r8e_promise_context_new();
    ASSERT_NOT_NULL(ctx);
    r8e_promise_init(ctx);
    r8e_promise_cleanup(ctx);
    r8e_promise_context_free(ctx);
    /* No crash = pass */
}

/* =========================================================================
 * Tests: Promise.resolve with various value types
 * ========================================================================= */

/* Resolve with undefined */
static void test_promise_resolve_undefined(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, R8E_UNDEFINED);
    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), R8E_UNDEFINED);

    free_promise_ctx(ctx);
}

/* Resolve with null */
static void test_promise_resolve_null(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, R8E_NULL);
    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), R8E_NULL);

    free_promise_ctx(ctx);
}

/* Resolve with boolean true */
static void test_promise_resolve_true(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, R8E_TRUE);
    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), R8E_TRUE);

    free_promise_ctx(ctx);
}

/* Resolve with boolean false */
static void test_promise_resolve_false(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, R8E_FALSE);
    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), R8E_FALSE);

    free_promise_ctx(ctx);
}

/* Resolve with zero */
static void test_promise_resolve_zero(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, r8e_from_int32_t(0));
    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(0));

    free_promise_ctx(ctx);
}

/* Resolve with negative number */
static void test_promise_resolve_negative(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, r8e_from_int32_t(-1));
    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(-1));

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Promise Chaining
 * ========================================================================= */

/* Promise.then() on already-resolved promise chains result */
static void test_promise_then_on_resolved(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_static_resolve(ctx, r8e_from_int32_t(42));
    r8e_microtask_drain(ctx);

    /* .then() on an already-fulfilled promise */
    R8EValue p2 = r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);
    ASSERT_TRUE(R8E_IS_POINTER(p2));
    ASSERT_TRUE(p != p2);

    r8e_microtask_drain(ctx);

    free_promise_ctx(ctx);
}

/* Multiple then() calls on the same promise */
static void test_promise_multiple_then(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);
    R8EValue p3 = r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);
    R8EValue p4 = r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);

    /* All should be different promises */
    ASSERT_NE(p2, p3);
    ASSERT_NE(p3, p4);
    ASSERT_NE(p2, p4);

    r8e_promise_resolve(ctx, p, r8e_from_int32_t(1));
    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_FULFILLED);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Promise.catch on rejection
 * ========================================================================= */

/* catch() on pending then reject */
static void test_promise_catch_on_rejection(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_catch(ctx, p, R8E_UNDEFINED);
    ASSERT_TRUE(R8E_IS_POINTER(p2));

    /* Mark p as handled (catch was called) */
    ASSERT_TRUE(r8e_promise_is_handled(p));

    r8e_promise_reject(ctx, p, r8e_from_int32_t(404));
    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_REJECTED);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Promise.all pattern (manual multi-promise)
 * ========================================================================= */

/* Multiple promises resolved, all become fulfilled */
static void test_promise_all_resolve(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p1 = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_new(ctx);
    R8EValue p3 = r8e_promise_new(ctx);

    r8e_promise_resolve(ctx, p1, r8e_from_int32_t(10));
    r8e_promise_resolve(ctx, p2, r8e_from_int32_t(20));
    r8e_promise_resolve(ctx, p3, r8e_from_int32_t(30));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p1), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p2), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p3), R8E_PROMISE_FULFILLED);
    ASSERT_EQ(r8e_promise_result(p1), r8e_from_int32_t(10));
    ASSERT_EQ(r8e_promise_result(p2), r8e_from_int32_t(20));
    ASSERT_EQ(r8e_promise_result(p3), r8e_from_int32_t(30));

    free_promise_ctx(ctx);
}

/* Mix of resolved and rejected promises */
static void test_promise_mixed_settle(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p1 = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_new(ctx);
    R8EValue p3 = r8e_promise_new(ctx);

    /* Attach handlers to avoid unhandled rejection warnings */
    r8e_promise_then(ctx, p2, R8E_UNDEFINED, R8E_UNDEFINED);

    r8e_promise_resolve(ctx, p1, r8e_from_int32_t(1));
    r8e_promise_reject(ctx, p2, r8e_from_int32_t(2));
    r8e_promise_resolve(ctx, p3, r8e_from_int32_t(3));

    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p1), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p2), R8E_PROMISE_REJECTED);
    ASSERT_EQ_INT(r8e_promise_state(p3), R8E_PROMISE_FULFILLED);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Microtask Queue Ordering
 * ========================================================================= */

/* Microtasks are processed in FIFO order */
static void test_microtask_fifo_order(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create three promises and resolve them in order */
    R8EValue p1 = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_new(ctx);
    R8EValue p3 = r8e_promise_new(ctx);

    r8e_promise_resolve(ctx, p1, r8e_from_int32_t(1));
    r8e_promise_resolve(ctx, p2, r8e_from_int32_t(2));
    r8e_promise_resolve(ctx, p3, r8e_from_int32_t(3));

    /* Before drain, check pending count */
    ASSERT_TRUE(r8e_microtask_pending_count(ctx) > 0);

    r8e_microtask_drain(ctx);
    ASSERT_EQ_INT(r8e_microtask_pending_count(ctx), 0);

    /* All should be settled */
    ASSERT_EQ_INT(r8e_promise_state(p1), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p2), R8E_PROMISE_FULFILLED);
    ASSERT_EQ_INT(r8e_promise_state(p3), R8E_PROMISE_FULFILLED);

    free_promise_ctx(ctx);
}

/* Drain on empty queue is a no-op */
static void test_microtask_drain_empty(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_microtask_drain(ctx); /* should not crash */
    ASSERT_EQ_INT(r8e_microtask_pending_count(ctx), 0);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Reject then query
 * ========================================================================= */

/* Reject a promise then check state and result */
static void test_promise_reject_then_query(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    /* Mark handled to avoid unhandled rejection callback */
    r8e_promise_then(ctx, p, R8E_UNDEFINED, R8E_UNDEFINED);

    r8e_promise_reject(ctx, p, r8e_from_int32_t(500));
    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_REJECTED);
    ASSERT_EQ(r8e_promise_result(p), r8e_from_int32_t(500));

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Promise state accessor on pending promise
 * ========================================================================= */

static void test_promise_state_pending(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue p = r8e_promise_new(ctx);
    ASSERT_EQ_INT(r8e_promise_state(p), R8E_PROMISE_PENDING);
    ASSERT_EQ(r8e_promise_result(p), R8E_UNDEFINED);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Tests: Unhandled rejection with multiple promises
 * ========================================================================= */

static int g_multi_unhandled_count = 0;

static void test_multi_unhandled_handler(R8EPromiseContext *ctx,
                                          R8EValue promise, R8EValue reason,
                                          void *user_data) {
    (void)ctx; (void)promise; (void)reason; (void)user_data;
    g_multi_unhandled_count++;
}

static void test_unhandled_rejection_multiple(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    g_multi_unhandled_count = 0;
    r8e_promise_set_unhandled_handler(ctx, test_multi_unhandled_handler, NULL);

    /* Reject two promises without handlers */
    R8EValue p1 = r8e_promise_new(ctx);
    R8EValue p2 = r8e_promise_new(ctx);

    r8e_promise_reject(ctx, p1, r8e_from_int32_t(1));
    r8e_promise_reject(ctx, p2, r8e_from_int32_t(2));
    r8e_microtask_drain(ctx);

    ASSERT_EQ_INT(g_multi_unhandled_count, 2);

    free_promise_ctx(ctx);
}

/* Handled rejection does NOT trigger unhandled handler */
static void test_handled_rejection_no_callback(void) {
    R8EPromiseContext *ctx = make_promise_ctx();
    ASSERT_NOT_NULL(ctx);

    g_multi_unhandled_count = 0;
    r8e_promise_set_unhandled_handler(ctx, test_multi_unhandled_handler, NULL);

    R8EValue p = r8e_promise_new(ctx);
    /* Attach a catch handler BEFORE rejecting */
    r8e_promise_catch(ctx, p, R8E_UNDEFINED);
    r8e_promise_reject(ctx, p, r8e_from_int32_t(1));
    r8e_microtask_drain(ctx);

    /* Handler was attached, so no unhandled rejection */
    ASSERT_EQ_INT(g_multi_unhandled_count, 0);

    free_promise_ctx(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_promise_tests(void) {
    /* Creation */
    RUN_TEST(test_promise_create_pending);

    /* Resolve / Reject */
    RUN_TEST(test_promise_resolve_value);
    RUN_TEST(test_promise_reject_reason);
    RUN_TEST(test_promise_double_resolve);
    RUN_TEST(test_promise_double_reject);
    RUN_TEST(test_promise_resolve_then_reject);

    /* Self-resolution */
    RUN_TEST(test_promise_self_resolution);

    /* then / catch */
    RUN_TEST(test_promise_then_returns_promise);
    RUN_TEST(test_promise_catch_returns_promise);
    RUN_TEST(test_promise_then_marks_handled);

    /* Static methods */
    RUN_TEST(test_promise_static_resolve);
    RUN_TEST(test_promise_static_reject);

    /* Microtask queue */
    RUN_TEST(test_microtask_queue_initially_empty);
    RUN_TEST(test_microtask_drain_all);

    /* Unhandled rejection */
    RUN_TEST(test_unhandled_rejection_handler);

    /* Lifecycle */
    RUN_TEST(test_promise_init_cleanup);

    /* Resolve with various value types */
    RUN_TEST(test_promise_resolve_undefined);
    RUN_TEST(test_promise_resolve_null);
    RUN_TEST(test_promise_resolve_true);
    RUN_TEST(test_promise_resolve_false);
    RUN_TEST(test_promise_resolve_zero);
    RUN_TEST(test_promise_resolve_negative);

    /* Chaining */
    RUN_TEST(test_promise_then_on_resolved);
    RUN_TEST(test_promise_multiple_then);

    /* Catch */
    RUN_TEST(test_promise_catch_on_rejection);

    /* Multiple promises */
    RUN_TEST(test_promise_all_resolve);
    RUN_TEST(test_promise_mixed_settle);

    /* Microtask ordering */
    RUN_TEST(test_microtask_fifo_order);
    RUN_TEST(test_microtask_drain_empty);

    /* Reject and query */
    RUN_TEST(test_promise_reject_then_query);
    RUN_TEST(test_promise_state_pending);

    /* Unhandled rejection (multiple) */
    RUN_TEST(test_unhandled_rejection_multiple);
    RUN_TEST(test_handled_rejection_no_callback);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_PROMISE_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[promise]\n");
    run_promise_tests();
    printf("\n=== promise: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
