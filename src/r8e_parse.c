/*
 * r8e_parse.c - Single-Pass Parser/Compiler (NO AST)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 5 (Single-Pass Compiler).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "../include/r8e_types.h"
#include "../include/r8e_opcodes.h"
#include "../include/r8e_atoms.h"

/* Include the lexer and bytecode buffer implementations */
#include "r8e_token.c"
#include "r8e_bc.c"
#include "r8e_scope.c"

/* =========================================================================
 * Parser State
 * ========================================================================= */

#define R8E_MAX_PATCH_POINTS 32
#define R8E_MAX_PARSE_ERRORS 16
#define R8E_MAX_FUNC_DEPTH   64

typedef struct {
    uint32_t offset;    /* bytecode offset to patch */
    uint8_t  kind;      /* what kind of patch */
} R8EPatchPoint;

typedef struct R8EParser {
    /* Lexer */
    R8ELexer         lex;
    R8EToken         cur;          /* current token */
    R8EToken         prev;         /* previous token */

    /* Context for atom interning */
    R8EAtomTable    *atoms;

    /* Bytecode output */
    R8EBytecodeBuffer *bc;         /* current function's bytecode */

    /* Scope tracking */
    R8EScope        *scope;        /* current scope */
    R8EScopePool     scope_pool;

    /* Break/continue labels */
    R8ELabelStack    labels;

    /* Function nesting */
    R8EBytecodeBuffer *func_stack[R8E_MAX_FUNC_DEPTH];
    R8EScope          *func_scope_stack[R8E_MAX_FUNC_DEPTH];
    uint16_t           func_max_regs[R8E_MAX_FUNC_DEPTH];
    int                func_depth;

    /* Arrow function speculation */
    R8EPatchPoint    patch_points[R8E_MAX_PATCH_POINTS];
    int              patch_count;

    /* Error state */
    bool             had_error;
    bool             panic_mode;
    char             error_msg[256];
    uint32_t         error_line;
    uint32_t         error_col;

    /* Parser context flags */
    bool             is_strict;
    bool             in_function;
    bool             in_loop;
    bool             in_switch;
    bool             in_async;
    bool             in_generator;
    bool             allow_in;      /* for-in disambiguation */

    /* Register high-water mark for current function */
    uint16_t         max_regs;

} R8EParser;

/* =========================================================================
 * Error Reporting
 * ========================================================================= */

static void r8e_parse_error(R8EParser *p, const char *msg)
{
    if (p->panic_mode) return;
    p->had_error = true;
    p->panic_mode = true;
    p->error_line = p->cur.line;
    p->error_col = p->cur.col;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "SyntaxError: %s at line %u col %u", msg, p->cur.line, p->cur.col);
    fprintf(stderr, "%s\n", p->error_msg);
}

static void r8e_parse_error_at(R8EParser *p, const R8EToken *tok,
                                const char *msg)
{
    if (p->panic_mode) return;
    p->had_error = true;
    p->panic_mode = true;
    p->error_line = tok->line;
    p->error_col = tok->col;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "SyntaxError: %s at line %u col %u", msg, tok->line, tok->col);
    fprintf(stderr, "%s\n", p->error_msg);
}

/* =========================================================================
 * Token Helpers
 * ========================================================================= */

static void r8e_advance(R8EParser *p)
{
    p->prev = p->cur;
    for (;;) {
        r8e_lexer_next(&p->lex, &p->cur);
        if (p->cur.type != R8E_TOK_ERROR) break;
        r8e_parse_error(p, p->cur.error_msg ? p->cur.error_msg : "scan error");
    }
}

static bool r8e_check(R8EParser *p, R8ETokenType type)
{
    return p->cur.type == type;
}

static bool r8e_match(R8EParser *p, R8ETokenType type)
{
    if (p->cur.type != type) return false;
    r8e_advance(p);
    return true;
}

static void r8e_consume(R8EParser *p, R8ETokenType type, const char *msg)
{
    if (p->cur.type == type) {
        r8e_advance(p);
        return;
    }
    r8e_parse_error(p, msg);
}

static bool r8e_parser_at_end(R8EParser *p)
{
    return p->cur.type == R8E_TOK_EOF;
}

/* Check if current token can start an expression */
static bool r8e_can_start_expr(R8EParser *p)
{
    switch (p->cur.type) {
    case R8E_TOK_IDENT:
    case R8E_TOK_NUMBER:
    case R8E_TOK_STRING:
    case R8E_TOK_TEMPLATE_HEAD:
    case R8E_TOK_TEMPLATE_FULL:
    case R8E_TOK_REGEX:
    case R8E_TOK_KW_THIS:
    case R8E_TOK_KW_NULL:
    case R8E_TOK_KW_TRUE:
    case R8E_TOK_KW_FALSE:
    case R8E_TOK_KW_UNDEFINED:
    case R8E_TOK_KW_FUNCTION:
    case R8E_TOK_KW_CLASS:
    case R8E_TOK_KW_NEW:
    case R8E_TOK_KW_DELETE:
    case R8E_TOK_KW_TYPEOF:
    case R8E_TOK_KW_VOID:
    case R8E_TOK_KW_AWAIT:
    case R8E_TOK_KW_YIELD:
    case R8E_TOK_KW_SUPER:
    case R8E_TOK_KW_IMPORT:
    case R8E_TOK_LPAREN:
    case R8E_TOK_LBRACKET:
    case R8E_TOK_LBRACE:
    case R8E_TOK_NOT:
    case R8E_TOK_TILDE:
    case R8E_TOK_PLUS:
    case R8E_TOK_MINUS:
    case R8E_TOK_INC:
    case R8E_TOK_DEC:
    case R8E_TOK_ELLIPSIS:
        return true;
    default:
        return false;
    }
}

/* =========================================================================
 * Atom Interning Helper
 * ========================================================================= */

static uint32_t r8e_parser_intern(R8EParser *p, const char *str, uint32_t len)
{
    return r8e_atom_intern(p->atoms, str, len);
}

static uint32_t r8e_parser_intern_token(R8EParser *p, const R8EToken *tok)
{
    return r8e_atom_intern(p->atoms, tok->str_val.str, tok->str_val.len);
}

/* =========================================================================
 * Bytecode Emission Helpers (delegate to bc module)
 * ========================================================================= */

static void emit_op(R8EParser *p, uint8_t op)
{
    r8e_bc_emit_op(p->bc, op);
}

static void emit_op_u8(R8EParser *p, uint8_t op, uint8_t val)
{
    r8e_bc_emit_op_u8(p->bc, op, val);
}

static void emit_op_u16(R8EParser *p, uint8_t op, uint16_t val)
{
    r8e_bc_emit_op_u16(p->bc, op, val);
}

static void emit_op_u32(R8EParser *p, uint8_t op, uint32_t val)
{
    r8e_bc_emit_op_u32(p->bc, op, val);
}

static uint32_t emit_jump(R8EParser *p, uint8_t op)
{
    return r8e_bc_emit_jump(p->bc, op);
}

static void patch_jump(R8EParser *p, uint32_t addr)
{
    r8e_bc_patch_jump(p->bc, addr);
}

static uint32_t current_offset(R8EParser *p)
{
    return r8e_bc_current_offset(p->bc);
}

static void emit_line(R8EParser *p)
{
    r8e_bc_add_line(p->bc, p->cur.line);
}

/* Emit a constant and push it */
static void emit_constant(R8EParser *p, R8EValue val)
{
    int idx = r8e_bc_add_constant(p->bc, val);
    if (idx < 0) {
        r8e_parse_error(p, "too many constants");
        return;
    }
    emit_op_u16(p, R8E_OP_LOAD_CONST, (uint16_t)idx);
}

/* Emit a loop back-jump to 'loop_start' */
static void emit_loop(R8EParser *p, uint32_t loop_start)
{
    emit_op(p, R8E_OP_CHECK_LIMITS);
    r8e_bc_emit_op(p->bc, R8E_OP_JUMP);
    int32_t offset = (int32_t)(loop_start - (p->bc->length + 4));
    r8e_bc_emit_i32(p->bc, offset);
}

/* Register tracking */
static void track_regs(R8EParser *p)
{
    uint16_t total = p->scope->local_base + p->scope->local_count;
    if (total > p->max_regs) {
        p->max_regs = total;
    }
}

/* =========================================================================
 * Scope Helpers
 * ========================================================================= */

static void push_scope(R8EParser *p, uint8_t flags)
{
    p->scope = r8e_scope_push(&p->scope_pool, p->scope, flags);
    if (!p->scope) {
        r8e_parse_error(p, "scope stack overflow");
    }
}

static void pop_scope(R8EParser *p)
{
    track_regs(p);
    p->scope = r8e_scope_pop(&p->scope_pool, p->scope);
}

static int define_var(R8EParser *p, uint32_t atom, uint8_t flags)
{
    int reg = r8e_scope_define_var(p->scope, atom, flags);
    if (reg == -1) {
        r8e_parse_error(p, "too many local variables");
    } else if (reg == -2) {
        r8e_parse_error(p, "duplicate declaration");
    }
    track_regs(p);
    return reg;
}

/* =========================================================================
 * ASI (Automatic Semicolon Insertion)
 * ========================================================================= */

static void consume_semicolon(R8EParser *p)
{
    if (r8e_match(p, R8E_TOK_SEMICOLON)) return;

    /* ASI: insert semicolon if newline before current token,
     * or current token is } or EOF */
    if ((p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE) ||
        p->cur.type == R8E_TOK_RBRACE ||
        p->cur.type == R8E_TOK_EOF) {
        return;
    }

    r8e_parse_error(p, "expected ';'");
}

/* =========================================================================
 * Forward Declarations
 * ========================================================================= */

static void parse_statement(R8EParser *p);
static void parse_declaration(R8EParser *p);
static void parse_expression(R8EParser *p);
static void parse_assignment_expr(R8EParser *p);
static void parse_conditional_expr(R8EParser *p);
static void parse_binary_expr(R8EParser *p, int min_prec);
static void parse_unary_expr(R8EParser *p);
static void parse_postfix_expr(R8EParser *p);
static void parse_call_expr(R8EParser *p);
static void parse_member_expr(R8EParser *p);
static void parse_primary_expr(R8EParser *p);
static void parse_block(R8EParser *p);
static void parse_var_declaration(R8EParser *p, R8ETokenType kind);
static void parse_function_decl(R8EParser *p, bool is_expr, bool is_async);
static void parse_class_decl(R8EParser *p, bool is_expr);
static void parse_if_stmt(R8EParser *p);
static void parse_while_stmt(R8EParser *p);
static void parse_do_while_stmt(R8EParser *p);
static void parse_for_stmt(R8EParser *p);
static void parse_switch_stmt(R8EParser *p);
static void parse_try_stmt(R8EParser *p);
static void parse_return_stmt(R8EParser *p);
static void parse_throw_stmt(R8EParser *p);
static void parse_break_stmt(R8EParser *p);
static void parse_continue_stmt(R8EParser *p);
static int  parse_arguments(R8EParser *p);
static void parse_array_literal(R8EParser *p);
static void parse_object_literal(R8EParser *p);
static void parse_arrow_function(R8EParser *p, bool is_async);

/* =========================================================================
 * Operator Precedence Table (Pratt parser)
 * ========================================================================= */

typedef enum {
    PREC_NONE        = 0,
    PREC_COMMA       = 1,
    PREC_ASSIGN      = 2,
    PREC_CONDITIONAL = 3,
    PREC_NULLISH     = 4,
    PREC_OR          = 5,
    PREC_AND         = 6,
    PREC_BIT_OR      = 7,
    PREC_BIT_XOR     = 8,
    PREC_BIT_AND     = 9,
    PREC_EQUALITY    = 10,
    PREC_RELATIONAL  = 11,
    PREC_SHIFT       = 12,
    PREC_ADD         = 13,
    PREC_MUL         = 14,
    PREC_EXPO        = 15,
    PREC_UNARY       = 16,
    PREC_POSTFIX     = 17,
    PREC_CALL        = 18,
    PREC_PRIMARY     = 19,
} R8EPrecedence;

