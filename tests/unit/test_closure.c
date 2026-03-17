/*
 * test_closure.c - Unit tests for r8e_closure.c (PVCC Closure System)
 *
 * Tests cover (14 original + 32 new = 46 total):
 *   - Create closure with 1 immutable capture -> INLINE mode
 *   - Create closure with 2 immutable captures -> INLINE mode
 *   - Create closure with 3 immutable captures -> ARRAY mode
 *   - Create closure with 9+ captures -> FRAME mode
 *   - Get immutable capture returns correct value
 *   - Set mutable capture updates value
 *   - Set immutable capture fails (returns false)
 *   - Shared env frame: two closures see each other's mutations
 *   - Env frame retain/release lifecycle
 *   - Closure with mixed captures: some mutable, some immutable
 *   - Zero captures (empty closure)
 *   - Mutable capture promotes 2-capture closure to ARRAY mode
 *   - Closure function retrieval
 *   --- NEW TESTS ---
 *   - Immutable capture snapshot semantics (PVCC copy)
 *   - Mutable shared upvalue: mutations visible across closures
 *   - Inline storage verification (no heap alloc for captures)
 *   - Array storage structure (values[], meta[], env pointers)
 *   - Frame storage: all captures through env frame
 *   - Nested closures: 3 levels deep with immutable captures
 *   - Nested closures: 3 levels deep with mutable capture chain
 *   - Closure over loop variable: let semantics (each iteration snapshots)
 *   - Closure over loop variable: var semantics (all share one mutable)
 *   - Multiple closures interleaved mutations on shared upvalue
 *   - Env frame with zero slots
 *   - Env frame set same value no-op optimization
 *   - Env frame overwrite repeatedly (100 times)
 *   - Env frame multiple retain/release cycles
 *   - All mutable captures in ARRAY mode
 *   - 8 captures at ARRAY boundary (max ARRAY)
 *   - Single mutable capture promotes to ARRAY
 *   - FRAME mode with pre-existing env frame reuse
 *   - Closure GC header kind bits
 *   - Env frame GC header kind bits
 *   - closure_visit_refs INLINE mode
 *   - closure_visit_refs ARRAY mode
 *   - closure_visit_refs FRAME mode
 *   - envframe_visit_refs (no pointer values)
 *   - envframe_visit_refs (with pointer values)
 *   - closure_visit_refs NULL closure safety
 *   - envframe_visit_refs NULL frame safety
 *   - env_frame_retain NULL safety
 *   - env_frame_release NULL safety
 *   - Closure with special NaN-boxed value captures
 *   - closure_new with NULL ctx returns NULL
 *   - closure_new with NULL func returns NULL
 *   - closure_new with NULL captures + nonzero count returns NULL
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

#define ASSERT_TRUE(cond)  ASSERT((cond))
#define ASSERT_FALSE(cond) ASSERT(!(cond))
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)
#define ASSERT_NULL(ptr)     ASSERT((ptr) == NULL)

/* =========================================================================
 * Types matching r8e_closure.c internal definitions
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)

static inline uint64_t r8e_from_int32_test(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline int32_t r8e_get_int32_test(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline void *r8e_get_pointer_test(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer_test(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* GC header flags layout (must match r8e_gc.c / r8e_closure.c) */
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

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* Capture classification constants */
#define R8E_CAPTURE_IMMUTABLE  0x00
#define R8E_CAPTURE_MUTABLE    0x01

/* Capture storage modes */
#define R8E_CAPTURE_MODE_INLINE 0
#define R8E_CAPTURE_MODE_ARRAY  1
#define R8E_CAPTURE_MODE_FRAME  2

/* Capture metadata */
#define R8E_CAP_META_DIRECT   0
#define R8E_CAP_META_INDIRECT 1

typedef struct {
    uint8_t  kind;
    uint8_t  index;
    uint16_t pad;
} R8ECaptureMeta;

/* Function stub */
typedef struct R8EFunction {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t *bytecode;
    uint32_t bytecode_len;
    uint16_t param_count;
    uint16_t local_count;
    uint16_t capture_count;
    uint16_t reserved;
    uint32_t name_atom;
} R8EFunction;

/* Environment frame */
typedef struct R8EEnvFrame {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  refcount;
    uint16_t  slot_count;
    uint16_t  reserved;
    R8EValue *slots;
} R8EEnvFrame;

/* Closure structure */
typedef struct R8EClosure {
    uint32_t      flags;
    uint32_t      proto_id;
    R8EFunction  *func;
    uint8_t       capture_count;
    uint8_t       capture_mode;
    uint16_t      reserved;
    union {
        struct {
            R8EValue inline_captures[2];
        } inl;
        struct {
            R8EValue       *values;
            R8ECaptureMeta *meta;
            R8EEnvFrame    *env;
        } arr;
        struct {
            R8EEnvFrame *env_frame;
        } frm;
    } storage;
} R8EClosure;

/* Capture descriptor */
typedef struct {
    R8EValue value;
    uint8_t  mutability;
    uint8_t  frame_slot;
    uint16_t reserved;
} R8ECaptureDesc;

/* GC state types (minimal, for context) */
typedef struct {
    R8EGCHeader **items;
    uint32_t      count;
    uint32_t      capacity;
} R8ESuspectList;

typedef struct {
    void    *entries;
    uint32_t count;
    uint32_t capacity;
} R8EExtRCTable;

typedef struct R8EGCState {
    R8ESuspectList  suspects;
    R8EExtRCTable   ext_rc;
    uint32_t        epoch_threshold;
    uint32_t        total_allocated;
    uint32_t        total_freed;
    uint32_t        cycles_collected;
} R8EGCState;

typedef struct R8EContext {
    R8EGCState gc;
} R8EContext;

/* =========================================================================
 * Forward declarations of functions under test
 * ========================================================================= */

/* From r8e_gc.c */
extern void r8e_gc_init(R8EContext *ctx);
extern void r8e_gc_destroy(R8EContext *ctx);
extern void r8e_retain(R8EContext *ctx, R8EValue val);
extern void r8e_release(R8EContext *ctx, R8EValue val);
extern void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind,
                                uint32_t proto_id);

/* From r8e_closure.c */
extern R8EClosure *r8e_closure_new(R8EContext *ctx, R8EFunction *func,
                                    const R8ECaptureDesc *captures,
                                    uint8_t count, R8EEnvFrame *env);
extern R8EValue r8e_closure_get_capture(const R8EClosure *closure,
                                          uint8_t index);
extern bool r8e_closure_set_capture(R8EContext *ctx, R8EClosure *closure,
                                      uint8_t index, R8EValue value);
extern bool r8e_closure_is_capture_mutable(const R8EClosure *closure,
                                             uint8_t index);
extern uint8_t r8e_closure_capture_mode(const R8EClosure *closure);
extern uint8_t r8e_closure_capture_count(const R8EClosure *closure);
extern R8EFunction *r8e_closure_get_function(const R8EClosure *closure);
extern R8EEnvFrame *r8e_closure_get_env_frame(const R8EClosure *closure);

extern R8EEnvFrame *r8e_env_frame_new(R8EContext *ctx, uint16_t slot_count);
extern void r8e_env_frame_retain(R8EEnvFrame *frame);
extern void r8e_env_frame_release(R8EContext *ctx, R8EEnvFrame *frame);
extern R8EValue r8e_env_frame_get(const R8EEnvFrame *frame, uint16_t index);
extern void r8e_env_frame_set(R8EContext *ctx, R8EEnvFrame *frame,
                                uint16_t index, R8EValue value);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EContext *make_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    r8e_gc_init(ctx);
    return ctx;
}

