/*
 * test_object.c - Unit tests for r8e_object.c (4-Tier CDOL Object Model)
 *
 * Tests cover:
 *   - Object creation (Tier 0)
 *   - Property set/get across all tiers
 *   - Tier promotion (0->1->2->3)
 *   - Property deletion
 *   - has() for existing and missing properties
 *   - keys() enumeration
 *   - Prototype chain (set proto, get inherited property)
 *   - get_with_proto chain walk
 *   - Object.freeze
 *   - Large object (100 properties via Robin Hood hash)
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
 * Test Harness Macros
 * ========================================================================= */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void name(void); \
    static void name##_register(void) __attribute__((constructor)); \
    static void name##_register(void) { (void)0; } \
    static void name(void)

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
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        printf("[FAIL]\n    Expected %llu == %llu\n    at %s:%d\n", \
               (unsigned long long)_a, (unsigned long long)_b, \
               __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a == _b) { \
        printf("[FAIL]\n    Expected %llu != %llu\n    at %s:%d\n", \
               (unsigned long long)_a, (unsigned long long)_b, \
               __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

/* =========================================================================
 * Duplicate the minimal types and declarations from the source file.
 * Each source module is self-contained, so tests must replicate the
 * minimal type context to link against the object code.
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)

static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}

#define R8E_OBJ_TIER_MASK  0x00000003u

#define R8E_PROTO_MAX    64
#define R8E_ATOM_EMPTY   0

/* Context must match the layout in r8e_object.c */
typedef struct R8EContext {
    R8EValue  proto_table[R8E_PROTO_MAX];
    uint32_t  proto_count;
    void *(*alloc)(size_t size, void *opaque);
    void *(*realloc_fn)(void *ptr, size_t size, void *opaque);
    void  (*free_fn)(void *ptr, void *opaque);
    void  *alloc_opaque;
    R8EValue  last_error;
} R8EContext;

/* Forward declarations of functions from r8e_object.c */
typedef struct R8EObjTier0 R8EObjTier0;

extern R8EObjTier0 *r8e_obj_new(R8EContext *ctx);
extern R8EObjTier0 *r8e_obj_new_with_proto(R8EContext *ctx, uint32_t proto_id);
extern void *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key, R8EValue val);
extern R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key);
extern bool r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key);
extern int r8e_obj_delete(R8EContext *ctx, void *obj, uint32_t key);
extern uint32_t r8e_obj_count(R8EContext *ctx, void *obj);
extern uint32_t r8e_obj_keys(R8EContext *ctx, void *obj,
                              uint32_t *out_keys, uint32_t max_keys);
extern void r8e_obj_set_prototype(R8EContext *ctx, void *obj, uint32_t proto_id);
extern R8EValue r8e_obj_get_with_proto(R8EContext *ctx, void *obj, uint32_t key);
extern void r8e_obj_freeze(R8EContext *ctx, void *obj);
extern bool r8e_obj_is_frozen(R8EContext *ctx, void *obj);
extern uint8_t r8e_obj_get_tier(void *obj);
extern void r8e_obj_destroy(R8EContext *ctx, void *obj);

/* =========================================================================
 * Helper: create a zeroed context with default allocators
 * ========================================================================= */

static R8EContext *make_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    /* NULL alloc/free means use stdlib malloc/free */
    return ctx;
}

static void free_ctx(R8EContext *ctx) {
    free(ctx);
}

/* =========================================================================
 * Tests
 * ========================================================================= */

