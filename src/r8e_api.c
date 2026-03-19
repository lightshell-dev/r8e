/*
 * r8e_api.c - Public API implementation (glue between parser and interpreter)
 *
 * Wires up the full eval pipeline:
 *   JS source -> r8e_compile_script (parser) -> bytecode
 *   bytecode  -> r8e_interpret (interpreter) -> result value
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../include/r8e_types.h"
#include "../include/r8e_api.h"

/* =========================================================================
 * Forward declarations for internal types and functions
 *
 * R8EBytecodeBuffer is defined in r8e_bc.c (included by r8e_parse.c).
 * R8EInterpContext is defined in r8e_interp.c.
 * We replicate the struct layouts here for access to their fields.
 * ========================================================================= */

/* --- Bytecode buffer (must match r8e_bc.c layout exactly) --- */
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
    uint16_t  cur_stack;
} R8EBytecodeBuffer;

/* --- Interpreter context (opaque, from r8e_interp.c) --- */
typedef struct R8EInterpContext R8EInterpContext;

/* --- External functions from r8e_parse.c --- */
extern R8EBytecodeBuffer *r8e_compile_script(R8EAtomTable *atoms,
                                              const char *source,
                                              uint32_t length,
                                              const char *filename);
/* r8e_bc_destroy is static in r8e_bc.c (included by r8e_parse.c) so we
 * cannot call it directly. We'll free bytecode buffer fields manually. */

/* --- External functions from r8e_interp.c --- */
extern R8EInterpContext *r8e_interp_context_new(void);
extern void              r8e_interp_context_free(R8EInterpContext *ctx);
extern void              r8e_interp_set_global(R8EInterpContext *ctx,
                                               R8EValue global);
extern R8EValue          r8e_interpret(R8EInterpContext *ctx,
                                       const uint8_t *bytecode,
                                       uint32_t bytecode_len,
                                       R8EValue *constants,
                                       uint16_t num_constants);
extern R8EValue          r8e_interpret_ex(R8EInterpContext *ctx,
                                           const uint8_t *bytecode,
                                           uint32_t bytecode_len,
                                           R8EValue *constants,
                                           uint16_t num_constants,
                                           uint16_t func_base);
extern bool              r8e_interp_has_exception(const R8EInterpContext *ctx);
extern R8EValue          r8e_interp_get_exception(const R8EInterpContext *ctx);
extern R8EValue          r8e_interp_clear_exception(R8EInterpContext *ctx);

/* --- External functions from r8e_atom.c ---
 * Note: r8e_atom.c has its own R8EContext definition ({ void *arena; }).
 * The functions accept a pointer but don't use it for atom operations
 * (they use a global g_atom_table). We declare them as taking void*. */
extern int r8e_atom_table_init(R8EAtomTable *table);
extern void r8e_atom_table_destroy(R8EAtomTable *table);

/* =========================================================================
 * Context lifecycle
 *
 * r8e_context_new allocates a large block (matching the stub's 4096 bytes)
 * to be compatible with other modules that have their own R8EContext layout.
 * ========================================================================= */

#define R8E_CTX_ALLOC_SIZE 4096

R8EContext *r8e_context_new(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, R8E_CTX_ALLOC_SIZE);
    if (!ctx) return NULL;

    /* Initialize the global atom table (idempotent - safe to call multiple times) */
    r8e_atom_table_init(NULL);

    /* Set defaults */
    ctx->realm_count = 0;
    ctx->current_realm = 0;
    ctx->stack_depth = 0;
    ctx->max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    ctx->epoch_threshold = R8E_DEFAULT_SUSPECT_THRESHOLD;
    ctx->error.has_exception = false;
    ctx->error.exception = R8E_UNDEFINED;

    return ctx;
}

void r8e_context_free(R8EContext *ctx) {
    if (!ctx) return;
    /* Don't destroy the global atom table here - it's shared across all
     * contexts and managed separately. Other tests may still need it. */
    free(ctx);
}

/* =========================================================================
 * Helper: free a bytecode buffer
 *
 * r8e_bc_destroy is static in r8e_bc.c, so we inline the destruction here.
 * Must match the r8e_bc_destroy logic exactly.
 * ========================================================================= */

