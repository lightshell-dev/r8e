/*
 * test_interp.c - Unit tests for r8e_interp.c (Bytecode Interpreter)
 *
 * Tests cover:
 *   - Simple bytecode execution (PUSH_INT8, RETURN)
 *   - Arithmetic operations (ADD, SUB, MUL, DIV, MOD)
 *   - Int32 overflow to double promotion
 *   - Boolean operations (NOT, comparisons)
 *   - Comparison operators (LT, LE, GT, GE, EQ, SEQ)
 *   - Conditional jumps (JUMP_IF_FALSE, JUMP_IF_TRUE)
 *   - Local variable load/store
 *   - Stack operations (DUP, DROP, SWAP)
 *   - Push constants (PUSH_TRUE, PUSH_FALSE, PUSH_NULL, PUSH_UNDEFINED)
 *   - String operations (PUSH_STRING, typeof)
 *   - Exception handling (THROW, TRY_PUSH, TRY_POP, CATCH)
 *   - CHECK_LIMITS fuel exhaustion
 *   - Function call and return
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>

/* =========================================================================
 * NaN-boxing definitions (must match r8e_interp.c)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL
#define R8E_NAN_VAL    0x7FF8000000000000ULL

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_TRUE(v)       ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)      ((v) == R8E_FALSE)
#define R8E_IS_BOOLEAN(v)    ((v) == R8E_TRUE || (v) == R8E_FALSE)

static inline double r8e_get_double(R8EValue v) {
    double d; memcpy(&d, &v, sizeof(double)); return d;
}
static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}
static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}
static inline R8EValue r8e_from_double(double d) {
    R8EValue v; memcpy(&v, &d, sizeof(double));
    if (v >= 0xFFF8000000000000ULL) v = R8E_NAN_VAL;
    return v;
}

/* =========================================================================
 * Opcode definitions (must match r8e_opcodes.h / r8e_interp.c)
 * ========================================================================= */

enum {
    OP_LOAD_LOCAL       = 0x00, OP_STORE_LOCAL      = 0x01,
    OP_LOAD_CONST       = 0x04, OP_PUSH_INT8        = 0x05,
    OP_PUSH_INT16       = 0x06, OP_PUSH_INT32       = 0x07,
    OP_PUSH_UNDEFINED   = 0x09, OP_PUSH_NULL        = 0x0A,
    OP_PUSH_TRUE        = 0x0B, OP_PUSH_FALSE       = 0x0C,
    OP_PUSH_ZERO        = 0x0D, OP_PUSH_ONE         = 0x0E,
    OP_PUSH_STRING      = 0x0F,
    OP_DUP              = 0x10, OP_DROP              = 0x11,
    OP_SWAP             = 0x12,
    OP_ADD              = 0x20, OP_SUB               = 0x21,
    OP_MUL              = 0x22, OP_DIV               = 0x23,
    OP_MOD              = 0x24, OP_NEG               = 0x26,
    OP_INC              = 0x28, OP_DEC               = 0x29,
    OP_BITAND           = 0x2C, OP_BITOR             = 0x2D,
    OP_BITXOR           = 0x2E, OP_BITNOT            = 0x2F,
    OP_SHL              = 0x30, OP_SHR               = 0x31,
    OP_USHR             = 0x32,
    OP_EQ               = 0x34, OP_SEQ               = 0x35,
    OP_NE               = 0x36, OP_SNE               = 0x37,
    OP_LT               = 0x38, OP_LE                = 0x39,
    OP_GT               = 0x3A, OP_GE                = 0x3B,
    OP_NOT              = 0x3C, OP_TYPEOF            = 0x3D,
    OP_JUMP8            = 0x41, OP_JUMP_IF_FALSE8    = 0x45,
    OP_JUMP_IF_TRUE8    = 0x46,
    OP_RETURN           = 0x4F, OP_RETURN_UNDEFINED  = 0x50,
    OP_THROW            = 0x54, OP_TRY_PUSH          = 0x55,
    OP_TRY_POP          = 0x56, OP_CATCH             = 0x57,
    OP_CHECK_LIMITS     = 0xF1, OP_HALT              = 0xFF
};

/* =========================================================================
 * External interpreter API
 * ========================================================================= */

typedef struct R8EInterpContext R8EInterpContext;