static int get_binary_precedence(R8ETokenType type)
{
    switch (type) {
    case R8E_TOK_NULLISH:       return PREC_NULLISH;
    case R8E_TOK_LOR:           return PREC_OR;
    case R8E_TOK_LAND:          return PREC_AND;
    case R8E_TOK_PIPE:          return PREC_BIT_OR;
    case R8E_TOK_CARET:         return PREC_BIT_XOR;
    case R8E_TOK_AMP:           return PREC_BIT_AND;
    case R8E_TOK_EQ:
    case R8E_TOK_NE:
    case R8E_TOK_SEQ:
    case R8E_TOK_SNE:           return PREC_EQUALITY;
    case R8E_TOK_LT:
    case R8E_TOK_GT:
    case R8E_TOK_LE:
    case R8E_TOK_GE:
    case R8E_TOK_KW_INSTANCEOF:
    case R8E_TOK_KW_IN:         return PREC_RELATIONAL;
    case R8E_TOK_SHL:
    case R8E_TOK_SHR:
    case R8E_TOK_USHR:          return PREC_SHIFT;
    case R8E_TOK_PLUS:
    case R8E_TOK_MINUS:         return PREC_ADD;
    case R8E_TOK_STAR:
    case R8E_TOK_SLASH:
    case R8E_TOK_PERCENT:       return PREC_MUL;
    case R8E_TOK_STAR_STAR:     return PREC_EXPO;
    default:                    return PREC_NONE;
    }
}

static uint8_t get_binary_opcode(R8ETokenType type)
{
    switch (type) {
    case R8E_TOK_PLUS:          return R8E_OP_ADD;
    case R8E_TOK_MINUS:         return R8E_OP_SUB;
    case R8E_TOK_STAR:          return R8E_OP_MUL;
    case R8E_TOK_SLASH:         return R8E_OP_DIV;
    case R8E_TOK_PERCENT:       return R8E_OP_MOD;
    case R8E_TOK_STAR_STAR:     return R8E_OP_POW;
    case R8E_TOK_AMP:           return R8E_OP_BITAND;
    case R8E_TOK_PIPE:          return R8E_OP_BITOR;
    case R8E_TOK_CARET:         return R8E_OP_BITXOR;
    case R8E_TOK_SHL:           return R8E_OP_SHL;
    case R8E_TOK_SHR:           return R8E_OP_SHR;
    case R8E_TOK_USHR:          return R8E_OP_USHR;
    case R8E_TOK_EQ:            return R8E_OP_EQ;
    case R8E_TOK_NE:            return R8E_OP_NE;
    case R8E_TOK_SEQ:           return R8E_OP_SEQ;
    case R8E_TOK_SNE:           return R8E_OP_SNE;
    case R8E_TOK_LT:            return R8E_OP_LT;
    case R8E_TOK_GT:            return R8E_OP_GT;
    case R8E_TOK_LE:            return R8E_OP_LE;
    case R8E_TOK_GE:            return R8E_OP_GE;
    case R8E_TOK_KW_INSTANCEOF: return R8E_OP_INSTANCEOF;
    case R8E_TOK_KW_IN:         return R8E_OP_IN;
    default:                    return R8E_OP_NOP;
    }
}

/* Check if a token is a compound assignment operator */
static bool is_compound_assign(R8ETokenType type)
{
    switch (type) {
    case R8E_TOK_PLUS_ASSIGN:
    case R8E_TOK_MINUS_ASSIGN:
    case R8E_TOK_STAR_ASSIGN:
    case R8E_TOK_SLASH_ASSIGN:
    case R8E_TOK_PERCENT_ASSIGN:
    case R8E_TOK_STAR_STAR_ASSIGN:
    case R8E_TOK_SHL_ASSIGN:
    case R8E_TOK_SHR_ASSIGN:
    case R8E_TOK_USHR_ASSIGN:
    case R8E_TOK_AND_ASSIGN:
    case R8E_TOK_OR_ASSIGN:
    case R8E_TOK_XOR_ASSIGN:
    case R8E_TOK_LAND_ASSIGN:
    case R8E_TOK_LOR_ASSIGN:
    case R8E_TOK_NULLISH_ASSIGN:
        return true;
    default:
        return false;
    }
}

/* Get the corresponding binary opcode for a compound assignment */
static uint8_t compound_assign_opcode(R8ETokenType type)
{
    switch (type) {
    case R8E_TOK_PLUS_ASSIGN:       return R8E_OP_ADD;
    case R8E_TOK_MINUS_ASSIGN:      return R8E_OP_SUB;
    case R8E_TOK_STAR_ASSIGN:       return R8E_OP_MUL;
    case R8E_TOK_SLASH_ASSIGN:      return R8E_OP_DIV;
    case R8E_TOK_PERCENT_ASSIGN:    return R8E_OP_MOD;
    case R8E_TOK_STAR_STAR_ASSIGN:  return R8E_OP_POW;
    case R8E_TOK_SHL_ASSIGN:        return R8E_OP_SHL;
    case R8E_TOK_SHR_ASSIGN:        return R8E_OP_SHR;
    case R8E_TOK_USHR_ASSIGN:       return R8E_OP_USHR;
    case R8E_TOK_AND_ASSIGN:        return R8E_OP_BITAND;
    case R8E_TOK_OR_ASSIGN:         return R8E_OP_BITOR;
    case R8E_TOK_XOR_ASSIGN:        return R8E_OP_BITXOR;
    default:                        return R8E_OP_NOP;
    }
}

/* =========================================================================
 * PART 2: Expression Parsing
 * ========================================================================= */

/* --- Emit variable load by name --- */
static void emit_load_var(R8EParser *p, uint32_t atom)
{
    R8EResolveResult res = r8e_scope_resolve_var(p->scope, atom);
    switch (res.kind) {
    case R8E_RESOLVE_LOCAL:
        emit_op_u8(p, R8E_OP_LOAD_LOCAL, res.reg);
        break;
    case R8E_RESOLVE_CLOSURE:
        emit_op_u8(p, R8E_OP_LOAD_CLOSURE, res.capture_idx);
        break;
    case R8E_RESOLVE_GLOBAL:
        emit_op_u32(p, R8E_OP_LOAD_GLOBAL, atom);
        break;
    default:
        emit_op(p, R8E_OP_PUSH_UNDEFINED);
        break;
    }
}

/* --- Emit variable store by name --- */
static void emit_store_var(R8EParser *p, uint32_t atom)
{
    R8EResolveResult res = r8e_scope_resolve_var(p->scope, atom);
    switch (res.kind) {
    case R8E_RESOLVE_LOCAL:
        if (res.flags & R8E_VAR_IS_CONST) {
            r8e_parse_error(p, "assignment to constant variable");
            return;
        }
        r8e_scope_mark_mutated(p->scope, atom);
        emit_op_u8(p, R8E_OP_STORE_LOCAL, res.reg);
        break;
    case R8E_RESOLVE_CLOSURE:
        emit_op_u8(p, R8E_OP_STORE_CLOSURE, res.capture_idx);
        break;
    case R8E_RESOLVE_GLOBAL:
        emit_op_u32(p, R8E_OP_STORE_GLOBAL, atom);
        break;
    default:
        break;
    }
}

/* --- Primary Expression --- */
static void parse_primary_expr(R8EParser *p)
{
    emit_line(p);

    switch (p->cur.type) {
    case R8E_TOK_NUMBER: {
        double num = p->cur.num_val;
        r8e_advance(p);
        /* Use specialized opcodes for common values */
        if (num == 0.0 && !signbit(num)) {
            emit_op(p, R8E_OP_PUSH_ZERO);
        } else if (num == 1.0) {
            emit_op(p, R8E_OP_PUSH_ONE);
        } else {
            int32_t ival = (int32_t)num;
            if ((double)ival == num && !(ival == 0 && signbit(num))) {
                if (ival >= -128 && ival <= 127) {
                    emit_op_u8(p, R8E_OP_PUSH_INT8, (uint8_t)(int8_t)ival);
                } else if (ival >= -32768 && ival <= 32767) {
                    emit_op_u16(p, R8E_OP_PUSH_INT16, (uint16_t)(int16_t)ival);
                } else {
                    emit_op_u32(p, R8E_OP_PUSH_INT32, (uint32_t)ival);
                }
            } else {
                emit_constant(p, r8e_from_double(num));
            }
        }
        break;
    }

    case R8E_TOK_STRING: {
        const char *str = p->cur.str_val.str;
        uint32_t len = p->cur.str_val.len;
        r8e_advance(p);
        /* Try inline short string */
        if (len <= 6) {
            bool all_ascii = true;
            for (uint32_t i = 0; i < len; i++) {
                if ((uint8_t)str[i] > 127) { all_ascii = false; break; }
            }
            if (all_ascii) {
                R8EValue sv = r8e_from_inline_str(str, (int)len);
                if (!R8E_IS_UNDEFINED(sv)) {
                    emit_constant(p, sv);
                    break;
                }
            }
        }
        /* Store as atom in constant pool */
        uint32_t atom = r8e_parser_intern(p, str, len);
        int idx = r8e_bc_add_constant(p->bc, r8e_from_atom(atom));
        if (idx >= 0) {
            emit_op_u16(p, R8E_OP_PUSH_STRING, (uint16_t)idx);
        }
        break;
    }

    case R8E_TOK_KW_TRUE:
        r8e_advance(p);
        emit_op(p, R8E_OP_PUSH_TRUE);
        break;

    case R8E_TOK_KW_FALSE:
        r8e_advance(p);
        emit_op(p, R8E_OP_PUSH_FALSE);
        break;

    case R8E_TOK_KW_NULL:
        r8e_advance(p);
        emit_op(p, R8E_OP_PUSH_NULL);
        break;

    case R8E_TOK_KW_UNDEFINED:
        r8e_advance(p);
        emit_op(p, R8E_OP_PUSH_UNDEFINED);
        break;

    case R8E_TOK_KW_THIS:
        r8e_advance(p);
        emit_op(p, R8E_OP_LOAD_THIS);
        break;

    case R8E_TOK_IDENT: {
        uint32_t atom = r8e_parser_intern_token(p, &p->cur);
        r8e_advance(p);
        /* Check for arrow function: ident => ... */
        if (r8e_check(p, R8E_TOK_ARROW)) {
            /* Single-parameter arrow function */
            r8e_advance(p); /* consume => */
            /* Save current state and compile as arrow */
            R8EBytecodeBuffer *outer_bc = p->bc;
            R8EScope *outer_scope = p->scope;
            uint16_t outer_max = p->max_regs;
            bool outer_in_func = p->in_function;

            int func_idx = r8e_bc_add_function(outer_bc);
            if (func_idx < 0) {
                r8e_parse_error(p, "too many nested functions");
                break;
            }

            R8EBytecodeBuffer *func_bc = (R8EBytecodeBuffer *)malloc(
                sizeof(R8EBytecodeBuffer));
            r8e_bc_init(func_bc);
            outer_bc->functions[func_idx].bc = func_bc;
            outer_bc->functions[func_idx].is_arrow = 1;
            outer_bc->functions[func_idx].param_count = 1;

            p->bc = func_bc;
            p->max_regs = 0;
            p->in_function = true;
            push_scope(p, R8E_SCOPE_IS_FUNCTION);

            /* Define the single parameter */
            define_var(p, atom, R8E_VAR_IS_PARAM);

            if (r8e_check(p, R8E_TOK_LBRACE)) {
                /* Arrow with block body */
                parse_block(p);
                emit_op(p, R8E_OP_RETURN_UNDEFINED);
            } else {
                /* Arrow with expression body */
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_RETURN);
            }

            outer_bc->functions[func_idx].local_count = p->max_regs;
            outer_bc->functions[func_idx].stack_size = p->bc->max_stack;

            pop_scope(p);
            p->bc = outer_bc;
            p->scope = outer_scope;
            p->max_regs = outer_max;
            p->in_function = outer_in_func;

            emit_op_u16(p, R8E_OP_NEW_FUNCTION, (uint16_t)func_idx);
            break;
        }
        emit_load_var(p, atom);
        break;
    }

    case R8E_TOK_LPAREN: {
        r8e_advance(p); /* consume ( */

        /* Could be: grouping (expr), arrow params, or empty arrow () => ... */
        if (r8e_check(p, R8E_TOK_RPAREN)) {
            /* () - must be arrow: () => ... */
            r8e_advance(p);
            if (!r8e_check(p, R8E_TOK_ARROW)) {
                r8e_parse_error(p, "unexpected ')'");
                break;
            }
            parse_arrow_function(p, false);
            break;
        }

        /* Parse as expression; if we see => after ), convert to arrow.
         * For now, simple approach: parse the expression normally. */
        parse_expression(p);
        r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");

        /* Check for arrow after ) */
        if (r8e_check(p, R8E_TOK_ARROW)) {
            /* TODO: full arrow speculation-and-patch.
             * For now, error on complex arrow params. */
            r8e_parse_error(p, "complex arrow parameters not yet supported "
                           "(use function expressions)");
        }
        break;
    }

    case R8E_TOK_LBRACKET:
        parse_array_literal(p);
        break;

    case R8E_TOK_LBRACE:
        parse_object_literal(p);
        break;

    case R8E_TOK_KW_FUNCTION:
        parse_function_decl(p, true, false);
        break;

    case R8E_TOK_KW_ASYNC:
        r8e_advance(p);
        if (r8e_check(p, R8E_TOK_KW_FUNCTION)) {
            parse_function_decl(p, true, true);
        } else if (r8e_check(p, R8E_TOK_IDENT)) {
            /* async arrow: async x => ... */
            /* TODO: implement async arrow */
            r8e_parse_error(p, "async arrow functions not yet supported");
        } else {
            r8e_parse_error(p, "expected 'function' after 'async'");
        }
        break;

    case R8E_TOK_KW_CLASS:
        parse_class_decl(p, true);
        break;

    case R8E_TOK_KW_NEW: {
        r8e_advance(p);
        /* new.target */
        if (r8e_check(p, R8E_TOK_DOT)) {
            r8e_advance(p);
            if (r8e_check(p, R8E_TOK_IDENT) &&
                p->cur.str_val.len == 6 &&
                memcmp(p->cur.str_val.str, "target", 6) == 0) {
                r8e_advance(p);
                emit_op(p, R8E_OP_LOAD_NEW_TARGET);
                break;
            }
            r8e_parse_error(p, "expected 'target' after 'new.'");
            break;
        }
        /* new Constructor(args) */
        parse_member_expr(p);
        int argc = 0;
        if (r8e_check(p, R8E_TOK_LPAREN)) {
            argc = parse_arguments(p);
        }
        emit_op_u8(p, R8E_OP_NEW, (uint8_t)argc);
        break;
    }

    case R8E_TOK_KW_SUPER:
        r8e_advance(p);
        emit_op(p, R8E_OP_LOAD_THIS); /* placeholder: super requires class ctx */
        break;

    case R8E_TOK_TEMPLATE_FULL: {
        /* No interpolation template: `string` */
        const char *str = p->cur.str_val.str;
        uint32_t len = p->cur.str_val.len;
        r8e_advance(p);
        uint32_t atom = r8e_parser_intern(p, str, len);
        int idx = r8e_bc_add_constant(p->bc, r8e_from_atom(atom));
        if (idx >= 0) emit_op_u16(p, R8E_OP_PUSH_STRING, (uint16_t)idx);
        break;
    }

    case R8E_TOK_TEMPLATE_HEAD: {
        /* Template with interpolation: `head${expr}middle${expr}tail` */
        const char *str = p->cur.str_val.str;
        uint32_t len = p->cur.str_val.len;
        r8e_advance(p);
        uint32_t atom = r8e_parser_intern(p, str, len);
        int idx = r8e_bc_add_constant(p->bc, r8e_from_atom(atom));
        if (idx >= 0) emit_op_u16(p, R8E_OP_TEMPLATE_HEAD, (uint16_t)idx);

        /* Parse interpolated expressions */
        for (;;) {
            parse_expression(p);
            emit_op(p, R8E_OP_ADD); /* concat */

            if (r8e_check(p, R8E_TOK_TEMPLATE_TAIL)) {
                str = p->cur.str_val.str;
                len = p->cur.str_val.len;
                r8e_advance(p);
                atom = r8e_parser_intern(p, str, len);
                idx = r8e_bc_add_constant(p->bc, r8e_from_atom(atom));
                if (idx >= 0) {
                    emit_op_u16(p, R8E_OP_PUSH_STRING, (uint16_t)idx);
                    emit_op(p, R8E_OP_ADD);
                }
                break;
            } else if (r8e_check(p, R8E_TOK_TEMPLATE_MIDDLE)) {
                str = p->cur.str_val.str;
                len = p->cur.str_val.len;
                r8e_advance(p);
                atom = r8e_parser_intern(p, str, len);
                idx = r8e_bc_add_constant(p->bc, r8e_from_atom(atom));
                if (idx >= 0) {
                    emit_op_u16(p, R8E_OP_PUSH_STRING, (uint16_t)idx);
                    emit_op(p, R8E_OP_ADD);
                }
            } else {
                r8e_parse_error(p, "unterminated template literal");
                break;
            }
        }
        break;
    }

    default:
        r8e_parse_error(p, "unexpected token in expression");
        r8e_advance(p);
        emit_op(p, R8E_OP_PUSH_UNDEFINED);
        break;
    }
}