TEST(test_create_empty_object) {
    R8EContext *ctx = make_ctx();
    R8EObjTier0 *obj = r8e_obj_new(ctx);
    ASSERT_NOT_NULL(obj);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 0);
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);
    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_set_one_property_tier0) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);
    ASSERT_NOT_NULL(obj);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(42));
    ASSERT_NOT_NULL(obj);
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);

    R8EValue v = r8e_obj_get(ctx, obj, 1);
    ASSERT(R8E_IS_INT32(v));
    ASSERT_EQ(r8e_get_int32(v), 42);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_promote_to_tier1) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(20));
    ASSERT_NOT_NULL(obj);
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 2);

    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 10);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 2)), 20);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_promote_to_tier2) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 5; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 100)));
        ASSERT_NOT_NULL(obj);
    }

    ASSERT_EQ(r8e_obj_get_tier(obj), 2);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 5);

    for (uint32_t i = 1; i <= 5; i++) {
        R8EValue v = r8e_obj_get(ctx, obj, i);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), (int32_t)(i * 100));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_promote_to_tier3) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 20; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
        ASSERT_NOT_NULL(obj);
    }

    ASSERT_EQ(r8e_obj_get_tier(obj), 3);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 20);

    for (uint32_t i = 1; i <= 20; i++) {
        R8EValue v = r8e_obj_get(ctx, obj, i);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), (int32_t)i);
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_property_deletion) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(42));
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);

    int rc = r8e_obj_delete(ctx, obj, 1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 0);

    /* Deleting a nonexistent key should not fail */
    rc = r8e_obj_delete(ctx, obj, 999);
    /* The return might be 0 or -1 depending on implementation; just verify
       count is still 0 */
    ASSERT_EQ(r8e_obj_count(ctx, obj), 0);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_has_existing_and_missing) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 10, r8e_from_int32(1));
    obj = r8e_obj_set(ctx, obj, 20, r8e_from_int32(2));

    ASSERT(r8e_obj_has(ctx, obj, 10));
    ASSERT(r8e_obj_has(ctx, obj, 20));
    ASSERT(!r8e_obj_has(ctx, obj, 30));
    ASSERT(!r8e_obj_has(ctx, obj, 0));

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_keys_enumeration) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Insert keys 5, 3, 7, 1 */
    obj = r8e_obj_set(ctx, obj, 5, r8e_from_int32(50));
    obj = r8e_obj_set(ctx, obj, 3, r8e_from_int32(30));
    obj = r8e_obj_set(ctx, obj, 7, r8e_from_int32(70));
    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));

    ASSERT_EQ(r8e_obj_count(ctx, obj), 4);

    uint32_t keys[8] = {0};
    uint32_t n = r8e_obj_keys(ctx, obj, keys, 8);
    ASSERT_EQ(n, 4);

    /* Verify all expected keys are present */
    bool found[8] = {false};
    for (uint32_t i = 0; i < n; i++) {
        if (keys[i] == 5) found[0] = true;
        else if (keys[i] == 3) found[1] = true;
        else if (keys[i] == 7) found[2] = true;
        else if (keys[i] == 1) found[3] = true;
    }
    ASSERT(found[0] && found[1] && found[2] && found[3]);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_prototype_chain) {
    R8EContext *ctx = make_ctx();

    /* Create a "prototype" object and register it in the prototype table */
    R8EObjTier0 *proto_obj = r8e_obj_new(ctx);
    ASSERT_NOT_NULL(proto_obj);

    void *proto = r8e_obj_set(ctx, proto_obj, 100, r8e_from_int32(999));
    ASSERT_NOT_NULL(proto);

    /* Store in prototype table at index 1 */
    R8EValue proto_ptr = 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)proto;
    ctx->proto_table[1] = proto_ptr;
    ctx->proto_count = 2;

    /* Create a child object with proto_id = 1 */
    R8EObjTier0 *child = r8e_obj_new_with_proto(ctx, 1);
    ASSERT_NOT_NULL(child);

    /* Child has no own property 100 */
    ASSERT(!r8e_obj_has(ctx, child, 100));

    /* But get_with_proto should walk the chain and find it */
    R8EValue v = r8e_obj_get_with_proto(ctx, child, 100);
    ASSERT(R8E_IS_INT32(v));
    ASSERT_EQ(r8e_get_int32(v), 999);

    r8e_obj_destroy(ctx, child);
    r8e_obj_destroy(ctx, proto);
    free_ctx(ctx);
}