static void free_ctx(R8EContext *ctx) {
    r8e_gc_destroy(ctx);
    free(ctx);
}

/*
 * Create a dummy R8EFunction for testing. The function is heap-allocated
 * with a GC header so retain/release work on it.
 */
static R8EFunction *make_dummy_func(R8EContext *ctx) {
    R8EFunction *func = (R8EFunction *)calloc(1, sizeof(R8EFunction));
    r8e_gc_header_init((R8EGCHeader *)func, R8E_GC_KIND_FUNCTION, 2);
    ctx->gc.total_allocated++;
    return func;
}

static uint32_t get_inline_rc(R8EGCHeader *hdr) {
    return (hdr->flags & R8E_GC_RC_INLINE_MASK) >> R8E_GC_RC_INLINE_SHIFT;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

/*
 * Test: 1 immutable capture -> INLINE mode
 */
static void test_one_immutable_capture_inline(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[1] = {
        { .value = r8e_from_int32_test(42), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 1, NULL);
    ASSERT_NOT_NULL(cl);

    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_INLINE);
    ASSERT_EQ(r8e_closure_capture_count(cl), 1);
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(42));
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 0));

    /* INLINE mode has no env frame */
    ASSERT_NULL(r8e_closure_get_env_frame(cl));

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: 2 immutable captures -> INLINE mode
 */
static void test_two_immutable_captures_inline(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[2] = {
        { .value = r8e_from_int32_test(10), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(20), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 2, NULL);
    ASSERT_NOT_NULL(cl);

    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_INLINE);
    ASSERT_EQ(r8e_closure_capture_count(cl), 2);
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(20));

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: 3 immutable captures -> ARRAY mode (exceeds INLINE limit of 2)
 */
static void test_three_immutable_captures_array(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[3] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(2), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(3), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 3, NULL);
    ASSERT_NOT_NULL(cl);

    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);
    ASSERT_EQ(r8e_closure_capture_count(cl), 3);

    /* All three values accessible */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(1));
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(2));
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), r8e_from_int32_test(3));

    /* All immutable in ARRAY mode */
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 0));
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 1));
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 2));

    /* No env frame needed when all immutable in ARRAY mode */
    ASSERT_NULL(r8e_closure_get_env_frame(cl));

    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: 9+ captures -> FRAME mode
 */
static void test_nine_captures_frame_mode(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[10];
    for (int i = 0; i < 10; i++) {
        caps[i].value = r8e_from_int32_test(i * 10);
        caps[i].mutability = R8E_CAPTURE_IMMUTABLE;
        caps[i].frame_slot = 0;
        caps[i].reserved = 0;
    }

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 10, NULL);
    ASSERT_NOT_NULL(cl);

    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_FRAME);
    ASSERT_EQ(r8e_closure_capture_count(cl), 10);

    /* Verify all values through the frame */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(r8e_closure_get_capture(cl, (uint8_t)i),
                  r8e_from_int32_test(i * 10));
    }

    /* FRAME mode env frame should be non-NULL */
    R8EEnvFrame *frame = r8e_closure_get_env_frame(cl);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->slot_count, 10);

    /* FRAME mode captures are all considered mutable */
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, (uint8_t)i));
    }

    r8e_env_frame_release(ctx, frame);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: get immutable capture returns correct NaN-boxed value
 */
static void test_get_immutable_capture_value(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* Test various NaN-boxed value types as captures */
    R8ECaptureDesc caps[2] = {
        { .value = R8E_TRUE, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_NULL, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 2, NULL);
    ASSERT_NOT_NULL(cl);

    ASSERT_EQ(r8e_closure_get_capture(cl, 0), R8E_TRUE);
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), R8E_NULL);

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: set mutable capture updates value
 */
static void test_set_mutable_capture(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* 3 captures: one mutable (forces ARRAY mode) */
    R8ECaptureDesc caps[3] = {
        { .value = r8e_from_int32_test(10), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(20), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(30), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 3, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    /* Mutable capture at index 1 is initially 20 */
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(20));
    ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, 1));

    /* Update the mutable capture */
    bool ok = r8e_closure_set_capture(ctx, cl, 1, r8e_from_int32_test(99));
    ASSERT_TRUE(ok);

    /* Read back the updated value */
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(99));

    /* Immutable captures are unchanged */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), r8e_from_int32_test(30));

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: set immutable capture returns false
 */
static void test_set_immutable_capture_fails(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* INLINE mode: all immutable */
    R8ECaptureDesc caps[1] = {
        { .value = r8e_from_int32_test(42), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 1, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_INLINE);

    /* Attempt to set immutable capture should fail */
    bool ok = r8e_closure_set_capture(ctx, cl, 0, r8e_from_int32_test(99));
    ASSERT_FALSE(ok);

    /* Value should be unchanged */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(42));

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: set immutable capture in ARRAY mode also fails
 */
static void test_set_immutable_in_array_mode_fails(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[3] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(2), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(3), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 3, NULL);
    ASSERT_NOT_NULL(cl);

    /* Index 0 is immutable (DIRECT) -> set should fail */
    bool ok0 = r8e_closure_set_capture(ctx, cl, 0, r8e_from_int32_test(99));
    ASSERT_FALSE(ok0);
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(1));

    /* Index 2 is immutable (DIRECT) -> set should fail */
    bool ok2 = r8e_closure_set_capture(ctx, cl, 2, r8e_from_int32_test(99));
    ASSERT_FALSE(ok2);
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), r8e_from_int32_test(3));

    /* Index 1 is mutable -> set should succeed */
    bool ok1 = r8e_closure_set_capture(ctx, cl, 1, r8e_from_int32_test(77));
    ASSERT_TRUE(ok1);
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(77));

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: shared env frame - two closures see each other's mutations
 */
