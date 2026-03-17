/*
 * test_function.c - Unit tests for r8e_function.c
 *
 * Tests cover:
 *   - Bytecode function creation and field verification
 *   - Constant pool management
 *   - Function arity (including rest params and defaults)
 *   - Flag queries (strict, arrow, generator, async, constructor)
 *   - Native function creation and invocation
 *   - Bound function creation and argument merging
 *   - Function source location info
 *   - Function free and cleanup
 *   - Arguments object creation (mapped and unmapped)
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
 * Type definitions (must match r8e_function.c layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)

static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}
static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}
static inline R8EValue r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* GC header */
#define R8E_GC_KIND_MASK         0x000000E0u
#define R8E_GC_KIND_SHIFT        5
#define R8E_GC_KIND_FUNCTION     5u
#define R8E_GC_KIND_NATIVE_FUNC  6u
#define R8E_GC_KIND_BOUND_FUNC   7u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_PROTO_FUNCTION   3

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

#define R8E_FUNC_IS_STRICT        0x01u
#define R8E_FUNC_IS_ARROW         0x02u
#define R8E_FUNC_IS_GENERATOR     0x04u
#define R8E_FUNC_IS_ASYNC         0x08u
#define R8E_FUNC_IS_CONSTRUCTOR   0x10u
#define R8E_FUNC_HAS_REST_PARAMS  0x40u
#define R8E_FUNC_HAS_DEFAULTS     0x80u

/* Context (minimal, must match r8e_function.c) */
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

typedef struct {
    R8ESuspectList  suspects;
    R8EExtRCTable   ext_rc;
    uint32_t        epoch_threshold;
    uint32_t        total_allocated;
    uint32_t        total_freed;
    uint32_t        cycles_collected;
} R8EGCState;

typedef struct {
    R8EValue    exception;
    bool        has_exception;
    uint16_t    error_line;
    uint16_t    error_col;
    const char *error_file;
} R8EErrorState;

typedef struct R8EFunction R8EFunction;
typedef struct R8ECallFrame {
    R8EFunction    *func;
    const uint8_t  *pc;
    R8EValue       *locals;
    R8EValue       *stack_base;
    R8EValue        this_val;
    R8EValue        new_target;
    uint32_t        line;
    uint16_t        col;
    const char     *source_name;
} R8ECallFrame;

#define R8E_MAX_CALL_FRAMES 512

typedef struct R8EContext {
    R8EGCState    gc;
    R8EErrorState error;
    uint32_t      stack_depth;
    uint32_t      max_stack_depth;
    R8ECallFrame  call_frames[R8E_MAX_CALL_FRAMES];
    uint32_t      frame_count;
} R8EContext;

/* =========================================================================
 * External function declarations (from r8e_function.c)
 * ========================================================================= */

struct R8EFunction {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t  *bytecode;
    uint32_t  bytecode_len;
    uint16_t  param_count;
    uint16_t  local_count;
    uint16_t  capture_count;
    uint16_t  stack_size;
    uint32_t  name_atom;
    R8EValue *constants;
    uint16_t  const_count;
    uint8_t   func_flags;
    uint8_t   rest_param_idx;
    uint16_t  source_line;
    uint16_t  source_col;
    const char *source_name;
    uint32_t *default_offsets;
};

typedef R8EValue (*R8ENativeFunc)(void *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv);

typedef struct R8ENativeFunction {
    uint32_t      flags;
    uint32_t      proto_id;
    R8ENativeFunc callback;
    uint32_t      name_atom;
    int16_t       arity;
    uint8_t       func_flags;
    uint8_t       reserved;
} R8ENativeFunction;

typedef struct R8EBoundFunction {
    uint32_t  flags;
    uint32_t  proto_id;
    R8EValue  target;
    R8EValue  bound_this;
    R8EValue *bound_args;
    uint16_t  bound_argc;
    uint16_t  remaining_arity;
    uint32_t  name_atom;
} R8EBoundFunction;

extern R8EFunction *r8e_function_new(R8EContext *ctx,
                                      const uint8_t *bytecode,
                                      uint32_t bytecode_len,
                                      uint16_t param_count,
                                      uint16_t local_count,
                                      uint32_t name_atom);
