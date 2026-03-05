/*
 * test_scope.c - Unit tests for r8e_scope.c (Compile-Time Scope Management)
 *
 * Tests cover:
 *   - Scope pool (init, alloc, release, overflow)
 *   - Scope push/pop (function scope, block scope)
 *   - Variable definition (let, const, var, params)
 *   - Duplicate variable detection
 *   - Var hoisting across block scopes
 *   - Variable resolution (local, closure, global)
 *   - Capture marking (IS_CAPTURED flag)
 *   - Mutation marking (IS_MUTATED_AFTER_CAPTURE)
 *   - RC classification (OWNED, BORROWED, TEMP)
 *   - Strict mode inheritance
 *   - Scope depth tracking
 *   - Block scope register continuation
 *   - Total register count
 *   - Break/continue label stack
 *   - Label find for break/continue (named and unnamed)
 *   - Break patch accumulation
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../include/r8e_types.h"

/* =========================================================================
 * Forward declarations for scope API (from r8e_scope.c)
 *
 * Since r8e_scope.c uses static functions that are #included by r8e_parse.c,
 * we declare them as extern here for direct testing.
 * ========================================================================= */

/* Scope Pool */
typedef struct R8EScopePool {
    R8EScope *free_list[16]; /* R8E_SCOPE_POOL_MAX */
    int       count;
} R8EScopePool;

extern void     r8e_scope_pool_init(R8EScopePool *pool);
extern void     r8e_scope_pool_destroy(R8EScopePool *pool);
extern R8EScope *r8e_scope_pool_alloc(R8EScopePool *pool);
extern void     r8e_scope_pool_release(R8EScopePool *pool, R8EScope *scope);

/* Variable Resolution */
typedef enum {
    R8E_RESOLVE_LOCAL   = 0,
    R8E_RESOLVE_CLOSURE = 1,
    R8E_RESOLVE_GLOBAL  = 2,
    R8E_RESOLVE_ERROR   = 3
} R8EResolveKind;

typedef struct {
    R8EResolveKind kind;
    uint8_t        reg;
    uint8_t        capture_idx;
    uint8_t        depth;
    uint8_t        classification;
    uint8_t        flags;
} R8EResolveResult;

/* Scope operations */
extern R8EScope *r8e_scope_push(R8EScopePool *pool, R8EScope *parent, uint8_t flags);
extern R8EScope *r8e_scope_pop(R8EScopePool *pool, R8EScope *scope);
extern int       r8e_scope_define_var(R8EScope *scope, uint32_t atom, uint8_t var_flags);
extern R8EResolveResult r8e_scope_resolve_var(R8EScope *scope, uint32_t atom);
extern R8EVarInfo *r8e_scope_find_var(R8EScope *scope, uint32_t atom);
extern void      r8e_scope_mark_captured(R8EScope *scope, uint32_t atom);
extern void      r8e_scope_mark_mutated(R8EScope *scope, uint32_t atom);
extern R8EVarClass r8e_classify_use(R8EScope *scope, uint32_t atom,
                                     bool is_store, bool is_return,
                                     bool is_captured);
extern uint16_t  r8e_scope_total_regs(const R8EScope *func_scope);
extern void      r8e_scope_dump(const R8EScope *scope, int indent);

/* Break/Continue Label Stack */
#define R8E_MAX_BREAK_PATCHES 64

typedef struct R8EBreakLabel {
    uint32_t  atom;
    uint32_t  break_patches[R8E_MAX_BREAK_PATCHES];
    uint16_t  break_count;
    uint32_t  continue_target;
    bool      is_switch;
} R8EBreakLabel;

#define R8E_MAX_LABEL_DEPTH 32

typedef struct R8ELabelStack {
    R8EBreakLabel labels[R8E_MAX_LABEL_DEPTH];
    int           depth;
} R8ELabelStack;

extern void           r8e_labels_init(R8ELabelStack *ls);
extern int            r8e_labels_push(R8ELabelStack *ls, uint32_t atom, bool is_switch);
extern R8EBreakLabel *r8e_labels_top(R8ELabelStack *ls);
extern R8EBreakLabel *r8e_labels_pop(R8ELabelStack *ls);
extern R8EBreakLabel *r8e_labels_find_break(R8ELabelStack *ls, uint32_t atom);
extern R8EBreakLabel *r8e_labels_find_continue(R8ELabelStack *ls, uint32_t atom);
extern int            r8e_label_add_break(R8EBreakLabel *label, uint32_t patch_addr);

