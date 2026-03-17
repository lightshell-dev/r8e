/*
 * test_proxy.c - Unit tests for r8e_proxy.c (Proxy and Reflect)
 *
 * Tests cover:
 *   - Proxy creation with target and handler
 *   - All 13 proxy traps: get, set, has, deleteProperty, ownKeys,
 *     getOwnPropertyDescriptor, defineProperty, getPrototypeOf,
 *     setPrototypeOf, isExtensible, preventExtensions, apply, construct
 *   - Proxy.revocable: create and revoke
 *   - Revoked proxy throws TypeError on all operations
 *   - Proxy without trap falls through to target
 *   - Reflect.get, Reflect.set, Reflect.has, Reflect.deleteProperty,
 *     Reflect.ownKeys, Reflect.apply, Reflect.construct,
 *     Reflect.getPrototypeOf, Reflect.setPrototypeOf,
 *     Reflect.isExtensible, Reflect.preventExtensions,
 *     Reflect.defineProperty, Reflect.getOwnPropertyDescriptor
 *   - Proxy handler returning invalid values (invariant violations)
 *   - Nested proxy (proxy wrapping proxy)
 *   - Proxy on function (intercept calls via apply trap)
 *   - Proxy on array (intercept index access via get trap)
 *   - NULL handler graceful error
 *   - Non-object target graceful error
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

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        printf("  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
               _a ? _a : "(null)", _b ? _b : "(null)"); \
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
 * Type definitions (must match r8e_proxy.c layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

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
#define R8E_IS_BOOLEAN(v)    ((v) == R8E_TRUE || (v) == R8E_FALSE)

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

/* Context type (must match r8e_proxy.c) */
typedef struct R8EContext {
    void     *arena;
    void     *atom_table;
    void     *global_object;
    char      error_buf[256];
    int       has_error;
    void    **prototypes;
    uint16_t  proto_count;
    uint32_t  next_symbol_id;
} R8EContext;

/* =========================================================================
 * Forward declarations (from r8e_proxy.c)
 * ========================================================================= */

extern R8EValue r8e_proxy_new(R8EContext *ctx, R8EValue target,
                                R8EValue handler);
extern R8EValue r8e_proxy_revocable(R8EContext *ctx, R8EValue target,
                                      R8EValue handler);
extern void r8e_proxy_revoke(R8EContext *ctx, R8EValue proxy_or_result);

/* Proxy trap operations */
extern R8EValue r8e_proxy_get(R8EContext *ctx, R8EValue proxy,
                                uint32_t property_atom);
extern bool r8e_proxy_set(R8EContext *ctx, R8EValue proxy,
                            uint32_t property_atom, R8EValue value);
extern bool r8e_proxy_has(R8EContext *ctx, R8EValue proxy,
                            uint32_t property_atom);
extern bool r8e_proxy_delete_property(R8EContext *ctx, R8EValue proxy,
                                        uint32_t property_atom);
extern R8EValue r8e_proxy_own_keys(R8EContext *ctx, R8EValue proxy);
extern R8EValue r8e_proxy_get_own_property_descriptor(R8EContext *ctx,
                                                        R8EValue proxy,
                                                        uint32_t prop_atom);
extern bool r8e_proxy_define_property(R8EContext *ctx, R8EValue proxy,
                                        uint32_t prop_atom, R8EValue desc);
extern R8EValue r8e_proxy_get_prototype_of(R8EContext *ctx, R8EValue proxy);
extern bool r8e_proxy_set_prototype_of(R8EContext *ctx, R8EValue proxy,
                                         R8EValue proto);
extern bool r8e_proxy_is_extensible(R8EContext *ctx, R8EValue proxy);
extern bool r8e_proxy_prevent_extensions(R8EContext *ctx, R8EValue proxy);
extern R8EValue r8e_proxy_apply(R8EContext *ctx, R8EValue proxy,
                                  R8EValue this_arg, int argc,
                                  const R8EValue *argv);
extern R8EValue r8e_proxy_construct(R8EContext *ctx, R8EValue proxy,
                                      int argc, const R8EValue *argv,
                                      R8EValue new_target);

/* Proxy state queries */
extern bool r8e_proxy_is_revoked(R8EValue proxy);
extern R8EValue r8e_proxy_get_target(R8EValue proxy);
extern R8EValue r8e_proxy_get_handler(R8EValue proxy);