static void test_shared_env_frame_mutations(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func1 = make_dummy_func(ctx);
    R8EFunction *func2 = make_dummy_func(ctx);

    /* Create a shared env frame with 2 slots */
    R8EEnvFrame *env = r8e_env_frame_new(ctx, 2);
    ASSERT_NOT_NULL(env);

    /* Populate the env frame */
    r8e_env_frame_set(ctx, env, 0, r8e_from_int32_test(100));
    r8e_env_frame_set(ctx, env, 1, r8e_from_int32_test(200));

    /* Create two closures sharing the same env frame.
     * Both have a single mutable capture referencing slot 0. */
    R8ECaptureDesc caps1[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 1, .reserved = 0 },
        { .value = r8e_from_int32_test(999), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8ECaptureDesc caps2[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 1, .reserved = 0 },
        { .value = r8e_from_int32_test(888), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl1 = r8e_closure_new(ctx, func1, caps1, 3, env);
    R8EClosure *cl2 = r8e_closure_new(ctx, func2, caps2, 3, env);
    ASSERT_NOT_NULL(cl1);
    ASSERT_NOT_NULL(cl2);

    /* Both should see the env frame values for mutable captures */
    ASSERT_EQ(r8e_closure_get_capture(cl1, 0), r8e_from_int32_test(100));
    ASSERT_EQ(r8e_closure_get_capture(cl2, 0), r8e_from_int32_test(100));

    /* Mutate via closure 1 */
    r8e_closure_set_capture(ctx, cl1, 0, r8e_from_int32_test(555));

    /* Closure 2 should see the mutation */
    ASSERT_EQ(r8e_closure_get_capture(cl2, 0), r8e_from_int32_test(555));

    /* Mutate via closure 2 */
    r8e_closure_set_capture(ctx, cl2, 1, r8e_from_int32_test(777));

    /* Closure 1 should see the mutation */
    ASSERT_EQ(r8e_closure_get_capture(cl1, 1), r8e_from_int32_test(777));

    /* Cleanup: each closure retained the env, plus our original ref */
    R8EEnvFrame *env1 = r8e_closure_get_env_frame(cl1);
    R8EEnvFrame *env2 = r8e_closure_get_env_frame(cl2);
    if (env1) r8e_env_frame_release(ctx, env1);
    if (env2) r8e_env_frame_release(ctx, env2);
    r8e_env_frame_release(ctx, env);

    free(cl1->storage.arr.values);
    free(cl1->storage.arr.meta);
    free(cl2->storage.arr.values);
    free(cl2->storage.arr.meta);
    free(cl1);
    free(cl2);
    free(func1);
    free(func2);
    free_ctx(ctx);
}

/*
 * Test: env frame retain/release lifecycle
 */
static void test_env_frame_lifecycle(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 4);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->refcount, 1);
    ASSERT_EQ(frame->slot_count, 4);

    /* All slots should be undefined initially */
    for (uint16_t i = 0; i < 4; i++) {
        ASSERT_EQ(r8e_env_frame_get(frame, i), R8E_UNDEFINED);
    }

    /* Set and read back */
    r8e_env_frame_set(ctx, frame, 0, r8e_from_int32_test(42));
    r8e_env_frame_set(ctx, frame, 3, R8E_TRUE);

    ASSERT_EQ(r8e_env_frame_get(frame, 0), r8e_from_int32_test(42));
    ASSERT_EQ(r8e_env_frame_get(frame, 1), R8E_UNDEFINED);
    ASSERT_EQ(r8e_env_frame_get(frame, 3), R8E_TRUE);

    /* Retain: refcount 1 -> 2 */
    r8e_env_frame_retain(frame);
    ASSERT_EQ(frame->refcount, 2);

    /* Release: 2 -> 1, not freed */
    r8e_env_frame_release(ctx, frame);
    ASSERT_EQ(frame->refcount, 1);

    /* Final release: 1 -> 0, freed */
    r8e_env_frame_release(ctx, frame);
    /* frame is freed, do not access */

    free_ctx(ctx);
}

/*
 * Test: closure with mixed captures (some mutable, some immutable)
 */
static void test_mixed_captures(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* 5 captures: mixed mutability, triggers ARRAY mode */
    R8ECaptureDesc caps[5] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(2), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(3), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(4), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 1, .reserved = 0 },
        { .value = r8e_from_int32_test(5), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 5, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    /* Verify mutability flags */
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 0));
    ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, 1));
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 2));
    ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, 3));
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 4));

    /* Read all values */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(1));
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(2));
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), r8e_from_int32_test(3));
    ASSERT_EQ(r8e_closure_get_capture(cl, 3), r8e_from_int32_test(4));
    ASSERT_EQ(r8e_closure_get_capture(cl, 4), r8e_from_int32_test(5));

    /* Mutate the mutable ones */
    r8e_closure_set_capture(ctx, cl, 1, r8e_from_int32_test(22));
    r8e_closure_set_capture(ctx, cl, 3, r8e_from_int32_test(44));

    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(22));
    ASSERT_EQ(r8e_closure_get_capture(cl, 3), r8e_from_int32_test(44));

    /* Immutable ones are unchanged */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(1));
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), r8e_from_int32_test(3));
    ASSERT_EQ(r8e_closure_get_capture(cl, 4), r8e_from_int32_test(5));

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: zero captures produces INLINE mode empty closure
 */
static void test_zero_captures(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8EClosure *cl = r8e_closure_new(ctx, func, NULL, 0, NULL);
    ASSERT_NOT_NULL(cl);

    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_INLINE);
    ASSERT_EQ(r8e_closure_capture_count(cl), 0);
    ASSERT_EQ(r8e_closure_get_function(cl), func);

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: mutable capture with 2 captures promotes to ARRAY mode
 * (count <= 2 but has mutable -> not eligible for INLINE)
 */
static void test_mutable_promotes_to_array(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[2] = {
        { .value = r8e_from_int32_test(10), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(20), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 2, NULL);
    ASSERT_NOT_NULL(cl);

    /* Should be ARRAY, not INLINE, because of the mutable capture */
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 0));
    ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, 1));

    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(20));

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: closure function retrieval
 */
static void test_closure_get_function(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);
    func->param_count = 3;
    func->name_atom = 42;

    R8ECaptureDesc caps[1] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 1, NULL);
    ASSERT_NOT_NULL(cl);

    R8EFunction *retrieved = r8e_closure_get_function(cl);
    ASSERT_EQ((uintptr_t)retrieved, (uintptr_t)func);
    ASSERT_EQ(retrieved->param_count, 3);
    ASSERT_EQ(retrieved->name_atom, 42);

    free(cl);
    free(func);
    free_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Expanded coverage for 90-95% target
 * ========================================================================= */

/* --- GC visitor declarations for testing --- */
typedef void (*R8EGCVisitorFn)(R8EGCHeader *ref, void *user_data);
extern void r8e_closure_visit_refs(const R8EClosure *closure,
                                     R8EGCVisitorFn visitor, void *ud);
extern void r8e_envframe_visit_refs(const R8EEnvFrame *frame,
                                      R8EGCVisitorFn visitor, void *ud);

/* Visitor helper for counting references */
static int visitor_count;
static R8EGCHeader *visitor_refs[64];

static void counting_visitor(R8EGCHeader *ref, void *ud) {
    (void)ud;
    if (visitor_count < 64) {
        visitor_refs[visitor_count] = ref;
    }
    visitor_count++;
}

static void reset_visitor(void) {
    visitor_count = 0;
    memset(visitor_refs, 0, sizeof(visitor_refs));
}

/*
 * Test: Immutable capture is a snapshot (PVCC copy semantics)
 * Verify that the closure stores a copy, not a reference to the original.
 * After creating the closure, changing the "original" has no effect.
 */
static void test_immutable_capture_snapshot(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* Simulate a local variable value */
    R8EValue original = r8e_from_int32_test(100);

    R8ECaptureDesc caps[1] = {
        { .value = original, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 1, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_INLINE);

    /* The closure holds the snapshot value */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(100));

    /* "Changing" the original (local var) has no effect on the closure */
    original = r8e_from_int32_test(999);
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(100));

    /* Cannot set it since it is immutable */
    ASSERT_FALSE(r8e_closure_set_capture(ctx, cl, 0, r8e_from_int32_test(999)));
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(100));

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Mutable capture shared upvalue - mutations visible across closures
 * Two closures sharing the same env frame see each other's writes.
 */