extern void r8e_function_free(R8EContext *ctx, R8EFunction *func);
extern void r8e_function_set_constants(R8EContext *ctx, R8EFunction *func,
                                        const R8EValue *constants,
                                        uint16_t count);
extern void r8e_function_set_flags(R8EFunction *func, uint8_t func_flags);
extern void r8e_function_set_rest_param(R8EFunction *func, uint8_t index);
extern void r8e_function_set_defaults(R8EFunction *func,
                                       const uint32_t *offsets,
                                       uint16_t count);
extern void r8e_function_set_source(R8EFunction *func,
                                     const char *source_name,
                                     uint16_t line, uint16_t col);
extern uint32_t r8e_function_get_name(const R8EFunction *func);
extern uint16_t r8e_function_get_arity(const R8EFunction *func);
extern bool r8e_function_is_strict(const R8EFunction *func);
extern bool r8e_function_is_arrow(const R8EFunction *func);
extern bool r8e_function_is_generator(const R8EFunction *func);
extern bool r8e_function_is_async(const R8EFunction *func);
extern bool r8e_function_is_constructor(const R8EFunction *func);

extern R8ENativeFunction *r8e_native_function_new(R8EContext *ctx,
                                                    R8ENativeFunc callback,
                                                    uint32_t name_atom,
                                                    int16_t arity);
extern void r8e_native_function_free(R8EContext *ctx, R8ENativeFunction *nf);
extern R8EValue r8e_native_function_call(R8EContext *ctx,
                                          R8ENativeFunction *nf,
                                          R8EValue this_val,
                                          int argc,
                                          const R8EValue *argv);

extern R8EBoundFunction *r8e_bound_function_new(R8EContext *ctx,
                                                  R8EValue target,
                                                  R8EValue bound_this,
                                                  const R8EValue *bound_args,
                                                  uint16_t bound_argc,
                                                  uint32_t name_atom);
extern void r8e_bound_function_free(R8EContext *ctx, R8EBoundFunction *bf);

/* These are provided by r8e_gc.c and r8e_error.c at link time */
extern void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind, uint32_t proto_id);
extern void r8e_retain(R8EContext *ctx, R8EValue val);
extern void r8e_release(R8EContext *ctx, R8EValue val);
extern R8EValue r8e_throw_type_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_range_error(R8EContext *ctx, const char *fmt, ...);

/* =========================================================================
 * Test Harness
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { if (!(e)) { printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #e); g_assert_fail = 1; return; } } while(0)
#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a,b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a,b) ASSERT_TRUE((a) != (b))
#define ASSERT_EQ_INT(a,b) do { int _a=(a), _b=(b); if (_a != _b) { printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_STR(a,b) do { if (strcmp((a),(b))!=0) { printf("  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_assert_fail = 1; return; } } while(0)
#define RUN_TEST(fn) do { g_assert_fail = 0; g_tests_run++; printf("  %s... ", #fn); fn(); if (g_assert_fail) { g_tests_failed++; } else { g_tests_passed++; printf("OK\n"); } } while(0)

/* =========================================================================
 * Helper: create a minimal test context
 * ========================================================================= */

static R8EContext *make_test_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    if (ctx) {
        ctx->max_stack_depth = 512;
    }
    return ctx;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

/* Create a bytecode function and verify fields */
static void test_func_create_basic(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x05, 42, 0x4F }; /* PUSH_INT8 42, RETURN */
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 2, 4, 100);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ_INT(f->param_count, 2);
    ASSERT_EQ_INT(f->local_count, 4);
    ASSERT_EQ(f->name_atom, (uint32_t)100);
    ASSERT_EQ(f->bytecode_len, (uint32_t)sizeof(bc));
    /* Bytecode should be copied */
    ASSERT_NE((uintptr_t)f->bytecode, (uintptr_t)bc);
    ASSERT_TRUE(memcmp(f->bytecode, bc, sizeof(bc)) == 0);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* NULL bytecode is OK (empty function) */
static void test_func_create_null_bytecode(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EFunction *f = r8e_function_new(ctx, NULL, 0, 0, 0, 0);
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(f->bytecode == NULL);
    ASSERT_EQ(f->bytecode_len, (uint32_t)0);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Function arity (simple case) */
static void test_func_arity_simple(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 }; /* RETURN_UNDEFINED */
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 3, 4, 0);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ_INT(r8e_function_get_arity(f), 3);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Function arity with rest parameter */
static void test_func_arity_rest(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 3, 4, 0);
    ASSERT_TRUE(f != NULL);

    r8e_function_set_rest_param(f, 2);
    /* Rest param not counted in arity */
    ASSERT_EQ_INT(r8e_function_get_arity(f), 2);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Function arity with default parameters */