/* --- Array Literal --- */
static void parse_array_literal(R8EParser *p)
{
    r8e_advance(p); /* consume [ */
    uint16_t count = 0;

    while (!r8e_check(p, R8E_TOK_RBRACKET) && !r8e_parser_at_end(p)) {
        if (r8e_check(p, R8E_TOK_COMMA)) {
            /* Elision: [,, x] */
            emit_op(p, R8E_OP_PUSH_UNDEFINED);
            count++;
            r8e_advance(p);
            continue;
        }

        if (r8e_check(p, R8E_TOK_ELLIPSIS)) {
            r8e_advance(p);
            parse_assignment_expr(p);
            emit_op(p, R8E_OP_SPREAD);
            count++;
        } else {
            parse_assignment_expr(p);
            count++;
        }

        if (!r8e_check(p, R8E_TOK_RBRACKET)) {
            r8e_consume(p, R8E_TOK_COMMA, "expected ',' or ']'");
        }
    }

    r8e_consume(p, R8E_TOK_RBRACKET, "expected ']'");
    emit_op_u16(p, R8E_OP_NEW_ARRAY, count);
}

/* --- Object Literal --- */
static void parse_object_literal(R8EParser *p)
{
    r8e_advance(p); /* consume { */
    emit_op(p, R8E_OP_NEW_OBJECT);

    while (!r8e_check(p, R8E_TOK_RBRACE) && !r8e_parser_at_end(p)) {
        if (r8e_check(p, R8E_TOK_ELLIPSIS)) {
            /* Spread: { ...obj } */
            r8e_advance(p);
            parse_assignment_expr(p);
            emit_op(p, R8E_OP_SPREAD);
            if (!r8e_check(p, R8E_TOK_RBRACE)) {
                r8e_consume(p, R8E_TOK_COMMA, "expected ',' or '}'");
            }
            continue;
        }

        /* Property name */
        uint32_t key_atom = 0;
        bool is_computed = false;

        if (r8e_check(p, R8E_TOK_LBRACKET)) {
            /* Computed property: { [expr]: val } */
            is_computed = true;
            r8e_advance(p);
            parse_assignment_expr(p);
            r8e_consume(p, R8E_TOK_RBRACKET, "expected ']'");
        } else if (r8e_check(p, R8E_TOK_IDENT) || r8e_check(p, R8E_TOK_STRING) ||
                   r8e_check(p, R8E_TOK_NUMBER) ||
                   (p->cur.type >= R8E_TOK_KW_AS &&
                    p->cur.type <= R8E_TOK_KW_YIELD)) {
            /* Named property (identifiers, strings, numbers, keywords as names) */
            if (r8e_check(p, R8E_TOK_NUMBER)) {
                /* Numeric key: convert to string */
                char buf[32];
                int len = snprintf(buf, sizeof(buf), "%g", p->cur.num_val);
                key_atom = r8e_parser_intern(p, buf, (uint32_t)len);
            } else {
                key_atom = r8e_parser_intern(p, p->cur.str_val.str,
                                              p->cur.str_val.len);
            }
            R8EToken name_tok = p->cur;
            r8e_advance(p);

            /* Shorthand: { x } means { x: x } */
            if (name_tok.type == R8E_TOK_IDENT &&
                !r8e_check(p, R8E_TOK_COLON) &&
                !r8e_check(p, R8E_TOK_LPAREN)) {
                emit_op(p, R8E_OP_DUP); /* dup the object */
                emit_load_var(p, key_atom);
                emit_op_u32(p, R8E_OP_INIT_PROP, key_atom);
                if (!r8e_check(p, R8E_TOK_RBRACE)) {
                    r8e_consume(p, R8E_TOK_COMMA, "expected ',' or '}'");
                }
                continue;
            }

            /* Method shorthand: { foo() {} } */
            if (r8e_check(p, R8E_TOK_LPAREN)) {
                emit_op(p, R8E_OP_DUP);
                parse_function_decl(p, true, false);
                emit_op_u32(p, R8E_OP_INIT_PROP, key_atom);
                if (!r8e_check(p, R8E_TOK_RBRACE)) {
                    r8e_consume(p, R8E_TOK_COMMA, "expected ',' or '}'");
                }
                continue;
            }
        } else {
            r8e_parse_error(p, "expected property name");
            r8e_advance(p);
            continue;
        }

        /* Property: key: value */
        r8e_consume(p, R8E_TOK_COLON, "expected ':'");
        emit_op(p, R8E_OP_DUP); /* dup the object for INIT_PROP */
        parse_assignment_expr(p);

        if (is_computed) {
            emit_op(p, R8E_OP_INIT_ELEM);
        } else {
            emit_op_u32(p, R8E_OP_INIT_PROP, key_atom);
        }

        if (!r8e_check(p, R8E_TOK_RBRACE)) {
            r8e_consume(p, R8E_TOK_COMMA, "expected ',' or '}'");
        }
    }

    r8e_consume(p, R8E_TOK_RBRACE, "expected '}'");
}

/* --- Member Expression: obj.prop, obj[expr], obj?.prop --- */
static void parse_member_expr(R8EParser *p)
{
    parse_primary_expr(p);

    for (;;) {
        if (r8e_check(p, R8E_TOK_DOT)) {
            r8e_advance(p);
            if (p->cur.type != R8E_TOK_IDENT &&
                !(p->cur.type >= R8E_TOK_KW_AS &&
                  p->cur.type <= R8E_TOK_KW_YIELD)) {
                r8e_parse_error(p, "expected property name after '.'");
                break;
            }
            uint32_t atom = r8e_parser_intern(p, p->cur.str_val.str,
                                               p->cur.str_val.len);
            r8e_advance(p);
            emit_op_u32(p, R8E_OP_GET_PROP, atom);
        } else if (r8e_check(p, R8E_TOK_LBRACKET)) {
            r8e_advance(p);
            parse_expression(p);
            r8e_consume(p, R8E_TOK_RBRACKET, "expected ']'");
            emit_op(p, R8E_OP_GET_ELEM);
        } else if (r8e_check(p, R8E_TOK_QUESTION_DOT)) {
            /* Optional chaining: obj?.prop */
            r8e_advance(p);
            uint32_t skip = emit_jump(p, R8E_OP_OPTIONAL_CHAIN);
            if (r8e_check(p, R8E_TOK_IDENT) ||
                (p->cur.type >= R8E_TOK_KW_AS &&
                 p->cur.type <= R8E_TOK_KW_YIELD)) {
                uint32_t atom = r8e_parser_intern(p, p->cur.str_val.str,
                                                   p->cur.str_val.len);
                r8e_advance(p);
                emit_op_u32(p, R8E_OP_GET_PROP, atom);
            } else if (r8e_check(p, R8E_TOK_LBRACKET)) {
                r8e_advance(p);
                parse_expression(p);
                r8e_consume(p, R8E_TOK_RBRACKET, "expected ']'");
                emit_op(p, R8E_OP_GET_ELEM);
            } else if (r8e_check(p, R8E_TOK_LPAREN)) {
                int argc = parse_arguments(p);
                emit_op_u8(p, R8E_OP_CALL, (uint8_t)argc);
            }
            patch_jump(p, skip);
        } else {
            break;
        }
    }
}

