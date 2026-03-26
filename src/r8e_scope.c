/*
 * r8e_scope.c - Compile-Time Scope Management
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 5.5 (Scope Tracking) and 5.6 (RC Classification).
 *
 * This module handles:
 *   - Scope stack push/pop for nested blocks and functions
 *   - Variable declaration and resolution (local, closure, global)
 *   - RC classification: OWNED, BORROWED, TEMP (53-61% elision, Section 5.6)
 *   - Capture detection: IS_CAPTURED, IS_MUTATED_AFTER_CAPTURE (Section 9.2)
 *   - Break/continue label tracking
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

#include "../include/r8e_types.h"
#include "../include/r8e_atoms.h"

/* =========================================================================
 * Scope Pool - Avoids malloc/free per scope push/pop
 *
 * Since R8EScope is large (R8E_MAX_LOCALS * sizeof(R8EVarInfo) + overhead),
 * we keep a small freelist of scope objects.
 * ========================================================================= */

#define R8E_SCOPE_POOL_MAX 16

typedef struct R8EScopePool {
    R8EScope *free_list[R8E_SCOPE_POOL_MAX];
    int       count;
} R8EScopePool;

static void r8e_scope_pool_init(R8EScopePool *pool)
{
    pool->count = 0;
    memset(pool->free_list, 0, sizeof(pool->free_list));
}

static void r8e_scope_pool_destroy(R8EScopePool *pool)
{
    for (int i = 0; i < pool->count; i++) {
        free(pool->free_list[i]);
    }
    pool->count = 0;
}

static R8EScope *r8e_scope_pool_alloc(R8EScopePool *pool)
{
    if (pool->count > 0) {
        return pool->free_list[--pool->count];
    }
    return (R8EScope *)malloc(sizeof(R8EScope));
}

static void r8e_scope_pool_release(R8EScopePool *pool, R8EScope *scope)
{
    if (pool->count < R8E_SCOPE_POOL_MAX) {
        pool->free_list[pool->count++] = scope;
    } else {
        free(scope);
    }
}

/* =========================================================================
 * Variable Resolution Result
 * ========================================================================= */

typedef enum {
    R8E_RESOLVE_LOCAL   = 0,   /* found in current function scope */
    R8E_RESOLVE_CLOSURE = 1,   /* found in outer function - must capture */
    R8E_RESOLVE_GLOBAL  = 2,   /* not found - assume global */
    R8E_RESOLVE_ERROR   = 3    /* error (e.g., const before initialization) */
} R8EResolveKind;

typedef struct {
    R8EResolveKind kind;
    uint8_t        reg;           /* register index (for LOCAL) */
    uint8_t        capture_idx;   /* capture index (for CLOSURE) */
    uint8_t        depth;         /* scope depth where found */
    uint8_t        classification;/* R8EVarClass */
    uint8_t        flags;         /* R8EVarInfo flags */
} R8EResolveResult;

/* =========================================================================
 * Scope Push / Pop
 * ========================================================================= */

/*
 * Push a new scope onto the scope stack.
 *
 * For function scopes, local_base resets. For block scopes, local_base
 * continues from the parent scope.
 *
 * Returns the new scope, or NULL on OOM.
 */
static R8EScope *r8e_scope_push(R8EScopePool *pool, R8EScope *parent,
                                uint8_t flags)
{
    R8EScope *scope = r8e_scope_pool_alloc(pool);
    if (!scope) return NULL;

    memset(scope, 0, sizeof(R8EScope));
    scope->parent = parent;
    scope->flags = flags;
    scope->depth = parent ? parent->depth + 1 : 0;

    if (flags & R8E_SCOPE_IS_FUNCTION) {
        /* Function scope: registers start from 0.
         * Parameters will be assigned register indices first. */
        scope->local_base = 0;
        scope->local_count = 0;
    } else {
        /* Block scope: continue from parent's register allocation */
        if (parent) {
            scope->local_base = parent->local_base + parent->local_count;
        } else {
            scope->local_base = 0;
        }
        scope->local_count = 0;
    }

    /* Inherit strict mode from parent */
    if (parent && (parent->flags & R8E_SCOPE_IS_STRICT)) {
        scope->flags |= R8E_SCOPE_IS_STRICT;
    }

    return scope;
}

/*
 * Pop a scope from the scope stack. Handles capture resolution for
 * function scopes.
 *
 * Returns the parent scope.
 */