TEST(test_get_with_proto_not_found) {
    R8EContext *ctx = make_ctx();

    R8EObjTier0 *obj = r8e_obj_new(ctx);
    ASSERT_NOT_NULL(obj);

    /* No prototype set (proto_id = 0), so chain walk ends immediately */
    R8EValue v = r8e_obj_get_with_proto(ctx, obj, 999);
    ASSERT_EQ(v, R8E_UNDEFINED);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_object_freeze) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(42));
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 42);

    r8e_obj_freeze(ctx, obj);
    ASSERT(r8e_obj_is_frozen(ctx, obj));

    /* Attempt to set a new property or modify existing - should be no-op
     * on a frozen object. The value should remain unchanged. */
    void *result = r8e_obj_set(ctx, obj, 1, r8e_from_int32(99));
    /* After freeze, the object pointer may be returned unchanged */
    R8EValue v = r8e_obj_get(ctx, result, 1);
    /* Frozen object: value should still be 42 (set is no-op) */
    ASSERT_EQ(r8e_get_int32(v), 42);

    r8e_obj_destroy(ctx, result);
    free_ctx(ctx);
}

TEST(test_large_object_100_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Set 100 properties */
    for (uint32_t i = 1; i <= 100; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 7)));
        ASSERT_NOT_NULL(obj);
    }

    ASSERT_EQ(r8e_obj_get_tier(obj), 3);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 100);

    /* Verify all 100 properties */
    for (uint32_t i = 1; i <= 100; i++) {
        R8EValue v = r8e_obj_get(ctx, obj, i);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), (int32_t)(i * 7));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_overwrite_existing_property) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 10);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(20));
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 20);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_get_missing_returns_undefined) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    R8EValue v = r8e_obj_get(ctx, obj, 42);
    ASSERT_EQ(v, R8E_UNDEFINED);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier_transitions_preserve_values) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Add properties one by one, verifying all previous ones survive promotion */
    for (uint32_t i = 1; i <= 20; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 3)));
        ASSERT_NOT_NULL(obj);

        /* Verify all properties 1..i are intact */
        for (uint32_t j = 1; j <= i; j++) {
            R8EValue v = r8e_obj_get(ctx, obj, j);
            ASSERT(R8E_IS_INT32(v));
            ASSERT_EQ(r8e_get_int32(v), (int32_t)(j * 3));
        }
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Tier 0 edge cases
 * ========================================================================= */

TEST(test_tier0_create_delete_remains_tier0) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Add one property, then delete it - should stay Tier 0 */
    obj = r8e_obj_set(ctx, obj, 5, r8e_from_int32(100));
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);

    int rc = r8e_obj_delete(ctx, obj, 5);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 0);
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);

    /* Can add a new property after deletion */
    obj = r8e_obj_set(ctx, obj, 7, r8e_from_int32(200));
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 7)), 200);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier0_overwrite_same_key) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);

    /* Overwrite same key: should stay Tier 0, not promote */
    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(20));
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 1);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 20);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier0_has_on_empty) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    ASSERT(!r8e_obj_has(ctx, obj, 0));
    ASSERT(!r8e_obj_has(ctx, obj, 1));
    ASSERT(!r8e_obj_has(ctx, obj, 0xFFFFFFFF));

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier0_store_special_values) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Store undefined */
    obj = r8e_obj_set(ctx, obj, 1, R8E_UNDEFINED);
    R8EValue v = r8e_obj_get(ctx, obj, 1);
    ASSERT_EQ(v, R8E_UNDEFINED);

    r8e_obj_destroy(ctx, obj);

    /* Store null */
    obj = r8e_obj_new(ctx);
    obj = r8e_obj_set(ctx, obj, 1, R8E_NULL);
    v = r8e_obj_get(ctx, obj, 1);
    ASSERT_EQ(v, R8E_NULL);

    r8e_obj_destroy(ctx, obj);

    /* Store true/false */
    obj = r8e_obj_new(ctx);
    obj = r8e_obj_set(ctx, obj, 1, R8E_TRUE);
    ASSERT_EQ(r8e_obj_get(ctx, obj, 1), R8E_TRUE);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Tier 1 detailed coverage
 * ========================================================================= */

