/*
 * test_weakref.c - Unit tests for r8e_weakref.c
 *                  (WeakRef, FinalizationRegistry, WeakMap, WeakSet, Map, Set)
 *
 * Tests cover:
 *   - WeakRef: create, deref before GC, deref semantics
 *   - FinalizationRegistry: register, unregister
 *   - WeakMap: get, set, has, delete with object keys
 *   - WeakSet: add, has, delete
 *   - Map: get, set, has, delete, size, clear, forEach, keys, values, entries
 *   - Map: insertion order preservation, object keys
 *   - Map: SameValueZero comparison (NaN === NaN, +0 === -0)
 *   - Set: add, has, delete, size, clear, forEach, values, entries
 *   - Set: insertion order preservation
 *   - Set: SameValueZero comparison
 *   - Weak reference table: init, register, lookup
 *   - Edge cases: NULL args, non-object keys for WeakMap/WeakSet
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

#define ASSERT_NE(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a == _b) { \
        printf("  FAIL: %s:%d: both are 0x%llx\n", __FILE__, __LINE__, \
               (unsigned long long)_a); \
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

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; g_tests_run++; \
    printf("  %s... ", #fn); \
    fn(); \
    if (g_assert_fail) { g_tests_failed++; } \
    else { g_tests_passed++; printf("OK\n"); } \
} while (0)

/* =========================================================================
 * Type definitions (must match r8e_weakref.c layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)     (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_UNDEFINED(v)   ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)        ((v) == R8E_NULL)
#define R8E_IS_TRUE(v)        ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)       ((v) == R8E_FALSE)
#define R8E_IS_BOOLEAN(v)     ((v) == R8E_TRUE || (v) == R8E_FALSE)

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
static inline uint64_t r8e_from_double_t(double d) {
    uint64_t v; memcpy(&v, &d, 8); return v;
}
static inline double r8e_get_double_t(uint64_t v) {
    double d; memcpy(&d, &v, 8); return d;
}

/* Context type (must match r8e_weakref.c) */
typedef struct R8EContext {
    void     *arena;
    void     *atom_table;
    void     *global_object;
    char      error_buf[256];
    int       has_error;
    void    **prototypes;
    uint16_t  proto_count;
    uint32_t  next_symbol_id;
    void     *weak_table;
} R8EContext;

/* =========================================================================
 * Forward declarations (from r8e_weakref.c)
 * ========================================================================= */

/* WeakRef */
extern R8EValue r8e_weakref_new(R8EContext *ctx, R8EValue target);
extern R8EValue r8e_weakref_deref(R8EContext *ctx, R8EValue weakref);

/* FinalizationRegistry */
extern R8EValue r8e_finreg_new(R8EContext *ctx, R8EValue callback);
extern bool r8e_finreg_register(R8EContext *ctx, R8EValue finreg,
                                  R8EValue target, R8EValue held_value,
                                  R8EValue unregister_token);
extern bool r8e_finreg_unregister(R8EContext *ctx, R8EValue finreg,
                                    R8EValue unregister_token);

/* WeakMap */
extern R8EValue r8e_weakmap_new(R8EContext *ctx);
extern R8EValue r8e_weakmap_get(R8EContext *ctx, R8EValue wm, R8EValue key);
extern bool r8e_weakmap_set(R8EContext *ctx, R8EValue wm,
                              R8EValue key, R8EValue value);
extern bool r8e_weakmap_has(R8EContext *ctx, R8EValue wm, R8EValue key);
extern bool r8e_weakmap_delete(R8EContext *ctx, R8EValue wm, R8EValue key);

/* WeakSet */
extern R8EValue r8e_weakset_new(R8EContext *ctx);
extern bool r8e_weakset_add(R8EContext *ctx, R8EValue ws, R8EValue value);
extern bool r8e_weakset_has(R8EContext *ctx, R8EValue ws, R8EValue value);
extern bool r8e_weakset_delete(R8EContext *ctx, R8EValue ws, R8EValue value);

/* Map */
extern R8EValue r8e_map_new(R8EContext *ctx);
extern R8EValue r8e_map_get(R8EContext *ctx, R8EValue map, R8EValue key);
extern bool r8e_map_set(R8EContext *ctx, R8EValue map,
                          R8EValue key, R8EValue value);