/* --- Parse Arguments: (expr, expr, ...) returns argc --- */
static int parse_arguments(R8EParser *p)
{
    r8e_consume(p, R8E_TOK_LPAREN, "expected '('");
    int argc = 0;

    while (!r8e_check(p, R8E_TOK_RPAREN) && !r8e_parser_at_end(p)) {
        if (r8e_check(p, R8E_TOK_ELLIPSIS)) {
            r8e_advance(p);
            parse_assignment_expr(p);
            emit_op(p, R8E_OP_SPREAD);
        } else {
            parse_assignment_expr(p);
        }
        argc++;
        if (!r8e_check(p, R8E_TOK_RPAREN)) {
            r8e_consume(p, R8E_TOK_COMMA, "expected ',' or ')'");
        }
    }

    r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");
    return argc;
}

/* --- Call Expression: func(args), obj.method(args) --- */
static void parse_call_expr(R8EParser *p)
{
    parse_member_expr(p);

    while (r8e_check(p, R8E_TOK_LPAREN) ||
           r8e_check(p, R8E_TOK_DOT) ||
           r8e_check(p, R8E_TOK_LBRACKET) ||
           r8e_check(p, R8E_TOK_QUESTION_DOT) ||
           r8e_check(p, R8E_TOK_TEMPLATE_HEAD) ||
           r8e_check(p, R8E_TOK_TEMPLATE_FULL)) {
        if (r8e_check(p, R8E_TOK_LPAREN)) {
            int argc = parse_arguments(p);
            emit_op_u8(p, R8E_OP_CALL, (uint8_t)argc);
        } else if (r8e_check(p, R8E_TOK_DOT)) {
            r8e_advance(p);
            if (p->cur.type != R8E_TOK_IDENT &&
                !(p->cur.type >= R8E_TOK_KW_AS &&
                  p->cur.type <= R8E_TOK_KW_YIELD)) {
                r8e_parse_error(p, "expected property name");
                break;
            }
            uint32_t atom = r8e_parser_intern(p, p->cur.str_val.str,
                                               p->cur.str_val.len);
            r8e_advance(p);

            /* Method call optimization: obj.method(args) */
            if (r8e_check(p, R8E_TOK_LPAREN)) {
                emit_op_u32(p, R8E_OP_GET_PROP_2, atom);
                int argc = parse_arguments(p);
                emit_op_u8(p, R8E_OP_CALL_METHOD, (uint8_t)argc);
            } else {
                emit_op_u32(p, R8E_OP_GET_PROP, atom);
            }
        } else if (r8e_check(p, R8E_TOK_LBRACKET)) {
            r8e_advance(p);
            parse_expression(p);
            r8e_consume(p, R8E_TOK_RBRACKET, "expected ']'");
            emit_op(p, R8E_OP_GET_ELEM);
        } else if (r8e_check(p, R8E_TOK_QUESTION_DOT)) {
            r8e_advance(p);
            uint32_t skip = emit_jump(p, R8E_OP_OPTIONAL_CHAIN);
            if (r8e_check(p, R8E_TOK_LPAREN)) {
                int argc = parse_arguments(p);
                emit_op_u8(p, R8E_OP_CALL, (uint8_t)argc);
            } else if (r8e_check(p, R8E_TOK_IDENT)) {
                uint32_t atom = r8e_parser_intern_token(p, &p->cur);
                r8e_advance(p);
                emit_op_u32(p, R8E_OP_GET_PROP, atom);
            } else if (r8e_check(p, R8E_TOK_LBRACKET)) {
                r8e_advance(p);
                parse_expression(p);
                r8e_consume(p, R8E_TOK_RBRACKET, "expected ']'");
                emit_op(p, R8E_OP_GET_ELEM);
            }
            patch_jump(p, skip);
        } else {
            /* Tagged template literal */
            break; /* TODO: tagged templates */
        }
    }
}

/* --- Postfix Expression: expr++, expr-- --- */
static void parse_postfix_expr(R8EParser *p)
{
    parse_call_expr(p);

    if (!(p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE)) {
        if (r8e_check(p, R8E_TOK_INC) || r8e_check(p, R8E_TOK_DEC)) {
            bool is_inc = r8e_check(p, R8E_TOK_INC);
            r8e_advance(p);
            /* Need to handle: variable++, obj.prop++, obj[key]++ */
            /* For simplicity, emit INC/DEC on the top-of-stack value.
             * A full implementation would track the LHS target. */
            emit_op(p, R8E_OP_DUP);
            emit_op(p, is_inc ? R8E_OP_INC : R8E_OP_DEC);
            /* The result is new value on top, old value below.
             * We need to store new and keep old. Simplified for now. */
            emit_op(p, R8E_OP_SWAP);
            emit_op(p, R8E_OP_DROP);
        }
    }
}

/* --- Unary Expression: !x, ~x, -x, +x, typeof x, void x, delete x --- */
static void parse_unary_expr(R8EParser *p)
{
    switch (p->cur.type) {
    case R8E_TOK_NOT:
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, R8E_OP_NOT);
        return;

    case R8E_TOK_TILDE:
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, R8E_OP_BITNOT);
        return;

    case R8E_TOK_MINUS:
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, R8E_OP_NEG);
        return;

    case R8E_TOK_PLUS:
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, R8E_OP_POS);
        return;

    case R8E_TOK_KW_TYPEOF:
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, R8E_OP_TYPEOF);
        return;

    case R8E_TOK_KW_VOID:
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, R8E_OP_VOID);
        return;

    case R8E_TOK_KW_DELETE: {
        r8e_advance(p);
        /* delete obj.prop or delete obj[key] */
        uint32_t before = p->bc->length;
        parse_unary_expr(p);
        /* Check if last instruction was GET_PROP (1 opcode + 4 operand = 5 bytes)
         * and patch it to DELETE_PROP */
        if (p->bc->length >= before + 5 &&
            p->bc->code[p->bc->length - 5] == R8E_OP_GET_PROP) {
            p->bc->code[p->bc->length - 5] = R8E_OP_DELETE_PROP;
        } else if (p->bc->length >= before + 1 &&
                   p->bc->code[p->bc->length - 1] == R8E_OP_GET_ELEM) {
            p->bc->code[p->bc->length - 1] = R8E_OP_DELETE_ELEM;
        } else {
            /* delete on non-property: evaluate and discard, push true */
            emit_op(p, R8E_OP_DROP);
            emit_op(p, R8E_OP_PUSH_TRUE);
        }
        return;
    }

    case R8E_TOK_KW_AWAIT:
        if (p->in_async) {
            r8e_advance(p);
            parse_unary_expr(p);
            emit_op(p, R8E_OP_AWAIT);
            return;
        }
        break;

    case R8E_TOK_KW_YIELD:
        if (p->in_generator) {
            r8e_advance(p);
            if (r8e_check(p, R8E_TOK_STAR)) {
                r8e_advance(p);
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_YIELD_STAR);
            } else if (r8e_can_start_expr(p) &&
                       !(p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE)) {
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_YIELD);
            } else {
                emit_op(p, R8E_OP_PUSH_UNDEFINED);
                emit_op(p, R8E_OP_YIELD);
            }
            return;
        }
        break;

    case R8E_TOK_INC:
    case R8E_TOK_DEC: {
        bool is_inc = r8e_check(p, R8E_TOK_INC);
        r8e_advance(p);
        parse_unary_expr(p);
        emit_op(p, is_inc ? R8E_OP_INC : R8E_OP_DEC);
        return;
    }

    default:
        break;
    }

    parse_postfix_expr(p);
}

/* --- Binary Expression (precedence climbing) --- */
static void parse_binary_expr(R8EParser *p, int min_prec)
{
    parse_unary_expr(p);

    for (;;) {
        int prec = get_binary_precedence(p->cur.type);
        if (prec == PREC_NONE || prec < min_prec) break;

        /* Handle 'in' restriction (for-in disambiguation) */
        if (p->cur.type == R8E_TOK_KW_IN && !p->allow_in) break;

        R8ETokenType op_type = p->cur.type;
        r8e_advance(p);

        /* Short-circuit operators.
         * JS semantics: && returns LHS if falsy, else RHS.
         *               || returns LHS if truthy, else RHS. */
        if (op_type == R8E_TOK_LAND) {
            /* DUP LHS, test. If falsy: keep LHS. If truthy: drop, eval RHS. */
            emit_op(p, R8E_OP_DUP);
            uint32_t skip = emit_jump(p, R8E_OP_JUMP_IF_FALSE);
            emit_op(p, R8E_OP_DROP);
            parse_binary_expr(p, prec + 1);
            patch_jump(p, skip);
            continue;
        }
        if (op_type == R8E_TOK_LOR) {
            /* DUP LHS, test. If truthy: keep LHS. If falsy: drop, eval RHS. */
            emit_op(p, R8E_OP_DUP);
            uint32_t skip = emit_jump(p, R8E_OP_JUMP_IF_TRUE);
            emit_op(p, R8E_OP_DROP);
            parse_binary_expr(p, prec + 1);
            patch_jump(p, skip);
            continue;
        }
        if (op_type == R8E_TOK_NULLISH) {
            /* LHS is on stack. DUP it, test for nullish.
             * If nullish: drop LHS, evaluate RHS.
             * If not nullish: skip RHS, keep LHS. */
            emit_op(p, R8E_OP_DUP);
            uint32_t nullish_jump = emit_jump(p, R8E_OP_JUMP_IF_NULLISH);
            /* Not nullish: skip RHS */
            uint32_t end = emit_jump(p, R8E_OP_JUMP);
            /* Nullish: drop old value, evaluate RHS */
            patch_jump(p, nullish_jump);
            emit_op(p, R8E_OP_DROP);
            parse_binary_expr(p, prec + 1);
            patch_jump(p, end);
            continue;
        }

        /* Right-associative: ** */
        int next_prec = (op_type == R8E_TOK_STAR_STAR) ? prec : prec + 1;
        parse_binary_expr(p, next_prec);

        uint8_t opcode = get_binary_opcode(op_type);
        if (opcode != R8E_OP_NOP) {
            emit_op(p, opcode);
        }
    }
}

/* --- Conditional Expression: a ? b : c --- */
static void parse_conditional_expr(R8EParser *p)
{
    parse_binary_expr(p, PREC_NULLISH);

    if (r8e_check(p, R8E_TOK_QUESTION)) {
        r8e_advance(p);
        uint32_t else_jump = emit_jump(p, R8E_OP_JUMP_IF_FALSE);
        parse_assignment_expr(p);
        uint32_t end_jump = emit_jump(p, R8E_OP_JUMP);
        patch_jump(p, else_jump);
        r8e_consume(p, R8E_TOK_COLON, "expected ':' in conditional");
        parse_assignment_expr(p);
        patch_jump(p, end_jump);
    }
}

