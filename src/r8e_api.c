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
extern R8EValue          r8e_call_function(R8EInterpContext *ctx,
                                            R8EValue func, R8EValue this_val,
                                            const R8EValue *args, int argc);

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
    if (!ctx) return R8E_UNDEFINED;

    /* The function value must be a pointer to a closure or native function */
    if (!R8E_IS_POINTER(func)) {
        ctx->error.has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Create a fresh interpreter context for the call */
    R8EInterpContext *interp = r8e_interp_context_new();
    if (!interp) {
        ctx->error.has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Invoke the function through the interpreter's call mechanism */
    R8EValue result = r8e_call_function(interp, func, this_val, argv, argc);

    /* Propagate exceptions from the interpreter to the public context */
    if (r8e_interp_has_exception(interp)) {
        ctx->error.has_exception = true;
        ctx->error.exception = r8e_interp_get_exception(interp);
    }

    /* Clean up the interpreter context */
    r8e_interp_context_free(interp);

    return result;
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

    /* Handle heap strings (pointer to R8EString or R8EStringInterp) */
    if (R8E_IS_POINTER(v)) {
        void *ptr = r8e_get_pointer(v);
        if (ptr) {
            uint32_t flags = *(uint32_t *)ptr;
            uint8_t kind_interp = (flags >> 5) & 0x7;
            if (kind_interp == 1) {
                /* Interpreter-compatible string: data follows the struct
                 * (ApiStringInterp / R8EStringInterp layout with offset_table
                 * pointer). Cast to get byte_length field at offset 8. */
                uint32_t *words = (uint32_t *)ptr;
                uint32_t byte_len = words[2]; /* byte_length at offset 8 */
                /* Data follows the struct header (5 words + 1 pointer).
                 * struct size: flags(4) + hash(4) + byte_length(4) +
                 * char_length(4) + offset_table(8) = 24 bytes on 64-bit */
                const char *data = (const char *)ptr +
                    sizeof(uint32_t) * 4 + sizeof(void *);
                if (outlen) *outlen = byte_len;
                return data;
            }
            /* r8e_types.h uses R8E_OBJ_IS_STRING = 0x60 in bits [7:5] = 3 */
            if ((flags & 0xE0U) == 0x60U) {
                R8EString *s = (R8EString *)ptr;
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
            return kind == 3 /* closure */ || kind == 5 /* function */
                || kind == 6 /* native function */;
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

/* =========================================================================
 * Object creation and property access
 *
 * These work with the interpreter's object layout (GC header with kind in
 * bits [7:5], tier in bits [1:0]).  Keys are atom-tagged NaN-boxed values:
 *   0xFFFC000000000000 | atom_id
 *
 * We create Tier 1 objects (up to 4 properties) matching OP_NEW_OBJECT.
 * ========================================================================= */

/* GC kind for objects (bits [7:5] = 0) */
#define API_OBJ_GC_KIND_OBJECT  0u
#define API_OBJ_GC_KIND_SHIFT   5

/* Atom-tagged key encoding used by the interpreter */
#define API_ATOM_TAG  0xFFFC000000000000ULL

/* Interpreter-compatible object tier structs (must match r8e_interp.c) */
typedef struct { R8EValue key; R8EValue val; } ApiPropPair;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} ApiObjTier0;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t   count;
    uint8_t   pad[7];
    ApiPropPair props[4];
} ApiObjTier1;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t   count;
    uint8_t   capacity;
    uint8_t   pad[6];
    ApiPropPair *props;
} ApiObjTier2;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint16_t  count;
    uint16_t  capacity;
    uint32_t  pad;
    ApiPropPair *buckets;
} ApiObjTier3;

/* Extern: atom interning */
extern uint32_t r8e_atom_intern_cstr(void *ctx, const char *cstr);

R8EValue r8e_make_object(R8EContext *ctx) {
    (void)ctx;
    /* Allocate a Tier 1 object matching OP_NEW_OBJECT in r8e_interp.c */
    ApiObjTier1 *t1 = (ApiObjTier1 *)calloc(1, sizeof(ApiObjTier1));
    if (!t1) return R8E_UNDEFINED;

    t1->flags = (API_OBJ_GC_KIND_OBJECT << API_OBJ_GC_KIND_SHIFT) | 1u; /* tier 1 */
    t1->proto_id = 1; /* PROTO_OBJECT */
    t1->count = 0;

    return r8e_from_pointer(t1);
}

R8EValue r8e_get_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    (void)ctx;
    if (!name || !R8E_IS_POINTER(obj)) return R8E_UNDEFINED;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return R8E_UNDEFINED;

    return r8e_get_prop_atom(ctx, obj, atom);
}