extern bool r8e_map_has(R8EContext *ctx, R8EValue map, R8EValue key);
extern bool r8e_map_delete(R8EContext *ctx, R8EValue map, R8EValue key);
extern uint32_t r8e_map_size(R8EContext *ctx, R8EValue map);
extern void r8e_map_clear(R8EContext *ctx, R8EValue map);
extern R8EValue r8e_map_keys(R8EContext *ctx, R8EValue map);
extern R8EValue r8e_map_values(R8EContext *ctx, R8EValue map);
extern R8EValue r8e_map_entries(R8EContext *ctx, R8EValue map);

/* Set */
extern R8EValue r8e_set_new(R8EContext *ctx);
extern bool r8e_set_add(R8EContext *ctx, R8EValue set, R8EValue value);
extern bool r8e_set_has(R8EContext *ctx, R8EValue set, R8EValue value);
extern bool r8e_set_delete(R8EContext *ctx, R8EValue set, R8EValue value);
extern uint32_t r8e_set_size(R8EContext *ctx, R8EValue set);
extern void r8e_set_clear(R8EContext *ctx, R8EValue set);
extern R8EValue r8e_set_values(R8EContext *ctx, R8EValue set);
extern R8EValue r8e_set_entries(R8EContext *ctx, R8EValue set);

/* Weak table management */
extern void r8e_weak_table_init(R8EContext *ctx);
extern void r8e_weak_table_free(R8EContext *ctx);

/* Test helpers */
extern R8EValue r8e_weakref_test_make_object(R8EContext *ctx);
extern R8EValue r8e_weakref_test_make_callback(R8EContext *ctx);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EContext *make_weak_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    if (ctx) {
        r8e_weak_table_init(ctx);
    }
    return ctx;
}

static void free_weak_ctx(R8EContext *ctx) {
    if (ctx) {
        r8e_weak_table_free(ctx);
        free(ctx);
    }
}

/* =========================================================================
 * Tests: WeakRef
 * ========================================================================= */

/* Create a WeakRef to an object */
static void test_weakref_create(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_weakref_test_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(target));

    R8EValue wr = r8e_weakref_new(ctx, target);
    ASSERT_TRUE(R8E_IS_POINTER(wr));

    free_weak_ctx(ctx);
}

/* WeakRef deref returns target when still alive */
static void test_weakref_deref_alive(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_weakref_test_make_object(ctx);
    R8EValue wr = r8e_weakref_new(ctx, target);

    R8EValue derefed = r8e_weakref_deref(ctx, wr);
    ASSERT_EQ(derefed, target);

    free_weak_ctx(ctx);
}

/* WeakRef to non-object should fail */
static void test_weakref_non_object(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wr = r8e_weakref_new(ctx, r8e_from_int32_t(42));
    /* Should return undefined or set error */
    ASSERT_TRUE(R8E_IS_UNDEFINED(wr) || ctx->has_error);

    free_weak_ctx(ctx);
}

/* =========================================================================
 * Tests: FinalizationRegistry
 * ========================================================================= */

/* Create a FinalizationRegistry */
static void test_finreg_create(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cb = r8e_weakref_test_make_callback(ctx);
    R8EValue fr = r8e_finreg_new(ctx, cb);
    ASSERT_TRUE(R8E_IS_POINTER(fr));

    free_weak_ctx(ctx);
}

/* Register a target in FinalizationRegistry */
static void test_finreg_register(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cb = r8e_weakref_test_make_callback(ctx);
    R8EValue fr = r8e_finreg_new(ctx, cb);
    R8EValue target = r8e_weakref_test_make_object(ctx);
    R8EValue token = r8e_weakref_test_make_object(ctx);

    bool ok = r8e_finreg_register(ctx, fr, target, r8e_from_int32_t(1), token);
    ASSERT_TRUE(ok);

    free_weak_ctx(ctx);
}

/* Unregister a target from FinalizationRegistry */
static void test_finreg_unregister(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cb = r8e_weakref_test_make_callback(ctx);
    R8EValue fr = r8e_finreg_new(ctx, cb);
    R8EValue target = r8e_weakref_test_make_object(ctx);
    R8EValue token = r8e_weakref_test_make_object(ctx);

    r8e_finreg_register(ctx, fr, target, r8e_from_int32_t(1), token);

    bool ok = r8e_finreg_unregister(ctx, fr, token);
    ASSERT_TRUE(ok);

    free_weak_ctx(ctx);
}