/* --- Assignment Expression --- */
static void parse_assignment_expr(R8EParser *p)
{
    /* We need to handle: lhs = rhs, lhs op= rhs
     * Strategy: parse LHS as conditional expression.
     * If followed by =, rewrite the last emitted load as a store target. */

    /* Save bytecode position before LHS */
    uint32_t lhs_start = current_offset(p);

    /* Check if LHS is a simple identifier */
    if (r8e_check(p, R8E_TOK_IDENT)) {
        uint32_t atom = r8e_parser_intern_token(p, &p->cur);
        R8EToken id_tok = p->cur;

        /* Peek ahead: if next is = or op=, this is assignment */
        R8EToken peek_tok;
        r8e_lexer_peek(&p->lex, &peek_tok);

        if (peek_tok.type == R8E_TOK_ASSIGN) {
            r8e_advance(p); /* consume ident */
            r8e_advance(p); /* consume = */
            parse_assignment_expr(p);
            emit_op(p, R8E_OP_DUP); /* keep value on stack as expression result */
            emit_store_var(p, atom);
            return;
        }

        if (is_compound_assign(peek_tok.type)) {
            r8e_advance(p); /* consume ident */
            R8ETokenType assign_type = p->cur.type;
            r8e_advance(p); /* consume op= */

            /* Handle &&=, ||=, ??= with short-circuit */
            if (assign_type == R8E_TOK_LAND_ASSIGN) {
                emit_load_var(p, atom);
                uint32_t skip = emit_jump(p, R8E_OP_JUMP_IF_FALSE);
                emit_op(p, R8E_OP_DROP);
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_DUP);
                emit_store_var(p, atom);
                patch_jump(p, skip);
                return;
            }
            if (assign_type == R8E_TOK_LOR_ASSIGN) {
                emit_load_var(p, atom);
                uint32_t skip = emit_jump(p, R8E_OP_JUMP_IF_TRUE);
                emit_op(p, R8E_OP_DROP);
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_DUP);
                emit_store_var(p, atom);
                patch_jump(p, skip);
                return;
            }
            if (assign_type == R8E_TOK_NULLISH_ASSIGN) {
                emit_load_var(p, atom);
                uint32_t skip = emit_jump(p, R8E_OP_JUMP_IF_NULLISH);
                /* Not nullish: keep the original value */
                uint32_t end = emit_jump(p, R8E_OP_JUMP);
                patch_jump(p, skip);
                emit_op(p, R8E_OP_DROP);
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_DUP);
                emit_store_var(p, atom);
                patch_jump(p, end);
                return;
            }

            /* Regular compound: x op= y -> x = x op y */
            emit_load_var(p, atom);
            parse_assignment_expr(p);
            emit_op(p, compound_assign_opcode(assign_type));
            emit_op(p, R8E_OP_DUP);
            emit_store_var(p, atom);
            return;
        }

        /* Check for arrow: ident => ... */
        if (peek_tok.type == R8E_TOK_ARROW) {
            r8e_advance(p); /* consume ident */
            r8e_advance(p); /* consume => */

            /* Compile as single-param arrow function */
            R8EBytecodeBuffer *outer_bc = p->bc;
            R8EScope *outer_scope = p->scope;
            uint16_t outer_max = p->max_regs;
            bool outer_in_func = p->in_function;

            int func_idx = r8e_bc_add_function(outer_bc);
            if (func_idx < 0) {
                r8e_parse_error(p, "too many nested functions");
                return;
            }

            R8EBytecodeBuffer *func_bc = (R8EBytecodeBuffer *)malloc(
                sizeof(R8EBytecodeBuffer));
            r8e_bc_init(func_bc);
            outer_bc->functions[func_idx].bc = func_bc;
            outer_bc->functions[func_idx].is_arrow = 1;
            outer_bc->functions[func_idx].param_count = 1;

            p->bc = func_bc;
            p->max_regs = 0;
            p->in_function = true;
            push_scope(p, R8E_SCOPE_IS_FUNCTION);

            define_var(p, atom, R8E_VAR_IS_PARAM);

            if (r8e_check(p, R8E_TOK_LBRACE)) {
                parse_block(p);
                emit_op(p, R8E_OP_RETURN_UNDEFINED);
            } else {
                parse_assignment_expr(p);
                emit_op(p, R8E_OP_RETURN);
            }

            outer_bc->functions[func_idx].local_count = p->max_regs;
            outer_bc->functions[func_idx].stack_size = p->bc->max_stack;

            pop_scope(p);
            p->bc = outer_bc;
            p->scope = outer_scope;
            p->max_regs = outer_max;
            p->in_function = outer_in_func;

            emit_op_u16(p, R8E_OP_NEW_FUNCTION, (uint16_t)func_idx);
            return;
        }
    }

    /* Not a simple assignment - parse as conditional */
    parse_conditional_expr(p);

    /* Check for assignment to member expression */
    if (r8e_check(p, R8E_TOK_ASSIGN) || is_compound_assign(p->cur.type)) {
        /* TODO: implement assignment to member expressions (obj.prop = x, etc.)
         * This requires rewriting the last emitted GET_PROP/GET_ELEM as SET */
        r8e_parse_error(p, "assignment to complex LHS not yet supported "
                       "(use simple variable assignment)");
    }
}

/* --- Top-level expression (comma operator) --- */
static void parse_expression(R8EParser *p)
{
    parse_assignment_expr(p);

    while (r8e_check(p, R8E_TOK_COMMA)) {
        r8e_advance(p);
        emit_op(p, R8E_OP_DROP);
        parse_assignment_expr(p);
    }
}

/* --- Arrow Function (called when => already consumed or params parsed) --- */
static void parse_arrow_function(R8EParser *p, bool is_async)
{
    /* This handles () => ... (no params case) */
    r8e_advance(p); /* consume => */

    R8EBytecodeBuffer *outer_bc = p->bc;
    R8EScope *outer_scope = p->scope;
    uint16_t outer_max = p->max_regs;
    bool outer_in_func = p->in_function;

    int func_idx = r8e_bc_add_function(outer_bc);
    if (func_idx < 0) {
        r8e_parse_error(p, "too many nested functions");
        return;
    }

    R8EBytecodeBuffer *func_bc = (R8EBytecodeBuffer *)malloc(
        sizeof(R8EBytecodeBuffer));
    r8e_bc_init(func_bc);
    outer_bc->functions[func_idx].bc = func_bc;
    outer_bc->functions[func_idx].is_arrow = 1;
    outer_bc->functions[func_idx].is_async = is_async ? 1 : 0;
    outer_bc->functions[func_idx].param_count = 0;

    p->bc = func_bc;
    p->max_regs = 0;
    p->in_function = true;
    push_scope(p, R8E_SCOPE_IS_FUNCTION);

    if (r8e_check(p, R8E_TOK_LBRACE)) {
        parse_block(p);
        emit_op(p, R8E_OP_RETURN_UNDEFINED);
    } else {
        parse_assignment_expr(p);
        emit_op(p, R8E_OP_RETURN);
    }

    outer_bc->functions[func_idx].local_count = p->max_regs;
    outer_bc->functions[func_idx].stack_size = p->bc->max_stack;

    pop_scope(p);
    p->bc = outer_bc;
    p->scope = outer_scope;
    p->max_regs = outer_max;
    p->in_function = outer_in_func;

    emit_op_u16(p, R8E_OP_NEW_FUNCTION, (uint16_t)func_idx);
}

/* =========================================================================
 * PART 3: Statement Parsing
 * ========================================================================= */

/* --- Block Statement: { stmts } --- */
static void parse_block(R8EParser *p)
{
    r8e_consume(p, R8E_TOK_LBRACE, "expected '{'");
    push_scope(p, R8E_SCOPE_IS_BLOCK);

    while (!r8e_check(p, R8E_TOK_RBRACE) && !r8e_parser_at_end(p)) {
        parse_declaration(p);
        if (p->panic_mode) {
            /* Error recovery: skip tokens until we find a sync point */
            p->panic_mode = false;
            while (!r8e_parser_at_end(p)) {
                if (p->prev.type == R8E_TOK_SEMICOLON) break;
                if (r8e_check(p, R8E_TOK_RBRACE)) break;
                if (r8e_check(p, R8E_TOK_KW_VAR) ||
                    r8e_check(p, R8E_TOK_KW_LET) ||
                    r8e_check(p, R8E_TOK_KW_CONST) ||
                    r8e_check(p, R8E_TOK_KW_FUNCTION) ||
                    r8e_check(p, R8E_TOK_KW_CLASS) ||
                    r8e_check(p, R8E_TOK_KW_IF) ||
                    r8e_check(p, R8E_TOK_KW_FOR) ||
                    r8e_check(p, R8E_TOK_KW_WHILE) ||
                    r8e_check(p, R8E_TOK_KW_RETURN)) break;
                r8e_advance(p);
            }
        }
    }

    pop_scope(p);
    r8e_consume(p, R8E_TOK_RBRACE, "expected '}'");
}

/* --- Variable Declaration: var/let/const name = expr --- */
static void parse_var_declaration(R8EParser *p, R8ETokenType kind)
{
    r8e_advance(p); /* consume var/let/const */

    uint8_t var_flags = 0;
    if (kind == R8E_TOK_KW_CONST) var_flags = R8E_VAR_IS_CONST;
    else if (kind == R8E_TOK_KW_LET) var_flags = R8E_VAR_IS_LET;

    do {
        if (r8e_check(p, R8E_TOK_LBRACE) || r8e_check(p, R8E_TOK_LBRACKET)) {
            /* Destructuring: const { a, b } = obj or const [a, b] = arr */
            /* TODO: implement destructuring declarations */
            r8e_parse_error(p, "destructuring declarations not yet supported");
            return;
        }

        r8e_consume(p, R8E_TOK_IDENT, "expected variable name");
        uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                           p->prev.str_val.len);
        int reg = define_var(p, atom, var_flags);
        if (reg < 0) return;

        if (r8e_match(p, R8E_TOK_ASSIGN)) {
            parse_assignment_expr(p);
            emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        } else if (kind == R8E_TOK_KW_CONST) {
            r8e_parse_error(p, "const declaration must have initializer");
            return;
        } else {
            /* Implicit undefined for let/var */
            emit_op(p, R8E_OP_PUSH_UNDEFINED);
            emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        }
    } while (r8e_match(p, R8E_TOK_COMMA));

    consume_semicolon(p);
}

/* --- If Statement --- */
static void parse_if_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'if' */
    r8e_consume(p, R8E_TOK_LPAREN, "expected '(' after 'if'");
    parse_expression(p);
    r8e_consume(p, R8E_TOK_RPAREN, "expected ')' after condition");

    uint32_t else_jump = emit_jump(p, R8E_OP_JUMP_IF_FALSE);
    parse_statement(p);

    if (r8e_match(p, R8E_TOK_KW_ELSE)) {
        uint32_t end_jump = emit_jump(p, R8E_OP_JUMP);
        patch_jump(p, else_jump);
        parse_statement(p);
        patch_jump(p, end_jump);
    } else {
        patch_jump(p, else_jump);
    }
}

/* --- While Statement --- */
static void parse_while_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'while' */
    r8e_consume(p, R8E_TOK_LPAREN, "expected '(' after 'while'");

    uint32_t loop_start = current_offset(p);

    r8e_labels_push(&p->labels, 0, false);
    R8EBreakLabel *label = r8e_labels_top(&p->labels);
    label->continue_target = loop_start;

    parse_expression(p);
    r8e_consume(p, R8E_TOK_RPAREN, "expected ')' after condition");

    uint32_t exit_jump = emit_jump(p, R8E_OP_JUMP_IF_FALSE);

    bool old_in_loop = p->in_loop;
    p->in_loop = true;
    parse_statement(p);
    p->in_loop = old_in_loop;

    emit_loop(p, loop_start);
    patch_jump(p, exit_jump);

    /* Patch all break jumps */
    label = r8e_labels_pop(&p->labels);
    if (label) {
        for (uint16_t i = 0; i < label->break_count; i++) {
            patch_jump(p, label->break_patches[i]);
        }
    }
}

/* --- Do-While Statement --- */
static void parse_do_while_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'do' */

    uint32_t loop_start = current_offset(p);

    r8e_labels_push(&p->labels, 0, false);
    R8EBreakLabel *label = r8e_labels_top(&p->labels);
    label->continue_target = loop_start;

    bool old_in_loop = p->in_loop;
    p->in_loop = true;
    parse_statement(p);
    p->in_loop = old_in_loop;

    r8e_consume(p, R8E_TOK_KW_WHILE, "expected 'while' after do body");
    r8e_consume(p, R8E_TOK_LPAREN, "expected '(' after 'while'");

    /* Update continue target to point to the condition */
    label->continue_target = current_offset(p);

    parse_expression(p);
    r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");

    /* Jump back if condition is true */
    r8e_bc_emit_op(p->bc, R8E_OP_CHECK_LIMITS);
    r8e_bc_emit_op(p->bc, R8E_OP_JUMP_IF_TRUE);
    int32_t offset = (int32_t)(loop_start - (p->bc->length + 4));
    r8e_bc_emit_i32(p->bc, offset);

    consume_semicolon(p);

    label = r8e_labels_pop(&p->labels);
    if (label) {
        for (uint16_t i = 0; i < label->break_count; i++) {
            patch_jump(p, label->break_patches[i]);
        }
    }
}