R8EValue r8e_get_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom) {
    (void)ctx;
    if (!R8E_IS_POINTER(obj)) return R8E_UNDEFINED;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return R8E_UNDEFINED;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return R8E_UNDEFINED;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        if (t0->key0 == key) return t0->val0;
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) return t1->props[i].val;
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) return t2->props[i].val;
            }
        }
    } else if (tier == 3) {
        ApiObjTier3 *t3 = (ApiObjTier3 *)ptr;
        if (t3->buckets && t3->capacity > 0) {
            uint32_t mask = t3->capacity - 1;
            uint32_t idx = atom & mask;
            for (uint16_t probe = 0; probe < t3->capacity; probe++) {
                R8EValue bkey = t3->buckets[idx].key;
                if (bkey == key) return t3->buckets[idx].val;
                if (bkey == 0) break; /* empty slot */
                idx = (idx + 1) & mask;
            }
        }
    }

    return R8E_UNDEFINED;
}

R8EStatus r8e_set_prop(R8EContext *ctx, R8EValue obj, const char *name,
                       R8EValue val) {
    if (!name || !R8E_IS_POINTER(obj)) return R8E_ERROR;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return R8E_ERROR;

    return r8e_set_prop_atom(ctx, obj, atom, val);
}

R8EStatus r8e_set_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom,
                            R8EValue val) {
    (void)ctx;
    if (!R8E_IS_POINTER(obj)) return R8E_ERROR;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return R8E_ERROR;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return R8E_ERROR;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        if (t0->key0 == 0 || t0->key0 == key) {
            t0->key0 = key;
            t0->val0 = val;
            return R8E_OK;
        }
        return R8E_ERROR; /* tier 0 full, no promotion from API */
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        /* Check for existing key */
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) {
                t1->props[i].val = val;
                return R8E_OK;
            }
        }
        /* Add new property */
        if (t1->count < 4) {
            t1->props[t1->count].key = key;
            t1->props[t1->count].val = val;
            t1->count++;
            return R8E_OK;
        }
        return R8E_ERROR; /* tier 1 full */
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) {
                    t2->props[i].val = val;
                    return R8E_OK;
                }
            }
            if (t2->count < t2->capacity) {
                t2->props[t2->count].key = key;
                t2->props[t2->count].val = val;
                t2->count++;
                return R8E_OK;
            }
        }
        return R8E_ERROR;
    }

    return R8E_ERROR;
}

bool r8e_has_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    if (!name || !R8E_IS_POINTER(obj)) return false;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return false;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return false;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return false;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    (void)ctx;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        return t0->key0 == key;
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) return true;
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) return true;
            }
        }
    }

    return false;
}

bool r8e_delete_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    if (!name || !R8E_IS_POINTER(obj)) return false;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return false;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return false;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return false;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    (void)ctx;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        if (t0->key0 == key) {
            t0->key0 = 0;
            t0->val0 = R8E_UNDEFINED;
            return true;
        }
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) {
                /* Shift remaining props down */
                for (uint8_t j = i; j < t1->count - 1; j++) {
                    t1->props[j] = t1->props[j + 1];
                }
                t1->count--;
                t1->props[t1->count].key = 0;
                t1->props[t1->count].val = R8E_UNDEFINED;
                return true;
            }
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) {
                    for (uint8_t j = i; j < t2->count - 1; j++) {
                        t2->props[j] = t2->props[j + 1];
                    }
                    t2->count--;
                    return true;
                }
            }
        }
    }

    return false;
}

/* =========================================================================
 * String creation: r8e_make_string, r8e_make_cstring
 *
 * Short ASCII strings (<=6 chars) are stored inline in the NaN-box.
 * Longer strings are heap-allocated with GC kind STRING (bits [7:5] = 1).
 * ========================================================================= */

/* Interpreter-compatible string struct (must match r8e_interp.c R8EStringInterp) */
typedef struct {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    void    *offset_table;
    /* char data[] follows via (struct + 1) */
} ApiStringInterp;

R8EValue r8e_make_string(R8EContext *ctx, const char *str, size_t len) {
    (void)ctx;
    if (!str) return R8E_UNDEFINED;
    if (len == 0) len = strlen(str);

    /* Try inline encoding for short ASCII strings */
    if (len <= 6) {
        R8EValue v = r8e_from_inline_str(str, (int)len);
        if (v != R8E_UNDEFINED) return v;
    }

    /* Heap-allocate interpreter-compatible string */
    size_t alloc_sz = sizeof(ApiStringInterp) + len + 1;
    ApiStringInterp *s = (ApiStringInterp *)calloc(1, alloc_sz);
    if (!s) return R8E_UNDEFINED;

    s->flags = (1u << API_OBJ_GC_KIND_SHIFT) | 0x01u; /* STRING kind + ASCII flag */
    s->hash = 0;
    s->byte_length = (uint32_t)len;
    s->char_length = (uint32_t)len;
    s->offset_table = NULL;
    char *dst = (char *)(s + 1);
    memcpy(dst, str, len);
    dst[len] = '\0';

    return r8e_from_pointer(s);
}

R8EValue r8e_make_cstring(R8EContext *ctx, const char *str) {
    if (!str) return R8E_UNDEFINED;
    return r8e_make_string(ctx, str, strlen(str));
}

/* =========================================================================
 * Array creation: r8e_make_array
 *
 * Creates interpreter-compatible array with GC kind ARRAY (bits [7:5] = 2).
 * ========================================================================= */

/* Interpreter-compatible array struct (must match r8e_interp.c R8EArrayInterp) */
typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *elements;
    void     *named;
} ApiArrayInterp;