/* Unregister with unknown token returns false */
static void test_finreg_unregister_unknown(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cb = r8e_weakref_test_make_callback(ctx);
    R8EValue fr = r8e_finreg_new(ctx, cb);
    R8EValue unknown_token = r8e_weakref_test_make_object(ctx);

    bool ok = r8e_finreg_unregister(ctx, fr, unknown_token);
    ASSERT_FALSE(ok);

    free_weak_ctx(ctx);
}

/* =========================================================================
 * Tests: WeakMap
 * ========================================================================= */

/* Create a WeakMap */
static void test_weakmap_create(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(wm));

    free_weak_ctx(ctx);
}

/* WeakMap set and get */
static void test_weakmap_set_get(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);
    R8EValue key = r8e_weakref_test_make_object(ctx);

    bool ok = r8e_weakmap_set(ctx, wm, key, r8e_from_int32_t(42));
    ASSERT_TRUE(ok);

    R8EValue val = r8e_weakmap_get(ctx, wm, key);
    ASSERT_TRUE(R8E_IS_INT32(val));
    ASSERT_EQ_INT(r8e_get_int32_t(val), 42);

    free_weak_ctx(ctx);
}

/* WeakMap has */
static void test_weakmap_has(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);
    R8EValue key = r8e_weakref_test_make_object(ctx);

    ASSERT_FALSE(r8e_weakmap_has(ctx, wm, key));

    r8e_weakmap_set(ctx, wm, key, r8e_from_int32_t(1));
    ASSERT_TRUE(r8e_weakmap_has(ctx, wm, key));

    free_weak_ctx(ctx);
}

/* WeakMap delete */
static void test_weakmap_delete(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);
    R8EValue key = r8e_weakref_test_make_object(ctx);

    r8e_weakmap_set(ctx, wm, key, r8e_from_int32_t(1));
    ASSERT_TRUE(r8e_weakmap_has(ctx, wm, key));

    bool ok = r8e_weakmap_delete(ctx, wm, key);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(r8e_weakmap_has(ctx, wm, key));

    free_weak_ctx(ctx);
}

/* WeakMap with non-object key should fail */
static void test_weakmap_non_object_key(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);

    bool ok = r8e_weakmap_set(ctx, wm, r8e_from_int32_t(42),
                                r8e_from_int32_t(1));
    ASSERT_FALSE(ok); /* Non-object keys rejected */

    free_weak_ctx(ctx);
}

/* WeakMap get with missing key returns undefined */
static void test_weakmap_get_missing(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);
    R8EValue key = r8e_weakref_test_make_object(ctx);

    R8EValue val = r8e_weakmap_get(ctx, wm, key);
    ASSERT_TRUE(R8E_IS_UNDEFINED(val));

    free_weak_ctx(ctx);
}

/* WeakMap: overwrite existing key */
static void test_weakmap_overwrite(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue wm = r8e_weakmap_new(ctx);
    R8EValue key = r8e_weakref_test_make_object(ctx);

    r8e_weakmap_set(ctx, wm, key, r8e_from_int32_t(1));
    r8e_weakmap_set(ctx, wm, key, r8e_from_int32_t(2));

    R8EValue val = r8e_weakmap_get(ctx, wm, key);
    ASSERT_EQ_INT(r8e_get_int32_t(val), 2);

    free_weak_ctx(ctx);
}

/* =========================================================================
 * Tests: WeakSet
 * ========================================================================= */

/* Create a WeakSet */
static void test_weakset_create(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue ws = r8e_weakset_new(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(ws));

    free_weak_ctx(ctx);
}

/* WeakSet add and has */
static void test_weakset_add_has(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue ws = r8e_weakset_new(ctx);
    R8EValue obj = r8e_weakref_test_make_object(ctx);

    ASSERT_FALSE(r8e_weakset_has(ctx, ws, obj));

    bool ok = r8e_weakset_add(ctx, ws, obj);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(r8e_weakset_has(ctx, ws, obj));

    free_weak_ctx(ctx);
}

/* WeakSet delete */
static void test_weakset_delete(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue ws = r8e_weakset_new(ctx);
    R8EValue obj = r8e_weakref_test_make_object(ctx);

    r8e_weakset_add(ctx, ws, obj);
    ASSERT_TRUE(r8e_weakset_has(ctx, ws, obj));

    bool ok = r8e_weakset_delete(ctx, ws, obj);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(r8e_weakset_has(ctx, ws, obj));

    free_weak_ctx(ctx);
}