static void test_mutable_shared_upvalue_cross_closure(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func1 = make_dummy_func(ctx);
    R8EFunction *func2 = make_dummy_func(ctx);

    /* Create shared env frame with 1 slot */
    R8EEnvFrame *env = r8e_env_frame_new(ctx, 1);
    ASSERT_NOT_NULL(env);
    r8e_env_frame_set(ctx, env, 0, r8e_from_int32_test(0));

    /* Both closures have 3 captures to force ARRAY mode, with slot 0 mutable */
    R8ECaptureDesc caps1[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(10), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(20), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };
    R8ECaptureDesc caps2[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(30), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(40), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl1 = r8e_closure_new(ctx, func1, caps1, 3, env);
    R8EClosure *cl2 = r8e_closure_new(ctx, func2, caps2, 3, env);
    ASSERT_NOT_NULL(cl1);
    ASSERT_NOT_NULL(cl2);

    /* Both read the same initial value from the shared slot */
    ASSERT_EQ(r8e_closure_get_capture(cl1, 0), r8e_from_int32_test(0));
    ASSERT_EQ(r8e_closure_get_capture(cl2, 0), r8e_from_int32_test(0));

    /* Closure 1 increments */
    r8e_closure_set_capture(ctx, cl1, 0, r8e_from_int32_test(1));
    /* Closure 2 sees the increment */
    ASSERT_EQ(r8e_closure_get_capture(cl2, 0), r8e_from_int32_test(1));

    /* Closure 2 increments further */
    r8e_closure_set_capture(ctx, cl2, 0, r8e_from_int32_test(2));
    /* Closure 1 sees it */
    ASSERT_EQ(r8e_closure_get_capture(cl1, 0), r8e_from_int32_test(2));

    /* Immutable captures remain independent */
    ASSERT_EQ(r8e_closure_get_capture(cl1, 1), r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl2, 1), r8e_from_int32_test(30));

    /* Cleanup */
    R8EEnvFrame *e1 = r8e_closure_get_env_frame(cl1);
    R8EEnvFrame *e2 = r8e_closure_get_env_frame(cl2);
    if (e1) r8e_env_frame_release(ctx, e1);
    if (e2) r8e_env_frame_release(ctx, e2);
    r8e_env_frame_release(ctx, env);

    free(cl1->storage.arr.values);
    free(cl1->storage.arr.meta);
    free(cl2->storage.arr.values);
    free(cl2->storage.arr.meta);
    free(cl1);
    free(cl2);
    free(func1);
    free(func2);
    free_ctx(ctx);
}

/*
 * Test: Inline storage verification - exactly 2 immutable, verify no env frame
 * and values live in inline_captures[] union member.
 */
static void test_inline_storage_no_heap_alloc(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[2] = {
        { .value = R8E_TRUE, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_FALSE, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 2, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_INLINE);

    /* Directly verify the inline storage matches */
    ASSERT_EQ(cl->storage.inl.inline_captures[0], R8E_TRUE);
    ASSERT_EQ(cl->storage.inl.inline_captures[1], R8E_FALSE);

    /* No env frame for INLINE */
    ASSERT_NULL(r8e_closure_get_env_frame(cl));

    /* Not mutable */
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 0));
    ASSERT_FALSE(r8e_closure_is_capture_mutable(cl, 1));

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Array capture storage verification (3-8 captures)
 * Verify values[], meta[], and env pointers are correctly populated.
 */
static void test_array_storage_structure(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[5] = {
        { .value = r8e_from_int32_test(10), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(20), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(30), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(40), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 1, .reserved = 0 },
        { .value = r8e_from_int32_test(50), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 5, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    /* Verify internal structure */
    ASSERT_NOT_NULL(cl->storage.arr.values);
    ASSERT_NOT_NULL(cl->storage.arr.meta);
    ASSERT_NOT_NULL(cl->storage.arr.env);

    /* Check meta kinds: immutable=DIRECT, mutable=INDIRECT */
    ASSERT_EQ(cl->storage.arr.meta[0].kind, R8E_CAP_META_DIRECT);
    ASSERT_EQ(cl->storage.arr.meta[1].kind, R8E_CAP_META_INDIRECT);
    ASSERT_EQ(cl->storage.arr.meta[2].kind, R8E_CAP_META_DIRECT);
    ASSERT_EQ(cl->storage.arr.meta[3].kind, R8E_CAP_META_INDIRECT);
    ASSERT_EQ(cl->storage.arr.meta[4].kind, R8E_CAP_META_DIRECT);

    /* Check INDIRECT captures reference correct frame slots */
    ASSERT_EQ(cl->storage.arr.meta[1].index, 0);
    ASSERT_EQ(cl->storage.arr.meta[3].index, 1);

    /* Env frame should have 2 slots (max_slot+1 = 1+1 = 2) */
    ASSERT_EQ(cl->storage.arr.env->slot_count, 2);

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Frame capture storage verification (9+ captures)
 * All captures stored through the env frame, all mutable.
 */
static void test_frame_storage_all_through_env(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[12];
    for (int i = 0; i < 12; i++) {
        caps[i].value = r8e_from_int32_test(i * 100);
        caps[i].mutability = R8E_CAPTURE_IMMUTABLE;
        caps[i].frame_slot = 0;
        caps[i].reserved = 0;
    }

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 12, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_FRAME);
    ASSERT_EQ(r8e_closure_capture_count(cl), 12);

    /* All captures accessible through the frame */
    for (int i = 0; i < 12; i++) {
        ASSERT_EQ(r8e_closure_get_capture(cl, (uint8_t)i),
                  r8e_from_int32_test(i * 100));
    }

    /* FRAME mode: all captures reported as mutable */
    for (int i = 0; i < 12; i++) {
        ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, (uint8_t)i));
    }

    /* Can set any capture in FRAME mode */
    bool ok = r8e_closure_set_capture(ctx, cl, 5, r8e_from_int32_test(9999));
    ASSERT_TRUE(ok);
    ASSERT_EQ(r8e_closure_get_capture(cl, 5), r8e_from_int32_test(9999));

    /* Other captures unaffected */
    ASSERT_EQ(r8e_closure_get_capture(cl, 4), r8e_from_int32_test(400));
    ASSERT_EQ(r8e_closure_get_capture(cl, 6), r8e_from_int32_test(600));

    R8EEnvFrame *frame = r8e_closure_get_env_frame(cl);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(frame->slot_count >= 12);

    r8e_env_frame_release(ctx, frame);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Nested closures 3 levels deep
 * Simulates: function outer() { let a=1; return function mid() { let b=2; return function inner() { return a+b; }}}
 * Each level captures from its parent scope.
 */
static void test_nested_closures_3_levels(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func_outer = make_dummy_func(ctx);
    R8EFunction *func_mid = make_dummy_func(ctx);
    R8EFunction *func_inner = make_dummy_func(ctx);

    /* Outer closure: no captures (top-level function) */
    R8EClosure *cl_outer = r8e_closure_new(ctx, func_outer, NULL, 0, NULL);
    ASSERT_NOT_NULL(cl_outer);
    ASSERT_EQ(r8e_closure_capture_count(cl_outer), 0);

    /* Mid closure: captures 'a' from outer (immutable, value=1) */
    R8ECaptureDesc mid_caps[1] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };
    R8EClosure *cl_mid = r8e_closure_new(ctx, func_mid, mid_caps, 1, NULL);
    ASSERT_NOT_NULL(cl_mid);
    ASSERT_EQ(r8e_closure_capture_mode(cl_mid), R8E_CAPTURE_MODE_INLINE);
    ASSERT_EQ(r8e_closure_get_capture(cl_mid, 0), r8e_from_int32_test(1));

    /* Inner closure: captures 'a' from outer and 'b' from mid (both immutable) */
    R8ECaptureDesc inner_caps[2] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(2), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };
    R8EClosure *cl_inner = r8e_closure_new(ctx, func_inner, inner_caps, 2, NULL);
    ASSERT_NOT_NULL(cl_inner);
    ASSERT_EQ(r8e_closure_capture_mode(cl_inner), R8E_CAPTURE_MODE_INLINE);
    ASSERT_EQ(r8e_closure_get_capture(cl_inner, 0), r8e_from_int32_test(1));
    ASSERT_EQ(r8e_closure_get_capture(cl_inner, 1), r8e_from_int32_test(2));

    /* Each closure is independent */
    ASSERT_EQ(r8e_closure_get_function(cl_outer), func_outer);
    ASSERT_EQ(r8e_closure_get_function(cl_mid), func_mid);
    ASSERT_EQ(r8e_closure_get_function(cl_inner), func_inner);

    free(cl_inner);
    free(cl_mid);
    free(cl_outer);
    free(func_inner);
    free(func_mid);
    free(func_outer);
    free_ctx(ctx);
}