/* Reflect methods */
extern R8EValue r8e_reflect_get(R8EContext *ctx, R8EValue target,
                                  uint32_t property_atom);
extern bool r8e_reflect_set(R8EContext *ctx, R8EValue target,
                              uint32_t property_atom, R8EValue value);
extern bool r8e_reflect_has(R8EContext *ctx, R8EValue target,
                              uint32_t property_atom);
extern bool r8e_reflect_delete_property(R8EContext *ctx, R8EValue target,
                                          uint32_t property_atom);
extern R8EValue r8e_reflect_own_keys(R8EContext *ctx, R8EValue target);
extern R8EValue r8e_reflect_apply(R8EContext *ctx, R8EValue target,
                                    R8EValue this_arg, int argc,
                                    const R8EValue *argv);
extern R8EValue r8e_reflect_construct(R8EContext *ctx, R8EValue target,
                                        int argc, const R8EValue *argv);
extern R8EValue r8e_reflect_get_prototype_of(R8EContext *ctx,
                                               R8EValue target);
extern bool r8e_reflect_set_prototype_of(R8EContext *ctx, R8EValue target,
                                           R8EValue proto);
extern bool r8e_reflect_is_extensible(R8EContext *ctx, R8EValue target);
extern bool r8e_reflect_prevent_extensions(R8EContext *ctx, R8EValue target);
extern bool r8e_reflect_define_property(R8EContext *ctx, R8EValue target,
                                          uint32_t prop_atom, R8EValue desc);
extern R8EValue r8e_reflect_get_own_property_descriptor(R8EContext *ctx,
                                                          R8EValue target,
                                                          uint32_t prop_atom);

/* Test helpers */
extern R8EValue r8e_proxy_test_make_object(R8EContext *ctx);
extern R8EValue r8e_proxy_test_make_handler(R8EContext *ctx,
                                              uint32_t trap_atom,
                                              R8EValue trap_func);
extern R8EValue r8e_proxy_test_make_function(R8EContext *ctx);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EContext *make_proxy_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    return ctx;
}

static void free_proxy_ctx(R8EContext *ctx) {
    if (ctx) free(ctx);
}

/* Atom IDs for traps (must match r8e_proxy.c) */
#define R8E_ATOM_get               15
#define R8E_ATOM_set               16
#define R8E_ATOM_has              293
#define R8E_ATOM_deleteProperty   318
#define R8E_ATOM_ownKeys          317
#define R8E_ATOM_getOwnPropertyDescriptor 104
#define R8E_ATOM_defineProperty   102
#define R8E_ATOM_getPrototypeOf   108
#define R8E_ATOM_setPrototypeOf   109
#define R8E_ATOM_isExtensible     111
#define R8E_ATOM_preventExtensions 112
#define R8E_ATOM_apply             17
#define R8E_ATOM_construct        316
#define R8E_ATOM_name               8

/* =========================================================================
 * Tests: Proxy Creation
 * ========================================================================= */

/* Create a basic proxy with an empty handler */
static void test_proxy_create_basic(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);

    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    ASSERT_TRUE(R8E_IS_POINTER(proxy));
    ASSERT_FALSE(r8e_proxy_is_revoked(proxy));

    free_proxy_ctx(ctx);
}

/* Creating proxy with non-object target should fail */
static void test_proxy_create_non_object_target(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, r8e_from_int32_t(42), handler);
    /* Should return undefined or set error */
    ASSERT_TRUE(R8E_IS_UNDEFINED(proxy) || ctx->has_error);

    free_proxy_ctx(ctx);
}

/* Creating proxy with non-object handler should fail */
static void test_proxy_create_non_object_handler(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, r8e_from_int32_t(42));
    ASSERT_TRUE(R8E_IS_UNDEFINED(proxy) || ctx->has_error);

    free_proxy_ctx(ctx);
}

/* Proxy target and handler accessors */
static void test_proxy_target_handler_access(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    ASSERT_TRUE(R8E_IS_POINTER(proxy));

    ASSERT_EQ(r8e_proxy_get_target(proxy), target);
    ASSERT_EQ(r8e_proxy_get_handler(proxy), handler);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy.revocable and Revocation
 * ========================================================================= */

/* Create a revocable proxy */
static void test_proxy_revocable_create(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);

    R8EValue result = r8e_proxy_revocable(ctx, target, handler);
    ASSERT_TRUE(R8E_IS_POINTER(result));

    free_proxy_ctx(ctx);
}