static void bc_free(R8EBytecodeBuffer *bc) {
    if (!bc) return;

    free(bc->code);
    free(bc->constants);

    /* Free nested function descriptors */
    for (uint16_t i = 0; i < bc->func_count; i++) {
        if (bc->functions[i].bc) {
            bc_free(bc->functions[i].bc);
            free(bc->functions[i].bc);
        }
        free(bc->functions[i].captures);
    }
    free(bc->functions);
    free(bc->line_table);
}

/* =========================================================================
 * Script evaluation: the full pipeline
 * ========================================================================= */

/* Opcode constants needed for bytecode patching */
#define R8E_OP_DROP  0x11
#define R8E_OP_HALT  0xFF
#define R8E_OP_NOP   0xFE

/* GC kind constants matching the interpreter */
#define API_GC_KIND_SHIFT    5
#define API_GC_KIND_CLOSURE  3u
#define API_GC_KIND_FUNCTION 5u

/* Interpreter-compatible function/closure structs (must match r8e_interp.c) */
typedef struct {
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
    uint8_t   is_strict;
    uint8_t   is_arrow;
    uint8_t   is_generator;
    uint8_t   is_async;
    uint16_t  source_line;
} ApiFunctionInterp;

typedef struct {
    uint8_t  kind;
    uint8_t  index;
    uint16_t pad;
} ApiCaptureMetaInterp;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  refcount;
    uint16_t  slot_count;
    uint16_t  reserved;
    R8EValue *slots;
} ApiEnvFrameInterp;

typedef struct {
    uint32_t              flags;
    uint32_t              proto_id;
    ApiFunctionInterp    *func;
    uint8_t               capture_count;
    uint8_t               capture_mode;
    uint16_t              reserved;
    union {
        struct { R8EValue inline_captures[2]; } inl;
        struct {
            R8EValue              *values;
            ApiCaptureMetaInterp  *meta;
            ApiEnvFrameInterp     *env;
        } arr;
        struct { ApiEnvFrameInterp *env_frame; } frm;
    } storage;
} ApiClosureInterp;

/**
 * Build interpreter-compatible closure objects from the parser's
 * function descriptors. Returns a malloc'd array of R8EValue pointers
 * (NaN-boxed closure pointers). Caller must free.
 */
static R8EValue *build_function_closures(R8EBytecodeBuffer *bc,
                                          uint16_t *out_count) {
    *out_count = bc->func_count;
    if (bc->func_count == 0) return NULL;

    R8EValue *closures = (R8EValue *)calloc(bc->func_count, sizeof(R8EValue));
    if (!closures) return NULL;

    for (uint16_t i = 0; i < bc->func_count; i++) {
        struct R8EFuncDesc *fd = &bc->functions[i];

        /* Allocate function object */
        ApiFunctionInterp *func = (ApiFunctionInterp *)calloc(
            1, sizeof(ApiFunctionInterp));
        if (!func) { closures[i] = R8E_UNDEFINED; continue; }

        func->flags = (API_GC_KIND_FUNCTION << API_GC_KIND_SHIFT);
        func->proto_id = 3; /* PROTO_FUNCTION */
        func->param_count = fd->param_count;
        func->local_count = fd->local_count;
        func->capture_count = fd->capture_count;
        func->stack_size = fd->stack_size > 0 ? fd->stack_size : 256;
        func->name_atom = fd->name_atom;
        func->is_strict = fd->is_strict;
        func->is_arrow = fd->is_arrow;
        func->is_generator = fd->is_generator;
        func->is_async = fd->is_async;
        func->source_line = fd->source_line;

        if (fd->bc) {
            func->bytecode = fd->bc->code;
            func->bytecode_len = fd->bc->length;
            func->constants = fd->bc->constants;
            func->const_count = fd->bc->const_count;

            /* Recursively build closures for nested functions */
            if (fd->bc->func_count > 0) {
                uint16_t nested_count;
                R8EValue *nested = build_function_closures(fd->bc,
                                                            &nested_count);
                if (nested) {
                    /* Expand the inner function's constants to include
                     * nested closures. The inner OP_NEW_FUNCTION indices
                     * are relative to the inner function's constants. */
                    uint16_t total = func->const_count + nested_count;
                    R8EValue *expanded = (R8EValue *)calloc(
                        total, sizeof(R8EValue));
                    if (expanded) {
                        if (func->constants && func->const_count > 0)
                            memcpy(expanded, func->constants,
                                   func->const_count * sizeof(R8EValue));
                        memcpy(expanded + func->const_count, nested,
                               nested_count * sizeof(R8EValue));
                        func->constants = expanded;
                        func->const_count = total;
                    }
                    free(nested);
                }
            }
        }

        /* Allocate closure wrapping the function */
        ApiClosureInterp *cl = (ApiClosureInterp *)calloc(
            1, sizeof(ApiClosureInterp));
        if (!cl) { closures[i] = R8E_UNDEFINED; continue; }

        cl->flags = (API_GC_KIND_CLOSURE << API_GC_KIND_SHIFT);
        cl->proto_id = 3; /* PROTO_FUNCTION */
        cl->func = func;
        cl->capture_count = 0;
        cl->capture_mode = 0; /* INLINE */

        closures[i] = r8e_from_pointer(cl);
    }

    return closures;
}