static void test_func_arity_defaults(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 3, 4, 0);
    ASSERT_TRUE(f != NULL);

    /* Parameter 1 has a default (offset 100), params 0 and 2 do not */
    uint32_t defaults[] = { 0, 100, 0 };
    r8e_function_set_defaults(f, defaults, 3);
    /* Arity = first parameter with default = index 1 */
    ASSERT_EQ_INT(r8e_function_get_arity(f), 1);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Flag queries */
static void test_func_flags(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 0, 0, 0);
    ASSERT_TRUE(f != NULL);

    /* Default: constructor flag is set */
    ASSERT_TRUE(r8e_function_is_constructor(f));

    /* Set strict + arrow */
    r8e_function_set_flags(f, R8E_FUNC_IS_STRICT | R8E_FUNC_IS_ARROW);
    ASSERT_TRUE(r8e_function_is_strict(f));
    ASSERT_TRUE(r8e_function_is_arrow(f));
    ASSERT_FALSE(r8e_function_is_generator(f));
    ASSERT_FALSE(r8e_function_is_async(f));
    /* Arrow cleared constructor flag */
    ASSERT_FALSE(r8e_function_is_constructor(f));

    /* Set generator + async */
    r8e_function_set_flags(f, R8E_FUNC_IS_GENERATOR | R8E_FUNC_IS_ASYNC);
    ASSERT_TRUE(r8e_function_is_generator(f));
    ASSERT_TRUE(r8e_function_is_async(f));
    ASSERT_FALSE(r8e_function_is_strict(f));

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Source location */
static void test_func_source_location(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 0, 0, 0);
    ASSERT_TRUE(f != NULL);

    r8e_function_set_source(f, "test.js", 42, 10);
    ASSERT_EQ_INT(f->source_line, 42);
    ASSERT_EQ_INT(f->source_col, 10);
    ASSERT_EQ_STR(f->source_name, "test.js");

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Constant pool management */
static void test_func_constants(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 0, 0, 0);
    ASSERT_TRUE(f != NULL);

    R8EValue consts[] = { r8e_from_int32(42), r8e_from_int32(99) };
    r8e_function_set_constants(ctx, f, consts, 2);
    ASSERT_EQ_INT(f->const_count, 2);
    ASSERT_TRUE(f->constants != NULL);
    ASSERT_TRUE(R8E_IS_INT32(f->constants[0]));
    ASSERT_EQ_INT(r8e_get_int32(f->constants[0]), 42);
    ASSERT_EQ_INT(r8e_get_int32(f->constants[1]), 99);

    /* Replace constants */
    R8EValue consts2[] = { r8e_from_int32(7) };
    r8e_function_set_constants(ctx, f, consts2, 1);
    ASSERT_EQ_INT(f->const_count, 1);
    ASSERT_EQ_INT(r8e_get_int32(f->constants[0]), 7);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Native function creation */
static R8EValue test_native_add(void *ctx, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 2) return R8E_UNDEFINED;
    if (R8E_IS_INT32(argv[0]) && R8E_IS_INT32(argv[1])) {
        return r8e_from_int32(r8e_get_int32(argv[0]) +
                              r8e_get_int32(argv[1]));
    }
    return R8E_UNDEFINED;
}

static void test_func_native_create(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 42, 2);
    ASSERT_TRUE(nf != NULL);
    ASSERT_EQ(nf->callback, test_native_add);
    ASSERT_EQ(nf->name_atom, (uint32_t)42);
    ASSERT_EQ_INT(nf->arity, 2);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Native function call */
static void test_func_native_call(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue args[] = { r8e_from_int32(10), r8e_from_int32(20) };
    R8EValue result = r8e_native_function_call(ctx, nf, R8E_UNDEFINED, 2, args);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 30);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Bound function creation */