TEST(test_tier1_exactly_2_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 10, r8e_from_int32(100));
    obj = r8e_obj_set(ctx, obj, 20, r8e_from_int32(200));
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 2);

    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 10)), 100);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 20)), 200);
    ASSERT(!r8e_obj_has(ctx, obj, 15));

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier1_exactly_3_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(20));
    obj = r8e_obj_set(ctx, obj, 3, r8e_from_int32(30));
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 3);

    for (uint32_t i = 1; i <= 3; i++) {
        ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, i)), (int32_t)(i * 10));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier1_exactly_4_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 4; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 10)));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 4);

    for (uint32_t i = 1; i <= 4; i++) {
        ASSERT(r8e_obj_has(ctx, obj, i));
        ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, i)), (int32_t)(i * 10));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier1_delete_middle_property) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(20));
    obj = r8e_obj_set(ctx, obj, 3, r8e_from_int32(30));
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);

    /* Delete the middle property */
    int rc = r8e_obj_delete(ctx, obj, 2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 2);

    /* Remaining properties should still be accessible */
    ASSERT(r8e_obj_has(ctx, obj, 1));
    ASSERT(!r8e_obj_has(ctx, obj, 2));
    ASSERT(r8e_obj_has(ctx, obj, 3));
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 10);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 3)), 30);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier1_overwrite_existing) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(20));
    ASSERT_EQ(r8e_obj_count(ctx, obj), 2);

    /* Overwrite key 2 */
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(99));
    ASSERT_EQ(r8e_obj_count(ctx, obj), 2); /* count stays 2 */
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 2)), 99);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Tier 2 detailed coverage (5-16 properties)
 * ========================================================================= */

TEST(test_tier2_exactly_5_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 5; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 5)));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 5);

    for (uint32_t i = 1; i <= 5; i++) {
        ASSERT(r8e_obj_has(ctx, obj, i));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier2_8_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 8; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 8);

    /* Verify all values */
    for (uint32_t i = 1; i <= 8; i++) {
        ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, i)), (int32_t)i);
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier2_12_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* 12 properties: >8, triggers binary search path in Tier 2 */
    for (uint32_t i = 1; i <= 12; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 3)));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 12);

    /* Verify: binary search should be used internally for count > 8 */
    for (uint32_t i = 1; i <= 12; i++) {
        ASSERT(r8e_obj_has(ctx, obj, i));
        ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, i)), (int32_t)(i * 3));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier2_16_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 16; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 16);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier2_delete_property) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 8; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);

    /* Delete from the middle */
    int rc = r8e_obj_delete(ctx, obj, 4);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 7);
    ASSERT(!r8e_obj_has(ctx, obj, 4));

    /* Other properties intact */
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 3)), 3);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 5)), 5);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Tier 3 detailed coverage (17+ properties, Robin Hood)
 * ========================================================================= */

TEST(test_tier3_50_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 50; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 11)));
        ASSERT_NOT_NULL(obj);
    }

    ASSERT_EQ(r8e_obj_get_tier(obj), 3);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 50);

    for (uint32_t i = 1; i <= 50; i++) {
        R8EValue v = r8e_obj_get(ctx, obj, i);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), (int32_t)(i * 11));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier3_delete_property) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 25; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 3);

    /* Delete several properties from the hash table */
    for (uint32_t i = 1; i <= 5; i++) {
        int rc = r8e_obj_delete(ctx, obj, i);
        ASSERT_EQ(rc, 0);
    }
    ASSERT_EQ(r8e_obj_count(ctx, obj), 20);

    /* Deleted properties are gone */
    for (uint32_t i = 1; i <= 5; i++) {
        ASSERT(!r8e_obj_has(ctx, obj, i));
        ASSERT_EQ(r8e_obj_get(ctx, obj, i), R8E_UNDEFINED);
    }

    /* Remaining properties intact */
    for (uint32_t i = 6; i <= 25; i++) {
        ASSERT(r8e_obj_has(ctx, obj, i));
        ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, i)), (int32_t)i);
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier3_overwrite_in_hash_table) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 20; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 3);

    /* Overwrite all values */
    for (uint32_t i = 1; i <= 20; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)(i * 100)));
    }
    ASSERT_EQ(r8e_obj_count(ctx, obj), 20); /* count unchanged */

    for (uint32_t i = 1; i <= 20; i++) {
        ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, i)), (int32_t)(i * 100));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_tier3_stress_100_properties_robin_hood) {
    /* Tests Robin Hood hash table with scattered keys to exercise collision handling */
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Use non-sequential keys to stress the hash distribution */
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t key = (i * 7919) % 10000 + 1; /* scattered primes */
        obj = r8e_obj_set(ctx, obj, key, r8e_from_int32((int32_t)i));
        ASSERT_NOT_NULL(obj);
    }

    ASSERT_EQ(r8e_obj_get_tier(obj), 3);

    /* Verify lookups work for all scattered keys */
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t key = (i * 7919) % 10000 + 1;
        ASSERT(r8e_obj_has(ctx, obj, key));
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Full tier chain promotion
 * ========================================================================= */