/* =========================================================================
 * Test Harness
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { if (!(e)) { printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #e); g_assert_fail = 1; return; } } while(0)
#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a,b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a,b) ASSERT_TRUE((a) != (b))
#define ASSERT_EQ_INT(a,b) do { int _a=(int)(a), _b=(int)(b); if (_a != _b) { printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); g_assert_fail = 1; return; } } while(0)
#define RUN_TEST(fn) do { g_assert_fail = 0; g_tests_run++; printf("  %s... ", #fn); fn(); if (g_assert_fail) { g_tests_failed++; } else { g_tests_passed++; printf("OK\n"); } } while(0)

/* Atom constants for testing (arbitrary values, not from the atom table) */
#define TEST_ATOM_X    1000
#define TEST_ATOM_Y    1001
#define TEST_ATOM_Z    1002
#define TEST_ATOM_A    1003
#define TEST_ATOM_B    1004
#define TEST_ATOM_I    1005
#define TEST_ATOM_LOOP 1006
#define TEST_ATOM_FOO  1007
#define TEST_ATOM_BAR  1008

/* =========================================================================
 * Tests: Scope Pool
 * ========================================================================= */

static void test_scope_pool_init(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    ASSERT_EQ_INT(pool.count, 0);
}

static void test_scope_pool_alloc_fresh(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_pool_alloc(&pool);
    ASSERT_TRUE(s != NULL);
    free(s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_pool_release_and_reuse(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_pool_alloc(&pool);
    ASSERT_TRUE(s != NULL);
    r8e_scope_pool_release(&pool, s);
    ASSERT_EQ_INT(pool.count, 1);
    /* Allocating again should return the released scope */
    R8EScope *s2 = r8e_scope_pool_alloc(&pool);
    ASSERT_TRUE(s2 == s);
    ASSERT_EQ_INT(pool.count, 0);
    free(s2);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_pool_overflow_frees(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    /* Fill the pool to max (16) */
    R8EScope *scopes[17];
    for (int i = 0; i < 17; i++) {
        scopes[i] = (R8EScope *)malloc(sizeof(R8EScope));
        ASSERT_TRUE(scopes[i] != NULL);
    }
    for (int i = 0; i < 17; i++) {
        r8e_scope_pool_release(&pool, scopes[i]);
    }
    /* Pool should hold max 16, the 17th should have been freed */
    ASSERT_EQ_INT(pool.count, 16);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Scope Push / Pop
 * ========================================================================= */

static void test_scope_push_function_root(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s->parent == NULL);
    ASSERT_EQ_INT(s->depth, 0);
    ASSERT_EQ_INT(s->local_base, 0);
    ASSERT_EQ_INT(s->local_count, 0);
    ASSERT_TRUE(s->flags & R8E_SCOPE_IS_FUNCTION);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_push_block_inherits_base(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *func = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    /* Define two vars in function scope */
    r8e_scope_define_var(func, TEST_ATOM_X, R8E_VAR_IS_LET);
    r8e_scope_define_var(func, TEST_ATOM_Y, R8E_VAR_IS_LET);

    /* Push a block scope - should continue from parent's registers */
    R8EScope *block = r8e_scope_push(&pool, func, R8E_SCOPE_IS_BLOCK);
    ASSERT_TRUE(block != NULL);
    ASSERT_TRUE(block->parent == func);
    ASSERT_EQ_INT(block->depth, 1);
    /* local_base should be parent's local_base + local_count = 0 + 2 = 2 */
    ASSERT_EQ_INT(block->local_base, 2);
    ASSERT_EQ_INT(block->local_count, 0);

    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, func);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_push_function_resets_base(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *outer = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(outer, TEST_ATOM_X, R8E_VAR_IS_LET);

    /* Push an inner function scope - should reset local_base to 0 */
    R8EScope *inner = r8e_scope_push(&pool, outer, R8E_SCOPE_IS_FUNCTION);
    ASSERT_EQ_INT(inner->local_base, 0);
    ASSERT_EQ_INT(inner->local_count, 0);

    r8e_scope_pop(&pool, inner);
    r8e_scope_pop(&pool, outer);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_pop_returns_parent(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *parent = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    R8EScope *child = r8e_scope_push(&pool, parent, R8E_SCOPE_IS_BLOCK);
    R8EScope *result = r8e_scope_pop(&pool, child);
    ASSERT_TRUE(result == parent);
    r8e_scope_pop(&pool, parent);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_strict_mode_inherits(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *parent = r8e_scope_push(&pool, NULL,
        R8E_SCOPE_IS_FUNCTION | R8E_SCOPE_IS_STRICT);
    /* Child should inherit strict mode */
    R8EScope *child = r8e_scope_push(&pool, parent, R8E_SCOPE_IS_BLOCK);
    ASSERT_TRUE(child->flags & R8E_SCOPE_IS_STRICT);
    r8e_scope_pop(&pool, child);
    r8e_scope_pop(&pool, parent);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_strict_mode_not_inherited_if_parent_not_strict(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *parent = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    R8EScope *child = r8e_scope_push(&pool, parent, R8E_SCOPE_IS_BLOCK);
    ASSERT_FALSE(child->flags & R8E_SCOPE_IS_STRICT);
    r8e_scope_pop(&pool, child);
    r8e_scope_pop(&pool, parent);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_depth_increments(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s0 = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    ASSERT_EQ_INT(s0->depth, 0);
    R8EScope *s1 = r8e_scope_push(&pool, s0, R8E_SCOPE_IS_BLOCK);
    ASSERT_EQ_INT(s1->depth, 1);
    R8EScope *s2 = r8e_scope_push(&pool, s1, R8E_SCOPE_IS_BLOCK);
    ASSERT_EQ_INT(s2->depth, 2);
    r8e_scope_pop(&pool, s2);
    r8e_scope_pop(&pool, s1);
    r8e_scope_pop(&pool, s0);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Variable Definition
 * ========================================================================= */

static void test_scope_define_let(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    ASSERT_EQ_INT(reg, 0);
    ASSERT_EQ_INT(s->local_count, 1);
    ASSERT_EQ_INT(s->vars[0].atom, TEST_ATOM_X);
    ASSERT_EQ_INT(s->vars[0].register_idx, 0);
    ASSERT_TRUE(s->vars[0].flags & R8E_VAR_IS_LET);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_const(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_CONST);
    ASSERT_EQ_INT(reg, 0);
    ASSERT_TRUE(s->vars[0].flags & R8E_VAR_IS_CONST);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_var_plain(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg = r8e_scope_define_var(s, TEST_ATOM_X, 0); /* plain var */
    ASSERT_EQ_INT(reg, 0);
    ASSERT_EQ_INT(s->vars[0].flags, 0);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_param(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_PARAM);
    ASSERT_EQ_INT(reg, 0);
    ASSERT_TRUE(s->vars[0].flags & R8E_VAR_IS_PARAM);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_multiple_vars(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg_x = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    int reg_y = r8e_scope_define_var(s, TEST_ATOM_Y, R8E_VAR_IS_LET);
    int reg_z = r8e_scope_define_var(s, TEST_ATOM_Z, R8E_VAR_IS_CONST);
    ASSERT_EQ_INT(reg_x, 0);
    ASSERT_EQ_INT(reg_y, 1);
    ASSERT_EQ_INT(reg_z, 2);
    ASSERT_EQ_INT(s->local_count, 3);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_duplicate_let_fails(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg1 = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    ASSERT_EQ_INT(reg1, 0);
    int reg2 = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    ASSERT_EQ_INT(reg2, -2); /* duplicate declaration */
    ASSERT_EQ_INT(s->local_count, 1); /* count should not increase */
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_duplicate_const_fails(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_CONST);
    int reg2 = r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_CONST);
    ASSERT_EQ_INT(reg2, -2);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_define_var_duplicate_reuses(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg1 = r8e_scope_define_var(s, TEST_ATOM_X, 0); /* var x */
    int reg2 = r8e_scope_define_var(s, TEST_ATOM_X, 0); /* var x again */
    /* Re-declaring var should reuse same register */
    ASSERT_EQ_INT(reg1, reg2);
    ASSERT_EQ_INT(s->local_count, 1);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_var_hoists_to_function(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *func = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg1 = r8e_scope_define_var(func, TEST_ATOM_X, 0); /* var x at function level */

    R8EScope *block = r8e_scope_push(&pool, func, R8E_SCOPE_IS_BLOCK);
    /* Redeclaring var x in block should find it at function level */
    int reg2 = r8e_scope_define_var(block, TEST_ATOM_X, 0);
    ASSERT_EQ_INT(reg1, reg2);

    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, func);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_block_let_shadows_function_var(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *func = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    int reg_func = r8e_scope_define_var(func, TEST_ATOM_X, 0); /* var x */

    R8EScope *block = r8e_scope_push(&pool, func, R8E_SCOPE_IS_BLOCK);
    int reg_block = r8e_scope_define_var(block, TEST_ATOM_X, R8E_VAR_IS_LET); /* let x */
    /* let in block should get a different register */
    ASSERT_NE(reg_func, reg_block);

    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, func);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_default_classification_borrowed(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    /* Default classification should be BORROWED */
    ASSERT_EQ_INT(s->vars[0].classification, R8E_VAR_BORROWED);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Variable Resolution
 * ========================================================================= */

static void test_scope_resolve_local(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);

    R8EResolveResult r = r8e_scope_resolve_var(s, TEST_ATOM_X);
    ASSERT_EQ_INT(r.kind, R8E_RESOLVE_LOCAL);
    ASSERT_EQ_INT(r.reg, 0);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_resolve_from_parent_block(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *func = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(func, TEST_ATOM_X, R8E_VAR_IS_LET);

    R8EScope *block = r8e_scope_push(&pool, func, R8E_SCOPE_IS_BLOCK);
    /* Resolve x from block scope: should find in parent function scope (LOCAL) */
    R8EResolveResult r = r8e_scope_resolve_var(block, TEST_ATOM_X);
    ASSERT_EQ_INT(r.kind, R8E_RESOLVE_LOCAL);
    ASSERT_EQ_INT(r.reg, 0);

    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, func);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_resolve_global(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    /* Don't define TEST_ATOM_X, so resolution should yield GLOBAL */
    R8EResolveResult r = r8e_scope_resolve_var(s, TEST_ATOM_X);
    ASSERT_EQ_INT(r.kind, R8E_RESOLVE_GLOBAL);
    ASSERT_EQ_INT(r.classification, R8E_VAR_OWNED);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_resolve_closure(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    /* Outer function scope defines x */
    R8EScope *outer = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(outer, TEST_ATOM_X, R8E_VAR_IS_LET);

    /* Inner function scope */
    R8EScope *inner = r8e_scope_push(&pool, outer, R8E_SCOPE_IS_FUNCTION);

    /* Resolving x from inner should cross function boundary => CLOSURE */
    R8EResolveResult r = r8e_scope_resolve_var(inner, TEST_ATOM_X);
    ASSERT_EQ_INT(r.kind, R8E_RESOLVE_CLOSURE);
    ASSERT_EQ_INT(r.classification, R8E_VAR_OWNED);
    /* Should also mark x as captured in outer scope */
    ASSERT_TRUE(outer->vars[0].flags & R8E_VAR_IS_CAPTURED);

    r8e_scope_pop(&pool, inner);
    r8e_scope_pop(&pool, outer);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_resolve_closure_through_block(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    /* Outer function -> block -> inner function */
    R8EScope *outer = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(outer, TEST_ATOM_X, R8E_VAR_IS_LET);

    R8EScope *block = r8e_scope_push(&pool, outer, R8E_SCOPE_IS_BLOCK);
    R8EScope *inner = r8e_scope_push(&pool, block, R8E_SCOPE_IS_FUNCTION);

    R8EResolveResult r = r8e_scope_resolve_var(inner, TEST_ATOM_X);
    ASSERT_EQ_INT(r.kind, R8E_RESOLVE_CLOSURE);
    ASSERT_TRUE(outer->vars[0].flags & R8E_VAR_IS_CAPTURED);

    r8e_scope_pop(&pool, inner);
    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, outer);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_resolve_shadows_outer(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *func = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(func, TEST_ATOM_X, R8E_VAR_IS_LET); /* reg 0 */

    R8EScope *block = r8e_scope_push(&pool, func, R8E_SCOPE_IS_BLOCK);
    r8e_scope_define_var(block, TEST_ATOM_X, R8E_VAR_IS_LET); /* reg 1 (shadow) */

    /* Should resolve to the inner (shadow) binding */
    R8EResolveResult r = r8e_scope_resolve_var(block, TEST_ATOM_X);
    ASSERT_EQ_INT(r.kind, R8E_RESOLVE_LOCAL);
    ASSERT_EQ_INT(r.reg, 1); /* block's register */

    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, func);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Capture Marking
 * ========================================================================= */

static void test_scope_mark_captured(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    ASSERT_FALSE(s->vars[0].flags & R8E_VAR_IS_CAPTURED);

    r8e_scope_mark_captured(s, TEST_ATOM_X);
    ASSERT_TRUE(s->vars[0].flags & R8E_VAR_IS_CAPTURED);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_mark_captured_traverses_parents(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *func = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(func, TEST_ATOM_X, R8E_VAR_IS_LET);

    R8EScope *block = r8e_scope_push(&pool, func, R8E_SCOPE_IS_BLOCK);

    /* Mark captured from block scope - should walk up to func */
    r8e_scope_mark_captured(block, TEST_ATOM_X);
    ASSERT_TRUE(func->vars[0].flags & R8E_VAR_IS_CAPTURED);

    r8e_scope_pop(&pool, block);
    r8e_scope_pop(&pool, func);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_mark_captured_nonexistent(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    /* Marking a nonexistent variable should not crash */
    r8e_scope_mark_captured(s, 9999);
    ASSERT_TRUE(1);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Mutation Marking
 * ========================================================================= */

static void test_scope_mark_mutated_promotes_to_owned(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    ASSERT_EQ_INT(s->vars[0].classification, R8E_VAR_BORROWED);

    r8e_scope_mark_mutated(s, TEST_ATOM_X);
    ASSERT_EQ_INT(s->vars[0].classification, R8E_VAR_OWNED);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_mark_mutated_after_capture(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);

    /* First capture, then mutate */
    r8e_scope_mark_captured(s, TEST_ATOM_X);
    r8e_scope_mark_mutated(s, TEST_ATOM_X);

    ASSERT_TRUE(s->vars[0].flags & R8E_VAR_IS_CAPTURED);
    ASSERT_TRUE(s->vars[0].flags & R8E_VAR_IS_MUTATED_AFTER_CAP);
    ASSERT_EQ_INT(s->vars[0].classification, R8E_VAR_OWNED);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_mark_mutated_without_capture(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);

    /* Mutate without capture: should set OWNED but NOT MUTATED_AFTER_CAP */
    r8e_scope_mark_mutated(s, TEST_ATOM_X);
    ASSERT_EQ_INT(s->vars[0].classification, R8E_VAR_OWNED);
    ASSERT_FALSE(s->vars[0].flags & R8E_VAR_IS_MUTATED_AFTER_CAP);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_mark_mutated_stops_at_function_boundary(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *outer = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(outer, TEST_ATOM_X, R8E_VAR_IS_LET);

    R8EScope *inner = r8e_scope_push(&pool, outer, R8E_SCOPE_IS_FUNCTION);
    /* mark_mutated should stop at function boundary and not find x in outer */
    r8e_scope_mark_mutated(inner, TEST_ATOM_X);
    /* outer's x should remain BORROWED since mark_mutated stops at function boundary */
    ASSERT_EQ_INT(outer->vars[0].classification, R8E_VAR_BORROWED);

    r8e_scope_pop(&pool, inner);
    r8e_scope_pop(&pool, outer);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: RC Classification
 * ========================================================================= */

static void test_classify_use_borrowed_default(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    R8EVarClass cls = r8e_classify_use(s, TEST_ATOM_X, false, false, false);
    ASSERT_EQ_INT(cls, R8E_VAR_BORROWED);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_classify_use_owned_on_store(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    R8EVarClass cls = r8e_classify_use(s, TEST_ATOM_X, true, false, false);
    ASSERT_EQ_INT(cls, R8E_VAR_OWNED);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_classify_use_owned_on_return(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    R8EVarClass cls = r8e_classify_use(s, TEST_ATOM_X, false, true, false);
    ASSERT_EQ_INT(cls, R8E_VAR_OWNED);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_classify_use_owned_on_capture(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    R8EVarClass cls = r8e_classify_use(s, TEST_ATOM_X, false, false, true);
    ASSERT_EQ_INT(cls, R8E_VAR_OWNED);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: find_var
 * ========================================================================= */

static void test_scope_find_var_present(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);

    R8EVarInfo *v = r8e_scope_find_var(s, TEST_ATOM_X);
    ASSERT_TRUE(v != NULL);
    ASSERT_EQ_INT(v->atom, TEST_ATOM_X);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_find_var_absent(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);

    R8EVarInfo *v = r8e_scope_find_var(s, TEST_ATOM_X);
    ASSERT_TRUE(v == NULL);

    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Total Register Count
 * ========================================================================= */

static void test_scope_total_regs_empty(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    ASSERT_EQ_INT(r8e_scope_total_regs(s), 0);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

static void test_scope_total_regs_with_vars(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL, R8E_SCOPE_IS_FUNCTION);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    r8e_scope_define_var(s, TEST_ATOM_Y, R8E_VAR_IS_LET);
    r8e_scope_define_var(s, TEST_ATOM_Z, R8E_VAR_IS_LET);
    ASSERT_EQ_INT(r8e_scope_total_regs(s), 3);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Scope Dump (smoke test - should not crash)
 * ========================================================================= */

static void test_scope_dump_smoke(void) {
    R8EScopePool pool;
    r8e_scope_pool_init(&pool);
    R8EScope *s = r8e_scope_push(&pool, NULL,
        R8E_SCOPE_IS_FUNCTION | R8E_SCOPE_IS_STRICT);
    r8e_scope_define_var(s, TEST_ATOM_X, R8E_VAR_IS_LET);
    r8e_scope_define_var(s, TEST_ATOM_Y, R8E_VAR_IS_CONST);
    r8e_scope_mark_captured(s, TEST_ATOM_X);
    /* Just call dump and verify no crash */
    r8e_scope_dump(s, 0);
    ASSERT_TRUE(1);
    r8e_scope_pop(&pool, s);
    r8e_scope_pool_destroy(&pool);
}

/* =========================================================================
 * Tests: Break/Continue Label Stack
 * ========================================================================= */

static void test_labels_init(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    ASSERT_EQ_INT(ls.depth, 0);
}

static void test_labels_push_pop(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    int rc = r8e_labels_push(&ls, 0, false);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(ls.depth, 1);

    R8EBreakLabel *popped = r8e_labels_pop(&ls);
    ASSERT_TRUE(popped != NULL);
    ASSERT_EQ_INT(ls.depth, 0);
}

static void test_labels_top(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    ASSERT_TRUE(r8e_labels_top(&ls) == NULL); /* empty */

    r8e_labels_push(&ls, TEST_ATOM_LOOP, false);
    R8EBreakLabel *top = r8e_labels_top(&ls);
    ASSERT_TRUE(top != NULL);
    ASSERT_EQ_INT(top->atom, TEST_ATOM_LOOP);
    ASSERT_FALSE(top->is_switch);

    r8e_labels_pop(&ls);
}

static void test_labels_pop_empty(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    R8EBreakLabel *popped = r8e_labels_pop(&ls);
    ASSERT_TRUE(popped == NULL);
}

static void test_labels_push_switch(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, 0, true); /* switch */
    R8EBreakLabel *top = r8e_labels_top(&ls);
    ASSERT_TRUE(top != NULL);
    ASSERT_TRUE(top->is_switch);
    r8e_labels_pop(&ls);
}

static void test_labels_find_break_unlabeled(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, 0, false); /* innermost loop */
    R8EBreakLabel *label = r8e_labels_find_break(&ls, 0);
    ASSERT_TRUE(label != NULL);
    r8e_labels_pop(&ls);
}

static void test_labels_find_break_named(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, TEST_ATOM_LOOP, false);
    r8e_labels_push(&ls, 0, false); /* inner unlabeled */

    /* Named break should find the outer label */
    R8EBreakLabel *label = r8e_labels_find_break(&ls, TEST_ATOM_LOOP);
    ASSERT_TRUE(label != NULL);
    ASSERT_EQ_INT(label->atom, TEST_ATOM_LOOP);

    r8e_labels_pop(&ls);
    r8e_labels_pop(&ls);
}

static void test_labels_find_break_not_found(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    R8EBreakLabel *label = r8e_labels_find_break(&ls, 0);
    ASSERT_TRUE(label == NULL);
}

static void test_labels_find_continue_skips_switch(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, 0, false); /* loop */
    r8e_labels_push(&ls, 0, true);  /* switch inside loop */

    /* Unlabeled continue should skip switch and find the loop */
    R8EBreakLabel *label = r8e_labels_find_continue(&ls, 0);
    ASSERT_TRUE(label != NULL);
    ASSERT_FALSE(label->is_switch);

    r8e_labels_pop(&ls);
    r8e_labels_pop(&ls);
}

static void test_labels_find_continue_named(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, TEST_ATOM_LOOP, false);
    r8e_labels_push(&ls, 0, true); /* switch */

    R8EBreakLabel *label = r8e_labels_find_continue(&ls, TEST_ATOM_LOOP);
    ASSERT_TRUE(label != NULL);
    ASSERT_EQ_INT(label->atom, TEST_ATOM_LOOP);

    r8e_labels_pop(&ls);
    r8e_labels_pop(&ls);
}

static void test_labels_find_continue_not_found(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    /* Only switch on stack, no loop */
    r8e_labels_push(&ls, 0, true);
    R8EBreakLabel *label = r8e_labels_find_continue(&ls, 0);
    ASSERT_TRUE(label == NULL);
    r8e_labels_pop(&ls);
}

static void test_labels_find_continue_named_not_found(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, TEST_ATOM_LOOP, false);
    /* Looking for a different named continue */
    R8EBreakLabel *label = r8e_labels_find_continue(&ls, TEST_ATOM_FOO);
    ASSERT_TRUE(label == NULL);
    r8e_labels_pop(&ls);
}

/* =========================================================================
 * Tests: Break Patch Accumulation
 * ========================================================================= */

static void test_label_add_break_patch(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, 0, false);
    R8EBreakLabel *label = r8e_labels_top(&ls);

    int rc = r8e_label_add_break(label, 42);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(label->break_count, 1);
    ASSERT_EQ_INT(label->break_patches[0], 42);

    rc = r8e_label_add_break(label, 100);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(label->break_count, 2);
    ASSERT_EQ_INT(label->break_patches[1], 100);

    r8e_labels_pop(&ls);
}

static void test_label_add_break_overflow(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, 0, false);
    R8EBreakLabel *label = r8e_labels_top(&ls);

    /* Fill all 64 patch slots */
    for (int i = 0; i < R8E_MAX_BREAK_PATCHES; i++) {
        int rc = r8e_label_add_break(label, (uint32_t)i);
        ASSERT_EQ_INT(rc, 0);
    }
    ASSERT_EQ_INT(label->break_count, R8E_MAX_BREAK_PATCHES);

    /* 65th should fail */
    int rc = r8e_label_add_break(label, 999);
    ASSERT_EQ_INT(rc, -1);

    r8e_labels_pop(&ls);
}

static void test_labels_continue_target(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    r8e_labels_push(&ls, 0, false);
    R8EBreakLabel *label = r8e_labels_top(&ls);
    label->continue_target = 42;
    ASSERT_EQ_INT(label->continue_target, 42);
    r8e_labels_pop(&ls);
}

static void test_labels_overflow(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);
    /* Push to max depth */
    for (int i = 0; i < R8E_MAX_LABEL_DEPTH; i++) {
        int rc = r8e_labels_push(&ls, (uint32_t)i, false);
        ASSERT_EQ_INT(rc, 0);
    }
    ASSERT_EQ_INT(ls.depth, R8E_MAX_LABEL_DEPTH);
    /* One more should fail */
    int rc = r8e_labels_push(&ls, 999, false);
    ASSERT_EQ_INT(rc, -1);
    /* Clean up */
    for (int i = 0; i < R8E_MAX_LABEL_DEPTH; i++) {
        r8e_labels_pop(&ls);
    }
}

static void test_labels_nested_loops(void) {
    R8ELabelStack ls;
    r8e_labels_init(&ls);

    /* Simulate: outer: for (...) { inner: for (...) { switch { ... } } } */
    r8e_labels_push(&ls, TEST_ATOM_A, false); /* outer for */
    r8e_labels_push(&ls, TEST_ATOM_B, false); /* inner for */
    r8e_labels_push(&ls, 0, true);            /* switch */

    /* Unlabeled break: should find switch (innermost) */
    R8EBreakLabel *brk = r8e_labels_find_break(&ls, 0);
    ASSERT_TRUE(brk != NULL);
    ASSERT_TRUE(brk->is_switch);

    /* Unlabeled continue: should skip switch, find inner for */
    R8EBreakLabel *cont = r8e_labels_find_continue(&ls, 0);
    ASSERT_TRUE(cont != NULL);
    ASSERT_EQ_INT(cont->atom, TEST_ATOM_B);

    /* Named continue to outer: should find outer for */
    R8EBreakLabel *cont_outer = r8e_labels_find_continue(&ls, TEST_ATOM_A);
    ASSERT_TRUE(cont_outer != NULL);
    ASSERT_EQ_INT(cont_outer->atom, TEST_ATOM_A);

    r8e_labels_pop(&ls);
    r8e_labels_pop(&ls);
    r8e_labels_pop(&ls);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_scope_tests(void) {
    /* Scope Pool */
    RUN_TEST(test_scope_pool_init);
    RUN_TEST(test_scope_pool_alloc_fresh);
    RUN_TEST(test_scope_pool_release_and_reuse);
    RUN_TEST(test_scope_pool_overflow_frees);

    /* Scope Push / Pop */
    RUN_TEST(test_scope_push_function_root);
    RUN_TEST(test_scope_push_block_inherits_base);
    RUN_TEST(test_scope_push_function_resets_base);
    RUN_TEST(test_scope_pop_returns_parent);
    RUN_TEST(test_scope_strict_mode_inherits);
    RUN_TEST(test_scope_strict_mode_not_inherited_if_parent_not_strict);
    RUN_TEST(test_scope_depth_increments);

    /* Variable Definition */
    RUN_TEST(test_scope_define_let);
    RUN_TEST(test_scope_define_const);
    RUN_TEST(test_scope_define_var_plain);
    RUN_TEST(test_scope_define_param);
    RUN_TEST(test_scope_define_multiple_vars);
    RUN_TEST(test_scope_define_duplicate_let_fails);
    RUN_TEST(test_scope_define_duplicate_const_fails);
    RUN_TEST(test_scope_define_var_duplicate_reuses);
    RUN_TEST(test_scope_var_hoists_to_function);
    RUN_TEST(test_scope_block_let_shadows_function_var);
    RUN_TEST(test_scope_default_classification_borrowed);

    /* Variable Resolution */
    RUN_TEST(test_scope_resolve_local);
    RUN_TEST(test_scope_resolve_from_parent_block);
    RUN_TEST(test_scope_resolve_global);
    RUN_TEST(test_scope_resolve_closure);
    RUN_TEST(test_scope_resolve_closure_through_block);
    RUN_TEST(test_scope_resolve_shadows_outer);

    /* Capture Marking */
    RUN_TEST(test_scope_mark_captured);
    RUN_TEST(test_scope_mark_captured_traverses_parents);
    RUN_TEST(test_scope_mark_captured_nonexistent);

    /* Mutation Marking */
    RUN_TEST(test_scope_mark_mutated_promotes_to_owned);
    RUN_TEST(test_scope_mark_mutated_after_capture);
    RUN_TEST(test_scope_mark_mutated_without_capture);
    RUN_TEST(test_scope_mark_mutated_stops_at_function_boundary);

    /* RC Classification */
    RUN_TEST(test_classify_use_borrowed_default);
    RUN_TEST(test_classify_use_owned_on_store);
    RUN_TEST(test_classify_use_owned_on_return);
    RUN_TEST(test_classify_use_owned_on_capture);

    /* find_var */
    RUN_TEST(test_scope_find_var_present);
    RUN_TEST(test_scope_find_var_absent);

    /* Total Register Count */
    RUN_TEST(test_scope_total_regs_empty);
    RUN_TEST(test_scope_total_regs_with_vars);

    /* Scope Dump */
    RUN_TEST(test_scope_dump_smoke);

    /* Label Stack - Basic */
    RUN_TEST(test_labels_init);
    RUN_TEST(test_labels_push_pop);
    RUN_TEST(test_labels_top);
    RUN_TEST(test_labels_pop_empty);
    RUN_TEST(test_labels_push_switch);

    /* Label Stack - Break */
    RUN_TEST(test_labels_find_break_unlabeled);
    RUN_TEST(test_labels_find_break_named);
    RUN_TEST(test_labels_find_break_not_found);

    /* Label Stack - Continue */
    RUN_TEST(test_labels_find_continue_skips_switch);
    RUN_TEST(test_labels_find_continue_named);
    RUN_TEST(test_labels_find_continue_not_found);
    RUN_TEST(test_labels_find_continue_named_not_found);

    /* Label Stack - Patches */
    RUN_TEST(test_label_add_break_patch);
    RUN_TEST(test_label_add_break_overflow);
    RUN_TEST(test_labels_continue_target);
    RUN_TEST(test_labels_overflow);
    RUN_TEST(test_labels_nested_loops);
}