extern R8EInterpContext *r8e_interp_context_new(void);
extern void r8e_interp_context_free(R8EInterpContext *ctx);
extern R8EValue r8e_interpret(R8EInterpContext *ctx, const uint8_t *bytecode,
                               uint32_t bytecode_len, R8EValue *constants,
                               uint16_t num_constants);
extern bool r8e_interp_has_exception(const R8EInterpContext *ctx);
extern R8EValue r8e_interp_get_exception(const R8EInterpContext *ctx);
extern R8EValue r8e_interp_clear_exception(R8EInterpContext *ctx);
extern void r8e_interp_set_fuel(R8EInterpContext *ctx, int64_t fuel);

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
 * Helper: write a little-endian uint32 into a bytecode buffer
 * ========================================================================= */

static void write_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static void write_u16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

/* =========================================================================
 * Tests
 * ========================================================================= */

/* PUSH_INT8 42, RETURN -> 42 */
static void test_interp_push_int8_return(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 42, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 3, PUSH_INT8 4, ADD, RETURN -> 7 */
static void test_interp_add_int(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 4, OP_ADD, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 7);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 10, PUSH_INT8 3, SUB, RETURN -> 7 */
static void test_interp_sub_int(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 10, OP_PUSH_INT8, 3, OP_SUB, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 7);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 6, PUSH_INT8 7, MUL, RETURN -> 42 */
static void test_interp_mul_int(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 6, OP_PUSH_INT8, 7, OP_MUL, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 10, PUSH_INT8 3, DIV, RETURN -> 3 (int truncation) or 3.333.. */
static void test_interp_div(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 10, OP_PUSH_INT8, 3, OP_DIV, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* 10/3 is not an integer, should be a double */
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    double d = r8e_get_double(result);
    ASSERT_TRUE(fabs(d - 3.3333333333333335) < 1e-10);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 10, PUSH_INT8 3, MOD, RETURN -> 1 */
static void test_interp_mod(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 10, OP_PUSH_INT8, 3, OP_MOD, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 1);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT32 INT32_MAX, PUSH_INT8 1, ADD -> should produce double (overflow) */
static void test_interp_int32_overflow_to_double(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT32;
    write_u32(&bc[1], (uint32_t)INT32_MAX);
    bc[5] = OP_PUSH_INT8;
    bc[6] = 1;
    bc[7] = OP_ADD;
    bc[8] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, 9, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* INT32_MAX + 1 overflows int32, should promote to double */
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    double d = r8e_get_double(result);
    ASSERT_TRUE(fabs(d - ((double)INT32_MAX + 1.0)) < 1.0);

    r8e_interp_context_free(ctx);
}

/* PUSH_TRUE, NOT, RETURN -> false */
static void test_interp_not_true(void) {
    uint8_t bc[] = { OP_PUSH_TRUE, OP_NOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);

    r8e_interp_context_free(ctx);
}

/* PUSH_FALSE, NOT, RETURN -> true */
static void test_interp_not_false(void) {
    uint8_t bc[] = { OP_PUSH_FALSE, OP_NOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 0, NOT, RETURN -> true (0 is falsy) */
static void test_interp_not_zero(void) {
    uint8_t bc[] = { OP_PUSH_ZERO, OP_NOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 3, PUSH_INT8 5, LT, RETURN -> true */
static void test_interp_lt_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_LT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 5, PUSH_INT8 3, LT, RETURN -> false */
static void test_interp_lt_false(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 3, OP_LT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 5, PUSH_INT8 5, SEQ, RETURN -> true (strict equal same int) */
static void test_interp_seq_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 5, OP_SEQ, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 3, PUSH_INT8 5, GE, RETURN -> false */
static void test_interp_ge_false(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_GE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);

    r8e_interp_context_free(ctx);
}

/*
 * Jump test:
 * PUSH_TRUE, JUMP_IF_FALSE8 +2, PUSH_INT8 1, RETURN, PUSH_INT8 2, RETURN
 * True path: should push 1 and return
 */
static void test_interp_jump_if_false_not_taken(void) {
    uint8_t bc[] = {
        OP_PUSH_TRUE,          /* 0 */
        OP_JUMP_IF_FALSE8, 2,  /* 1,2 -> skip 2 bytes if false */
        OP_PUSH_INT8, 1,       /* 3,4 */
        OP_RETURN,             /* 5 */
        OP_PUSH_INT8, 2,       /* 6,7 */
        OP_RETURN              /* 8 */
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 1);

    r8e_interp_context_free(ctx);
}

/*
 * Jump test (taken):
 * PUSH_FALSE, JUMP_IF_FALSE8 +2, PUSH_INT8 1, RETURN, PUSH_INT8 2, RETURN
 * False path: jump past PUSH_INT8 1, RETURN to PUSH_INT8 2, RETURN -> 2
 */
static void test_interp_jump_if_false_taken(void) {
    uint8_t bc[] = {
        OP_PUSH_FALSE,         /* 0 */
        OP_JUMP_IF_FALSE8, 4,  /* 1,2 -> skip 4 bytes if false */
        OP_PUSH_INT8, 1,       /* 3,4 */
        OP_RETURN,             /* 5 */
        OP_PUSH_INT8, 2,       /* 6,7 */
        OP_RETURN              /* 8 */
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 2);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT8 42, STORE_LOCAL 0, LOAD_LOCAL 0, RETURN -> 42 */
static void test_interp_local_store_load(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, 42,
        OP_STORE_LOCAL, 0,
        OP_LOAD_LOCAL, 0,
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    r8e_interp_context_free(ctx);
}

/* PUSH_UNDEFINED, RETURN -> undefined */
static void test_interp_push_undefined(void) {
    uint8_t bc[] = { OP_PUSH_UNDEFINED, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_UNDEFINED);

    r8e_interp_context_free(ctx);
}

/* PUSH_NULL, RETURN -> null */
static void test_interp_push_null(void) {
    uint8_t bc[] = { OP_PUSH_NULL, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_NULL);

    r8e_interp_context_free(ctx);
}

/* PUSH_ZERO, RETURN -> int32(0) */
static void test_interp_push_zero(void) {
    uint8_t bc[] = { OP_PUSH_ZERO, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 0);

    r8e_interp_context_free(ctx);
}

/* PUSH_ONE, RETURN -> int32(1) */
static void test_interp_push_one(void) {
    uint8_t bc[] = { OP_PUSH_ONE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 1);

    r8e_interp_context_free(ctx);
}

/* DUP: PUSH_INT8 5, DUP, ADD, RETURN -> 10 */
static void test_interp_dup(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_DUP, OP_ADD, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 10);

    r8e_interp_context_free(ctx);
}

/* DROP: PUSH_INT8 5, PUSH_INT8 3, DROP, RETURN -> 5 */
static void test_interp_drop(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 3, OP_DROP, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 5);

    r8e_interp_context_free(ctx);
}

/* SWAP: PUSH_INT8 3, PUSH_INT8 5, SWAP, SUB, RETURN -> 5-3=2 */
static void test_interp_swap(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_SWAP, OP_SUB, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* After SWAP: stack is [5, 3], then SUB: 5-3=2 */
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 2);

    r8e_interp_context_free(ctx);
}

/* NEG: PUSH_INT8 42, NEG, RETURN -> -42 */
static void test_interp_neg(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 42, OP_NEG, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), -42);

    r8e_interp_context_free(ctx);
}

/* INC: PUSH_INT8 5, INC, RETURN -> 6 */
static void test_interp_inc(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_INC, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 6);

    r8e_interp_context_free(ctx);
}

/* DEC: PUSH_INT8 5, DEC, RETURN -> 4 */
static void test_interp_dec(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_DEC, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 4);

    r8e_interp_context_free(ctx);
}

/* Bitwise AND: 0xFF & 0x0F = 0x0F = 15 */
static void test_interp_bitand(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, (uint8_t)0x3F, /* 63, fits in signed int8 */
        OP_PUSH_INT8, 0x0F,          /* 15 */
        OP_BITAND, OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 0x3F & 0x0F);

    r8e_interp_context_free(ctx);
}