TEST(test_full_tier_chain_0_to_3) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    /* Tier 0: 0 properties */
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj), 0);

    /* Tier 0: 1 property */
    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(1));
    ASSERT_EQ(r8e_obj_get_tier(obj), 0);

    /* Promotion to Tier 1: 2 properties */
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(2));
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);

    /* Stay in Tier 1: 4 properties */
    obj = r8e_obj_set(ctx, obj, 3, r8e_from_int32(3));
    obj = r8e_obj_set(ctx, obj, 4, r8e_from_int32(4));
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);

    /* Promotion to Tier 2: 5 properties */
    obj = r8e_obj_set(ctx, obj, 5, r8e_from_int32(5));
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);

    /* Stay in Tier 2 up to 16 properties */
    for (uint32_t i = 6; i <= 16; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 2);

    /* Promotion to Tier 3: 17 properties */
    obj = r8e_obj_set(ctx, obj, 17, r8e_from_int32(17));
    ASSERT_EQ(r8e_obj_get_tier(obj), 3);

    /* Verify ALL properties survived every promotion */
    for (uint32_t i = 1; i <= 17; i++) {
        R8EValue v = r8e_obj_get(ctx, obj, i);
        ASSERT(R8E_IS_INT32(v));
        ASSERT_EQ(r8e_get_int32(v), (int32_t)i);
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Prototype chain with depth > 2
 * ========================================================================= */

TEST(test_prototype_chain_depth_3) {
    R8EContext *ctx = make_ctx();

    /* Create grandparent */
    void *grandparent = r8e_obj_new(ctx);
    grandparent = r8e_obj_set(ctx, grandparent, 100, r8e_from_int32(1000));
    grandparent = r8e_obj_set(ctx, grandparent, 200, r8e_from_int32(2000));

    /* Register grandparent as proto index 1 */
    ctx->proto_table[1] = 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)grandparent;
    ctx->proto_count = 2;

    /* Create parent with proto_id = 1 */
    void *parent = r8e_obj_new_with_proto(ctx, 1);
    parent = r8e_obj_set(ctx, parent, 300, r8e_from_int32(3000));

    /* Register parent as proto index 2 */
    ctx->proto_table[2] = 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)parent;
    ctx->proto_count = 3;

    /* Create child with proto_id = 2 */
    void *child = r8e_obj_new_with_proto(ctx, 2);
    child = r8e_obj_set(ctx, child, 400, r8e_from_int32(4000));

    /* Child has own property 400 */
    ASSERT(r8e_obj_has(ctx, child, 400));
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, child, 400)), 4000);

    /* Child does NOT have own property 300, 100, 200 */
    ASSERT(!r8e_obj_has(ctx, child, 300));
    ASSERT(!r8e_obj_has(ctx, child, 100));

    /* get_with_proto should walk the chain */
    R8EValue v300 = r8e_obj_get_with_proto(ctx, child, 300);
    ASSERT(R8E_IS_INT32(v300));
    ASSERT_EQ(r8e_get_int32(v300), 3000);

    R8EValue v100 = r8e_obj_get_with_proto(ctx, child, 100);
    ASSERT(R8E_IS_INT32(v100));
    ASSERT_EQ(r8e_get_int32(v100), 1000);

    /* Property not in any proto returns undefined */
    R8EValue vNone = r8e_obj_get_with_proto(ctx, child, 999);
    ASSERT_EQ(vNone, R8E_UNDEFINED);

    r8e_obj_destroy(ctx, child);
    r8e_obj_destroy(ctx, parent);
    r8e_obj_destroy(ctx, grandparent);
    free_ctx(ctx);
}