static void test_func_bound_create(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    /* Create a native function as target */
    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue target = r8e_from_pointer(nf);
    R8EValue bound_this = r8e_from_int32(999);
    R8EValue bound_args[] = { r8e_from_int32(5) };

    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, target, bound_this, bound_args, 1, 0);
    ASSERT_TRUE(bf != NULL);
    ASSERT_EQ(bf->target, target);
    ASSERT_EQ(bf->bound_this, bound_this);
    ASSERT_EQ_INT(bf->bound_argc, 1);
    ASSERT_TRUE(bf->bound_args != NULL);
    ASSERT_TRUE(R8E_IS_INT32(bf->bound_args[0]));
    ASSERT_EQ_INT(r8e_get_int32(bf->bound_args[0]), 5);

    r8e_bound_function_free(ctx, bf);
    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Bound function with no args */
static void test_func_bound_no_args(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue target = r8e_from_pointer(nf);
    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, target, R8E_NULL, NULL, 0, 0);
    ASSERT_TRUE(bf != NULL);
    ASSERT_EQ_INT(bf->bound_argc, 0);
    ASSERT_TRUE(bf->bound_args == NULL);

    r8e_bound_function_free(ctx, bf);
    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* NULL ctx should return NULL */
static void test_func_create_null_ctx(void) {
    R8EFunction *f = r8e_function_new(NULL, NULL, 0, 0, 0, 0);
    ASSERT_TRUE(f == NULL);
}

/* Function name atom */
static void test_func_name(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 0, 0, 42);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(r8e_function_get_name(f), (uint32_t)42);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* =========================================================================
 * NEW: Arguments Object Tests
 * ========================================================================= */

/* External declarations for arguments API */
typedef struct R8EArguments {
    uint32_t  flags;
    uint32_t  proto_id;
    R8EValue *values;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue  callee;
    bool      is_mapped;
    R8EValue *mapped_locals;
    uint16_t  mapped_count;
} R8EArguments;

extern R8EArguments *r8e_arguments_new(R8EContext *ctx,
                                         const R8EValue *argv, int argc,
                                         R8EValue callee, bool is_strict,
                                         R8EValue *locals, uint16_t param_count);
extern R8EValue r8e_arguments_get(const R8EArguments *args, uint32_t index);
extern void r8e_arguments_set(R8EContext *ctx, R8EArguments *args,
                                uint32_t index, R8EValue val);
extern void r8e_arguments_free(R8EContext *ctx, R8EArguments *args);

/* Callable kind enum and functions */
typedef enum {
    R8E_CALLABLE_NONE    = 0,
    R8E_CALLABLE_CLOSURE = 1,
    R8E_CALLABLE_NATIVE  = 2,
    R8E_CALLABLE_BOUND   = 3
} R8ECallableKind;

extern R8ECallableKind r8e_get_callable_kind(R8EValue val);
extern bool r8e_is_callable(R8EValue val);

/* Function call/apply/bind */
extern R8EValue r8e_function_call(R8EContext *ctx, R8EValue func_val,
                                    R8EValue this_val, int argc,
                                    const R8EValue *argv);
extern R8EValue r8e_function_apply(R8EContext *ctx, R8EValue func_val,
                                     R8EValue this_val, R8EValue args_array);
extern R8EValue r8e_function_bind(R8EContext *ctx, R8EValue func_val,
                                    R8EValue this_val, int argc,
                                    const R8EValue *argv);
extern R8EValue r8e_construct(R8EContext *ctx, R8EValue func_val,
                                int argc, const R8EValue *argv,
                                R8EValue new_target);