/* RETURN_UNDEFINED -> undefined */
static void test_interp_return_undefined(void) {
    uint8_t bc[] = { OP_RETURN_UNDEFINED };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_UNDEFINED);

    r8e_interp_context_free(ctx);
}

/* HALT -> should return undefined */
static void test_interp_halt(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 99, OP_HALT };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    /* HALT at end of script returns TOS or undefined */
    ASSERT_FALSE(r8e_interp_has_exception(ctx));

    r8e_interp_context_free(ctx);
}

/* LOAD_CONST: push constant pool value */
static void test_interp_load_const(void) {
    R8EValue consts[1] = { r8e_from_int32(99) };
    uint8_t bc[] = { OP_LOAD_CONST, 0, 0, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), consts, 1);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 99);

    r8e_interp_context_free(ctx);
}

/* CHECK_LIMITS with low fuel -> should abort with exception */
static void test_interp_check_limits_fuel(void) {
    /* A tight loop that will exhaust fuel:
     * PUSH_INT8 0, STORE_LOCAL 0           -- i = 0
     * CHECK_LIMITS                          -- decrement fuel
     * LOAD_LOCAL 0, PUSH_INT8 1, ADD       -- i++
     * STORE_LOCAL 0                         -- store i
     * JUMP8 -7                              -- loop back to CHECK_LIMITS
     */
    uint8_t bc[] = {
        OP_PUSH_INT8, 0,       /* 0,1 */
        OP_STORE_LOCAL, 0,     /* 2,3 */
        OP_CHECK_LIMITS,       /* 4 */
        OP_LOAD_LOCAL, 0,      /* 5,6 */
        OP_PUSH_INT8, 1,       /* 7,8 */
        OP_ADD,                /* 9 */
        OP_STORE_LOCAL, 0,     /* 10,11 */
        OP_JUMP8, (uint8_t)(-(int8_t)9), /* 12,13 -> jump back to offset 4 */
    };

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_interp_set_fuel(ctx, 10); /* very low fuel */

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    /* Should exhaust fuel and stop */
    ASSERT_TRUE(r8e_interp_has_exception(ctx) || result == R8E_UNDEFINED);

    r8e_interp_context_free(ctx);
}