TEST(test_has_own_vs_prototype) {
    R8EContext *ctx = make_ctx();

    void *proto = r8e_obj_new(ctx);
    proto = r8e_obj_set(ctx, proto, 10, r8e_from_int32(100));
    ctx->proto_table[1] = 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)proto;
    ctx->proto_count = 2;

    void *child = r8e_obj_new_with_proto(ctx, 1);
    child = r8e_obj_set(ctx, child, 20, r8e_from_int32(200));

    /* has() checks own properties ONLY */
    ASSERT(r8e_obj_has(ctx, child, 20));   /* own property */
    ASSERT(!r8e_obj_has(ctx, child, 10));  /* prototype property */

    /* get_with_proto finds both */
    ASSERT_EQ(r8e_get_int32(r8e_obj_get_with_proto(ctx, child, 20)), 200);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get_with_proto(ctx, child, 10)), 100);

    r8e_obj_destroy(ctx, child);
    r8e_obj_destroy(ctx, proto);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Object.freeze detailed coverage
 * ========================================================================= */

TEST(test_freeze_prevents_new_properties) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(42));
    r8e_obj_freeze(ctx, obj);

    /* Attempting to add a new property after freeze */
    void *result = r8e_obj_set(ctx, obj, 2, r8e_from_int32(99));
    /* Should not have been added */
    ASSERT_EQ(r8e_obj_count(ctx, result), 1);
    ASSERT(!r8e_obj_has(ctx, result, 2));

    r8e_obj_destroy(ctx, result);
    free_ctx(ctx);
}

TEST(test_freeze_on_empty_object) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    r8e_obj_freeze(ctx, obj);
    ASSERT(r8e_obj_is_frozen(ctx, obj));

    /* Cannot add properties to frozen empty object */
    void *result = r8e_obj_set(ctx, obj, 1, r8e_from_int32(42));
    ASSERT_EQ(r8e_obj_count(ctx, result), 0);

    r8e_obj_destroy(ctx, result);
    free_ctx(ctx);
}

TEST(test_freeze_tier1_object) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(10));
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(20));
    ASSERT_EQ(r8e_obj_get_tier(obj), 1);

    r8e_obj_freeze(ctx, obj);
    ASSERT(r8e_obj_is_frozen(ctx, obj));

    /* Values readable but not modifiable */
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 1)), 10);
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, obj, 2)), 20);

    void *result = r8e_obj_set(ctx, obj, 1, r8e_from_int32(99));
    ASSERT_EQ(r8e_get_int32(r8e_obj_get(ctx, result, 1)), 10); /* unchanged */

    r8e_obj_destroy(ctx, result);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Keys enumeration edge cases
 * ========================================================================= */

TEST(test_keys_empty_object) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    uint32_t keys[4] = {0};
    uint32_t n = r8e_obj_keys(ctx, obj, keys, 4);
    ASSERT_EQ(n, 0);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_keys_buffer_smaller_than_count) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 4; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }

    /* Request only 2 keys when there are 4 */
    uint32_t keys[2] = {0};
    uint32_t n = r8e_obj_keys(ctx, obj, keys, 2);
    ASSERT_EQ(n, 2);

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