/* --- For Statement: for (init; cond; update) body --- */
static void parse_for_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'for' */
    r8e_consume(p, R8E_TOK_LPAREN, "expected '(' after 'for'");

    push_scope(p, R8E_SCOPE_IS_BLOCK | R8E_SCOPE_IS_LOOP);

    /* Initializer */
    if (r8e_check(p, R8E_TOK_SEMICOLON)) {
        r8e_advance(p); /* empty init */
    } else if (r8e_check(p, R8E_TOK_KW_VAR) ||
               r8e_check(p, R8E_TOK_KW_LET) ||
               r8e_check(p, R8E_TOK_KW_CONST)) {
        R8ETokenType kind = p->cur.type;

        /* Check for for-in/for-of */
        r8e_advance(p); /* consume var/let/const */
        r8e_consume(p, R8E_TOK_IDENT, "expected variable name");
        uint32_t iter_atom = r8e_parser_intern(p, p->prev.str_val.str,
                                                p->prev.str_val.len);

        if (r8e_check(p, R8E_TOK_KW_IN)) {
            /* for (var x in obj) */
            r8e_advance(p);
            uint8_t var_flags = 0;
            if (kind == R8E_TOK_KW_CONST) var_flags = R8E_VAR_IS_CONST;
            else if (kind == R8E_TOK_KW_LET) var_flags = R8E_VAR_IS_LET;
            int reg = define_var(p, iter_atom, var_flags);

            parse_expression(p);
            r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");

            emit_op(p, R8E_OP_FOR_IN_INIT);
            uint32_t loop_start = current_offset(p);

            r8e_labels_push(&p->labels, 0, false);
            R8EBreakLabel *label = r8e_labels_top(&p->labels);
            label->continue_target = loop_start;

            uint32_t done_jump = emit_jump(p, R8E_OP_FOR_IN_NEXT);
            if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);

            bool old_in_loop = p->in_loop;
            p->in_loop = true;
            parse_statement(p);
            p->in_loop = old_in_loop;

            emit_loop(p, loop_start);
            patch_jump(p, done_jump);

            label = r8e_labels_pop(&p->labels);
            if (label) {
                for (uint16_t i = 0; i < label->break_count; i++)
                    patch_jump(p, label->break_patches[i]);
            }
            pop_scope(p);
            return;
        }

        if (r8e_check(p, R8E_TOK_KW_OF)) {
            /* for (var x of iterable) */
            r8e_advance(p);
            uint8_t var_flags = 0;
            if (kind == R8E_TOK_KW_CONST) var_flags = R8E_VAR_IS_CONST;
            else if (kind == R8E_TOK_KW_LET) var_flags = R8E_VAR_IS_LET;
            int reg = define_var(p, iter_atom, var_flags);

            parse_expression(p);
            r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");

            emit_op(p, R8E_OP_FOR_OF_INIT);
            uint32_t loop_start = current_offset(p);

            r8e_labels_push(&p->labels, 0, false);
            R8EBreakLabel *label = r8e_labels_top(&p->labels);
            label->continue_target = loop_start;

            uint32_t done_jump = emit_jump(p, R8E_OP_FOR_OF_NEXT);
            if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);

            bool old_in_loop = p->in_loop;
            p->in_loop = true;
            parse_statement(p);
            p->in_loop = old_in_loop;

            emit_loop(p, loop_start);
            patch_jump(p, done_jump);

            label = r8e_labels_pop(&p->labels);
            if (label) {
                for (uint16_t i = 0; i < label->break_count; i++)
                    patch_jump(p, label->break_patches[i]);
            }
            pop_scope(p);
            return;
        }

        /* Standard for loop init: var/let/const x = expr */
        uint8_t var_flags = 0;
        if (kind == R8E_TOK_KW_CONST) var_flags = R8E_VAR_IS_CONST;
        else if (kind == R8E_TOK_KW_LET) var_flags = R8E_VAR_IS_LET;
        int reg = define_var(p, iter_atom, var_flags);

        if (r8e_match(p, R8E_TOK_ASSIGN)) {
            parse_assignment_expr(p);
            if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        } else {
            emit_op(p, R8E_OP_PUSH_UNDEFINED);
            if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        }

        /* Handle additional declarators */
        while (r8e_match(p, R8E_TOK_COMMA)) {
            r8e_consume(p, R8E_TOK_IDENT, "expected variable name");
            uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
            reg = define_var(p, atom, var_flags);
            if (r8e_match(p, R8E_TOK_ASSIGN)) {
                parse_assignment_expr(p);
                if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
            } else {
                emit_op(p, R8E_OP_PUSH_UNDEFINED);
                if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
            }
        }
        r8e_consume(p, R8E_TOK_SEMICOLON, "expected ';' after for init");
    } else {
        /* Expression initializer */
        bool old_allow = p->allow_in;
        p->allow_in = false;
        parse_expression(p);
        p->allow_in = old_allow;
        emit_op(p, R8E_OP_DROP);
        r8e_consume(p, R8E_TOK_SEMICOLON, "expected ';' after for init");
    }

    /* Condition */
    uint32_t loop_start = current_offset(p);
    uint32_t exit_jump = 0;
    bool has_cond = false;

    if (!r8e_check(p, R8E_TOK_SEMICOLON)) {
        parse_expression(p);
        exit_jump = emit_jump(p, R8E_OP_JUMP_IF_FALSE);
        has_cond = true;
    }
    r8e_consume(p, R8E_TOK_SEMICOLON, "expected ';' after for condition");

    /* Update expression: save its bytecode and emit after body */
    uint32_t update_start = current_offset(p);
    uint32_t body_jump = 0;
    bool has_update = false;

    if (!r8e_check(p, R8E_TOK_RPAREN)) {
        /* Jump over update to body first */
        body_jump = emit_jump(p, R8E_OP_JUMP);
        has_update = true;
        update_start = current_offset(p);
        parse_expression(p);
        emit_op(p, R8E_OP_DROP);
        emit_loop(p, loop_start);
    }

    r8e_consume(p, R8E_TOK_RPAREN, "expected ')' after for clauses");

    r8e_labels_push(&p->labels, 0, false);
    R8EBreakLabel *label = r8e_labels_top(&p->labels);
    label->continue_target = has_update ? update_start : loop_start;

    if (has_update) {
        patch_jump(p, body_jump);
    }

    /* Body */
    bool old_in_loop = p->in_loop;
    p->in_loop = true;
    parse_statement(p);
    p->in_loop = old_in_loop;

    if (has_update) {
        emit_loop(p, update_start);
    } else {
        emit_loop(p, loop_start);
    }

    if (has_cond) {
        patch_jump(p, exit_jump);
    }

    label = r8e_labels_pop(&p->labels);
    if (label) {
        for (uint16_t i = 0; i < label->break_count; i++)
            patch_jump(p, label->break_patches[i]);
    }

    pop_scope(p);
}

/* --- Switch Statement --- */
static void parse_switch_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'switch' */
    r8e_consume(p, R8E_TOK_LPAREN, "expected '(' after 'switch'");
    parse_expression(p);
    r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");
    r8e_consume(p, R8E_TOK_LBRACE, "expected '{'");

    r8e_labels_push(&p->labels, 0, true);

    bool old_in_switch = p->in_switch;
    p->in_switch = true;

    /* Collect case jumps */
    uint32_t case_jumps[256];
    int case_count = 0;
    uint32_t default_jump = 0;
    bool has_default = false;

    /* First pass: emit comparisons and conditional jumps */
    /* We use a simple approach: for each case, DUP the switch value,
     * compare, and jump to the body if equal. */

    while (!r8e_check(p, R8E_TOK_RBRACE) && !r8e_parser_at_end(p)) {
        if (r8e_check(p, R8E_TOK_KW_CASE)) {
            r8e_advance(p);
            emit_op(p, R8E_OP_DUP); /* dup switch value */
            parse_expression(p);
            emit_op(p, R8E_OP_SEQ); /* strict equality */
            r8e_consume(p, R8E_TOK_COLON, "expected ':'");
            uint32_t body_jump = emit_jump(p, R8E_OP_JUMP_IF_TRUE);
            if (case_count < 256) case_jumps[case_count++] = body_jump;

            /* Parse case body statements */
            while (!r8e_check(p, R8E_TOK_KW_CASE) &&
                   !r8e_check(p, R8E_TOK_KW_DEFAULT) &&
                   !r8e_check(p, R8E_TOK_RBRACE) &&
                   !r8e_parser_at_end(p)) {
                parse_declaration(p);
            }
        } else if (r8e_check(p, R8E_TOK_KW_DEFAULT)) {
            r8e_advance(p);
            r8e_consume(p, R8E_TOK_COLON, "expected ':'");
            has_default = true;

            while (!r8e_check(p, R8E_TOK_KW_CASE) &&
                   !r8e_check(p, R8E_TOK_KW_DEFAULT) &&
                   !r8e_check(p, R8E_TOK_RBRACE) &&
                   !r8e_parser_at_end(p)) {
                parse_declaration(p);
            }
        } else {
            r8e_parse_error(p, "expected 'case' or 'default'");
            break;
        }
    }

    emit_op(p, R8E_OP_DROP); /* drop switch value */

    p->in_switch = old_in_switch;

    /* Patch all case jumps (simplified: they fall through) */
    for (int i = 0; i < case_count; i++) {
        patch_jump(p, case_jumps[i]);
    }

    R8EBreakLabel *label = r8e_labels_pop(&p->labels);
    if (label) {
        for (uint16_t i = 0; i < label->break_count; i++)
            patch_jump(p, label->break_patches[i]);
    }

    r8e_consume(p, R8E_TOK_RBRACE, "expected '}'");
}

/* --- Try/Catch/Finally --- */
static void parse_try_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'try' */

    uint32_t try_start = emit_jump(p, R8E_OP_TRY_PUSH);

    parse_block(p);
    emit_op(p, R8E_OP_TRY_POP);
    uint32_t skip_catch = emit_jump(p, R8E_OP_JUMP);

    /* Catch */
    patch_jump(p, try_start);
    if (r8e_match(p, R8E_TOK_KW_CATCH)) {
        push_scope(p, R8E_SCOPE_IS_BLOCK);

        if (r8e_match(p, R8E_TOK_LPAREN)) {
            r8e_consume(p, R8E_TOK_IDENT, "expected catch parameter");
            uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
            int reg = define_var(p, atom, R8E_VAR_IS_LET);
            emit_op(p, R8E_OP_CATCH);
            if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
            r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");
        } else {
            /* catch without binding (ES2019+) */
            emit_op(p, R8E_OP_CATCH);
            emit_op(p, R8E_OP_DROP);
        }

        parse_block(p);
        pop_scope(p);
    }

    patch_jump(p, skip_catch);

    /* Finally */
    if (r8e_match(p, R8E_TOK_KW_FINALLY)) {
        emit_op(p, R8E_OP_FINALLY);
        parse_block(p);
    }
}

/* --- Return Statement --- */
static void parse_return_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'return' */

    if (!p->in_function) {
        r8e_parse_error(p, "'return' outside of function");
        return;
    }

    if (r8e_check(p, R8E_TOK_SEMICOLON) || r8e_check(p, R8E_TOK_RBRACE) ||
        r8e_parser_at_end(p) || (p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE)) {
        emit_op(p, R8E_OP_RETURN_UNDEFINED);
    } else {
        parse_expression(p);
        emit_op(p, R8E_OP_RETURN);
    }

    consume_semicolon(p);
}

/* --- Throw Statement --- */
static void parse_throw_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'throw' */

    if (p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE) {
        r8e_parse_error(p, "no newline allowed after 'throw'");
        return;
    }

    parse_expression(p);
    emit_op(p, R8E_OP_THROW);
    consume_semicolon(p);
}

/* --- Break Statement --- */
static void parse_break_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'break' */

    if (!p->in_loop && !p->in_switch) {
        r8e_parse_error(p, "'break' outside of loop or switch");
        return;
    }

    uint32_t label_atom = 0;
    if (r8e_check(p, R8E_TOK_IDENT) &&
        !(p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE)) {
        label_atom = r8e_parser_intern_token(p, &p->cur);
        r8e_advance(p);
    }

    R8EBreakLabel *label = r8e_labels_find_break(&p->labels, label_atom);
    if (!label) {
        r8e_parse_error(p, "invalid break target");
        consume_semicolon(p);
        return;
    }

    uint32_t patch = emit_jump(p, R8E_OP_JUMP);
    r8e_label_add_break(label, patch);
    consume_semicolon(p);
}

