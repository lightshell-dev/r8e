/*
 * test_parse.c - Unit tests for r8e_parse.c (Single-Pass Parser/Compiler)
 *
 * Tests cover:
 *   - Numeric literal parsing
 *   - Binary expression parsing (1 + 2)
 *   - Variable declarations (var/let/const)
 *   - If/else statements
 *   - For loop compilation
 *   - Function declarations and calls
 *   - String literals
 *   - Array literals
 *   - Object literals
 *   - Syntax error detection
 *   - Automatic semicolon insertion (ASI) edge case
 *   - Bytecode verification of emitted opcodes
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
#include "../../include/r8e_opcodes.h"
#include "../../include/r8e_atoms.h"

/* =========================================================================
 * Forward declarations for parser/bytecode buffer API
 * ========================================================================= */

/* R8EBytecodeBuffer is defined in r8e_bc.c. We only need the struct layout
 * for accessing .code, .length, .constants, etc. Since it is #included by
 * r8e_parse.c, we re-declare the minimal layout here for test use. */
typedef struct R8EBytecodeBuffer {
    uint8_t  *code;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *constants;
    uint16_t  const_count;
    uint16_t  const_capacity;
    uint32_t *line_table;
    uint16_t  line_count;
    uint16_t  line_capacity;
    struct R8EFuncDesc {
        struct R8EBytecodeBuffer *bc;
        uint32_t  name_atom;
        uint16_t  param_count;
        uint16_t  local_count;
        uint16_t  capture_count;
        uint16_t  stack_size;
        uint8_t   is_strict;
        uint8_t   is_arrow;
        uint8_t   is_generator;
        uint8_t   is_async;
        uint16_t  source_line;
        struct R8ECaptureInfo {
            uint32_t atom;
            uint8_t  src_reg;
            uint8_t  src_depth;
            uint8_t  is_mutable;
        } *captures;
    } *functions;
    uint16_t  func_count;
    uint16_t  func_capacity;
    uint16_t  max_stack;
    uint16_t  pad;
} R8EBytecodeBuffer;

/* External APIs from r8e_parse.c / r8e_bc.c */
extern int r8e_parse(R8EAtomTable *atoms, const char *source, uint32_t length,
                     R8EBytecodeBuffer *out_bc, const char *filename);
extern R8EBytecodeBuffer *r8e_compile_script(R8EAtomTable *atoms,
                                              const char *source,
                                              uint32_t length,
                                              const char *filename);
extern int  r8e_bc_init(R8EBytecodeBuffer *bc);
extern void r8e_bc_destroy(R8EBytecodeBuffer *bc);

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
 * Helper: initialize atom table for testing
 * ========================================================================= */

static R8EAtomTable *create_test_atoms(void) {
    R8EAtomTable *table = (R8EAtomTable *)calloc(1, sizeof(R8EAtomTable));
    if (!table) return NULL;
    r8e_atom_table_init(table);
    return table;
}

static void free_test_atoms(R8EAtomTable *table) {
    if (table) {
        r8e_atom_table_free(table);
        free(table);
    }
}

/* Helper: compile a script and check success/failure */
static R8EBytecodeBuffer *compile_ok(R8EAtomTable *atoms,
                                      const char *source) {
    R8EBytecodeBuffer *bc = r8e_compile_script(
        atoms, source, (uint32_t)strlen(source), "<test>");
    return bc;
}

/* Helper: check if a specific opcode exists in bytecode */
static bool bc_contains_opcode(const R8EBytecodeBuffer *bc, uint8_t opcode) {
    if (!bc || !bc->code) return false;
    for (uint32_t i = 0; i < bc->length; i++) {
        if (bc->code[i] == opcode) return true;
    }
    return false;
}

/* =========================================================================
 * Tests: Successful Parsing
 * ========================================================================= */