TEST(test_keys_tier3_large_object) {
    R8EContext *ctx = make_ctx();
    void *obj = r8e_obj_new(ctx);

    for (uint32_t i = 1; i <= 30; i++) {
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    }
    ASSERT_EQ(r8e_obj_get_tier(obj), 3);

    uint32_t keys[40] = {0};
    uint32_t n = r8e_obj_keys(ctx, obj, keys, 40);
    ASSERT_EQ(n, 30);

    /* Verify all keys are present (order may differ in hash table) */
    bool found[31] = {false};
    for (uint32_t i = 0; i < n; i++) {
        ASSERT(keys[i] >= 1 && keys[i] <= 30);
        found[keys[i]] = true;
    }
    for (uint32_t i = 1; i <= 30; i++) {
        ASSERT(found[i]);
    }

    r8e_obj_destroy(ctx, obj);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Miscellaneous edge cases
 * ========================================================================= */

TEST(test_delete_nonexistent_key_all_tiers) {
    R8EContext *ctx = make_ctx();

    /* Tier 0 */
    void *obj0 = r8e_obj_new(ctx);
    ASSERT_EQ(r8e_obj_delete(ctx, obj0, 999), 0);
    r8e_obj_destroy(ctx, obj0);

    /* Tier 1 */
    void *obj1 = r8e_obj_new(ctx);
    obj1 = r8e_obj_set(ctx, obj1, 1, r8e_from_int32(1));
    obj1 = r8e_obj_set(ctx, obj1, 2, r8e_from_int32(2));
    ASSERT_EQ(r8e_obj_delete(ctx, obj1, 999), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj1), 2);
    r8e_obj_destroy(ctx, obj1);

    /* Tier 2 */
    void *obj2 = r8e_obj_new(ctx);
    for (uint32_t i = 1; i <= 8; i++)
        obj2 = r8e_obj_set(ctx, obj2, i, r8e_from_int32((int32_t)i));
    ASSERT_EQ(r8e_obj_delete(ctx, obj2, 999), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj2), 8);
    r8e_obj_destroy(ctx, obj2);

    /* Tier 3 */
    void *obj3 = r8e_obj_new(ctx);
    for (uint32_t i = 1; i <= 20; i++)
        obj3 = r8e_obj_set(ctx, obj3, i, r8e_from_int32((int32_t)i));
    ASSERT_EQ(r8e_obj_delete(ctx, obj3, 999), 0);
    ASSERT_EQ(r8e_obj_count(ctx, obj3), 20);
    r8e_obj_destroy(ctx, obj3);

    free_ctx(ctx);
}

TEST(test_get_missing_all_tiers) {
    R8EContext *ctx = make_ctx();

    /* Tier 0 */
    void *obj = r8e_obj_new(ctx);
    ASSERT_EQ(r8e_obj_get(ctx, obj, 999), R8E_UNDEFINED);
    r8e_obj_destroy(ctx, obj);

    /* Tier 1 */
    obj = r8e_obj_new(ctx);
    obj = r8e_obj_set(ctx, obj, 1, r8e_from_int32(1));
    obj = r8e_obj_set(ctx, obj, 2, r8e_from_int32(2));
    ASSERT_EQ(r8e_obj_get(ctx, obj, 999), R8E_UNDEFINED);
    r8e_obj_destroy(ctx, obj);

    /* Tier 2 */
    obj = r8e_obj_new(ctx);
    for (uint32_t i = 1; i <= 8; i++)
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    ASSERT_EQ(r8e_obj_get(ctx, obj, 999), R8E_UNDEFINED);
    r8e_obj_destroy(ctx, obj);

    /* Tier 3 */
    obj = r8e_obj_new(ctx);
    for (uint32_t i = 1; i <= 20; i++)
        obj = r8e_obj_set(ctx, obj, i, r8e_from_int32((int32_t)i));
    ASSERT_EQ(r8e_obj_get(ctx, obj, 999), R8E_UNDEFINED);
    r8e_obj_destroy(ctx, obj);

    free_ctx(ctx);
}