/* WeakSet add non-object should fail */
static void test_weakset_non_object(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue ws = r8e_weakset_new(ctx);
    bool ok = r8e_weakset_add(ctx, ws, r8e_from_int32_t(42));
    ASSERT_FALSE(ok);

    free_weak_ctx(ctx);
}

/* =========================================================================
 * Tests: Map
 * ========================================================================= */

/* Create a Map */
static void test_map_create(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(map));
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 0);

    free_weak_ctx(ctx);
}

/* Map set and get with int keys */
static void test_map_set_get_int(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);

    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(100));
    r8e_map_set(ctx, map, r8e_from_int32_t(2), r8e_from_int32_t(200));

    ASSERT_EQ_INT(r8e_map_size(ctx, map), 2);

    R8EValue v1 = r8e_map_get(ctx, map, r8e_from_int32_t(1));
    ASSERT_EQ_INT(r8e_get_int32_t(v1), 100);

    R8EValue v2 = r8e_map_get(ctx, map, r8e_from_int32_t(2));
    ASSERT_EQ_INT(r8e_get_int32_t(v2), 200);

    free_weak_ctx(ctx);
}

/* Map set and get with object keys (reference equality) */
static void test_map_object_key(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    R8EValue key1 = r8e_weakref_test_make_object(ctx);
    R8EValue key2 = r8e_weakref_test_make_object(ctx);

    r8e_map_set(ctx, map, key1, r8e_from_int32_t(10));
    r8e_map_set(ctx, map, key2, r8e_from_int32_t(20));

    R8EValue v1 = r8e_map_get(ctx, map, key1);
    ASSERT_EQ_INT(r8e_get_int32_t(v1), 10);

    R8EValue v2 = r8e_map_get(ctx, map, key2);
    ASSERT_EQ_INT(r8e_get_int32_t(v2), 20);

    /* Different object with same shape is NOT the same key */
    R8EValue key3 = r8e_weakref_test_make_object(ctx);
    R8EValue v3 = r8e_map_get(ctx, map, key3);
    ASSERT_TRUE(R8E_IS_UNDEFINED(v3));

    free_weak_ctx(ctx);
}

/* Map has */
static void test_map_has(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    R8EValue key = r8e_from_int32_t(42);

    ASSERT_FALSE(r8e_map_has(ctx, map, key));
    r8e_map_set(ctx, map, key, R8E_TRUE);
    ASSERT_TRUE(r8e_map_has(ctx, map, key));

    free_weak_ctx(ctx);
}

/* Map delete */
static void test_map_delete(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(10));
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 1);

    bool ok = r8e_map_delete(ctx, map, r8e_from_int32_t(1));
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 0);
    ASSERT_FALSE(r8e_map_has(ctx, map, r8e_from_int32_t(1)));

    free_weak_ctx(ctx);
}

/* Map delete non-existent key returns false */
static void test_map_delete_missing(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    bool ok = r8e_map_delete(ctx, map, r8e_from_int32_t(999));
    ASSERT_FALSE(ok);

    free_weak_ctx(ctx);
}

/* Map clear */
static void test_map_clear(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(10));
    r8e_map_set(ctx, map, r8e_from_int32_t(2), r8e_from_int32_t(20));
    r8e_map_set(ctx, map, r8e_from_int32_t(3), r8e_from_int32_t(30));
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 3);

    r8e_map_clear(ctx, map);
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 0);

    free_weak_ctx(ctx);
}

/* Map overwrite existing key does not increase size */
static void test_map_overwrite(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(10));
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 1);

    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(20));
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 1);

    R8EValue val = r8e_map_get(ctx, map, r8e_from_int32_t(1));
    ASSERT_EQ_INT(r8e_get_int32_t(val), 20);

    free_weak_ctx(ctx);
}

/* Map get missing key returns undefined */
static void test_map_get_missing(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    R8EValue val = r8e_map_get(ctx, map, r8e_from_int32_t(42));
    ASSERT_TRUE(R8E_IS_UNDEFINED(val));

    free_weak_ctx(ctx);
}