static R8EScope *r8e_scope_pop(R8EScopePool *pool, R8EScope *scope)
{
    R8EScope *parent = scope->parent;
    r8e_scope_pool_release(pool, scope);
    return parent;
}

/* =========================================================================
 * Variable Definition
 *
 * Define a variable in the current scope. Assigns a register index.
 * Returns the register index, or -1 on error (scope full, duplicate let/const).
 * ========================================================================= */

static int r8e_scope_define_var(R8EScope *scope, uint32_t atom,
                                uint8_t var_flags)
{
    /* Check for max locals */
    if (scope->local_count >= R8E_MAX_LOCALS) {
        return -1; /* too many locals */
    }

    /* For let/const: check for duplicate declaration in same scope */
    if (var_flags & (R8E_VAR_IS_CONST | R8E_VAR_IS_LET)) {
        for (uint16_t i = 0; i < scope->local_count; i++) {
            if (scope->vars[i].atom == atom) {
                return -2; /* duplicate declaration */
            }
        }
    }

    /* For var: check if already declared (allowed, just reuse) */
    if (!(var_flags & (R8E_VAR_IS_CONST | R8E_VAR_IS_LET))) {
        /* var hoists to function scope, so search up to function boundary */
        R8EScope *s = scope;
        while (s) {
            for (uint16_t i = 0; i < s->local_count; i++) {
                if (s->vars[i].atom == atom &&
                    !(s->vars[i].flags & (R8E_VAR_IS_CONST | R8E_VAR_IS_LET))) {
                    /* Already declared as var - reuse register */
                    return s->vars[i].register_idx;
                }
            }
            if (s->flags & R8E_SCOPE_IS_FUNCTION) break;
            s = s->parent;
        }
    }

    uint8_t reg = scope->local_base + scope->local_count;
    R8EVarInfo *var = &scope->vars[scope->local_count];
    var->atom = atom;
    var->register_idx = reg;
    var->classification = R8E_VAR_BORROWED; /* default: may be promoted */
    var->flags = var_flags;
    var->capture_slot = 0;
    scope->local_count++;

    return reg;
}

/* =========================================================================
 * Variable Resolution
 *
 * Resolve a variable name to its binding. Searches from the current scope
 * outward through parents. If found in an outer function scope, marks it
 * as captured.
 * ========================================================================= */

static R8EResolveResult r8e_scope_resolve_var(R8EScope *scope, uint32_t atom)
{
    R8EResolveResult result;
    memset(&result, 0, sizeof(result));

    bool crossed_function = false;
    R8EScope *s = scope;

    while (s) {
        for (uint16_t i = 0; i < s->local_count; i++) {
            if (s->vars[i].atom == atom) {
                if (!crossed_function) {
                    /* Local variable in same function */
                    result.kind = R8E_RESOLVE_LOCAL;
                    result.reg = s->vars[i].register_idx;
                    result.classification = s->vars[i].classification;
                    result.flags = s->vars[i].flags;
                    result.depth = s->depth;
                } else {
                    /* Variable in outer function - needs capture */
                    result.kind = R8E_RESOLVE_CLOSURE;
                    result.reg = s->vars[i].register_idx;
                    result.classification = R8E_VAR_OWNED;
                    result.flags = s->vars[i].flags;
                    result.depth = s->depth;

                    /* Mark as captured in the defining scope */
                    s->vars[i].flags |= R8E_VAR_IS_CAPTURED;

                    /* Assign capture index: find the current function scope
                     * and use its capture_count as the index */
                    R8EScope *func_scope = scope;
                    while (func_scope && !(func_scope->flags & R8E_SCOPE_IS_FUNCTION)) {
                        func_scope = func_scope->parent;
                    }
                    if (func_scope) {
                        /* Check if this variable already has a capture idx
                         * assigned (for repeated accesses to same captured var) */
                        bool already_captured = false;
                        /* Use a simple approach: store capture_idx in the var's
                         * register_idx field is not safe. Instead, bump capture_count
                         * only on first capture. Use a flag bit to detect. */
                        if (!(s->vars[i].flags & 0x40)) { /* 0x40 = already has capture idx */
                            s->vars[i].flags |= 0x40;
                            s->vars[i].capture_slot = func_scope->capture_count;
                            func_scope->capture_count++;
                        }
                        result.capture_idx = s->vars[i].capture_slot;
                    }
                }
                return result;
            }
        }

        if (s->flags & R8E_SCOPE_IS_FUNCTION) {
            crossed_function = true;
        }
        s = s->parent;
    }

    /* Not found - global */
    result.kind = R8E_RESOLVE_GLOBAL;
    result.classification = R8E_VAR_OWNED;
    return result;
}