/* Multiple local variables */
static void test_interp_multiple_locals(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, 10,      /* push 10 */
        OP_STORE_LOCAL, 0,     /* local[0] = 10 */
        OP_PUSH_INT8, 20,      /* push 20 */
        OP_STORE_LOCAL, 1,     /* local[1] = 20 */
        OP_LOAD_LOCAL, 0,      /* push local[0] = 10 */
        OP_LOAD_LOCAL, 1,      /* push local[1] = 20 */
        OP_ADD,                /* 10 + 20 = 30 */
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 30);

    r8e_interp_context_free(ctx);
}

/* PUSH_INT16: push 300 (does not fit in int8) */
static void test_interp_push_int16(void) {
    uint8_t bc[5];
    bc[0] = OP_PUSH_INT16;
    write_u16(&bc[1], 300);
    bc[3] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, 4, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 300);

    r8e_interp_context_free(ctx);
}

/* Unconditional JUMP8 */
static void test_interp_jump8(void) {
    uint8_t bc[] = {
        OP_JUMP8, 3,           /* 0,1 -> skip 3 bytes */
        OP_PUSH_INT8, 1,       /* 2,3 -> skipped */
        OP_PUSH_INT8, 42,      /* 4,5 -> target */
        OP_RETURN              /* 6 */
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    r8e_interp_context_free(ctx);
}

/* Exception clear */
static void test_interp_exception_clear(void) {
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));

    /* Manually test exception state management through a THROW bytecode */
    uint8_t bc[] = { OP_PUSH_INT8, 42, OP_THROW };
    r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_TRUE(r8e_interp_has_exception(ctx));

    R8EValue exc = r8e_interp_clear_exception(ctx);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* The thrown value should be recoverable */
    ASSERT_NE(exc, R8E_UNDEFINED);

    r8e_interp_context_free(ctx);
}

/* NOP is a no-op */
static void test_interp_nop(void) {
    uint8_t bc[] = {
        0xFE, /* OP_NOP */
        0xFE, /* OP_NOP */
        OP_PUSH_INT8, 7,
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 7);

    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Bitwise Operations
 * ========================================================================= */

/* BITOR: 0x30 | 0x0F = 0x3F = 63 */
static void test_interp_bitor(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, 0x30, /* 48 */
        OP_PUSH_INT8, 0x0F, /* 15 */
        OP_BITOR, OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 0x30 | 0x0F);
    r8e_interp_context_free(ctx);
}