/*
 * Test: Nested closures 3 levels with mutable capture chain
 * Simulates: outer creates mutable 'counter', mid and inner both share it
 */
static void test_nested_closures_mutable_chain(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func_mid = make_dummy_func(ctx);
    R8EFunction *func_inner = make_dummy_func(ctx);

    /* Shared env frame for the counter variable */
    R8EEnvFrame *env = r8e_env_frame_new(ctx, 1);
    ASSERT_NOT_NULL(env);
    r8e_env_frame_set(ctx, env, 0, r8e_from_int32_test(0));

    /* Mid closure: captures counter (mutable) + a local immutable + padding */
    R8ECaptureDesc mid_caps[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(10), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(20), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    /* Inner closure: also captures counter (mutable) from same env frame */
    R8ECaptureDesc inner_caps[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(30), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(40), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl_mid = r8e_closure_new(ctx, func_mid, mid_caps, 3, env);
    R8EClosure *cl_inner = r8e_closure_new(ctx, func_inner, inner_caps, 3, env);
    ASSERT_NOT_NULL(cl_mid);
    ASSERT_NOT_NULL(cl_inner);

    /* Both start with counter = 0 */
    ASSERT_EQ(r8e_closure_get_capture(cl_mid, 0), r8e_from_int32_test(0));
    ASSERT_EQ(r8e_closure_get_capture(cl_inner, 0), r8e_from_int32_test(0));

    /* Mid increments counter */
    r8e_closure_set_capture(ctx, cl_mid, 0, r8e_from_int32_test(1));

    /* Inner sees the increment */
    ASSERT_EQ(r8e_closure_get_capture(cl_inner, 0), r8e_from_int32_test(1));

    /* Inner increments further */
    r8e_closure_set_capture(ctx, cl_inner, 0, r8e_from_int32_test(2));

    /* Mid sees it */
    ASSERT_EQ(r8e_closure_get_capture(cl_mid, 0), r8e_from_int32_test(2));

    /* Immutable captures are independent */
    ASSERT_EQ(r8e_closure_get_capture(cl_mid, 1), r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl_inner, 1), r8e_from_int32_test(30));

    /* Cleanup */
    R8EEnvFrame *e_mid = r8e_closure_get_env_frame(cl_mid);
    R8EEnvFrame *e_inner = r8e_closure_get_env_frame(cl_inner);
    if (e_mid) r8e_env_frame_release(ctx, e_mid);
    if (e_inner) r8e_env_frame_release(ctx, e_inner);
    r8e_env_frame_release(ctx, env);

    free(cl_mid->storage.arr.values);
    free(cl_mid->storage.arr.meta);
    free(cl_inner->storage.arr.values);
    free(cl_inner->storage.arr.meta);
    free(cl_mid);
    free(cl_inner);
    free(func_mid);
    free(func_inner);
    free_ctx(ctx);
}

/*
 * Test: Closure over loop variable (let semantics)
 * Each iteration creates a separate closure with its own snapshot.
 * Simulates: for (let i = 0; i < 3; i++) { closures.push(() => i); }
 */
static void test_closure_loop_let_semantics(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);
    R8EClosure *closures[3];

    /* Each iteration creates a closure capturing the current value of i */
    for (int i = 0; i < 3; i++) {
        R8ECaptureDesc caps[1] = {
            { .value = r8e_from_int32_test(i), .mutability = R8E_CAPTURE_IMMUTABLE,
              .frame_slot = 0, .reserved = 0 }
        };
        closures[i] = r8e_closure_new(ctx, func, caps, 1, NULL);
        ASSERT_NOT_NULL(closures[i]);
    }

    /* Each closure captured a different i value (let semantics) */
    ASSERT_EQ(r8e_closure_get_capture(closures[0], 0), r8e_from_int32_test(0));
    ASSERT_EQ(r8e_closure_get_capture(closures[1], 0), r8e_from_int32_test(1));
    ASSERT_EQ(r8e_closure_get_capture(closures[2], 0), r8e_from_int32_test(2));

    for (int i = 0; i < 3; i++) {
        free(closures[i]);
    }
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Closure over loop variable (var semantics)
 * All closures share the same mutable variable.
 * Simulates: for (var i = 0; i < 3; i++) { closures.push(() => i); }
 * After the loop, all closures see i=3.
 */
static void test_closure_loop_var_semantics(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* Shared env frame for the var-scoped variable */
    R8EEnvFrame *env = r8e_env_frame_new(ctx, 1);
    ASSERT_NOT_NULL(env);

    R8EClosure *closures[3];

    /* Each iteration creates a closure capturing shared mutable 'i' */
    for (int i = 0; i < 3; i++) {
        /* Update the shared variable to current loop value */
        r8e_env_frame_set(ctx, env, 0, r8e_from_int32_test(i));

        R8ECaptureDesc caps[3] = {
            { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
              .frame_slot = 0, .reserved = 0 },
            { .value = r8e_from_int32_test(100 + i), .mutability = R8E_CAPTURE_IMMUTABLE,
              .frame_slot = 0, .reserved = 0 },
            { .value = r8e_from_int32_test(200 + i), .mutability = R8E_CAPTURE_IMMUTABLE,
              .frame_slot = 0, .reserved = 0 }
        };
        closures[i] = r8e_closure_new(ctx, func, caps, 3, env);
        ASSERT_NOT_NULL(closures[i]);
    }

    /* After loop, set i to 3 (the final value after for loop exits) */
    r8e_env_frame_set(ctx, env, 0, r8e_from_int32_test(3));

    /* All closures see i=3 (var semantics: all share same slot) */
    ASSERT_EQ(r8e_closure_get_capture(closures[0], 0), r8e_from_int32_test(3));
    ASSERT_EQ(r8e_closure_get_capture(closures[1], 0), r8e_from_int32_test(3));
    ASSERT_EQ(r8e_closure_get_capture(closures[2], 0), r8e_from_int32_test(3));

    /* But each closure's immutable captures are distinct snapshots */
    ASSERT_EQ(r8e_closure_get_capture(closures[0], 1), r8e_from_int32_test(100));
    ASSERT_EQ(r8e_closure_get_capture(closures[1], 1), r8e_from_int32_test(101));
    ASSERT_EQ(r8e_closure_get_capture(closures[2], 1), r8e_from_int32_test(102));

    /* Cleanup */
    for (int i = 0; i < 3; i++) {
        R8EEnvFrame *e = r8e_closure_get_env_frame(closures[i]);
        if (e) r8e_env_frame_release(ctx, e);
        free(closures[i]->storage.arr.values);
        free(closures[i]->storage.arr.meta);
        free(closures[i]);
    }
    r8e_env_frame_release(ctx, env);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Multiple closures sharing same upvalue with interleaved mutations
 */
static void test_multiple_closures_interleaved_mutations(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func_inc = make_dummy_func(ctx);
    R8EFunction *func_dec = make_dummy_func(ctx);
    R8EFunction *func_read = make_dummy_func(ctx);

    R8EEnvFrame *env = r8e_env_frame_new(ctx, 1);
    ASSERT_NOT_NULL(env);
    r8e_env_frame_set(ctx, env, 0, r8e_from_int32_test(10));

    /* Three closures all sharing the same mutable variable */
    R8ECaptureDesc caps[3] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_NULL, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_TRUE, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl_inc = r8e_closure_new(ctx, func_inc, caps, 3, env);
    R8EClosure *cl_dec = r8e_closure_new(ctx, func_dec, caps, 3, env);
    R8EClosure *cl_read = r8e_closure_new(ctx, func_read, caps, 3, env);
    ASSERT_NOT_NULL(cl_inc);
    ASSERT_NOT_NULL(cl_dec);
    ASSERT_NOT_NULL(cl_read);

    /* All see 10 */
    ASSERT_EQ(r8e_closure_get_capture(cl_read, 0), r8e_from_int32_test(10));

    /* inc: 10 -> 11 */
    r8e_closure_set_capture(ctx, cl_inc, 0, r8e_from_int32_test(11));
    ASSERT_EQ(r8e_closure_get_capture(cl_read, 0), r8e_from_int32_test(11));

    /* dec: 11 -> 10 */
    r8e_closure_set_capture(ctx, cl_dec, 0, r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl_inc, 0), r8e_from_int32_test(10));
    ASSERT_EQ(r8e_closure_get_capture(cl_read, 0), r8e_from_int32_test(10));

    /* inc: 10 -> 11, then inc: 11 -> 12 */
    r8e_closure_set_capture(ctx, cl_inc, 0, r8e_from_int32_test(11));
    r8e_closure_set_capture(ctx, cl_inc, 0, r8e_from_int32_test(12));
    ASSERT_EQ(r8e_closure_get_capture(cl_dec, 0), r8e_from_int32_test(12));

    /* Cleanup */
    R8EEnvFrame *e;
    e = r8e_closure_get_env_frame(cl_inc);
    if (e) r8e_env_frame_release(ctx, e);
    e = r8e_closure_get_env_frame(cl_dec);
    if (e) r8e_env_frame_release(ctx, e);
    e = r8e_closure_get_env_frame(cl_read);
    if (e) r8e_env_frame_release(ctx, e);
    r8e_env_frame_release(ctx, env);

    free(cl_inc->storage.arr.values);
    free(cl_inc->storage.arr.meta);
    free(cl_dec->storage.arr.values);
    free(cl_dec->storage.arr.meta);
    free(cl_read->storage.arr.values);
    free(cl_read->storage.arr.meta);
    free(cl_inc);
    free(cl_dec);
    free(cl_read);
    free(func_inc);
    free(func_dec);
    free(func_read);
    free_ctx(ctx);
}

/*
 * Test: Env frame with zero slots
 */
static void test_env_frame_zero_slots(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 0);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->refcount, 1);
    ASSERT_EQ(frame->slot_count, 0);
    ASSERT_NULL(frame->slots);

    r8e_env_frame_release(ctx, frame);
    free_ctx(ctx);
}