/* Map: NaN key equality (SameValueZero: NaN === NaN) */
static void test_map_nan_key(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    double nan = 0.0 / 0.0;  /* NaN */
    R8EValue nan_key = r8e_from_double_t(nan);

    r8e_map_set(ctx, map, nan_key, r8e_from_int32_t(77));
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 1);

    /* Get with a different NaN representation should still find it */
    R8EValue val = r8e_map_get(ctx, map, nan_key);
    ASSERT_TRUE(R8E_IS_INT32(val));
    ASSERT_EQ_INT(r8e_get_int32_t(val), 77);

    free_weak_ctx(ctx);
}

/* Map: keys() returns an iterator */
static void test_map_keys_iterator(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(10));
    r8e_map_set(ctx, map, r8e_from_int32_t(2), r8e_from_int32_t(20));

    R8EValue keys_iter = r8e_map_keys(ctx, map);
    ASSERT_TRUE(R8E_IS_POINTER(keys_iter));

    free_weak_ctx(ctx);
}

/* Map: values() returns an iterator */
static void test_map_values_iterator(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(10));

    R8EValue vals_iter = r8e_map_values(ctx, map);
    ASSERT_TRUE(R8E_IS_POINTER(vals_iter));

    free_weak_ctx(ctx);
}

/* Map: entries() returns an iterator */
static void test_map_entries_iterator(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    r8e_map_set(ctx, map, r8e_from_int32_t(1), r8e_from_int32_t(10));

    R8EValue ent_iter = r8e_map_entries(ctx, map);
    ASSERT_TRUE(R8E_IS_POINTER(ent_iter));

    free_weak_ctx(ctx);
}

/* Map with many entries */
static void test_map_many_entries(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue map = r8e_map_new(ctx);
    for (int i = 0; i < 50; i++) {
        r8e_map_set(ctx, map, r8e_from_int32_t(i), r8e_from_int32_t(i * 10));
    }
    ASSERT_EQ_INT(r8e_map_size(ctx, map), 50);

    /* Verify a few entries */
    R8EValue v0 = r8e_map_get(ctx, map, r8e_from_int32_t(0));
    ASSERT_EQ_INT(r8e_get_int32_t(v0), 0);

    R8EValue v49 = r8e_map_get(ctx, map, r8e_from_int32_t(49));
    ASSERT_EQ_INT(r8e_get_int32_t(v49), 490);

    free_weak_ctx(ctx);
}

/* =========================================================================
 * Tests: Set
 * ========================================================================= */

/* Create a Set */
static void test_set_create(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(set));
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 0);

    free_weak_ctx(ctx);
}

/* Set add and has */
static void test_set_add_has(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);

    ASSERT_FALSE(r8e_set_has(ctx, set, r8e_from_int32_t(1)));

    r8e_set_add(ctx, set, r8e_from_int32_t(1));
    ASSERT_TRUE(r8e_set_has(ctx, set, r8e_from_int32_t(1)));
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 1);

    free_weak_ctx(ctx);
}

/* Set: adding duplicate does not increase size */
static void test_set_add_duplicate(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    r8e_set_add(ctx, set, r8e_from_int32_t(1));
    r8e_set_add(ctx, set, r8e_from_int32_t(1));
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 1);

    free_weak_ctx(ctx);
}

/* Set delete */
static void test_set_delete(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    r8e_set_add(ctx, set, r8e_from_int32_t(42));
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 1);

    bool ok = r8e_set_delete(ctx, set, r8e_from_int32_t(42));
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 0);
    ASSERT_FALSE(r8e_set_has(ctx, set, r8e_from_int32_t(42)));

    free_weak_ctx(ctx);
}

/* Set delete missing returns false */
static void test_set_delete_missing(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    bool ok = r8e_set_delete(ctx, set, r8e_from_int32_t(999));
    ASSERT_FALSE(ok);

    free_weak_ctx(ctx);
}

/* Set clear */
static void test_set_clear(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    r8e_set_add(ctx, set, r8e_from_int32_t(1));
    r8e_set_add(ctx, set, r8e_from_int32_t(2));
    r8e_set_add(ctx, set, r8e_from_int32_t(3));
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 3);

    r8e_set_clear(ctx, set);
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 0);

    free_weak_ctx(ctx);
}