TEST(test_prototype_shadow_property) {
    /* Child's own property should shadow the prototype's */
    R8EContext *ctx = make_ctx();

    void *proto = r8e_obj_new(ctx);
    proto = r8e_obj_set(ctx, proto, 1, r8e_from_int32(100));
    ctx->proto_table[1] = 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)proto;
    ctx->proto_count = 2;

    void *child = r8e_obj_new_with_proto(ctx, 1);
    child = r8e_obj_set(ctx, child, 1, r8e_from_int32(999));

    /* get_with_proto should return child's own value, not proto */
    R8EValue v = r8e_obj_get_with_proto(ctx, child, 1);
    ASSERT_EQ(r8e_get_int32(v), 999);

    r8e_obj_destroy(ctx, child);
    r8e_obj_destroy(ctx, proto);
    free_ctx(ctx);
}

/* =========================================================================
 * Test Runner
 * ========================================================================= */

void run_object_tests(void) {
    printf("\n=== Object Model Tests ===\n");

    /* Original tests */
    RUN_TEST(test_create_empty_object);
    RUN_TEST(test_set_one_property_tier0);
    RUN_TEST(test_promote_to_tier1);
    RUN_TEST(test_promote_to_tier2);
    RUN_TEST(test_promote_to_tier3);
    RUN_TEST(test_property_deletion);
    RUN_TEST(test_has_existing_and_missing);
    RUN_TEST(test_keys_enumeration);
    RUN_TEST(test_prototype_chain);
    RUN_TEST(test_get_with_proto_not_found);
    RUN_TEST(test_object_freeze);
    RUN_TEST(test_large_object_100_properties);
    RUN_TEST(test_overwrite_existing_property);
    RUN_TEST(test_get_missing_returns_undefined);
    RUN_TEST(test_tier_transitions_preserve_values);

    /* NEW: Tier 0 edge cases */
    RUN_TEST(test_tier0_create_delete_remains_tier0);
    RUN_TEST(test_tier0_overwrite_same_key);
    RUN_TEST(test_tier0_has_on_empty);
    RUN_TEST(test_tier0_store_special_values);

    /* NEW: Tier 1 detailed */
    RUN_TEST(test_tier1_exactly_2_properties);
    RUN_TEST(test_tier1_exactly_3_properties);
    RUN_TEST(test_tier1_exactly_4_properties);
    RUN_TEST(test_tier1_delete_middle_property);
    RUN_TEST(test_tier1_overwrite_existing);

    /* NEW: Tier 2 detailed */
    RUN_TEST(test_tier2_exactly_5_properties);
    RUN_TEST(test_tier2_8_properties);
    RUN_TEST(test_tier2_12_properties);
    RUN_TEST(test_tier2_16_properties);
    RUN_TEST(test_tier2_delete_property);

    /* NEW: Tier 3 detailed */
    RUN_TEST(test_tier3_50_properties);
    RUN_TEST(test_tier3_delete_property);
    RUN_TEST(test_tier3_overwrite_in_hash_table);
    RUN_TEST(test_tier3_stress_100_properties_robin_hood);

    /* NEW: Full promotion chain */
    RUN_TEST(test_full_tier_chain_0_to_3);

    /* NEW: Prototype chain */
    RUN_TEST(test_prototype_chain_depth_3);
    RUN_TEST(test_has_own_vs_prototype);
    RUN_TEST(test_prototype_shadow_property);

    /* NEW: Freeze */
    RUN_TEST(test_freeze_prevents_new_properties);
    RUN_TEST(test_freeze_on_empty_object);
    RUN_TEST(test_freeze_tier1_object);

    /* NEW: Keys */
    RUN_TEST(test_keys_empty_object);
    RUN_TEST(test_keys_buffer_smaller_than_count);
    RUN_TEST(test_keys_tier3_large_object);

    /* NEW: Cross-tier edge cases */
    RUN_TEST(test_delete_nonexistent_key_all_tiers);
    RUN_TEST(test_get_missing_all_tiers);

    printf("\n  Object Tests: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf("\n");
}

#ifdef TEST_OBJECT_MAIN
int main(void) {
    run_object_tests();
    return tests_failed > 0 ? 1 : 0;
}
#endif