/*
 * Test: Env frame set same value is no-op (optimization path)
 */
static void test_env_frame_set_same_value_noop(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 2);
    ASSERT_NOT_NULL(frame);

    /* Set a value */
    r8e_env_frame_set(ctx, frame, 0, r8e_from_int32_test(42));
    ASSERT_EQ(r8e_env_frame_get(frame, 0), r8e_from_int32_test(42));

    /* Setting the same value again should be a no-op */
    r8e_env_frame_set(ctx, frame, 0, r8e_from_int32_test(42));
    ASSERT_EQ(r8e_env_frame_get(frame, 0), r8e_from_int32_test(42));

    r8e_env_frame_release(ctx, frame);
    free_ctx(ctx);
}

/*
 * Test: Env frame overwrite multiple times
 */
static void test_env_frame_overwrite_repeatedly(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 1);
    ASSERT_NOT_NULL(frame);

    /* Overwrite the same slot 100 times */
    for (int i = 0; i < 100; i++) {
        r8e_env_frame_set(ctx, frame, 0, r8e_from_int32_test(i));
    }

    /* Final value should be 99 */
    ASSERT_EQ(r8e_env_frame_get(frame, 0), r8e_from_int32_test(99));

    r8e_env_frame_release(ctx, frame);
    free_ctx(ctx);
}

/*
 * Test: Closure with ALL mutable captures in ARRAY mode
 * Every capture is INDIRECT, all go through the env frame.
 */
static void test_all_mutable_captures_array(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[4] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(2), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 1, .reserved = 0 },
        { .value = r8e_from_int32_test(3), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 2, .reserved = 0 },
        { .value = r8e_from_int32_test(4), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 3, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 4, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    /* All captures should be mutable (INDIRECT) */
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, (uint8_t)i));
        ASSERT_EQ(cl->storage.arr.meta[i].kind, R8E_CAP_META_INDIRECT);
    }

    /* All values accessible and correct */
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(1));
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), r8e_from_int32_test(2));
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), r8e_from_int32_test(3));
    ASSERT_EQ(r8e_closure_get_capture(cl, 3), r8e_from_int32_test(4));

    /* Can set all of them */
    for (int i = 0; i < 4; i++) {
        bool ok = r8e_closure_set_capture(ctx, cl, (uint8_t)i,
                                            r8e_from_int32_test(i * 100));
        ASSERT_TRUE(ok);
    }

    /* Verify updates */
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(r8e_closure_get_capture(cl, (uint8_t)i),
                  r8e_from_int32_test(i * 100));
    }

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: 8 captures at ARRAY boundary (max ARRAY mode)
 */
static void test_eight_captures_array_boundary(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[8];
    for (int i = 0; i < 8; i++) {
        caps[i].value = r8e_from_int32_test(i + 1);
        caps[i].mutability = R8E_CAPTURE_IMMUTABLE;
        caps[i].frame_slot = 0;
        caps[i].reserved = 0;
    }

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 8, NULL);
    ASSERT_NOT_NULL(cl);

    /* 8 captures, all immutable: should be ARRAY mode (3-8 range) */
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);
    ASSERT_EQ(r8e_closure_capture_count(cl), 8);

    /* No env frame since all immutable */
    ASSERT_NULL(r8e_closure_get_env_frame(cl));

    /* All values correct */
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(r8e_closure_get_capture(cl, (uint8_t)i),
                  r8e_from_int32_test(i + 1));
    }

    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: FRAME mode with pre-existing env frame reuse
 * When an env frame is provided externally, FRAME mode retains it.
 */