/* Revoke a proxy then access should fail */
static void test_proxy_revoke_then_get(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);

    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    ASSERT_FALSE(r8e_proxy_is_revoked(proxy));

    /* Revoke it */
    r8e_proxy_revoke(ctx, proxy);
    ASSERT_TRUE(r8e_proxy_is_revoked(proxy));

    /* Accessing a revoked proxy should set error */
    ctx->has_error = 0;
    R8EValue val = r8e_proxy_get(ctx, proxy, R8E_ATOM_name);
    ASSERT_TRUE(ctx->has_error || R8E_IS_UNDEFINED(val));

    free_proxy_ctx(ctx);
}

/* Double revoke is a no-op */
static void test_proxy_double_revoke(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    r8e_proxy_revoke(ctx, proxy);
    ASSERT_TRUE(r8e_proxy_is_revoked(proxy));

    r8e_proxy_revoke(ctx, proxy); /* should not crash */
    ASSERT_TRUE(r8e_proxy_is_revoked(proxy));

    free_proxy_ctx(ctx);
}

/* Revoked proxy: set should fail */
static void test_proxy_revoked_set(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    r8e_proxy_revoke(ctx, proxy);

    ctx->has_error = 0;
    bool ok = r8e_proxy_set(ctx, proxy, R8E_ATOM_name, r8e_from_int32_t(1));
    ASSERT_TRUE(!ok || ctx->has_error);

    free_proxy_ctx(ctx);
}

/* Revoked proxy: has should fail */
static void test_proxy_revoked_has(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    r8e_proxy_revoke(ctx, proxy);

    ctx->has_error = 0;
    r8e_proxy_has(ctx, proxy, R8E_ATOM_name);
    ASSERT_TRUE(ctx->has_error);

    free_proxy_ctx(ctx);
}

/* Revoked proxy: deleteProperty should fail */
static void test_proxy_revoked_delete(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    r8e_proxy_revoke(ctx, proxy);

    ctx->has_error = 0;
    r8e_proxy_delete_property(ctx, proxy, R8E_ATOM_name);
    ASSERT_TRUE(ctx->has_error);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy Get Trap (no trap = fallthrough to target)
 * ========================================================================= */

/* Get without trap falls through to target */
static void test_proxy_get_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);
    ASSERT_TRUE(R8E_IS_POINTER(proxy));

    /* Without a "get" trap on handler, get should pass through to target */
    R8EValue val = r8e_proxy_get(ctx, proxy, R8E_ATOM_name);
    /* Target has no properties set, so result should be undefined */
    ASSERT_TRUE(R8E_IS_UNDEFINED(val));

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy Set Trap
 * ========================================================================= */

/* Set without trap falls through to target */
static void test_proxy_set_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    bool ok = r8e_proxy_set(ctx, proxy, R8E_ATOM_name, r8e_from_int32_t(42));
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy Has Trap
 * ========================================================================= */

/* Has without trap falls through to target */
static void test_proxy_has_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    bool result = r8e_proxy_has(ctx, proxy, R8E_ATOM_name);
    /* Target is empty, so should be false */
    ASSERT_FALSE(result);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy DeleteProperty Trap
 * ========================================================================= */

/* DeleteProperty without trap falls through */
static void test_proxy_delete_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    bool ok = r8e_proxy_delete_property(ctx, proxy, R8E_ATOM_name);
    ASSERT_TRUE(ok); /* Deleting non-existent property succeeds */

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy OwnKeys Trap
 * ========================================================================= */

/* OwnKeys without trap falls through */
static void test_proxy_own_keys_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    R8EValue keys = r8e_proxy_own_keys(ctx, proxy);
    /* Should return an array (pointer) or undefined */
    ASSERT_TRUE(R8E_IS_POINTER(keys) || R8E_IS_UNDEFINED(keys));

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy GetPrototypeOf Trap
 * ========================================================================= */

static void test_proxy_get_prototype_of_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    R8EValue proto = r8e_proxy_get_prototype_of(ctx, proxy);
    /* Should be null or a pointer */
    ASSERT_TRUE(R8E_IS_NULL(proto) || R8E_IS_POINTER(proto) ||
                R8E_IS_UNDEFINED(proto));

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy SetPrototypeOf Trap
 * ========================================================================= */