/**
 * Patch the compiled bytecode so that the last DROP before HALT is removed.
 *
 * The parser emits OP_DROP after every expression statement to pop the
 * value off the stack. For r8e_eval(), we want to keep the last
 * expression's value on the stack so OP_HALT can return it.
 *
 * We scan backwards from the HALT looking for the last DROP and NOP it out.
 */
static void patch_eval_bytecode(uint8_t *code, uint32_t length) {
    if (length < 2) return;

    /* Find the HALT (should be last byte) */
    if (code[length - 1] != R8E_OP_HALT) return;

    /* Walk backwards to find the last DROP before HALT */
    for (int32_t i = (int32_t)(length - 2); i >= 0; i--) {
        if (code[i] == R8E_OP_DROP) {
            code[i] = R8E_OP_NOP;
            return;
        }
        /* Stop if we hit another control flow instruction */
        if (code[i] == R8E_OP_HALT) return;
    }
}

R8EValue r8e_eval(R8EContext *ctx, const char *source, size_t len) {
    if (!ctx || !source) return R8E_UNDEFINED;

    if (len == 0) len = strlen(source);

    /* Ensure atom table is initialized */
    r8e_atom_table_init(NULL);

    /* Step 1: Compile source to bytecode */
    R8EBytecodeBuffer *bc = r8e_compile_script(
        NULL, source, (uint32_t)len, NULL);
    if (!bc) {
        ctx->error.has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Step 2: Patch bytecode - remove last DROP so HALT returns the value */
    patch_eval_bytecode(bc->code, bc->length);

    /* Step 3: Build function closures and combined constants array.
     *
     * The parser emits OP_NEW_FUNCTION <func_idx> where func_idx is a
     * 0-based index into bc->functions[]. The interpreter's OP_NEW_FUNCTION
     * reads constants[func_base + func_idx] where func_base is the offset
     * where function closures start in the combined constants array.
     *
     * Layout: [original_constants...] [function_closures...]
     *         ^index 0                ^func_base
     */
    R8EValue *eval_constants = bc->constants;
    uint16_t  eval_const_count = bc->const_count;
    uint16_t  func_base = bc->const_count;
    R8EValue *func_closures = NULL;
    R8EValue *combined_constants = NULL;

    if (bc->func_count > 0) {
        uint16_t func_count = 0;
        func_closures = build_function_closures(bc, &func_count);

        if (func_closures && func_count > 0) {
            uint16_t total = bc->const_count + func_count;
            combined_constants = (R8EValue *)calloc(total, sizeof(R8EValue));
            if (combined_constants) {
                if (bc->constants && bc->const_count > 0)
                    memcpy(combined_constants, bc->constants,
                           bc->const_count * sizeof(R8EValue));
                memcpy(combined_constants + bc->const_count, func_closures,
                       func_count * sizeof(R8EValue));
                eval_constants = combined_constants;
                eval_const_count = total;
            }
        }
    }

    /* Step 4: Create interpreter context and execute */
    R8EInterpContext *interp = r8e_interp_context_new();
    if (!interp) {
        free(combined_constants);
        free(func_closures);
        free(bc);
        return R8E_UNDEFINED;
    }

    R8EValue result = r8e_interpret_ex(
        interp, bc->code, bc->length,
        eval_constants, eval_const_count, func_base);

    /* Step 5: Propagate exceptions */
    if (r8e_interp_has_exception(interp)) {
        ctx->error.has_exception = true;
        ctx->error.exception = r8e_interp_get_exception(interp);
    }

    /* Step 6: Clean up */
    r8e_interp_context_free(interp);
    free(combined_constants);
    free(func_closures);
    /* Note: we don't free bc->code or bc->constants because function
     * closures reference them. This is a deliberate leak until proper
     * GC integration is done. */
    free(bc);

    return result;
}

R8EValue r8e_eval_file(R8EContext *ctx, const char *source, size_t len,
                        const char *filename) {
    (void)filename;
    return r8e_eval(ctx, source, len);
}

R8EValue r8e_compile(R8EContext *ctx, const char *source, size_t len,
                      const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

R8EValue r8e_call(R8EContext *ctx, R8EValue func, R8EValue this_val,
                   int argc, const R8EValue *argv) {
    (void)ctx; (void)func; (void)this_val; (void)argc; (void)argv;
    return R8E_UNDEFINED;
}

/* =========================================================================
 * Value extraction: r8e_get_cstring
 * ========================================================================= */

const char *r8e_get_cstring(R8EValue v, char *buf, size_t *outlen) {
    /* Handle inline short strings */
    if (R8E_IS_INLINE_STR(v)) {
        int len = r8e_inline_str_decode(v, buf);
        if (outlen) *outlen = (size_t)len;
        return buf;
    }

    /* Handle heap strings (pointer to R8EString) */
    if (R8E_IS_POINTER(v)) {
        R8EString *s = (R8EString *)r8e_get_pointer(v);
        if (s) {
            /* Check GC kind bits - in the interpreter, kind is at bits [7:5],
             * and STRING kind = 1.  In r8e_types.h the kind encoding differs
             * (uses bits [2:0] for tier). We check both conventions. */
            uint32_t flags = s->flags;
            /* Interpreter convention: kind in bits [7:5] */
            uint8_t kind_interp = (flags >> 5) & 0x7;
            /* r8e_types.h uses R8E_OBJ_IS_STRING = 0x60 in bits [7:5] = 3 */
            if (kind_interp == 1 /* interp STRING */ ||
                (flags & 0xE0U) == 0x60U /* types.h OBJ_IS_STRING */) {
                if (outlen) *outlen = s->byte_length;
                return s->data;
            }
        }
    }

    /* Not a string */
    if (buf) buf[0] = '\0';
    if (outlen) *outlen = 0;
    return buf ? buf : "";
}

/* =========================================================================
 * Other API functions
 * ========================================================================= */

bool r8e_is_string(R8EValue v) {
    if (R8E_IS_INLINE_STR(v)) return true;
    if (R8E_IS_POINTER(v)) {
        R8EString *s = (R8EString *)r8e_get_pointer(v);
        if (s) {
            uint8_t kind = (s->flags >> 5) & 0x7;
            return kind == 1;
        }
    }
    return false;
}

bool r8e_is_function(R8EValue v) {
    if (R8E_IS_POINTER(v)) {
        uint32_t *flags = (uint32_t *)r8e_get_pointer(v);
        if (flags) {
            uint8_t kind = (*flags >> 5) & 0x7;
            return kind == 3 /* closure */ || kind == 5 /* function */;
        }
    }
    return false;
}

bool r8e_is_array(R8EValue v) {
    if (R8E_IS_POINTER(v)) {
        uint32_t *flags = (uint32_t *)r8e_get_pointer(v);
        if (flags) {
            uint8_t kind = (*flags >> 5) & 0x7;
            return kind == 2; /* R8E_GC_KIND_ARRAY */
        }
    }
    return false;
}

bool r8e_to_bool(R8EValue v) {
    if (v == R8E_FALSE || v == R8E_NULL || v == R8E_UNDEFINED) return false;
    if (v == R8E_TRUE) return true;
    if (R8E_IS_INT32(v)) return r8e_get_int32(v) != 0;
    if (R8E_IS_DOUBLE(v)) {
        double d = r8e_get_double(v);
        return d != 0.0 && d == d; /* false for 0 and NaN */
    }
    if (R8E_IS_INLINE_STR(v)) return r8e_inline_str_len(v) > 0;
    if (R8E_IS_POINTER(v)) return true;
    return false;
}

R8EValue r8e_make_number(double d) {
    if (d == (double)(int32_t)d && (d != 0.0 || (1.0 / d > 0))) {
        return r8e_from_int32((int32_t)d);
    }
    return r8e_from_double(d);
}