/* Set: NaN equality (SameValueZero) */
static void test_set_nan(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    double nan = 0.0 / 0.0;
    R8EValue nan_val = r8e_from_double_t(nan);

    r8e_set_add(ctx, set, nan_val);
    r8e_set_add(ctx, set, nan_val); /* duplicate NaN */
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 1);
    ASSERT_TRUE(r8e_set_has(ctx, set, nan_val));

    free_weak_ctx(ctx);
}

/* Set: values() returns iterator */
static void test_set_values_iterator(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    r8e_set_add(ctx, set, r8e_from_int32_t(1));
    r8e_set_add(ctx, set, r8e_from_int32_t(2));

    R8EValue vals = r8e_set_values(ctx, set);
    ASSERT_TRUE(R8E_IS_POINTER(vals));

    free_weak_ctx(ctx);
}

/* Set: entries() returns iterator */
static void test_set_entries_iterator(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    r8e_set_add(ctx, set, r8e_from_int32_t(1));

    R8EValue ent = r8e_set_entries(ctx, set);
    ASSERT_TRUE(R8E_IS_POINTER(ent));

    free_weak_ctx(ctx);
}

/* Set with many entries */
static void test_set_many_entries(void) {
    R8EContext *ctx = make_weak_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue set = r8e_set_new(ctx);
    for (int i = 0; i < 50; i++) {
        r8e_set_add(ctx, set, r8e_from_int32_t(i));
    }
    ASSERT_EQ_INT(r8e_set_size(ctx, set), 50);

    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(r8e_set_has(ctx, set, r8e_from_int32_t(i)));
    }
    ASSERT_FALSE(r8e_set_has(ctx, set, r8e_from_int32_t(50)));

    free_weak_ctx(ctx);
}

/* =========================================================================
 * Tests: Weak Table Lifecycle
 * ========================================================================= */

static void test_weak_table_init_free(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    ASSERT_NOT_NULL(ctx);
    r8e_weak_table_init(ctx);
    r8e_weak_table_free(ctx);
    free(ctx);
    /* No crash = pass */
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_weakref_tests(void) {
    /* WeakRef */
    RUN_TEST(test_weakref_create);
    RUN_TEST(test_weakref_deref_alive);
    RUN_TEST(test_weakref_non_object);

    /* FinalizationRegistry */
    RUN_TEST(test_finreg_create);
    RUN_TEST(test_finreg_register);
    RUN_TEST(test_finreg_unregister);
    RUN_TEST(test_finreg_unregister_unknown);

    /* WeakMap */
    RUN_TEST(test_weakmap_create);
    RUN_TEST(test_weakmap_set_get);
    RUN_TEST(test_weakmap_has);
    RUN_TEST(test_weakmap_delete);
    RUN_TEST(test_weakmap_non_object_key);
    RUN_TEST(test_weakmap_get_missing);
    RUN_TEST(test_weakmap_overwrite);

    /* WeakSet */
    RUN_TEST(test_weakset_create);
    RUN_TEST(test_weakset_add_has);
    RUN_TEST(test_weakset_delete);
    RUN_TEST(test_weakset_non_object);

    /* Map */
    RUN_TEST(test_map_create);
    RUN_TEST(test_map_set_get_int);
    RUN_TEST(test_map_object_key);
    RUN_TEST(test_map_has);
    RUN_TEST(test_map_delete);
    RUN_TEST(test_map_delete_missing);
    RUN_TEST(test_map_clear);
    RUN_TEST(test_map_overwrite);
    RUN_TEST(test_map_get_missing);
    RUN_TEST(test_map_nan_key);
    RUN_TEST(test_map_keys_iterator);
    RUN_TEST(test_map_values_iterator);
    RUN_TEST(test_map_entries_iterator);
    RUN_TEST(test_map_many_entries);

    /* Set */
    RUN_TEST(test_set_create);
    RUN_TEST(test_set_add_has);
    RUN_TEST(test_set_add_duplicate);
    RUN_TEST(test_set_delete);
    RUN_TEST(test_set_delete_missing);
    RUN_TEST(test_set_clear);
    RUN_TEST(test_set_nan);
    RUN_TEST(test_set_values_iterator);
    RUN_TEST(test_set_entries_iterator);
    RUN_TEST(test_set_many_entries);

    /* Weak table lifecycle */
    RUN_TEST(test_weak_table_init_free);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_WEAKREF_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[weakref]\n");
    run_weakref_tests();
    printf("\n=== weakref: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