/* --- Continue Statement --- */
static void parse_continue_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'continue' */

    if (!p->in_loop) {
        r8e_parse_error(p, "'continue' outside of loop");
        return;
    }

    uint32_t label_atom = 0;
    if (r8e_check(p, R8E_TOK_IDENT) &&
        !(p->cur.flags & R8E_TOKFLAG_NEWLINE_BEFORE)) {
        label_atom = r8e_parser_intern_token(p, &p->cur);
        r8e_advance(p);
    }

    R8EBreakLabel *label = r8e_labels_find_continue(&p->labels, label_atom);
    if (!label) {
        r8e_parse_error(p, "invalid continue target");
        consume_semicolon(p);
        return;
    }

    emit_loop(p, label->continue_target);
    consume_semicolon(p);
}

/* --- Function Declaration/Expression --- */
static void parse_function_decl(R8EParser *p, bool is_expr, bool is_async)
{
    r8e_advance(p); /* consume 'function' */

    bool is_generator = false;
    if (r8e_match(p, R8E_TOK_STAR)) {
        is_generator = true;
    }

    /* Function name (optional for expressions) */
    uint32_t name_atom = 0;
    if (r8e_check(p, R8E_TOK_IDENT)) {
        name_atom = r8e_parser_intern_token(p, &p->cur);
        r8e_advance(p);
    } else if (!is_expr) {
        r8e_parse_error(p, "function declaration requires a name");
        return;
    }

    /* Save outer state */
    R8EBytecodeBuffer *outer_bc = p->bc;
    R8EScope *outer_scope = p->scope;
    uint16_t outer_max = p->max_regs;
    bool outer_in_func = p->in_function;
    bool outer_in_loop = p->in_loop;
    bool outer_in_switch = p->in_switch;
    bool outer_in_async = p->in_async;
    bool outer_in_gen = p->in_generator;

    /* Create new function bytecode buffer */
    int func_idx = r8e_bc_add_function(outer_bc);
    if (func_idx < 0) {
        r8e_parse_error(p, "too many nested functions");
        return;
    }

    R8EBytecodeBuffer *func_bc = (R8EBytecodeBuffer *)malloc(
        sizeof(R8EBytecodeBuffer));
    r8e_bc_init(func_bc);

    struct R8EFuncDesc *fd = &outer_bc->functions[func_idx];
    fd->bc = func_bc;
    fd->name_atom = name_atom;
    fd->is_async = is_async ? 1 : 0;
    fd->is_generator = is_generator ? 1 : 0;
    fd->source_line = (uint16_t)p->prev.line;

    p->bc = func_bc;
    p->max_regs = 0;
    p->in_function = true;
    p->in_loop = false;
    p->in_switch = false;
    p->in_async = is_async;
    p->in_generator = is_generator;

    push_scope(p, R8E_SCOPE_IS_FUNCTION |
                  (p->is_strict ? R8E_SCOPE_IS_STRICT : 0));

    /* Parameters */
    r8e_consume(p, R8E_TOK_LPAREN, "expected '(' for parameters");
    uint16_t param_count = 0;

    while (!r8e_check(p, R8E_TOK_RPAREN) && !r8e_parser_at_end(p)) {
        if (r8e_check(p, R8E_TOK_ELLIPSIS)) {
            /* Rest parameter */
            r8e_advance(p);
            r8e_consume(p, R8E_TOK_IDENT, "expected rest parameter name");
            uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
            int reg = define_var(p, atom, R8E_VAR_IS_PARAM);
            if (reg >= 0) {
                emit_op_u8(p, R8E_OP_LOAD_REST_ARGS, (uint8_t)param_count);
                emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
            }
            param_count++;
            break; /* rest must be last */
        }

        r8e_consume(p, R8E_TOK_IDENT, "expected parameter name");
        uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                           p->prev.str_val.len);
        int reg = define_var(p, atom, R8E_VAR_IS_PARAM);

        if (reg >= 0) {
            emit_op_u8(p, R8E_OP_LOAD_ARG, (uint8_t)param_count);

            /* Default parameter value */
            if (r8e_match(p, R8E_TOK_ASSIGN)) {
                uint32_t skip = emit_jump(p, R8E_OP_JUMP_IF_FALSE);
                /* Value is not undefined, use it */
                emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
                uint32_t end = emit_jump(p, R8E_OP_JUMP);
                patch_jump(p, skip);
                /* Value is undefined, use default */
                emit_op(p, R8E_OP_DROP);
                parse_assignment_expr(p);
                emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
                patch_jump(p, end);
            } else {
                emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
            }
        }

        param_count++;

        if (!r8e_check(p, R8E_TOK_RPAREN)) {
            r8e_consume(p, R8E_TOK_COMMA, "expected ',' or ')'");
        }
    }

    r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");
    fd->param_count = param_count;

    /* Function body */
    parse_block(p);
    emit_op(p, R8E_OP_RETURN_UNDEFINED);

    fd->local_count = p->max_regs;
    fd->stack_size = func_bc->max_stack;

    pop_scope(p);

    /* Restore outer state */
    p->bc = outer_bc;
    p->scope = outer_scope;
    p->max_regs = outer_max;
    p->in_function = outer_in_func;
    p->in_loop = outer_in_loop;
    p->in_switch = outer_in_switch;
    p->in_async = outer_in_async;
    p->in_generator = outer_in_gen;

    /* Emit the function creation opcode */
    emit_op_u16(p, R8E_OP_NEW_FUNCTION, (uint16_t)func_idx);

    /* For declarations (not expressions), bind the function name */
    if (!is_expr && name_atom != 0) {
        int reg = define_var(p, name_atom, 0);
        if (reg >= 0) {
            emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        }
    }
}

/* --- Class Declaration (simplified) --- */
static void parse_class_decl(R8EParser *p, bool is_expr)
{
    r8e_advance(p); /* consume 'class' */

    uint32_t name_atom = 0;
    if (r8e_check(p, R8E_TOK_IDENT)) {
        name_atom = r8e_parser_intern_token(p, &p->cur);
        r8e_advance(p);
    } else if (!is_expr) {
        r8e_parse_error(p, "class declaration requires a name");
        return;
    }

    /* Extends */
    if (r8e_match(p, R8E_TOK_KW_EXTENDS)) {
        parse_assignment_expr(p); /* superclass expression */
    } else {
        emit_op(p, R8E_OP_PUSH_NULL); /* no superclass */
    }

    emit_op_u32(p, R8E_OP_CLASS_CREATE, name_atom);

    r8e_consume(p, R8E_TOK_LBRACE, "expected '{'");

    while (!r8e_check(p, R8E_TOK_RBRACE) && !r8e_parser_at_end(p)) {
        if (r8e_match(p, R8E_TOK_SEMICOLON)) continue;

        bool is_static = false;
        if (r8e_check(p, R8E_TOK_KW_STATIC)) {
            is_static = true;
            r8e_advance(p);
        }

        /* Method name */
        bool is_getter = false, is_setter = false;
        if (r8e_check(p, R8E_TOK_KW_GET)) {
            R8EToken peek;
            r8e_lexer_peek(&p->lex, &peek);
            if (peek.type == R8E_TOK_LPAREN) {
                /* get() is a method named 'get' */
            } else {
                is_getter = true;
                r8e_advance(p);
            }
        } else if (r8e_check(p, R8E_TOK_KW_SET)) {
            R8EToken peek;
            r8e_lexer_peek(&p->lex, &peek);
            if (peek.type == R8E_TOK_LPAREN) {
                /* set() is a method named 'set' */
            } else {
                is_setter = true;
                r8e_advance(p);
            }
        }

        if (r8e_check(p, R8E_TOK_IDENT) || r8e_check(p, R8E_TOK_STRING) ||
            (p->cur.type >= R8E_TOK_KW_AS &&
             p->cur.type <= R8E_TOK_KW_YIELD)) {
            uint32_t method_atom = r8e_parser_intern(p, p->cur.str_val.str,
                                                      p->cur.str_val.len);
            r8e_advance(p);

            emit_op(p, R8E_OP_DUP); /* dup class object */
            parse_function_decl(p, true, false);

            uint8_t mflags = 0;
            if (is_getter) mflags |= 0x01;
            if (is_setter) mflags |= 0x02;

            if (is_static) {
                r8e_bc_emit_op(p->bc, R8E_OP_CLASS_STATIC);
                r8e_bc_emit_u32(p->bc, method_atom);
                r8e_bc_emit(p->bc, mflags);
            } else {
                r8e_bc_emit_op(p->bc, R8E_OP_CLASS_METHOD);
                r8e_bc_emit_u32(p->bc, method_atom);
                r8e_bc_emit(p->bc, mflags);
            }
        } else {
            r8e_parse_error(p, "expected method name in class body");
            r8e_advance(p);
        }
    }

    r8e_consume(p, R8E_TOK_RBRACE, "expected '}'");

    /* Bind name for declarations */
    if (!is_expr && name_atom != 0) {
        int reg = define_var(p, name_atom, R8E_VAR_IS_LET);
        if (reg >= 0) {
            emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        }
    }
}

/* =========================================================================
 * PART 4: Statement Dispatcher, Import/Export, and Public API
 * ========================================================================= */

/* --- Import Statement (simplified) --- */
static void parse_import_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'import' */

    /* import('module') - dynamic import as expression statement */
    if (r8e_check(p, R8E_TOK_LPAREN)) {
        parse_expression(p);
        emit_op(p, R8E_OP_DROP);
        consume_semicolon(p);
        return;
    }

    /* import defaultExport from 'module' */
    /* import { named } from 'module' */
    /* import * as ns from 'module' */

    if (r8e_check(p, R8E_TOK_IDENT)) {
        /* Default import */
        uint32_t atom = r8e_parser_intern_token(p, &p->cur);
        r8e_advance(p);
        int reg = define_var(p, atom, R8E_VAR_IS_CONST);

        if (r8e_match(p, R8E_TOK_COMMA)) {
            /* import default, { named } from '...' */
            /* TODO: named imports after default */
        }

        /* Expect 'from' */
        if (p->cur.type == R8E_TOK_KW_FROM) {
            r8e_advance(p);
        } else {
            r8e_parse_error(p, "expected 'from'");
        }

        r8e_consume(p, R8E_TOK_STRING, "expected module specifier");
        uint32_t mod_atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
        int mod_idx = r8e_bc_add_constant(p->bc, r8e_from_atom(mod_atom));
        if (mod_idx >= 0) {
            emit_op_u16(p, R8E_OP_IMPORT, (uint16_t)mod_idx);
        }
        if (reg >= 0) {
            emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
        }
    } else if (r8e_check(p, R8E_TOK_LBRACE)) {
        /* Named imports: import { a, b as c } from '...' */
        r8e_advance(p);

        while (!r8e_check(p, R8E_TOK_RBRACE) && !r8e_parser_at_end(p)) {
            r8e_consume(p, R8E_TOK_IDENT, "expected import name");
            uint32_t import_name = r8e_parser_intern(p, p->prev.str_val.str,
                                                      p->prev.str_val.len);
            uint32_t local_name = import_name;

            if (p->cur.type == R8E_TOK_KW_AS) {
                r8e_advance(p);
                r8e_consume(p, R8E_TOK_IDENT, "expected local name");
                local_name = r8e_parser_intern(p, p->prev.str_val.str,
                                                p->prev.str_val.len);
            }

            define_var(p, local_name, R8E_VAR_IS_CONST);

            if (!r8e_check(p, R8E_TOK_RBRACE)) {
                r8e_consume(p, R8E_TOK_COMMA, "expected ',' or '}'");
            }
        }
        r8e_consume(p, R8E_TOK_RBRACE, "expected '}'");

        if (p->cur.type == R8E_TOK_KW_FROM) {
            r8e_advance(p);
        } else {
            r8e_parse_error(p, "expected 'from'");
        }

        r8e_consume(p, R8E_TOK_STRING, "expected module specifier");
        uint32_t mod_atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
        int mod_idx = r8e_bc_add_constant(p->bc, r8e_from_atom(mod_atom));
        if (mod_idx >= 0) {
            emit_op_u16(p, R8E_OP_IMPORT, (uint16_t)mod_idx);
        }
        emit_op(p, R8E_OP_DROP);
    } else if (r8e_check(p, R8E_TOK_STAR)) {
        /* Namespace import: import * as ns from '...' */
        r8e_advance(p);
        if (p->cur.type == R8E_TOK_KW_AS) {
            r8e_advance(p);
        } else {
            r8e_parse_error(p, "expected 'as'");
        }
        r8e_consume(p, R8E_TOK_IDENT, "expected namespace name");
        uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                           p->prev.str_val.len);
        int reg = define_var(p, atom, R8E_VAR_IS_CONST);

        if (p->cur.type == R8E_TOK_KW_FROM) {
            r8e_advance(p);
        } else {
            r8e_parse_error(p, "expected 'from'");
        }

        r8e_consume(p, R8E_TOK_STRING, "expected module specifier");
        uint32_t mod_atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
        int mod_idx = r8e_bc_add_constant(p->bc, r8e_from_atom(mod_atom));
        if (mod_idx >= 0) {
            emit_op_u16(p, R8E_OP_IMPORT, (uint16_t)mod_idx);
        }
        if (reg >= 0) emit_op_u8(p, R8E_OP_STORE_LOCAL, (uint8_t)reg);
    } else if (r8e_check(p, R8E_TOK_STRING)) {
        /* Side-effect import: import 'module' */
        uint32_t mod_atom = r8e_parser_intern(p, p->cur.str_val.str,
                                               p->cur.str_val.len);
        r8e_advance(p);
        int mod_idx = r8e_bc_add_constant(p->bc, r8e_from_atom(mod_atom));
        if (mod_idx >= 0) {
            emit_op_u16(p, R8E_OP_IMPORT, (uint16_t)mod_idx);
            emit_op(p, R8E_OP_DROP);
        }
    }

    consume_semicolon(p);
}