R8EValue r8e_make_array(R8EContext *ctx, uint32_t capacity) {
    (void)ctx;
    ApiArrayInterp *arr = (ApiArrayInterp *)calloc(1, sizeof(ApiArrayInterp));
    if (!arr) return R8E_UNDEFINED;

    arr->flags = (2u << API_OBJ_GC_KIND_SHIFT); /* ARRAY kind */
    arr->proto_id = 2; /* PROTO_ARRAY */
    arr->length = 0;
    arr->named = NULL;

    uint32_t cap = capacity > 0 ? capacity : 4;
    arr->elements = (R8EValue *)calloc(cap, sizeof(R8EValue));
    if (!arr->elements) {
        free(arr);
        return R8E_UNDEFINED;
    }
    arr->capacity = cap;

    /* Initialize elements to undefined */
    for (uint32_t i = 0; i < cap; i++)
        arr->elements[i] = R8E_UNDEFINED;

    return r8e_from_pointer(arr);
}

/* =========================================================================
 * Array element access: r8e_get_element, r8e_set_element, r8e_get_length
 * ========================================================================= */

R8EValue r8e_get_element(R8EContext *ctx, R8EValue arr, uint32_t index) {
    (void)ctx;
    if (!R8E_IS_POINTER(arr)) return R8E_UNDEFINED;

    ApiArrayInterp *a = (ApiArrayInterp *)r8e_get_pointer(arr);
    if (!a) return R8E_UNDEFINED;

    uint8_t kind = (a->flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != 2) return R8E_UNDEFINED; /* not an array */

    if (index >= a->length) return R8E_UNDEFINED;
    return a->elements[index];
}

R8EStatus r8e_set_element(R8EContext *ctx, R8EValue arr, uint32_t index,
                           R8EValue val) {
    (void)ctx;
    if (!R8E_IS_POINTER(arr)) return R8E_ERROR;

    ApiArrayInterp *a = (ApiArrayInterp *)r8e_get_pointer(arr);
    if (!a) return R8E_ERROR;

    uint8_t kind = (a->flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != 2) return R8E_ERROR; /* not an array */

    /* Grow if needed */
    if (index >= a->capacity) {
        uint32_t new_cap = a->capacity;
        while (new_cap <= index) {
            uint32_t next = new_cap * 2;
            if (next < new_cap) { new_cap = index + 1; break; }
            new_cap = next;
        }
        R8EValue *new_elems = (R8EValue *)realloc(
            a->elements, new_cap * sizeof(R8EValue));
        if (!new_elems) return R8E_ERROR_OOM;
        for (uint32_t i = a->capacity; i < new_cap; i++)
            new_elems[i] = R8E_UNDEFINED;
        a->elements = new_elems;
        a->capacity = new_cap;
    }

    a->elements[index] = val;
    if (index >= a->length)
        a->length = index + 1;

    return R8E_OK;
}

int32_t r8e_get_length(R8EContext *ctx, R8EValue v) {
    (void)ctx;

    /* Inline strings */
    if (R8E_IS_INLINE_STR(v))
        return (int32_t)r8e_inline_str_len(v);

    if (!R8E_IS_POINTER(v)) return -1;

    void *ptr = r8e_get_pointer(v);
    if (!ptr) return -1;

    uint32_t flags = *(uint32_t *)ptr;
    uint8_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;

    if (kind == 2) { /* ARRAY */
        ApiArrayInterp *a = (ApiArrayInterp *)ptr;
        return (int32_t)a->length;
    }
    if (kind == 1) { /* STRING */
        ApiStringInterp *s = (ApiStringInterp *)ptr;
        return (int32_t)s->char_length;
    }

    return -1;
}

/* =========================================================================
 * Native function creation: r8e_make_native_func
 *
 * Creates an R8ENativeFunction with GC kind NATIVE_FUNC (bits [7:5] = 6).
 * ========================================================================= */

/* Interpreter-compatible native function struct */
typedef struct {
    uint32_t      flags;
    uint32_t      proto_id;
    R8ENativeFunc callback;
    uint32_t      name_atom;
    int16_t       arity;
    uint8_t       func_flags;
    uint8_t       reserved;
} ApiNativeFuncInterp;

R8EValue r8e_make_native_func(R8EContext *ctx, R8ENativeFunc func,
                               const char *name, int argc) {
    (void)ctx;
    if (!func) return R8E_UNDEFINED;

    ApiNativeFuncInterp *nf = (ApiNativeFuncInterp *)calloc(
        1, sizeof(ApiNativeFuncInterp));
    if (!nf) return R8E_UNDEFINED;

    nf->flags = (6u << API_OBJ_GC_KIND_SHIFT); /* NATIVE_FUNC kind */
    nf->proto_id = 3; /* PROTO_FUNCTION */
    nf->callback = func;
    nf->name_atom = name ? r8e_atom_intern_cstr(NULL, name) : 0;
    nf->arity = (int16_t)argc;
    nf->func_flags = 0;
    nf->reserved = 0;

    return r8e_from_pointer(nf);
}