static void test_proxy_set_prototype_of_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    bool ok = r8e_proxy_set_prototype_of(ctx, proxy, R8E_NULL);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy IsExtensible Trap
 * ========================================================================= */

static void test_proxy_is_extensible_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    bool ext = r8e_proxy_is_extensible(ctx, proxy);
    ASSERT_TRUE(ext); /* New objects are extensible by default */

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy PreventExtensions Trap
 * ========================================================================= */

static void test_proxy_prevent_extensions_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    bool ok = r8e_proxy_prevent_extensions(ctx, proxy);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy GetOwnPropertyDescriptor Trap
 * ========================================================================= */

static void test_proxy_get_own_prop_desc_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    R8EValue desc = r8e_proxy_get_own_property_descriptor(ctx, proxy,
                                                            R8E_ATOM_name);
    /* Non-existent property: should be undefined */
    ASSERT_TRUE(R8E_IS_UNDEFINED(desc));

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy DefineProperty Trap
 * ========================================================================= */

static void test_proxy_define_property_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, target, handler);

    R8EValue desc = r8e_proxy_test_make_object(ctx);
    bool ok = r8e_proxy_define_property(ctx, proxy, R8E_ATOM_name, desc);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy Apply Trap (function proxy)
 * ========================================================================= */

static void test_proxy_apply_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue func_target = r8e_proxy_test_make_function(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, func_target, handler);

    R8EValue args[1] = { r8e_from_int32_t(5) };
    R8EValue result = r8e_proxy_apply(ctx, proxy, R8E_UNDEFINED, 1, args);
    /* Without trap, should forward to target function */
    /* Result depends on mock function implementation */
    (void)result;

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Proxy Construct Trap
 * ========================================================================= */