/* Parse "42" -> should produce bytecode with PUSH and HALT */
static void test_parse_number_literal(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "42;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);
    ASSERT_TRUE(bc->code != NULL);

    /* The bytecode should end with HALT */
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_HALT));

    /* Should contain a push instruction for 42 */
    bool has_push = bc_contains_opcode(bc, R8E_OP_PUSH_INT8) ||
                    bc_contains_opcode(bc, R8E_OP_PUSH_INT32) ||
                    bc_contains_opcode(bc, R8E_OP_LOAD_CONST);
    ASSERT_TRUE(has_push);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "1 + 2" -> should produce ADD opcode */
static void test_parse_addition(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "1 + 2;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_ADD));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "3 * 4 - 1" -> should produce MUL and SUB opcodes */
static void test_parse_arithmetic_precedence(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "3 * 4 - 1;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_MUL));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_SUB));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "var x = 10;" -> should produce STORE_LOCAL */
static void test_parse_var_decl(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var x = 10;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);
    /* Should have a store to a local */
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_STORE_LOCAL) ||
                bc_contains_opcode(bc, R8E_OP_STORE_LOCAL_POP));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "let a = 5; a;" -> should produce STORE_LOCAL and LOAD_LOCAL */
static void test_parse_let_decl_and_use(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "let a = 5; a;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_LOAD_LOCAL));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "if (true) 1; else 2;" -> should produce JUMP_IF_FALSE */
static void test_parse_if_else(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "if (true) 1; else 2;");
    ASSERT_TRUE(bc != NULL);
    /* Should contain a conditional jump */
    bool has_cond_jump = bc_contains_opcode(bc, R8E_OP_JUMP_IF_FALSE) ||
                         bc_contains_opcode(bc, R8E_OP_JUMP_IF_FALSE8) ||
                         bc_contains_opcode(bc, R8E_OP_JUMP_IF_FALSE16);
    ASSERT_TRUE(has_cond_jump);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "while (false) {}" -> should compile successfully */
static void test_parse_while_loop(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "while (false) {}");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);
    /* Should contain a backward jump or conditional jump */
    bool has_jump = bc_contains_opcode(bc, R8E_OP_JUMP) ||
                    bc_contains_opcode(bc, R8E_OP_JUMP8) ||
                    bc_contains_opcode(bc, R8E_OP_JUMP16);
    ASSERT_TRUE(has_jump);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "for (var i = 0; i < 3; i++) {}" -> should contain loop constructs */
static void test_parse_for_loop(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "for (var i = 0; i < 3; i++) {}");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);
    /* Should have comparison and jump */
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_LT) ||
                bc_contains_opcode(bc, R8E_OP_LOOP_CMP_BRANCH));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "function f() { return 42; }" -> should produce NEW_FUNCTION */
static void test_parse_function_decl(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "function f() { return 42; }");
    ASSERT_TRUE(bc != NULL);
    /* Should have at least one inner function */
    ASSERT_TRUE(bc->func_count >= 1);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "'hello'" -> should produce a string constant */