/* Create unmapped (strict) arguments object */
static void test_func_args_strict(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue argv[] = { r8e_from_int32(10), r8e_from_int32(20) };
    R8EArguments *args = r8e_arguments_new(
        ctx, argv, 2, R8E_UNDEFINED, true, NULL, 0);
    ASSERT_TRUE(args != NULL);
    ASSERT_EQ_INT(args->length, 2);
    ASSERT_FALSE(args->is_mapped);
    ASSERT_TRUE(args->callee == R8E_UNDEFINED);

    /* Get values */
    R8EValue v0 = r8e_arguments_get(args, 0);
    ASSERT_TRUE(R8E_IS_INT32(v0));
    ASSERT_EQ_INT(r8e_get_int32(v0), 10);

    R8EValue v1 = r8e_arguments_get(args, 1);
    ASSERT_TRUE(R8E_IS_INT32(v1));
    ASSERT_EQ_INT(r8e_get_int32(v1), 20);

    /* Out of range returns undefined */
    R8EValue v2 = r8e_arguments_get(args, 2);
    ASSERT_TRUE(v2 == R8E_UNDEFINED);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* Create mapped (sloppy) arguments object */
static void test_func_args_mapped(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue callee = r8e_from_int32(999); /* dummy callee */
    R8EValue argv[] = { r8e_from_int32(10), r8e_from_int32(20) };
    R8EValue locals[2] = { r8e_from_int32(10), r8e_from_int32(20) };

    R8EArguments *args = r8e_arguments_new(
        ctx, argv, 2, callee, false, locals, 2);
    ASSERT_TRUE(args != NULL);
    ASSERT_TRUE(args->is_mapped);
    ASSERT_EQ_INT(args->mapped_count, 2);
    /* Callee should be stored */
    ASSERT_EQ(args->callee, callee);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* Arguments set on strict mode */
static void test_func_args_set_strict(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue argv[] = { r8e_from_int32(10) };
    R8EArguments *args = r8e_arguments_new(
        ctx, argv, 1, R8E_UNDEFINED, true, NULL, 0);
    ASSERT_TRUE(args != NULL);

    /* Set value */
    r8e_arguments_set(ctx, args, 0, r8e_from_int32(99));
    R8EValue v0 = r8e_arguments_get(args, 0);
    ASSERT_TRUE(R8E_IS_INT32(v0));
    ASSERT_EQ_INT(r8e_get_int32(v0), 99);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* Arguments set on mapped mode also writes to locals */
static void test_func_args_set_mapped(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue argv[] = { r8e_from_int32(10) };
    R8EValue locals[1] = { r8e_from_int32(10) };

    R8EArguments *args = r8e_arguments_new(
        ctx, argv, 1, R8E_UNDEFINED, false, locals, 1);
    ASSERT_TRUE(args != NULL);

    /* Set through arguments */
    r8e_arguments_set(ctx, args, 0, r8e_from_int32(77));

    /* Local should also be updated */
    ASSERT_TRUE(R8E_IS_INT32(locals[0]));
    ASSERT_EQ_INT(r8e_get_int32(locals[0]), 77);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* Mapped mode get reads from locals */
static void test_func_args_mapped_get_reads_local(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue argv[] = { r8e_from_int32(10) };
    R8EValue locals[1] = { r8e_from_int32(10) };

    R8EArguments *args = r8e_arguments_new(
        ctx, argv, 1, R8E_UNDEFINED, false, locals, 1);
    ASSERT_TRUE(args != NULL);

    /* Modify local directly */
    locals[0] = r8e_from_int32(55);

    /* Arguments.get should read from local */
    R8EValue v = r8e_arguments_get(args, 0);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 55);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* Arguments with zero args */
static void test_func_args_empty(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EArguments *args = r8e_arguments_new(
        ctx, NULL, 0, R8E_UNDEFINED, true, NULL, 0);
    ASSERT_TRUE(args != NULL);
    ASSERT_EQ_INT(args->length, 0);
    ASSERT_TRUE(r8e_arguments_get(args, 0) == R8E_UNDEFINED);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* Arguments NULL ctx returns NULL */
static void test_func_args_null_ctx(void) {
    R8EArguments *args = r8e_arguments_new(
        NULL, NULL, 0, R8E_UNDEFINED, true, NULL, 0);
    ASSERT_TRUE(args == NULL);
}

/* Arguments set out of range is a no-op */
static void test_func_args_set_out_of_range(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue argv[] = { r8e_from_int32(10) };
    R8EArguments *args = r8e_arguments_new(
        ctx, argv, 1, R8E_UNDEFINED, true, NULL, 0);
    ASSERT_TRUE(args != NULL);

    /* Set index 5 (out of range) - should not crash */
    r8e_arguments_set(ctx, args, 5, r8e_from_int32(42));
    /* Value should still be undefined at index 5 */
    ASSERT_TRUE(r8e_arguments_get(args, 5) == R8E_UNDEFINED);

    r8e_arguments_free(ctx, args);
    free(ctx);
}

/* =========================================================================
 * NEW: Callable Kind Identification
 * ========================================================================= */

/* Non-pointer is not callable */
static void test_func_callable_kind_int(void) {
    R8EValue v = r8e_from_int32(42);
    ASSERT_EQ_INT(r8e_get_callable_kind(v), R8E_CALLABLE_NONE);
    ASSERT_FALSE(r8e_is_callable(v));
}

/* Undefined is not callable */
static void test_func_callable_kind_undefined(void) {
    ASSERT_EQ_INT(r8e_get_callable_kind(R8E_UNDEFINED), R8E_CALLABLE_NONE);
    ASSERT_FALSE(r8e_is_callable(R8E_UNDEFINED));
}

/* Null is not callable */
static void test_func_callable_kind_null(void) {
    ASSERT_EQ_INT(r8e_get_callable_kind(R8E_NULL), R8E_CALLABLE_NONE);
    ASSERT_FALSE(r8e_is_callable(R8E_NULL));
}

/* Native function is callable */
static void test_func_callable_kind_native(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue v = r8e_from_pointer(nf);
    ASSERT_EQ_INT(r8e_get_callable_kind(v), R8E_CALLABLE_NATIVE);
    ASSERT_TRUE(r8e_is_callable(v));

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Bound function is callable */
static void test_func_callable_kind_bound(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue target = r8e_from_pointer(nf);
    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, target, R8E_NULL, NULL, 0, 0);
    ASSERT_TRUE(bf != NULL);

    R8EValue bv = r8e_from_pointer(bf);
    ASSERT_EQ_INT(r8e_get_callable_kind(bv), R8E_CALLABLE_BOUND);
    ASSERT_TRUE(r8e_is_callable(bv));

    r8e_bound_function_free(ctx, bf);
    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* =========================================================================
 * NEW: Function Call via r8e_function_call
 * ========================================================================= */

/* Call native function through function_call */
static void test_func_call_native(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue func_val = r8e_from_pointer(nf);
    R8EValue args[] = { r8e_from_int32(15), r8e_from_int32(27) };
    R8EValue result = r8e_function_call(ctx, func_val, R8E_UNDEFINED, 2, args);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Call with not-a-function value */
static void test_func_call_not_callable(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_function_call(ctx, r8e_from_int32(42),
                                          R8E_UNDEFINED, 0, NULL);
    /* Should return undefined (error thrown) */
    ASSERT_EQ(result, R8E_UNDEFINED);

    free(ctx);
}

/* Call with NULL ctx */
static void test_func_call_null_ctx(void) {
    R8EValue result = r8e_function_call(NULL, R8E_UNDEFINED,
                                          R8E_UNDEFINED, 0, NULL);
    ASSERT_EQ(result, R8E_UNDEFINED);
}

/* =========================================================================
 * NEW: Function Apply
 * ========================================================================= */

/* Apply with null args -> calls with 0 arguments */
static void test_func_apply_null_args(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue func_val = r8e_from_pointer(nf);
    R8EValue result = r8e_function_apply(ctx, func_val, R8E_UNDEFINED, R8E_NULL);
    /* test_native_add returns UNDEFINED for argc < 2 */
    ASSERT_EQ(result, R8E_UNDEFINED);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Apply with undefined args -> calls with 0 arguments */
static void test_func_apply_undef_args(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue func_val = r8e_from_pointer(nf);
    R8EValue result = r8e_function_apply(ctx, func_val, R8E_UNDEFINED,
                                           R8E_UNDEFINED);
    ASSERT_EQ(result, R8E_UNDEFINED);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Apply with non-object args -> type error */
static void test_func_apply_bad_args(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue func_val = r8e_from_pointer(nf);
    R8EValue result = r8e_function_apply(ctx, func_val, R8E_UNDEFINED,
                                           r8e_from_int32(42));
    ASSERT_EQ(result, R8E_UNDEFINED);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* =========================================================================
 * NEW: Function Bind
 * ========================================================================= */

/* Bind a native function */
static void test_func_bind_native(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue func_val = r8e_from_pointer(nf);
    R8EValue bound_args[] = { r8e_from_int32(5) };
    R8EValue bound_val = r8e_function_bind(ctx, func_val,
                                             R8E_UNDEFINED, 1, bound_args);
    ASSERT_TRUE(R8E_IS_POINTER(bound_val));

    /* Clean up */
    R8EBoundFunction *bf = (R8EBoundFunction *)r8e_get_pointer(bound_val);
    r8e_bound_function_free(ctx, bf);
    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Bind with non-callable -> error */
static void test_func_bind_not_callable(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_function_bind(ctx, r8e_from_int32(42),
                                          R8E_UNDEFINED, 0, NULL);
    ASSERT_EQ(result, R8E_UNDEFINED);

    free(ctx);
}

/* =========================================================================
 * NEW: Bound Function with multiple args
 * ========================================================================= */

/* Bound function with 3 pre-supplied args */
static void test_func_bound_multiple_args(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    R8EValue target = r8e_from_pointer(nf);
    R8EValue bound_args[] = { r8e_from_int32(1), r8e_from_int32(2),
                               r8e_from_int32(3) };
    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, target, R8E_UNDEFINED, bound_args, 3, 0);
    ASSERT_TRUE(bf != NULL);
    ASSERT_EQ_INT(bf->bound_argc, 3);
    ASSERT_TRUE(bf->bound_args != NULL);
    ASSERT_EQ_INT(r8e_get_int32(bf->bound_args[0]), 1);
    ASSERT_EQ_INT(r8e_get_int32(bf->bound_args[1]), 2);
    ASSERT_EQ_INT(r8e_get_int32(bf->bound_args[2]), 3);

    r8e_bound_function_free(ctx, bf);
    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Bound function with non-pointer target -> returns NULL */
static void test_func_bound_bad_target(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, r8e_from_int32(42), R8E_UNDEFINED, NULL, 0, 0);
    ASSERT_TRUE(bf == NULL);

    free(ctx);
}

/* =========================================================================
 * NEW: Native function with zero arity callback
 * ========================================================================= */

static R8EValue test_native_constant(void *ctx, R8EValue this_val,
                                       int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return r8e_from_int32(42);
}

/* Native function with 0 arity */
static void test_func_native_zero_arity(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(
        ctx, test_native_constant, 0, 0);
    ASSERT_TRUE(nf != NULL);
    ASSERT_EQ_INT(nf->arity, 0);

    R8EValue result = r8e_native_function_call(ctx, nf, R8E_UNDEFINED, 0, NULL);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Native function with variadic arity (-1) */
static void test_func_native_variadic(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(
        ctx, test_native_add, 0, -1);
    ASSERT_TRUE(nf != NULL);
    ASSERT_EQ_INT(nf->arity, -1);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Native function NULL callback -> returns NULL */
static void test_func_native_null_callback(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, NULL, 0, 0);
    ASSERT_TRUE(nf == NULL);

    free(ctx);
}

/* =========================================================================
 * NEW: Function GC tracking
 * ========================================================================= */

/* Function creation increments total_allocated, free increments total_freed */
static void test_func_gc_tracking(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint32_t before_alloc = ctx->gc.total_allocated;
    uint32_t before_freed = ctx->gc.total_freed;

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 0, 0, 0);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ_INT(ctx->gc.total_allocated, before_alloc + 1);

    r8e_function_free(ctx, f);
    ASSERT_EQ_INT(ctx->gc.total_freed, before_freed + 1);

    free(ctx);
}

/* Native function GC tracking */
static void test_func_native_gc_tracking(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint32_t before = ctx->gc.total_allocated;
    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);
    ASSERT_EQ_INT(ctx->gc.total_allocated, before + 1);

    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* Bound function GC tracking */
static void test_func_bound_gc_tracking(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8ENativeFunction *nf = r8e_native_function_new(ctx, test_native_add, 0, 2);
    ASSERT_TRUE(nf != NULL);

    uint32_t before = ctx->gc.total_allocated;
    R8EValue target = r8e_from_pointer(nf);
    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, target, R8E_NULL, NULL, 0, 0);
    ASSERT_TRUE(bf != NULL);
    ASSERT_EQ_INT(ctx->gc.total_allocated, before + 1);

    r8e_bound_function_free(ctx, bf);
    r8e_native_function_free(ctx, nf);
    free(ctx);
}

/* =========================================================================
 * NEW: Function defaults with all parameters having defaults
 * ========================================================================= */

static void test_func_arity_all_defaults(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 3, 4, 0);
    ASSERT_TRUE(f != NULL);

    /* All params have defaults starting from index 0 */
    uint32_t defaults[] = { 10, 20, 30 };
    r8e_function_set_defaults(f, defaults, 3);
    /* Arity = first param with default = 0 */
    ASSERT_EQ_INT(r8e_function_get_arity(f), 0);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Function with both rest param and defaults */
static void test_func_arity_rest_and_defaults(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    uint8_t bc[] = { 0x50 };
    R8EFunction *f = r8e_function_new(ctx, bc, sizeof(bc), 4, 5, 0);
    ASSERT_TRUE(f != NULL);

    /* Param 2 has a default, param 3 is rest */
    uint32_t defaults[] = { 0, 0, 100, 0 };
    r8e_function_set_defaults(f, defaults, 4);
    r8e_function_set_rest_param(f, 3);

    /* Arity: first default is at index 2, rest is excluded anyway */
    uint16_t arity = r8e_function_get_arity(f);
    ASSERT_TRUE(arity <= 2);

    r8e_function_free(ctx, f);
    free(ctx);
}

/* Function free with NULL is safe */
static void test_func_free_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    r8e_function_free(ctx, NULL);
    /* No crash */

    free(ctx);
}

/* Native function free with NULL is safe */
static void test_func_native_free_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    r8e_native_function_free(ctx, NULL);
    /* No crash */

    free(ctx);
}

/* Bound function free with NULL is safe */
static void test_func_bound_free_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    r8e_bound_function_free(ctx, NULL);
    /* No crash */

    free(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_function_tests(void) {
    /* Basic creation and fields */
    RUN_TEST(test_func_create_basic);
    RUN_TEST(test_func_create_null_bytecode);
    RUN_TEST(test_func_create_null_ctx);
    RUN_TEST(test_func_name);

    /* Arity */
    RUN_TEST(test_func_arity_simple);
    RUN_TEST(test_func_arity_rest);
    RUN_TEST(test_func_arity_defaults);
    RUN_TEST(test_func_arity_all_defaults);
    RUN_TEST(test_func_arity_rest_and_defaults);

    /* Flags and metadata */
    RUN_TEST(test_func_flags);
    RUN_TEST(test_func_source_location);
    RUN_TEST(test_func_constants);

    /* Native function */
    RUN_TEST(test_func_native_create);
    RUN_TEST(test_func_native_call);
    RUN_TEST(test_func_native_zero_arity);
    RUN_TEST(test_func_native_variadic);
    RUN_TEST(test_func_native_null_callback);

    /* Bound function */
    RUN_TEST(test_func_bound_create);
    RUN_TEST(test_func_bound_no_args);
    RUN_TEST(test_func_bound_multiple_args);
    RUN_TEST(test_func_bound_bad_target);

    /* Arguments object */
    RUN_TEST(test_func_args_strict);
    RUN_TEST(test_func_args_mapped);
    RUN_TEST(test_func_args_set_strict);
    RUN_TEST(test_func_args_set_mapped);
    RUN_TEST(test_func_args_mapped_get_reads_local);
    RUN_TEST(test_func_args_empty);
    RUN_TEST(test_func_args_null_ctx);
    RUN_TEST(test_func_args_set_out_of_range);

    /* Callable kind identification */
    RUN_TEST(test_func_callable_kind_int);
    RUN_TEST(test_func_callable_kind_undefined);
    RUN_TEST(test_func_callable_kind_null);
    RUN_TEST(test_func_callable_kind_native);
    RUN_TEST(test_func_callable_kind_bound);

    /* Function call */
    RUN_TEST(test_func_call_native);
    RUN_TEST(test_func_call_not_callable);
    RUN_TEST(test_func_call_null_ctx);

    /* Function apply */
    RUN_TEST(test_func_apply_null_args);
    RUN_TEST(test_func_apply_undef_args);
    RUN_TEST(test_func_apply_bad_args);

    /* Function bind */
    RUN_TEST(test_func_bind_native);
    RUN_TEST(test_func_bind_not_callable);

    /* GC tracking */
    RUN_TEST(test_func_gc_tracking);
    RUN_TEST(test_func_native_gc_tracking);
    RUN_TEST(test_func_bound_gc_tracking);

    /* Null safety */
    RUN_TEST(test_func_free_null);
    RUN_TEST(test_func_native_free_null);
    RUN_TEST(test_func_bound_free_null);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_FUNCTION_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[function]\n");
    run_function_tests();
    printf("\n=== function: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