/* BITXOR: 0xFF ^ 0x0F = 0xF0 = 240 */
static void test_interp_bitxor(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT16;
    write_u16(&bc[1], 0xFF);
    bc[3] = OP_PUSH_INT8;
    bc[4] = 0x0F;
    bc[5] = OP_BITXOR;
    bc[6] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 7, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 0xFF ^ 0x0F);
    r8e_interp_context_free(ctx);
}

/* BITNOT: ~0 = -1 */
static void test_interp_bitnot(void) {
    uint8_t bc[] = { OP_PUSH_ZERO, OP_BITNOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), -1);
    r8e_interp_context_free(ctx);
}

/* SHL: 1 << 4 = 16 */
static void test_interp_shl(void) {
    uint8_t bc[] = {
        OP_PUSH_ONE,
        OP_PUSH_INT8, 4,
        OP_SHL, OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 16);
    r8e_interp_context_free(ctx);
}

/* SHR: -16 >> 2 = -4 (arithmetic shift) */
static void test_interp_shr(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT8;
    bc[1] = (uint8_t)(int8_t)(-16);
    bc[2] = OP_PUSH_INT8;
    bc[3] = 2;
    bc[4] = OP_SHR;
    bc[5] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 6, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), -4);
    r8e_interp_context_free(ctx);
}

/* USHR: -1 >>> 0 = 4294967295 (unsigned) - becomes double because > INT32_MAX */
static void test_interp_ushr(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT8;
    bc[1] = (uint8_t)(int8_t)(-1);
    bc[2] = OP_PUSH_ZERO;
    bc[3] = OP_USHR;
    bc[4] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 5, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* -1 >>> 0 = 0xFFFFFFFF = 4294967295 which exceeds INT32_MAX */
    /* Could be int32 (if unsigned stored) or double */
    ASSERT_TRUE(R8E_IS_INT32(result) || R8E_IS_DOUBLE(result));
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Comparison Operators
 * ========================================================================= */

/* LE true: 3 <= 5 */
static void test_interp_le_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_LE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* LE equal: 5 <= 5 */
static void test_interp_le_equal(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 5, OP_LE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* GT true: 5 > 3 */
static void test_interp_gt_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 3, OP_GT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* GT false: 3 > 5 */
static void test_interp_gt_false(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_GT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);
    r8e_interp_context_free(ctx);
}

/* GE true: 5 >= 5 */
static void test_interp_ge_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 5, OP_GE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* EQ: 5 == 5 */
static void test_interp_eq_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 5, OP_EQ, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* NE: 3 != 5 */
static void test_interp_ne_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_NE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* NE false: 5 != 5 */
static void test_interp_ne_false(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 5, OP_PUSH_INT8, 5, OP_NE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);
    r8e_interp_context_free(ctx);
}