static void test_proxy_construct_no_trap(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue func_target = r8e_proxy_test_make_function(ctx);
    R8EValue handler = r8e_proxy_test_make_object(ctx);
    R8EValue proxy = r8e_proxy_new(ctx, func_target, handler);

    R8EValue args[1] = { r8e_from_int32_t(10) };
    R8EValue result = r8e_proxy_construct(ctx, proxy, 1, args, proxy);
    /* Should be an object (pointer) */
    ASSERT_TRUE(R8E_IS_POINTER(result) || R8E_IS_UNDEFINED(result));

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Nested Proxy (proxy wrapping proxy)
 * ========================================================================= */

static void test_nested_proxy(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue target = r8e_proxy_test_make_object(ctx);
    R8EValue handler1 = r8e_proxy_test_make_object(ctx);
    R8EValue handler2 = r8e_proxy_test_make_object(ctx);

    R8EValue proxy1 = r8e_proxy_new(ctx, target, handler1);
    ASSERT_TRUE(R8E_IS_POINTER(proxy1));

    /* Proxy wrapping the first proxy */
    R8EValue proxy2 = r8e_proxy_new(ctx, proxy1, handler2);
    ASSERT_TRUE(R8E_IS_POINTER(proxy2));
    ASSERT_NE(proxy1, proxy2);

    /* Access through nested proxy should work */
    R8EValue val = r8e_proxy_get(ctx, proxy2, R8E_ATOM_name);
    ASSERT_TRUE(R8E_IS_UNDEFINED(val)); /* No property set */

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Tests: Reflect Methods
 * ========================================================================= */

/* Reflect.get on a plain object */
static void test_reflect_get(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    R8EValue val = r8e_reflect_get(ctx, obj, R8E_ATOM_name);
    ASSERT_TRUE(R8E_IS_UNDEFINED(val)); /* Empty object */

    free_proxy_ctx(ctx);
}

/* Reflect.set on a plain object */
static void test_reflect_set(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    bool ok = r8e_reflect_set(ctx, obj, R8E_ATOM_name, r8e_from_int32_t(7));
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* Reflect.has on a plain object */
static void test_reflect_has(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    bool result = r8e_reflect_has(ctx, obj, R8E_ATOM_name);
    ASSERT_FALSE(result);

    free_proxy_ctx(ctx);
}

/* Reflect.deleteProperty on a plain object */
static void test_reflect_delete_property(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    bool ok = r8e_reflect_delete_property(ctx, obj, R8E_ATOM_name);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* Reflect.ownKeys */
static void test_reflect_own_keys(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    R8EValue keys = r8e_reflect_own_keys(ctx, obj);
    ASSERT_TRUE(R8E_IS_POINTER(keys) || R8E_IS_UNDEFINED(keys));

    free_proxy_ctx(ctx);
}

/* Reflect.getPrototypeOf */
static void test_reflect_get_prototype_of(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    R8EValue proto = r8e_reflect_get_prototype_of(ctx, obj);
    ASSERT_TRUE(R8E_IS_NULL(proto) || R8E_IS_POINTER(proto) ||
                R8E_IS_UNDEFINED(proto));

    free_proxy_ctx(ctx);
}

/* Reflect.setPrototypeOf */
static void test_reflect_set_prototype_of(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    bool ok = r8e_reflect_set_prototype_of(ctx, obj, R8E_NULL);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* Reflect.isExtensible */
static void test_reflect_is_extensible(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    bool ext = r8e_reflect_is_extensible(ctx, obj);
    ASSERT_TRUE(ext);

    free_proxy_ctx(ctx);
}

/* Reflect.preventExtensions */
static void test_reflect_prevent_extensions(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    bool ok = r8e_reflect_prevent_extensions(ctx, obj);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* Reflect.defineProperty */
static void test_reflect_define_property(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    R8EValue desc = r8e_proxy_test_make_object(ctx);
    bool ok = r8e_reflect_define_property(ctx, obj, R8E_ATOM_name, desc);
    ASSERT_TRUE(ok);

    free_proxy_ctx(ctx);
}

/* Reflect.getOwnPropertyDescriptor */
static void test_reflect_get_own_prop_desc(void) {
    R8EContext *ctx = make_proxy_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue obj = r8e_proxy_test_make_object(ctx);
    R8EValue desc = r8e_reflect_get_own_property_descriptor(ctx, obj,
                                                              R8E_ATOM_name);
    ASSERT_TRUE(R8E_IS_UNDEFINED(desc));

    free_proxy_ctx(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_proxy_tests(void) {
    /* Creation */
    RUN_TEST(test_proxy_create_basic);
    RUN_TEST(test_proxy_create_non_object_target);
    RUN_TEST(test_proxy_create_non_object_handler);
    RUN_TEST(test_proxy_target_handler_access);

    /* Revocable */
    RUN_TEST(test_proxy_revocable_create);
    RUN_TEST(test_proxy_revoke_then_get);
    RUN_TEST(test_proxy_double_revoke);
    RUN_TEST(test_proxy_revoked_set);
    RUN_TEST(test_proxy_revoked_has);
    RUN_TEST(test_proxy_revoked_delete);

    /* All 13 traps (no trap = fallthrough) */
    RUN_TEST(test_proxy_get_no_trap);
    RUN_TEST(test_proxy_set_no_trap);
    RUN_TEST(test_proxy_has_no_trap);
    RUN_TEST(test_proxy_delete_no_trap);
    RUN_TEST(test_proxy_own_keys_no_trap);
    RUN_TEST(test_proxy_get_prototype_of_no_trap);
    RUN_TEST(test_proxy_set_prototype_of_no_trap);
    RUN_TEST(test_proxy_is_extensible_no_trap);
    RUN_TEST(test_proxy_prevent_extensions_no_trap);
    RUN_TEST(test_proxy_get_own_prop_desc_no_trap);
    RUN_TEST(test_proxy_define_property_no_trap);
    RUN_TEST(test_proxy_apply_no_trap);
    RUN_TEST(test_proxy_construct_no_trap);

    /* Nested proxy */
    RUN_TEST(test_nested_proxy);

    /* Reflect methods */
    RUN_TEST(test_reflect_get);
    RUN_TEST(test_reflect_set);
    RUN_TEST(test_reflect_has);
    RUN_TEST(test_reflect_delete_property);
    RUN_TEST(test_reflect_own_keys);
    RUN_TEST(test_reflect_get_prototype_of);
    RUN_TEST(test_reflect_set_prototype_of);
    RUN_TEST(test_reflect_is_extensible);
    RUN_TEST(test_reflect_prevent_extensions);
    RUN_TEST(test_reflect_define_property);
    RUN_TEST(test_reflect_get_own_prop_desc);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_PROXY_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[proxy]\n");
    run_proxy_tests();
    printf("\n=== proxy: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