/* =========================================================================
 * Capture Marking
 *
 * When a variable is found in an outer function scope, mark it as captured.
 * When it is later assigned, mark it as mutated after capture.
 * ========================================================================= */

/* Find a variable by atom in a specific scope */
static R8EVarInfo *r8e_scope_find_var(R8EScope *scope, uint32_t atom)
{
    for (uint16_t i = 0; i < scope->local_count; i++) {
        if (scope->vars[i].atom == atom) {
            return &scope->vars[i];
        }
    }
    return NULL;
}

/* Mark a variable as captured in its defining scope */
static void r8e_scope_mark_captured(R8EScope *scope, uint32_t atom)
{
    R8EScope *s = scope;
    while (s) {
        R8EVarInfo *var = r8e_scope_find_var(s, atom);
        if (var) {
            var->flags |= R8E_VAR_IS_CAPTURED;
            return;
        }
        s = s->parent;
    }
}

/* Mark a variable as mutated (used for assignment tracking).
 * If the variable is already captured, set IS_MUTATED_AFTER_CAPTURE. */
static void r8e_scope_mark_mutated(R8EScope *scope, uint32_t atom)
{
    R8EScope *s = scope;
    while (s) {
        R8EVarInfo *var = r8e_scope_find_var(s, atom);
        if (var) {
            if (var->flags & R8E_VAR_IS_CAPTURED) {
                var->flags |= R8E_VAR_IS_MUTATED_AFTER_CAP;
            }
            /* Promote to OWNED since value is stored */
            var->classification = R8E_VAR_OWNED;
            return;
        }
        if (s->flags & R8E_SCOPE_IS_FUNCTION) break;
        s = s->parent;
    }
}

/* =========================================================================
 * RC Classification (Section 5.6)
 *
 * Classify a value use based on how it flows:
 *   TEMP:     consumed immediately (e.g., expression subresult)
 *   BORROWED: used in same scope, never stored elsewhere
 *   OWNED:    stored into property, captured, or returned
 *
 * This is called during compilation to decide whether to emit
 * RC increment/decrement operations.
 * ========================================================================= */

static R8EVarClass r8e_classify_use(R8EScope *scope, uint32_t atom,
                                     bool is_store, bool is_return,
                                     bool is_captured)
{
    (void)scope;
    (void)atom;

    if (is_return || is_captured) {
        return R8E_VAR_OWNED;  /* value escapes: must RC */
    }
    if (is_store) {
        return R8E_VAR_OWNED;  /* stored to property: must RC */
    }
    return R8E_VAR_BORROWED;   /* local use: skip RC */
}

/* =========================================================================
 * Total Register Count
 *
 * Compute the total number of registers needed for a function,
 * traversing all nested block scopes.
 * ========================================================================= */

static uint16_t r8e_scope_total_regs(const R8EScope *func_scope)
{
    /* The total registers is local_base + local_count of the deepest scope.
     * Since block scopes extend the parent's register space, we need the
     * maximum register index used across all scopes within this function. */
    uint16_t max_reg = func_scope->local_base + func_scope->local_count;
    /* Note: block scopes track this cumulatively, so the parser needs to
     * track the high-water mark of registers during compilation. This
     * function returns the current scope's max. The parser tracks the global
     * maximum across all scopes within a function. */
    return max_reg;
}

/* =========================================================================
 * Scope Info for Debug
 * ========================================================================= */