/* --- Export Statement (simplified) --- */
static void parse_export_stmt(R8EParser *p)
{
    r8e_advance(p); /* consume 'export' */

    if (r8e_match(p, R8E_TOK_KW_DEFAULT)) {
        /* export default expr */
        parse_assignment_expr(p);
        uint32_t atom = r8e_parser_intern(p, "default", 7);
        emit_op_u32(p, R8E_OP_EXPORT, atom);
        consume_semicolon(p);
        return;
    }

    if (r8e_check(p, R8E_TOK_KW_FUNCTION)) {
        parse_function_decl(p, false, false);
        /* The function is already stored in a local; export it */
        uint32_t atom = p->bc->functions[p->bc->func_count - 1].name_atom;
        emit_load_var(p, atom);
        emit_op_u32(p, R8E_OP_EXPORT, atom);
        return;
    }

    if (r8e_check(p, R8E_TOK_KW_CLASS)) {
        parse_class_decl(p, false);
        return;
    }

    if (r8e_check(p, R8E_TOK_KW_VAR) || r8e_check(p, R8E_TOK_KW_LET) ||
        r8e_check(p, R8E_TOK_KW_CONST)) {
        parse_var_declaration(p, p->cur.type);
        return;
    }

    if (r8e_check(p, R8E_TOK_LBRACE)) {
        /* export { a, b, c } */
        r8e_advance(p);
        while (!r8e_check(p, R8E_TOK_RBRACE) && !r8e_parser_at_end(p)) {
            r8e_consume(p, R8E_TOK_IDENT, "expected export name");
            uint32_t atom = r8e_parser_intern(p, p->prev.str_val.str,
                                               p->prev.str_val.len);
            uint32_t export_name = atom;

            if (p->cur.type == R8E_TOK_KW_AS) {
                r8e_advance(p);
                r8e_consume(p, R8E_TOK_IDENT, "expected export alias");
                export_name = r8e_parser_intern(p, p->prev.str_val.str,
                                                 p->prev.str_val.len);
            }

            emit_load_var(p, atom);
            emit_op_u32(p, R8E_OP_EXPORT, export_name);

            if (!r8e_check(p, R8E_TOK_RBRACE)) {
                r8e_consume(p, R8E_TOK_COMMA, "expected ',' or '}'");
            }
        }
        r8e_consume(p, R8E_TOK_RBRACE, "expected '}'");
        consume_semicolon(p);
        return;
    }

    r8e_parse_error(p, "unexpected token after 'export'");
}

/* --- Statement Dispatcher --- */
static void parse_statement(R8EParser *p)
{
    if (p->had_error && p->panic_mode) return;

    emit_line(p);

    switch (p->cur.type) {
    case R8E_TOK_LBRACE:
        parse_block(p);
        return;

    case R8E_TOK_KW_IF:
        parse_if_stmt(p);
        return;

    case R8E_TOK_KW_WHILE:
        parse_while_stmt(p);
        return;

    case R8E_TOK_KW_DO:
        parse_do_while_stmt(p);
        return;

    case R8E_TOK_KW_FOR:
        parse_for_stmt(p);
        return;

    case R8E_TOK_KW_SWITCH:
        parse_switch_stmt(p);
        return;

    case R8E_TOK_KW_TRY:
        parse_try_stmt(p);
        return;

    case R8E_TOK_KW_RETURN:
        parse_return_stmt(p);
        return;

    case R8E_TOK_KW_THROW:
        parse_throw_stmt(p);
        return;

    case R8E_TOK_KW_BREAK:
        parse_break_stmt(p);
        return;

    case R8E_TOK_KW_CONTINUE:
        parse_continue_stmt(p);
        return;

    case R8E_TOK_KW_DEBUGGER:
        r8e_advance(p);
        emit_op(p, R8E_OP_DEBUGGER);
        consume_semicolon(p);
        return;

    case R8E_TOK_KW_WITH:
        r8e_advance(p);
        if (p->is_strict) {
            r8e_parse_error(p, "'with' not allowed in strict mode");
            return;
        }
        r8e_consume(p, R8E_TOK_LPAREN, "expected '(' after 'with'");
        parse_expression(p);
        r8e_consume(p, R8E_TOK_RPAREN, "expected ')'");
        emit_op(p, R8E_OP_WITH_ENTER);
        parse_statement(p);
        emit_op(p, R8E_OP_WITH_EXIT);
        return;

    case R8E_TOK_SEMICOLON:
        /* Empty statement */
        r8e_advance(p);
        return;

    default:
        break;
    }

    /* Labeled statement: ident : stmt */
    if (r8e_check(p, R8E_TOK_IDENT)) {
        R8EToken peek;
        r8e_lexer_peek(&p->lex, &peek);
        if (peek.type == R8E_TOK_COLON) {
            uint32_t label_atom = r8e_parser_intern_token(p, &p->cur);
            r8e_advance(p); /* consume ident */
            r8e_advance(p); /* consume : */
            r8e_labels_push(&p->labels, label_atom, false);

            if (r8e_check(p, R8E_TOK_KW_FOR) || r8e_check(p, R8E_TOK_KW_WHILE) ||
                r8e_check(p, R8E_TOK_KW_DO)) {
                parse_statement(p);
            } else {
                parse_statement(p);
            }

            R8EBreakLabel *label = r8e_labels_pop(&p->labels);
            if (label) {
                for (uint16_t i = 0; i < label->break_count; i++)
                    patch_jump(p, label->break_patches[i]);
            }
            return;
        }
    }

    /* Expression statement */
    parse_expression(p);
    emit_op(p, R8E_OP_DROP);
    consume_semicolon(p);
}

/* --- Declaration (top-level or in block) --- */
static void parse_declaration(R8EParser *p)
{
    if (p->had_error && p->panic_mode) return;

    switch (p->cur.type) {
    case R8E_TOK_KW_VAR:
    case R8E_TOK_KW_LET:
    case R8E_TOK_KW_CONST:
        parse_var_declaration(p, p->cur.type);
        return;

    case R8E_TOK_KW_FUNCTION:
        parse_function_decl(p, false, false);
        return;

    case R8E_TOK_KW_ASYNC: {
        R8EToken peek;
        r8e_lexer_peek(&p->lex, &peek);
        if (peek.type == R8E_TOK_KW_FUNCTION) {
            r8e_advance(p); /* consume async */
            parse_function_decl(p, false, true);
            return;
        }
        break;
    }

    case R8E_TOK_KW_CLASS:
        parse_class_decl(p, false);
        return;

    case R8E_TOK_KW_IMPORT:
        parse_import_stmt(p);
        return;

    case R8E_TOK_KW_EXPORT:
        parse_export_stmt(p);
        return;

    default:
        break;
    }

    parse_statement(p);
}

/* =========================================================================
 * PART 5: Public API - Parser Entry Point
 * ========================================================================= */

/*
 * Initialize and run the parser on a source string.
 * Returns 0 on success, -1 on error.
 *
 * The compiled bytecode is stored in *out_bc.
 */
int r8e_parse(R8EAtomTable *atoms, const char *source, uint32_t length,
              R8EBytecodeBuffer *out_bc, const char *filename)
{
    (void)filename; /* reserved for error reporting */

    R8EParser parser;
    memset(&parser, 0, sizeof(parser));

    /* Initialize lexer */
    r8e_lexer_init(&parser.lex, source, length);

    /* Initialize parser state */
    parser.atoms = atoms;
    parser.bc = out_bc;
    parser.allow_in = true;
    parser.is_strict = false;
    parser.in_function = false;
    parser.in_loop = false;
    parser.in_switch = false;
    parser.in_async = false;
    parser.in_generator = false;
    parser.func_depth = 0;
    parser.max_regs = 0;

    r8e_scope_pool_init(&parser.scope_pool);
    r8e_labels_init(&parser.labels);

    /* Create the top-level scope (global/script scope) */
    push_scope(&parser, R8E_SCOPE_IS_FUNCTION);

    /* Prime the lexer */
    r8e_advance(&parser);

    /* Check for "use strict" directive */
    if (r8e_check(&parser, R8E_TOK_STRING) &&
        parser.cur.str_val.len == 10 &&
        memcmp(parser.cur.str_val.str, "use strict", 10) == 0) {
        parser.is_strict = true;
        parser.scope->flags |= R8E_SCOPE_IS_STRICT;
        r8e_advance(&parser);
        consume_semicolon(&parser);
    }

    /* Parse all declarations/statements */
    while (!r8e_parser_at_end(&parser) && !parser.had_error) {
        parse_declaration(&parser);

        /* Error recovery */
        if (parser.panic_mode) {
            parser.panic_mode = false;
            while (!r8e_parser_at_end(&parser)) {
                if (parser.prev.type == R8E_TOK_SEMICOLON) break;
                if (r8e_check(&parser, R8E_TOK_KW_VAR) ||
                    r8e_check(&parser, R8E_TOK_KW_LET) ||
                    r8e_check(&parser, R8E_TOK_KW_CONST) ||
                    r8e_check(&parser, R8E_TOK_KW_FUNCTION) ||
                    r8e_check(&parser, R8E_TOK_KW_CLASS) ||
                    r8e_check(&parser, R8E_TOK_KW_IF) ||
                    r8e_check(&parser, R8E_TOK_KW_FOR) ||
                    r8e_check(&parser, R8E_TOK_KW_WHILE) ||
                    r8e_check(&parser, R8E_TOK_KW_RETURN) ||
                    r8e_check(&parser, R8E_TOK_KW_IMPORT) ||
                    r8e_check(&parser, R8E_TOK_KW_EXPORT)) break;
                r8e_advance(&parser);
            }
        }
    }

    /* Emit halt at end of script */
    emit_op(&parser, R8E_OP_HALT);

    /* Store register count */
    out_bc->max_stack = parser.max_regs > out_bc->max_stack
                      ? parser.max_regs : out_bc->max_stack;

    /* Clean up */
    pop_scope(&parser);
    r8e_scope_pool_destroy(&parser.scope_pool);

    return parser.had_error ? -1 : 0;
}

/*
 * Convenience: compile a script, creating a new bytecode buffer.
 * Returns the buffer (caller must call r8e_bc_destroy), or NULL on error.
 */
R8EBytecodeBuffer *r8e_compile_script(R8EAtomTable *atoms,
                                       const char *source, uint32_t length,
                                       const char *filename)
{
    R8EBytecodeBuffer *bc = (R8EBytecodeBuffer *)malloc(sizeof(R8EBytecodeBuffer));
    if (!bc) return NULL;

    if (r8e_bc_init(bc) != 0) {
        free(bc);
        return NULL;
    }

    if (r8e_parse(atoms, source, length, bc, filename) != 0) {
        r8e_bc_destroy(bc);
        free(bc);
        return NULL;
    }

    return bc;
}