static void test_frame_mode_reuse_existing_env(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* Pre-create an env frame with specific values */
    R8EEnvFrame *env = r8e_env_frame_new(ctx, 10);
    ASSERT_NOT_NULL(env);

    for (int i = 0; i < 10; i++) {
        r8e_env_frame_set(ctx, env, (uint16_t)i, r8e_from_int32_test(i * 7));
    }

    /* Create a closure with 10 captures, reusing the existing env */
    R8ECaptureDesc caps[10];
    for (int i = 0; i < 10; i++) {
        caps[i].value = r8e_from_int32_test(i * 10);
        caps[i].mutability = R8E_CAPTURE_IMMUTABLE;
        caps[i].frame_slot = 0;
        caps[i].reserved = 0;
    }

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 10, env);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_FRAME);

    /* The closure should reuse the existing env frame, not create a new one */
    R8EEnvFrame *cl_env = r8e_closure_get_env_frame(cl);
    ASSERT_NOT_NULL(cl_env);
    ASSERT_EQ((uintptr_t)cl_env, (uintptr_t)env);

    /* Values in the frame are what we set, not what captures provided
     * (because existing frame is reused as-is) */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(r8e_closure_get_capture(cl, (uint8_t)i),
                  r8e_from_int32_test(i * 7));
    }

    /* Env frame refcount should be 2 (our ref + closure's ref) */
    ASSERT_EQ(env->refcount, 2);

    r8e_env_frame_release(ctx, cl_env);
    r8e_env_frame_release(ctx, env);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Closure GC header correctness (kind bits)
 */