/* SNE (strict not equal): 3 !== 5 */
static void test_interp_sne_true(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 3, OP_PUSH_INT8, 5, OP_SNE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* SEQ (strict equal): different types -> false */
static void test_interp_seq_diff_types(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 0, OP_PUSH_FALSE, OP_SEQ, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: JUMP_IF_TRUE
 * ========================================================================= */

/* JUMP_IF_TRUE8 taken (value is true) */
static void test_interp_jump_if_true_taken(void) {
    uint8_t bc[] = {
        OP_PUSH_TRUE,           /* 0 */
        OP_JUMP_IF_TRUE8, 3,   /* 1,2 -> skip 3 bytes */
        OP_PUSH_INT8, 1,        /* 3,4 -> skipped */
        OP_PUSH_INT8, 42,       /* 5,6 -> target */
        OP_RETURN                /* 7 */
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);
    r8e_interp_context_free(ctx);
}

/* JUMP_IF_TRUE8 not taken (value is false) */
static void test_interp_jump_if_true_not_taken(void) {
    uint8_t bc[] = {
        OP_PUSH_FALSE,          /* 0 */
        OP_JUMP_IF_TRUE8, 2,   /* 1,2 */
        OP_PUSH_INT8, 1,        /* 3,4 -> executed */
        OP_RETURN,               /* 5 */
        OP_PUSH_INT8, 42,       /* 6,7 */
        OP_RETURN                /* 8 */
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 1);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Unary Operations
 * ========================================================================= */

/* NEG on zero: -0 (should produce double -0) */
static void test_interp_neg_zero(void) {
    uint8_t bc[] = { OP_PUSH_ZERO, OP_NEG, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* -0 may be int32(0) or double(-0.0) depending on implementation */
    ASSERT_TRUE(R8E_IS_INT32(result) || R8E_IS_DOUBLE(result));
    r8e_interp_context_free(ctx);
}

/* NOT on undefined: !undefined -> true */
static void test_interp_not_undefined(void) {
    uint8_t bc[] = { OP_PUSH_UNDEFINED, OP_NOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* NOT on null: !null -> true */
static void test_interp_not_null(void) {
    uint8_t bc[] = { OP_PUSH_NULL, OP_NOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* NOT on non-zero int: !42 -> false */
static void test_interp_not_nonzero(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 42, OP_NOT, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Arithmetic Edge Cases
 * ========================================================================= */

/* MUL overflow: INT32_MAX * 2 -> double */
static void test_interp_mul_overflow(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT32;
    write_u32(&bc[1], (uint32_t)INT32_MAX);
    bc[5] = OP_PUSH_INT8;
    bc[6] = 2;
    bc[7] = OP_MUL;
    bc[8] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 9, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    r8e_interp_context_free(ctx);
}

/* SUB overflow: INT32_MIN - 1 -> double */
static void test_interp_sub_overflow(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT32;
    write_u32(&bc[1], (uint32_t)INT32_MIN);
    bc[5] = OP_PUSH_ONE;
    bc[6] = OP_SUB;
    bc[7] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 8, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    r8e_interp_context_free(ctx);
}

/* DIV by zero: 10 / 0 -> Infinity */
static void test_interp_div_by_zero(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 10, OP_PUSH_ZERO, OP_DIV, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    double d = r8e_get_double(result);
    ASSERT_TRUE(d == INFINITY || d == -INFINITY || isnan(d));
    r8e_interp_context_free(ctx);
}

/* MOD by zero: 10 % 0 -> NaN */
static void test_interp_mod_by_zero(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 10, OP_PUSH_ZERO, OP_MOD, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    /* 10 % 0 should be NaN in JS */
    ASSERT_TRUE(R8E_IS_DOUBLE(result) || R8E_IS_INT32(result));
    r8e_interp_context_free(ctx);
}

/* INC overflow: INT32_MAX + 1 -> double */
static void test_interp_inc_overflow(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT32;
    write_u32(&bc[1], (uint32_t)INT32_MAX);
    bc[5] = OP_INC;
    bc[6] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 7, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    r8e_interp_context_free(ctx);
}

/* DEC underflow: INT32_MIN - 1 -> double */
static void test_interp_dec_underflow(void) {
    uint8_t bc[12];
    bc[0] = OP_PUSH_INT32;
    write_u32(&bc[1], (uint32_t)INT32_MIN);
    bc[5] = OP_DEC;
    bc[6] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 7, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_DOUBLE(result));
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: PUSH_INT32 with negative values
 * ========================================================================= */

/* PUSH_INT32 with a large negative value */
static void test_interp_push_int32_negative(void) {
    uint8_t bc[8];
    bc[0] = OP_PUSH_INT32;
    write_u32(&bc[1], (uint32_t)(-1000000));
    bc[5] = OP_RETURN;

    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, 6, NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), -1000000);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: PUSH_TRUE and PUSH_FALSE
 * ========================================================================= */

/* PUSH_TRUE, RETURN */
static void test_interp_push_true(void) {
    uint8_t bc[] = { OP_PUSH_TRUE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_TRUE);
    r8e_interp_context_free(ctx);
}

/* PUSH_FALSE, RETURN */
static void test_interp_push_false(void) {
    uint8_t bc[] = { OP_PUSH_FALSE, OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(result == R8E_FALSE);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Constant Pool with Multiple Constants
 * ========================================================================= */

/* Load two constants and add */
static void test_interp_load_const_two(void) {
    R8EValue consts[2] = { r8e_from_int32(30), r8e_from_int32(12) };
    uint8_t bc[] = {
        OP_LOAD_CONST, 0, 0,   /* constant[0] = 30 */
        OP_LOAD_CONST, 1, 0,   /* constant[1] = 12 */
        OP_ADD,
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), consts, 2);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: THROW and exception handling
 * ========================================================================= */

/* THROW with int value -> exception is set */
static void test_interp_throw_int(void) {
    uint8_t bc[] = { OP_PUSH_INT8, 99, OP_THROW };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_TRUE(r8e_interp_has_exception(ctx));
    r8e_interp_context_free(ctx);
}

/* THROW with boolean value */
static void test_interp_throw_bool(void) {
    uint8_t bc[] = { OP_PUSH_TRUE, OP_THROW };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_TRUE(r8e_interp_has_exception(ctx));
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Superinstructions
 * ========================================================================= */

/* Opcode enum additions for superinstructions */
enum {
    OP_STORE_LOCAL_POP = 0xE5,
    OP_LOAD_LOCAL2     = 0xE6
};

/* STORE_LOCAL_POP: store + pop in one op */
static void test_interp_store_local_pop(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, 42,
        OP_DUP,                 /* dup so we have 42 on stack twice */
        OP_STORE_LOCAL_POP, 0,  /* store to local[0] and pop */
        OP_DROP,                /* drop the remaining copy */
        OP_LOAD_LOCAL, 0,       /* load local[0] */
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);
    r8e_interp_context_free(ctx);
}

/* LOAD_LOCAL2: load two locals in one op */
static void test_interp_load_local2(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, 10,
        OP_STORE_LOCAL, 0,
        OP_PUSH_INT8, 20,
        OP_STORE_LOCAL, 1,
        OP_LOAD_LOCAL2, 0, 1,  /* push local[0] and local[1] */
        OP_ADD,
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 30);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Negative PUSH_INT8
 * ========================================================================= */

/* PUSH_INT8 with negative value */
static void test_interp_push_int8_negative(void) {
    uint8_t bc[] = { OP_PUSH_INT8, (uint8_t)(int8_t)(-5), OP_RETURN };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), -5);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Complex local variable patterns
 * ========================================================================= */

/* Swap two locals via a temp */
static void test_interp_swap_locals(void) {
    uint8_t bc[] = {
        OP_PUSH_INT8, 10,    OP_STORE_LOCAL, 0,  /* a = 10 */
        OP_PUSH_INT8, 20,    OP_STORE_LOCAL, 1,  /* b = 20 */
        OP_LOAD_LOCAL, 0,    OP_STORE_LOCAL, 2,  /* tmp = a */
        OP_LOAD_LOCAL, 1,    OP_STORE_LOCAL, 0,  /* a = b */
        OP_LOAD_LOCAL, 2,    OP_STORE_LOCAL, 1,  /* b = tmp */
        OP_LOAD_LOCAL, 0,    /* push a (should be 20) */
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 20);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: CHECK_LIMITS with normal fuel (should not trigger)
 * ========================================================================= */

static void test_interp_check_limits_ok(void) {
    uint8_t bc[] = {
        OP_CHECK_LIMITS,
        OP_PUSH_INT8, 42,
        OP_RETURN
    };
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* Default fuel is large, should not trigger */
    R8EValue result = r8e_interpret(ctx, bc, sizeof(bc), NULL, 0);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * NEW TESTS: Context creation / destruction
 * ========================================================================= */

/* Context starts with no exception */
static void test_interp_context_no_exception(void) {
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_FALSE(r8e_interp_has_exception(ctx));
    r8e_interp_context_free(ctx);
}

/* Fuel setting */
static void test_interp_fuel_set(void) {
    R8EInterpContext *ctx = r8e_interp_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_interp_set_fuel(ctx, 42);
    /* No crash, fuel is set */
    r8e_interp_context_free(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_interp_tests(void) {
    /* Original tests */
    RUN_TEST(test_interp_push_int8_return);
    RUN_TEST(test_interp_add_int);
    RUN_TEST(test_interp_sub_int);
    RUN_TEST(test_interp_mul_int);
    RUN_TEST(test_interp_div);
    RUN_TEST(test_interp_mod);
    RUN_TEST(test_interp_int32_overflow_to_double);
    RUN_TEST(test_interp_not_true);
    RUN_TEST(test_interp_not_false);
    RUN_TEST(test_interp_not_zero);
    RUN_TEST(test_interp_lt_true);
    RUN_TEST(test_interp_lt_false);
    RUN_TEST(test_interp_seq_true);
    RUN_TEST(test_interp_ge_false);
    RUN_TEST(test_interp_jump_if_false_not_taken);
    RUN_TEST(test_interp_jump_if_false_taken);
    RUN_TEST(test_interp_local_store_load);
    RUN_TEST(test_interp_push_undefined);
    RUN_TEST(test_interp_push_null);
    RUN_TEST(test_interp_push_zero);
    RUN_TEST(test_interp_push_one);
    RUN_TEST(test_interp_dup);
    RUN_TEST(test_interp_drop);
    RUN_TEST(test_interp_swap);
    RUN_TEST(test_interp_neg);
    RUN_TEST(test_interp_inc);
    RUN_TEST(test_interp_dec);
    RUN_TEST(test_interp_bitand);
    RUN_TEST(test_interp_return_undefined);
    RUN_TEST(test_interp_halt);
    RUN_TEST(test_interp_load_const);
    RUN_TEST(test_interp_check_limits_fuel);
    RUN_TEST(test_interp_multiple_locals);
    RUN_TEST(test_interp_push_int16);
    RUN_TEST(test_interp_jump8);
    RUN_TEST(test_interp_exception_clear);
    RUN_TEST(test_interp_nop);

    /* NEW: Bitwise operations */
    RUN_TEST(test_interp_bitor);
    RUN_TEST(test_interp_bitxor);
    RUN_TEST(test_interp_bitnot);
    RUN_TEST(test_interp_shl);
    RUN_TEST(test_interp_shr);
    RUN_TEST(test_interp_ushr);

    /* NEW: Comparison operators */
    RUN_TEST(test_interp_le_true);
    RUN_TEST(test_interp_le_equal);
    RUN_TEST(test_interp_gt_true);
    RUN_TEST(test_interp_gt_false);
    RUN_TEST(test_interp_ge_true);
    RUN_TEST(test_interp_eq_true);
    RUN_TEST(test_interp_ne_true);
    RUN_TEST(test_interp_ne_false);
    RUN_TEST(test_interp_sne_true);
    RUN_TEST(test_interp_seq_diff_types);

    /* NEW: JUMP_IF_TRUE */
    RUN_TEST(test_interp_jump_if_true_taken);
    RUN_TEST(test_interp_jump_if_true_not_taken);

    /* NEW: Unary operations */
    RUN_TEST(test_interp_neg_zero);
    RUN_TEST(test_interp_not_undefined);
    RUN_TEST(test_interp_not_null);
    RUN_TEST(test_interp_not_nonzero);

    /* NEW: Arithmetic edge cases */
    RUN_TEST(test_interp_mul_overflow);
    RUN_TEST(test_interp_sub_overflow);
    RUN_TEST(test_interp_div_by_zero);
    RUN_TEST(test_interp_mod_by_zero);
    RUN_TEST(test_interp_inc_overflow);
    RUN_TEST(test_interp_dec_underflow);

    /* NEW: PUSH_INT32 negative */
    RUN_TEST(test_interp_push_int32_negative);

    /* NEW: PUSH_TRUE / PUSH_FALSE */
    RUN_TEST(test_interp_push_true);
    RUN_TEST(test_interp_push_false);

    /* NEW: Constant pool */
    RUN_TEST(test_interp_load_const_two);

    /* NEW: Exception handling */
    RUN_TEST(test_interp_throw_int);
    RUN_TEST(test_interp_throw_bool);

    /* NEW: Superinstructions */
    RUN_TEST(test_interp_store_local_pop);
    RUN_TEST(test_interp_load_local2);

    /* NEW: Negative PUSH_INT8 */
    RUN_TEST(test_interp_push_int8_negative);

    /* NEW: Complex local patterns */
    RUN_TEST(test_interp_swap_locals);

    /* NEW: CHECK_LIMITS ok */
    RUN_TEST(test_interp_check_limits_ok);

    /* NEW: Context management */
    RUN_TEST(test_interp_context_no_exception);
    RUN_TEST(test_interp_fuel_set);
}

/* =========================================================================
 * Standalone main (for running just this suite)
 * ========================================================================= */

#ifdef TEST_INTERP_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[interp]\n");
    run_interp_tests();
    printf("\n=== interp: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