static void r8e_scope_dump(const R8EScope *scope, int indent)
{
    (void)indent; /* unused for now */

    const char *kind = "block";
    if (scope->flags & R8E_SCOPE_IS_FUNCTION) kind = "function";
    if (scope->flags & R8E_SCOPE_IS_LOOP) kind = "loop";
    if (scope->flags & R8E_SCOPE_IS_SWITCH) kind = "switch";

    fprintf(stderr, "%*sScope(%s) depth=%u base=%u count=%u",
            indent * 2, "", kind, scope->depth,
            scope->local_base, scope->local_count);
    if (scope->flags & R8E_SCOPE_IS_STRICT) fprintf(stderr, " strict");
    if (scope->flags & R8E_SCOPE_HAS_EVAL)  fprintf(stderr, " has_eval");
    fprintf(stderr, "\n");

    for (uint16_t i = 0; i < scope->local_count; i++) {
        const R8EVarInfo *v = &scope->vars[i];
        fprintf(stderr, "%*s  r%u: atom=%u",
                indent * 2, "", v->register_idx, v->atom);
        if (v->flags & R8E_VAR_IS_CONST) fprintf(stderr, " const");
        if (v->flags & R8E_VAR_IS_LET)   fprintf(stderr, " let");
        if (v->flags & R8E_VAR_IS_PARAM) fprintf(stderr, " param");
        if (v->flags & R8E_VAR_IS_CAPTURED) fprintf(stderr, " captured");
        if (v->flags & R8E_VAR_IS_MUTATED_AFTER_CAP) fprintf(stderr, " mut_cap");
        const char *cls = "owned";
        if (v->classification == R8E_VAR_BORROWED) cls = "borrowed";
        if (v->classification == R8E_VAR_TEMP) cls = "temp";
        fprintf(stderr, " [%s]\n", cls);
    }
}

/* =========================================================================
 * Break/Continue Label Stack
 *
 * Tracks break/continue targets for loops and switch statements.
 * Each entry stores the bytecode patch addresses for forward jumps.
 * ========================================================================= */

#define R8E_MAX_BREAK_PATCHES 64

typedef struct R8EBreakLabel {
    uint32_t  atom;             /* label atom (0 = unlabeled) */
    uint32_t  break_patches[R8E_MAX_BREAK_PATCHES];
    uint16_t  break_count;
    uint32_t  continue_target;  /* bytecode offset for continue jump */
    bool      is_switch;        /* true for switch (no continue) */
} R8EBreakLabel;

#define R8E_MAX_LABEL_DEPTH 32

typedef struct R8ELabelStack {
    R8EBreakLabel labels[R8E_MAX_LABEL_DEPTH];
    int           depth;
} R8ELabelStack;

static void r8e_labels_init(R8ELabelStack *ls)
{
    ls->depth = 0;
}

static int r8e_labels_push(R8ELabelStack *ls, uint32_t atom, bool is_switch)
{
    if (ls->depth >= R8E_MAX_LABEL_DEPTH) return -1;

    R8EBreakLabel *label = &ls->labels[ls->depth++];
    label->atom = atom;
    label->break_count = 0;
    label->continue_target = 0;
    label->is_switch = is_switch;
    return 0;
}

static R8EBreakLabel *r8e_labels_top(R8ELabelStack *ls)
{
    if (ls->depth <= 0) return NULL;
    return &ls->labels[ls->depth - 1];
}

static R8EBreakLabel *r8e_labels_pop(R8ELabelStack *ls)
{
    if (ls->depth <= 0) return NULL;
    return &ls->labels[--ls->depth];
}

/* Find the innermost label for break (any loop/switch or a named label) */
static R8EBreakLabel *r8e_labels_find_break(R8ELabelStack *ls, uint32_t atom)
{
    if (atom == 0) {
        /* Unlabeled break: find innermost loop or switch */
        for (int i = ls->depth - 1; i >= 0; i--) {
            return &ls->labels[i];
        }
        return NULL;
    }

    /* Named break */
    for (int i = ls->depth - 1; i >= 0; i--) {
        if (ls->labels[i].atom == atom) {
            return &ls->labels[i];
        }
    }
    return NULL;
}

/* Find the innermost label for continue (loops only, not switch) */
static R8EBreakLabel *r8e_labels_find_continue(R8ELabelStack *ls, uint32_t atom)
{
    if (atom == 0) {
        /* Unlabeled continue: find innermost non-switch */
        for (int i = ls->depth - 1; i >= 0; i--) {
            if (!ls->labels[i].is_switch) {
                return &ls->labels[i];
            }
        }
        return NULL;
    }

    /* Named continue */
    for (int i = ls->depth - 1; i >= 0; i--) {
        if (ls->labels[i].atom == atom && !ls->labels[i].is_switch) {
            return &ls->labels[i];
        }
    }
    return NULL;
}

/* Add a break patch address to a label */
static int r8e_label_add_break(R8EBreakLabel *label, uint32_t patch_addr)
{
    if (label->break_count >= R8E_MAX_BREAK_PATCHES) return -1;
    label->break_patches[label->break_count++] = patch_addr;
    return 0;
}