static void test_closure_gc_header_kind(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8EClosure *cl = r8e_closure_new(ctx, func, NULL, 0, NULL);
    ASSERT_NOT_NULL(cl);

    /* GC header should have kind = CLOSURE */
    uint32_t kind = (cl->flags & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT;
    ASSERT_EQ(kind, R8E_GC_KIND_CLOSURE);

    /* Proto should be R8E_PROTO_FUNCTION (2) */
    ASSERT_EQ(cl->proto_id, 2);

    /* Initial refcount should be 1 (from gc_header_init) */
    uint32_t rc = get_inline_rc((R8EGCHeader *)cl);
    ASSERT_EQ(rc, 1);

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: Env frame GC header correctness (kind bits)
 */
static void test_env_frame_gc_header_kind(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 3);
    ASSERT_NOT_NULL(frame);

    /* GC header should have kind = ENVFRAME */
    uint32_t kind = (frame->flags & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT;
    ASSERT_EQ(kind, R8E_GC_KIND_ENVFRAME);

    r8e_env_frame_release(ctx, frame);
    free_ctx(ctx);
}

/*
 * Test: closure_visit_refs in INLINE mode visits function and inline values
 */
static void test_visit_refs_inline_mode(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* Create an inline closure with 1 immutable non-pointer capture */
    R8ECaptureDesc caps[1] = {
        { .value = r8e_from_int32_test(42), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 1, NULL);
    ASSERT_NOT_NULL(cl);

    reset_visitor();
    r8e_closure_visit_refs(cl, counting_visitor, NULL);

    /* Should visit exactly 1 ref: the function (int32 captures are not pointers) */
    ASSERT_EQ(visitor_count, 1);
    ASSERT_EQ((uintptr_t)visitor_refs[0], (uintptr_t)func);

    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: closure_visit_refs in ARRAY mode visits function, direct values, and env
 */
static void test_visit_refs_array_mode(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    /* Create array closure with mixed captures */
    R8ECaptureDesc caps[3] = {
        { .value = r8e_from_int32_test(1), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(2), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = r8e_from_int32_test(3), .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 3, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    reset_visitor();
    r8e_closure_visit_refs(cl, counting_visitor, NULL);

    /* Should visit: function + env frame.
     * int32 values in DIRECT slots are not pointers, so not visited.
     * The env frame is visited as a whole reference. */
    ASSERT_TRUE(visitor_count >= 2); /* at least function + env */

    /* First ref should be the function */
    ASSERT_EQ((uintptr_t)visitor_refs[0], (uintptr_t)func);

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: closure_visit_refs in FRAME mode visits function and env frame
 */
static void test_visit_refs_frame_mode(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[10];
    for (int i = 0; i < 10; i++) {
        caps[i].value = r8e_from_int32_test(i);
        caps[i].mutability = R8E_CAPTURE_IMMUTABLE;
        caps[i].frame_slot = 0;
        caps[i].reserved = 0;
    }

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 10, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_FRAME);

    reset_visitor();
    r8e_closure_visit_refs(cl, counting_visitor, NULL);

    /* Should visit: function + env_frame = 2 */
    ASSERT_EQ(visitor_count, 2);
    ASSERT_EQ((uintptr_t)visitor_refs[0], (uintptr_t)func);

    R8EEnvFrame *frame = r8e_closure_get_env_frame(cl);
    ASSERT_EQ((uintptr_t)visitor_refs[1], (uintptr_t)frame);

    r8e_env_frame_release(ctx, frame);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: envframe_visit_refs enumerates pointer values in slots
 */
static void test_envframe_visit_refs(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 3);
    ASSERT_NOT_NULL(frame);

    /* Set slot 0 to a non-pointer (int32), slot 1 to a pointer-like value,
     * slot 2 to undefined. Only pointer values should be visited. */
    r8e_env_frame_set(ctx, frame, 0, r8e_from_int32_test(42));
    r8e_env_frame_set(ctx, frame, 2, R8E_NULL);
    /* slot 1 remains R8E_UNDEFINED */

    reset_visitor();
    r8e_envframe_visit_refs(frame, counting_visitor, NULL);

    /* None of int32, undefined, or null are pointers, so visitor_count = 0 */
    ASSERT_EQ(visitor_count, 0);

    r8e_env_frame_release(ctx, frame);
    free_ctx(ctx);
}

/*
 * Test: envframe_visit_refs with pointer values in slots
 */
static void test_envframe_visit_refs_with_pointers(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 2);
    ASSERT_NOT_NULL(frame);

    /* Create a dummy function and store its pointer in a slot */
    R8EFunction *func = make_dummy_func(ctx);
    R8EValue ptr_val = r8e_from_pointer_test(func);

    r8e_env_frame_set(ctx, frame, 0, ptr_val);
    r8e_env_frame_set(ctx, frame, 1, r8e_from_int32_test(99));

    reset_visitor();
    r8e_envframe_visit_refs(frame, counting_visitor, NULL);

    /* Only slot 0 is a pointer, slot 1 is int32 */
    ASSERT_EQ(visitor_count, 1);
    ASSERT_EQ((uintptr_t)visitor_refs[0], (uintptr_t)func);

    r8e_env_frame_release(ctx, frame);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: closure_visit_refs with NULL closure is safe
 */
static void test_visit_refs_null_closure(void) {
    reset_visitor();
    r8e_closure_visit_refs(NULL, counting_visitor, NULL);
    ASSERT_EQ(visitor_count, 0);
}

/*
 * Test: envframe_visit_refs with NULL frame is safe
 */
static void test_envframe_visit_refs_null(void) {
    reset_visitor();
    r8e_envframe_visit_refs(NULL, counting_visitor, NULL);
    ASSERT_EQ(visitor_count, 0);
}

/*
 * Test: Env frame retain NULL is safe (no-op)
 */
static void test_env_frame_retain_null_safe(void) {
    /* Should not crash */
    r8e_env_frame_retain(NULL);
}

/*
 * Test: Env frame release NULL is safe (no-op)
 */
static void test_env_frame_release_null_safe(void) {
    R8EContext *ctx = make_ctx();
    /* Should not crash */
    r8e_env_frame_release(ctx, NULL);
    free_ctx(ctx);
}

/*
 * Test: Closure with special NaN-boxed values as captures
 * Cover all the special singleton values: undefined, null, true, false.
 */
static void test_closure_special_value_captures(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[4] = {
        { .value = R8E_UNDEFINED, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_NULL, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_TRUE, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 },
        { .value = R8E_FALSE, .mutability = R8E_CAPTURE_IMMUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 4, NULL);
    ASSERT_NOT_NULL(cl);
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);

    ASSERT_EQ(r8e_closure_get_capture(cl, 0), R8E_UNDEFINED);
    ASSERT_EQ(r8e_closure_get_capture(cl, 1), R8E_NULL);
    ASSERT_EQ(r8e_closure_get_capture(cl, 2), R8E_TRUE);
    ASSERT_EQ(r8e_closure_get_capture(cl, 3), R8E_FALSE);

    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/*
 * Test: closure_new with NULL context returns NULL
 */
static void test_closure_new_null_ctx(void) {
    R8EFunction func;
    memset(&func, 0, sizeof(func));

    R8EClosure *cl = r8e_closure_new(NULL, &func, NULL, 0, NULL);
    ASSERT_NULL(cl);
}

/*
 * Test: closure_new with NULL func returns NULL
 */
static void test_closure_new_null_func(void) {
    R8EContext *ctx = make_ctx();

    R8EClosure *cl = r8e_closure_new(ctx, NULL, NULL, 0, NULL);
    ASSERT_NULL(cl);

    free_ctx(ctx);
}

/*
 * Test: closure_new with non-zero count but NULL captures returns NULL
 */
static void test_closure_new_null_captures_nonzero_count(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8EClosure *cl = r8e_closure_new(ctx, func, NULL, 5, NULL);
    ASSERT_NULL(cl);

    free(func);
    free_ctx(ctx);
}

/*
 * Test: Env frame multiple retain/release cycles
 */
static void test_env_frame_multiple_retain_release(void) {
    R8EContext *ctx = make_ctx();

    R8EEnvFrame *frame = r8e_env_frame_new(ctx, 2);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->refcount, 1);

    /* Retain 5 times */
    for (int i = 0; i < 5; i++) {
        r8e_env_frame_retain(frame);
    }
    ASSERT_EQ(frame->refcount, 6);

    /* Release 5 times (back to 1) */
    for (int i = 0; i < 5; i++) {
        r8e_env_frame_release(ctx, frame);
    }
    ASSERT_EQ(frame->refcount, 1);

    /* Final release frees the frame */
    r8e_env_frame_release(ctx, frame);

    free_ctx(ctx);
}

/*
 * Test: Single mutable capture with 1 capture count promotes to ARRAY
 */
static void test_single_mutable_promotes_to_array(void) {
    R8EContext *ctx = make_ctx();
    R8EFunction *func = make_dummy_func(ctx);

    R8ECaptureDesc caps[1] = {
        { .value = r8e_from_int32_test(7), .mutability = R8E_CAPTURE_MUTABLE,
          .frame_slot = 0, .reserved = 0 }
    };

    R8EClosure *cl = r8e_closure_new(ctx, func, caps, 1, NULL);
    ASSERT_NOT_NULL(cl);

    /* count=1 but has mutable -> NOT eligible for INLINE.
     * count <= 8 -> ARRAY mode */
    ASSERT_EQ(r8e_closure_capture_mode(cl), R8E_CAPTURE_MODE_ARRAY);
    ASSERT_TRUE(r8e_closure_is_capture_mutable(cl, 0));
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(7));

    /* Can update */
    r8e_closure_set_capture(ctx, cl, 0, r8e_from_int32_test(77));
    ASSERT_EQ(r8e_closure_get_capture(cl, 0), r8e_from_int32_test(77));

    R8EEnvFrame *env = r8e_closure_get_env_frame(cl);
    if (env) r8e_env_frame_release(ctx, env);
    free(cl->storage.arr.values);
    free(cl->storage.arr.meta);
    free(cl);
    free(func);
    free_ctx(ctx);
}

/* =========================================================================
 * Test Runner
 * ========================================================================= */

void run_closure_tests(void) {
    printf("\n=== Closure Tests ===\n");

    /* Original tests */
    RUN_TEST(test_one_immutable_capture_inline);
    RUN_TEST(test_two_immutable_captures_inline);
    RUN_TEST(test_three_immutable_captures_array);
    RUN_TEST(test_nine_captures_frame_mode);
    RUN_TEST(test_get_immutable_capture_value);
    RUN_TEST(test_set_mutable_capture);
    RUN_TEST(test_set_immutable_capture_fails);
    RUN_TEST(test_set_immutable_in_array_mode_fails);
    RUN_TEST(test_shared_env_frame_mutations);
    RUN_TEST(test_env_frame_lifecycle);
    RUN_TEST(test_mixed_captures);
    RUN_TEST(test_zero_captures);
    RUN_TEST(test_mutable_promotes_to_array);
    RUN_TEST(test_closure_get_function);

    /* New tests: PVCC snapshot and shared upvalue semantics */
    RUN_TEST(test_immutable_capture_snapshot);
    RUN_TEST(test_mutable_shared_upvalue_cross_closure);
    RUN_TEST(test_inline_storage_no_heap_alloc);
    RUN_TEST(test_array_storage_structure);
    RUN_TEST(test_frame_storage_all_through_env);

    /* New tests: nested closures */
    RUN_TEST(test_nested_closures_3_levels);
    RUN_TEST(test_nested_closures_mutable_chain);

    /* New tests: loop variable semantics */
    RUN_TEST(test_closure_loop_let_semantics);
    RUN_TEST(test_closure_loop_var_semantics);

    /* New tests: multiple closures sharing upvalue */
    RUN_TEST(test_multiple_closures_interleaved_mutations);

    /* New tests: env frame edge cases */
    RUN_TEST(test_env_frame_zero_slots);
    RUN_TEST(test_env_frame_set_same_value_noop);
    RUN_TEST(test_env_frame_overwrite_repeatedly);
    RUN_TEST(test_env_frame_multiple_retain_release);

    /* New tests: ARRAY mode edge cases */
    RUN_TEST(test_all_mutable_captures_array);
    RUN_TEST(test_eight_captures_array_boundary);
    RUN_TEST(test_single_mutable_promotes_to_array);

    /* New tests: FRAME mode with env reuse */
    RUN_TEST(test_frame_mode_reuse_existing_env);

    /* New tests: GC header and visitor coverage */
    RUN_TEST(test_closure_gc_header_kind);
    RUN_TEST(test_env_frame_gc_header_kind);
    RUN_TEST(test_visit_refs_inline_mode);
    RUN_TEST(test_visit_refs_array_mode);
    RUN_TEST(test_visit_refs_frame_mode);
    RUN_TEST(test_envframe_visit_refs);
    RUN_TEST(test_envframe_visit_refs_with_pointers);
    RUN_TEST(test_visit_refs_null_closure);
    RUN_TEST(test_envframe_visit_refs_null);

    /* New tests: NULL safety and error paths */
    RUN_TEST(test_env_frame_retain_null_safe);
    RUN_TEST(test_env_frame_release_null_safe);
    RUN_TEST(test_closure_special_value_captures);
    RUN_TEST(test_closure_new_null_ctx);
    RUN_TEST(test_closure_new_null_func);
    RUN_TEST(test_closure_new_null_captures_nonzero_count);

    printf("\n  Closure Tests: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf("\n");
}

#ifdef TEST_CLOSURE_MAIN
int main(void) {
    run_closure_tests();
    return tests_failed > 0 ? 1 : 0;
}
#endif