static void test_parse_string_literal(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "'hello';");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "[1, 2, 3]" -> should produce NEW_ARRAY */
static void test_parse_array_literal(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "[1, 2, 3];");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_NEW_ARRAY));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "{x: 1}" -> should produce NEW_OBJECT or INIT_PROP */
static void test_parse_object_literal(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var o = {x: 1};");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_NEW_OBJECT));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_INIT_PROP));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "true" and "false" literals */
static void test_parse_boolean_literals(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "true; false;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_PUSH_TRUE));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_PUSH_FALSE));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "null" and "undefined" */
static void test_parse_null_undefined(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "null; undefined;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_PUSH_NULL));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse "var x = 10; var y = 20; x + y;" -> verify multiple vars */
static void test_parse_multiple_vars(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var x = 10; var y = 20; x + y;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_ADD));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse typeof operator */
static void test_parse_typeof(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "typeof 42;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_TYPEOF));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Parse unary negation */
static void test_parse_unary_neg(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "-42;");
    ASSERT_TRUE(bc != NULL);
    /* Could be optimized as a negative constant or NEG opcode */
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* =========================================================================
 * Tests: Syntax Errors
 * ========================================================================= */

/* "var" alone is a syntax error */
static void test_parse_error_incomplete_var(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int result = r8e_parse(atoms, "var", 3, &bc, "<test>");
    ASSERT_EQ_INT(result, -1); /* should fail */

    r8e_bc_destroy(&bc);
    free_test_atoms(atoms);
}

/* "1 +" is incomplete, should fail */
static void test_parse_error_incomplete_expr(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int result = r8e_parse(atoms, "1 +", 3, &bc, "<test>");
    ASSERT_EQ_INT(result, -1);

    r8e_bc_destroy(&bc);
    free_test_atoms(atoms);
}

/* Unterminated brace */
static void test_parse_error_unterminated_brace(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int result = r8e_parse(atoms, "{", 1, &bc, "<test>");
    ASSERT_EQ_INT(result, -1);

    r8e_bc_destroy(&bc);
    free_test_atoms(atoms);
}

/* Empty source -> should succeed (empty program is valid) */
static void test_parse_empty_source(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "");
    ASSERT_TRUE(bc != NULL);
    /* Should just contain HALT */
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_HALT));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Semicolons only -> valid empty program */
static void test_parse_only_semicolons(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, ";;;");
    ASSERT_TRUE(bc != NULL);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Block statement */
static void test_parse_block(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "{ var x = 1; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Comparison operators */
static void test_parse_comparisons(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "1 < 2; 3 > 2; 4 <= 4; 5 >= 5; 6 == 6; 7 !== 8;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_LT));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_GT));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Logical NOT */
static void test_parse_logical_not(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "!true;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_NOT));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* Return statement in function */
static void test_parse_return_value(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "function f() { return 42; }");
    ASSERT_TRUE(bc != NULL);
    /* The inner function should contain RETURN */
    if (bc->func_count >= 1 && bc->functions[0].bc) {
        ASSERT_TRUE(bc_contains_opcode(bc->functions[0].bc, R8E_OP_RETURN));
    }

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* =========================================================================
 * NEW TESTS: Extended Parser Coverage
 * ========================================================================= */

/* --- const declaration --- */
static void test_parse_const_decl(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "const PI = 3.14;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Multiple variable declaration in single statement --- */
static void test_parse_multi_var_decl(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var a = 1, b = 2, c = 3;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Arrow function with single param (no parens) --- */
static void test_parse_arrow_single_param(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var f = x => x + 1;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->func_count >= 1);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Arrow function with no params --- */
static void test_parse_arrow_no_params(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var f = () => 42;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->func_count >= 1);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Arrow function with block body --- */
static void test_parse_arrow_block_body(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var f = x => { return x * 2; };");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->func_count >= 1);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Do-while loop --- */
static void test_parse_do_while(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var i = 0; do { i = i + 1; } while (i < 10);");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Nested if statements --- */
static void test_parse_nested_if(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "if (true) { if (false) 1; else 2; } else 3;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Switch statement --- */
static void test_parse_switch(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "switch (1) { case 1: break; case 2: break; default: break; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Try/catch --- */
static void test_parse_try_catch(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "try { throw 1; } catch (e) { e; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_TRY_PUSH));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_THROW));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Try/catch/finally --- */
static void test_parse_try_catch_finally(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "try { 1; } catch (e) { 2; } finally { 3; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Try/finally (no catch) --- */
static void test_parse_try_finally(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "try { 1; } finally { 2; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Bitwise operators --- */
static void test_parse_bitwise_ops(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "1 & 2; 3 | 4; 5 ^ 6; ~7; 8 << 1; 16 >> 1; 32 >>> 1;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_BITAND));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_BITOR));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_BITXOR));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_BITNOT));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Logical AND/OR --- */
static void test_parse_logical_ops(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "true && false; true || false;");
    ASSERT_TRUE(bc != NULL);
    /* Logical ops compile to conditional jumps */
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Ternary/conditional expression --- */
static void test_parse_ternary(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "true ? 1 : 2;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Property access (dot) --- */
static void test_parse_property_access(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var o = {}; o.x;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_GET_PROP));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Computed property access (bracket) --- */
static void test_parse_computed_access(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var a = [1]; a[0];");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_GET_ELEM));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Function call --- */
static void test_parse_function_call(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "function f(x) { return x; } f(42);");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_CALL));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- New expression --- */
static void test_parse_new_expr(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "new Object();");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_NEW));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Delete operator --- */
static void test_parse_delete(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var o = {x:1}; delete o.x;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_DELETE_PROP));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Void operator --- */
static void test_parse_void(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "void 0;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_VOID));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Prefix increment --- */
static void test_parse_prefix_inc(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var x = 0; ++x;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_INC));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Postfix increment --- */
static void test_parse_postfix_inc(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "var x = 0; x++;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Compound assignment operators --- */
static void test_parse_compound_assign(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var x = 10; x += 5; x -= 3; x *= 2; x /= 4; x %= 3;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_ADD));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_SUB));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_MUL));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- This keyword --- */
static void test_parse_this(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "this;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_LOAD_THIS));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Template literal (no interpolation) --- */
static void test_parse_template_full(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "`hello world`;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Nested function --- */
static void test_parse_nested_function(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "function outer() { function inner() { return 1; } return inner; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->func_count >= 1);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Break in loop --- */
static void test_parse_break(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "while (true) { break; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Continue in loop --- */
static void test_parse_continue(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "for (var i = 0; i < 10; i++) { continue; }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Exponentiation operator --- */
static void test_parse_exponentiation(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "2 ** 10;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_POW));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Nested blocks with scoping --- */
static void test_parse_nested_blocks(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "{ let x = 1; { let y = 2; x + y; } }");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_ADD));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Object shorthand property --- */
static void test_parse_object_shorthand(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var x = 1; var o = {x};");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_NEW_OBJECT));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_INIT_PROP));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Spread in array --- */
static void test_parse_array_spread(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "var a = [1,2]; var b = [...a, 3];");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_SPREAD));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Comma expression --- */
static void test_parse_comma_expression(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "(1, 2, 3);");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc->length > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Strict equality operators --- */
static void test_parse_strict_equality(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "1 === 1; 2 !== 3;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_SEQ));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_SNE));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Error: missing closing paren --- */
static void test_parse_error_missing_rparen(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int result = r8e_parse(atoms, "if (true {}", 11, &bc, "<test>");
    ASSERT_EQ_INT(result, -1);

    r8e_bc_destroy(&bc);
    free_test_atoms(atoms);
}

/* --- Error: missing closing bracket --- */
static void test_parse_error_missing_rbracket(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int result = r8e_parse(atoms, "var a = [1, 2", 13, &bc, "<test>");
    ASSERT_EQ_INT(result, -1);

    r8e_bc_destroy(&bc);
    free_test_atoms(atoms);
}

/* --- ASI after return --- */
static void test_parse_asi_return(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    /* ASI after return means "return undefined" then "42" is separate statement */
    R8EBytecodeBuffer *bc = compile_ok(atoms,
        "function f() { return\n42; }");
    ASSERT_TRUE(bc != NULL);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Large integer constant --- */
static void test_parse_large_int(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "1000000;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_PUSH_INT32) ||
                bc_contains_opcode(bc, R8E_OP_LOAD_CONST));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Float constant uses const pool --- */
static void test_parse_float_constant(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "3.14;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_LOAD_CONST));
    ASSERT_TRUE(bc->const_count > 0);

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* --- Zero and one use specialized opcodes --- */
static void test_parse_zero_one_specialized(void) {
    R8EAtomTable *atoms = create_test_atoms();
    ASSERT_TRUE(atoms != NULL);

    R8EBytecodeBuffer *bc = compile_ok(atoms, "0; 1;");
    ASSERT_TRUE(bc != NULL);
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_PUSH_ZERO));
    ASSERT_TRUE(bc_contains_opcode(bc, R8E_OP_PUSH_ONE));

    r8e_bc_destroy(bc);
    free(bc);
    free_test_atoms(atoms);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_parse_tests(void) {
    /* Existing tests */
    RUN_TEST(test_parse_number_literal);
    RUN_TEST(test_parse_addition);
    RUN_TEST(test_parse_arithmetic_precedence);
    RUN_TEST(test_parse_var_decl);
    RUN_TEST(test_parse_let_decl_and_use);
    RUN_TEST(test_parse_if_else);
    RUN_TEST(test_parse_while_loop);
    RUN_TEST(test_parse_for_loop);
    RUN_TEST(test_parse_function_decl);
    RUN_TEST(test_parse_string_literal);
    RUN_TEST(test_parse_array_literal);
    RUN_TEST(test_parse_object_literal);
    RUN_TEST(test_parse_boolean_literals);
    RUN_TEST(test_parse_null_undefined);
    RUN_TEST(test_parse_multiple_vars);
    RUN_TEST(test_parse_typeof);
    RUN_TEST(test_parse_unary_neg);
    RUN_TEST(test_parse_error_incomplete_var);
    RUN_TEST(test_parse_error_incomplete_expr);
    RUN_TEST(test_parse_error_unterminated_brace);
    RUN_TEST(test_parse_empty_source);
    RUN_TEST(test_parse_only_semicolons);
    RUN_TEST(test_parse_block);
    RUN_TEST(test_parse_comparisons);
    RUN_TEST(test_parse_logical_not);
    RUN_TEST(test_parse_return_value);

    /* NEW tests */
    RUN_TEST(test_parse_const_decl);
    RUN_TEST(test_parse_multi_var_decl);
    RUN_TEST(test_parse_arrow_single_param);
    RUN_TEST(test_parse_arrow_no_params);
    RUN_TEST(test_parse_arrow_block_body);
    RUN_TEST(test_parse_do_while);
    RUN_TEST(test_parse_nested_if);
    RUN_TEST(test_parse_switch);
    RUN_TEST(test_parse_try_catch);
    RUN_TEST(test_parse_try_catch_finally);
    RUN_TEST(test_parse_try_finally);
    RUN_TEST(test_parse_bitwise_ops);
    RUN_TEST(test_parse_logical_ops);
    RUN_TEST(test_parse_ternary);
    RUN_TEST(test_parse_property_access);
    RUN_TEST(test_parse_computed_access);
    RUN_TEST(test_parse_function_call);
    RUN_TEST(test_parse_new_expr);
    RUN_TEST(test_parse_delete);
    RUN_TEST(test_parse_void);
    RUN_TEST(test_parse_prefix_inc);
    RUN_TEST(test_parse_postfix_inc);
    RUN_TEST(test_parse_compound_assign);
    RUN_TEST(test_parse_this);
    RUN_TEST(test_parse_template_full);
    RUN_TEST(test_parse_nested_function);
    RUN_TEST(test_parse_break);
    RUN_TEST(test_parse_continue);
    RUN_TEST(test_parse_exponentiation);
    RUN_TEST(test_parse_nested_blocks);
    RUN_TEST(test_parse_object_shorthand);
    RUN_TEST(test_parse_array_spread);
    RUN_TEST(test_parse_comma_expression);
    RUN_TEST(test_parse_strict_equality);
    RUN_TEST(test_parse_error_missing_rparen);
    RUN_TEST(test_parse_error_missing_rbracket);
    RUN_TEST(test_parse_asi_return);
    RUN_TEST(test_parse_large_int);
    RUN_TEST(test_parse_float_constant);
    RUN_TEST(test_parse_zero_one_specialized);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_PARSE_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[parse]\n");
    run_parse_tests();
    printf("\n=== parse: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
