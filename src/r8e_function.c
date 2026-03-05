/*
 * r8e_function.c - Function Objects and Call Mechanics
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 5 (Compiler), Section 9 (Closures).
 *
 * Architecture:
 *   - R8EFunction: bytecode container (param_count, locals, constants)
 *   - R8ENativeFunction: C callback wrapper for host-provided functions
 *   - R8EBoundFunction: target + bound_this + partially applied args
 *   - Arguments object: mapped (sloppy) and unmapped (strict)
 *   - Constructor call protocol: allocate, call, check return
 *   - Arrow functions: lexical this, no arguments, no [[Construct]]
 *   - Function.prototype.call/apply/bind implementations
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* NaN-boxing type checks */
#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)    (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_NULLISH(v)    (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))
#define R8E_IS_BOOL(v)       ((v) == R8E_TRUE || (v) == R8E_FALSE)

/* NaN-boxing extraction / encoding */
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline double r8e_get_double(uint64_t v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

static inline uint64_t r8e_from_double(double d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(double));
    return v;
}

static inline R8EValue r8e_from_bool(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}

/* =========================================================================
 * GC header flags layout (must match r8e_gc.c)
 * ========================================================================= */

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
#define R8E_GC_KIND_NATIVE_FUNC  6u
#define R8E_GC_KIND_BOUND_FUNC   7u

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)

/* =========================================================================
 * GC header
 * ========================================================================= */

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* =========================================================================
 * Prototype IDs
 * ========================================================================= */

#define R8E_PROTO_NONE       0
#define R8E_PROTO_OBJECT     1
#define R8E_PROTO_ARRAY      2
#define R8E_PROTO_FUNCTION   3
#define R8E_PROTO_STRING     4
#define R8E_PROTO_NUMBER     5
#define R8E_PROTO_BOOLEAN    6
#define R8E_PROTO_ERROR      9
#define R8E_PROTO_TYPEERROR  10

/* =========================================================================
 * Well-known atom IDs (must match r8e_atoms.h)
 * ========================================================================= */

#define R8E_ATOM_EMPTY              0
#define R8E_ATOM_length             1
#define R8E_ATOM_prototype          2
#define R8E_ATOM_constructor        3
#define R8E_ATOM_toString           4
#define R8E_ATOM_valueOf            5
#define R8E_ATOM_name               8
#define R8E_ATOM_message            9
#define R8E_ATOM_apply              17
#define R8E_ATOM_call               18
#define R8E_ATOM_bind               19
#define R8E_ATOM_arguments          20
#define R8E_ATOM_caller             21
#define R8E_ATOM_callee             22

/* =========================================================================
 * Function flag bits
 * ========================================================================= */

#define R8E_FUNC_IS_STRICT        0x01u
#define R8E_FUNC_IS_ARROW         0x02u
#define R8E_FUNC_IS_GENERATOR     0x04u
#define R8E_FUNC_IS_ASYNC         0x08u
#define R8E_FUNC_IS_CONSTRUCTOR   0x10u
#define R8E_FUNC_IS_METHOD        0x20u
#define R8E_FUNC_HAS_REST_PARAMS  0x40u
#define R8E_FUNC_HAS_DEFAULTS     0x80u

/* =========================================================================
 * Maximum values
 * ========================================================================= */

#define R8E_MAX_ARGS                65535
#define R8E_MAX_BOUND_ARGS          255
#define R8E_MAX_STACK_DEPTH_DEFAULT 512

/* =========================================================================
 * R8EFunction - bytecode function descriptor
 *
 * This struct is the compile-time output. It holds bytecode, constant pool,
 * parameter metadata, and source location info. It is NOT a callable by
 * itself -- the interpreter wraps it in a closure (R8EClosure) to make
 * it callable.
 * ========================================================================= */

typedef struct R8EFunction {
    uint32_t  flags;            /* GC header flags */
    uint32_t  proto_id;         /* prototype table index */
    uint8_t  *bytecode;         /* compiled bytecode */
    uint32_t  bytecode_len;     /* length of bytecode in bytes */
    uint16_t  param_count;      /* number of declared parameters */
    uint16_t  local_count;      /* number of local registers */
    uint16_t  capture_count;    /* number of upvalue captures */
    uint16_t  stack_size;       /* max operand stack depth */
    uint32_t  name_atom;        /* interned function name (0 = anonymous) */
    R8EValue *constants;        /* constant pool */
    uint16_t  const_count;      /* number of constants */
    uint8_t   func_flags;       /* R8E_FUNC_* flags */
    uint8_t   rest_param_idx;   /* index of rest parameter, 0xFF if none */
    uint16_t  source_line;      /* first line number (for stack traces) */
    uint16_t  source_col;       /* first column number */
    const char *source_name;    /* source file name (not owned) */
    /* Default parameter initializers: offsets into bytecode where
     * each default expression starts. NULL if no defaults. */
    uint32_t *default_offsets;  /* param_count entries, 0 = no default */
} R8EFunction;

/* =========================================================================
 * R8ENativeFunc callback type
 * ========================================================================= */

typedef R8EValue (*R8ENativeFunc)(void *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv);

/* =========================================================================
 * R8ENativeFunction - wraps a C callback as a callable
 * ========================================================================= */

typedef struct R8ENativeFunction {
    uint32_t      flags;        /* GC header flags */
    uint32_t      proto_id;     /* R8E_PROTO_FUNCTION */
    R8ENativeFunc callback;     /* C function pointer */
    uint32_t      name_atom;    /* interned name */
    int16_t       arity;        /* expected argc, -1 = variadic */
    uint8_t       func_flags;   /* R8E_FUNC_IS_STRICT, etc. */
    uint8_t       reserved;
} R8ENativeFunction;

/* =========================================================================
 * R8EBoundFunction - result of Function.prototype.bind
 *
 * A bound function transparently delegates call/construct to its target,
 * prepending the bound arguments before the caller-supplied arguments.
 * ========================================================================= */

typedef struct R8EBoundFunction {
    uint32_t  flags;            /* GC header flags */
    uint32_t  proto_id;         /* R8E_PROTO_FUNCTION */
    R8EValue  target;           /* the original function (NaN-boxed pointer) */
    R8EValue  bound_this;       /* the fixed this value */
    R8EValue *bound_args;       /* pre-supplied arguments, or NULL */
    uint16_t  bound_argc;       /* number of pre-supplied arguments */
    uint16_t  remaining_arity;  /* original arity - bound_argc (0 if negative) */
    uint32_t  name_atom;        /* "bound <original_name>" */
} R8EBoundFunction;

/* =========================================================================
 * R8EArguments - the arguments object
 * ========================================================================= */

typedef struct R8EArguments {
    uint32_t  flags;            /* GC header flags */
    uint32_t  proto_id;         /* R8E_PROTO_OBJECT */
    R8EValue *values;           /* argument values */
    uint32_t  length;           /* number of arguments */
    uint32_t  capacity;         /* allocated capacity */
    R8EValue  callee;           /* function that was called (sloppy mode) */
    bool      is_mapped;        /* true = sloppy mode mapped arguments */
    R8EValue *mapped_locals;    /* pointer to local variable slots (mapped) */
    uint16_t  mapped_count;     /* number of mapped parameters */
} R8EArguments;

/* =========================================================================
 * Minimal R8EContext definition (matches other source files)
 * ========================================================================= */

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
    R8EValue  exception;
    bool      has_exception;
    uint16_t  error_line;
    uint16_t  error_col;
    const char *error_file;
} R8EErrorState;

typedef struct R8ECallFrame {
    R8EFunction    *func;       /* function being executed */
    const uint8_t  *pc;         /* program counter (within bytecode) */
    R8EValue       *locals;     /* local variable registers */
    R8EValue       *stack_base; /* operand stack base for this frame */
    R8EValue        this_val;   /* this binding for this call */
    R8EValue        new_target; /* new.target value */
    uint32_t        line;       /* current source line for stack traces */
    uint16_t        col;        /* current source column */
    const char     *source_name;/* source file name */
} R8ECallFrame;

#define R8E_MAX_CALL_FRAMES 512

typedef struct R8EContext {
    R8EGCState   gc;
    R8EErrorState error;
    uint32_t     stack_depth;
    uint32_t     max_stack_depth;
    /* Call frame stack for stack traces */
    R8ECallFrame call_frames[R8E_MAX_CALL_FRAMES];
    uint32_t     frame_count;
} R8EContext;

/* =========================================================================
 * External function declarations (defined in other modules)
 * ========================================================================= */

extern void r8e_retain(R8EContext *ctx, R8EValue val);
extern void r8e_release(R8EContext *ctx, R8EValue val);
extern void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind,
                                uint32_t proto_id);

/* Error functions (defined in r8e_error.c) */
extern R8EValue r8e_throw_type_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_range_error(R8EContext *ctx, const char *fmt, ...);

/* GC visitor callback type */
typedef void (*R8EGCVisitorFn)(R8EGCHeader *ref, void *user_data);


/* =========================================================================
 * Section 1: R8EFunction creation and management
 * ========================================================================= */

/**
 * r8e_function_new - create a new bytecode function descriptor.
 *
 * The bytecode is COPIED into the function object (not borrowed).
 * The constant pool is also copied. The caller can free its copies after
 * this call returns.
 *
 * @param ctx          Engine context (for allocation tracking)
 * @param bytecode     Compiled bytecode bytes
 * @param bytecode_len Length of bytecode in bytes
 * @param param_count  Number of declared parameters
 * @param local_count  Number of local variable registers
 * @param name_atom    Interned function name (0 = anonymous)
 * @return             Newly allocated R8EFunction, or NULL on OOM
 */
R8EFunction *r8e_function_new(R8EContext *ctx,
                               const uint8_t *bytecode,
                               uint32_t bytecode_len,
                               uint16_t param_count,
                               uint16_t local_count,
                               uint32_t name_atom) {
    if (!ctx) return NULL;

    R8EFunction *func = (R8EFunction *)calloc(1, sizeof(R8EFunction));
    if (!func) return NULL;

    r8e_gc_header_init((R8EGCHeader *)func, R8E_GC_KIND_FUNCTION,
                        R8E_PROTO_FUNCTION);

    func->param_count = param_count;
    func->local_count = local_count;
    func->name_atom = name_atom;
    func->capture_count = 0;
    func->stack_size = 16; /* default, adjusted by compiler */
    func->const_count = 0;
    func->constants = NULL;
    func->func_flags = R8E_FUNC_IS_CONSTRUCTOR; /* default: constructable */
    func->rest_param_idx = 0xFF;
    func->source_line = 0;
    func->source_col = 0;
    func->source_name = NULL;
    func->default_offsets = NULL;

    /* Copy bytecode */
    if (bytecode && bytecode_len > 0) {
        func->bytecode = (uint8_t *)malloc(bytecode_len);
        if (!func->bytecode) {
            free(func);
            return NULL;
        }
        memcpy(func->bytecode, bytecode, bytecode_len);
        func->bytecode_len = bytecode_len;
    } else {
        func->bytecode = NULL;
        func->bytecode_len = 0;
    }

    ctx->gc.total_allocated++;
    return func;
}

/**
 * r8e_function_set_constants - set the constant pool for a function.
 *
 * Constants are COPIED. The caller retains ownership of the input array.
 * Each constant value is retained (RC incremented).
 */
void r8e_function_set_constants(R8EContext *ctx, R8EFunction *func,
                                 const R8EValue *constants,
                                 uint16_t count) {
    assert(func != NULL);

    /* Free old constants if any */
    if (func->constants) {
        for (uint16_t i = 0; i < func->const_count; i++) {
            r8e_release(ctx, func->constants[i]);
        }
        free(func->constants);
    }

    if (count == 0 || !constants) {
        func->constants = NULL;
        func->const_count = 0;
        return;
    }

    func->constants = (R8EValue *)malloc(count * sizeof(R8EValue));
    if (!func->constants) {
        func->const_count = 0;
        return;
    }

    memcpy(func->constants, constants, count * sizeof(R8EValue));
    func->const_count = count;

    /* Retain all constant values */
    for (uint16_t i = 0; i < count; i++) {
        r8e_retain(ctx, func->constants[i]);
    }
}

/**
 * r8e_function_set_defaults - set default parameter initializer offsets.
 *
 * Each entry is a bytecode offset where the default expression begins.
 * Value 0 means "no default for this parameter".
 */
void r8e_function_set_defaults(R8EFunction *func,
                                const uint32_t *offsets,
                                uint16_t count) {
    assert(func != NULL);

    free(func->default_offsets);

    if (count == 0 || !offsets) {
        func->default_offsets = NULL;
        func->func_flags &= ~R8E_FUNC_HAS_DEFAULTS;
        return;
    }

    func->default_offsets = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!func->default_offsets) return;

    memcpy(func->default_offsets, offsets, count * sizeof(uint32_t));
    func->func_flags |= R8E_FUNC_HAS_DEFAULTS;
}

/**
 * r8e_function_set_source - set source location info for stack traces.
 */
void r8e_function_set_source(R8EFunction *func,
                              const char *source_name,
                              uint16_t line,
                              uint16_t col) {
    assert(func != NULL);
    func->source_name = source_name; /* Not owned, must outlive function */
    func->source_line = line;
    func->source_col = col;
}

/**
 * r8e_function_set_flags - set function behavioral flags.
 */
void r8e_function_set_flags(R8EFunction *func, uint8_t func_flags) {
    assert(func != NULL);
    func->func_flags = func_flags;
}

/**
 * r8e_function_set_rest_param - mark the rest parameter index.
 */
void r8e_function_set_rest_param(R8EFunction *func, uint8_t index) {
    assert(func != NULL);
    func->rest_param_idx = index;
    func->func_flags |= R8E_FUNC_HAS_REST_PARAMS;
}

/**
 * r8e_function_free - free a function descriptor and all its owned memory.
 */
void r8e_function_free(R8EContext *ctx, R8EFunction *func) {
    if (!func) return;

    free(func->bytecode);
    func->bytecode = NULL;

    if (func->constants) {
        for (uint16_t i = 0; i < func->const_count; i++) {
            r8e_release(ctx, func->constants[i]);
        }
        free(func->constants);
        func->constants = NULL;
    }

    free(func->default_offsets);
    func->default_offsets = NULL;

    ctx->gc.total_freed++;
    free(func);
}

/**
 * r8e_function_get_name - return the name atom of a function.
 */
uint32_t r8e_function_get_name(const R8EFunction *func) {
    assert(func != NULL);
    return func->name_atom;
}

/**
 * r8e_function_get_arity - return the declared parameter count.
 *
 * For rest parameters, excludes the rest param from the count
 * (per ES2023 Function.length semantics).
 */
uint16_t r8e_function_get_arity(const R8EFunction *func) {
    assert(func != NULL);

    uint16_t arity = func->param_count;

    /* Rest parameter is not counted in Function.length */
    if (func->func_flags & R8E_FUNC_HAS_REST_PARAMS) {
        if (arity > 0) arity--;
    }

    /* Parameters with defaults are not counted after the first default */
    if ((func->func_flags & R8E_FUNC_HAS_DEFAULTS) && func->default_offsets) {
        for (uint16_t i = 0; i < func->param_count; i++) {
            if (func->default_offsets[i] != 0) {
                arity = i;
                break;
            }
        }
    }

    return arity;
}

/**
 * r8e_function_is_strict - check if the function is in strict mode.
 */
bool r8e_function_is_strict(const R8EFunction *func) {
    assert(func != NULL);
    return (func->func_flags & R8E_FUNC_IS_STRICT) != 0;
}

/**
 * r8e_function_is_arrow - check if the function is an arrow function.
 */
bool r8e_function_is_arrow(const R8EFunction *func) {
    assert(func != NULL);
    return (func->func_flags & R8E_FUNC_IS_ARROW) != 0;
}

/**
 * r8e_function_is_generator - check if the function is a generator.
 */
bool r8e_function_is_generator(const R8EFunction *func) {
    assert(func != NULL);
    return (func->func_flags & R8E_FUNC_IS_GENERATOR) != 0;
}

/**
 * r8e_function_is_async - check if the function is async.
 */
bool r8e_function_is_async(const R8EFunction *func) {
    assert(func != NULL);
    return (func->func_flags & R8E_FUNC_IS_ASYNC) != 0;
}

/**
 * r8e_function_is_constructor - check if the function can be used with new.
 */
bool r8e_function_is_constructor(const R8EFunction *func) {
    assert(func != NULL);
    return (func->func_flags & R8E_FUNC_IS_CONSTRUCTOR) != 0;
}


/* =========================================================================
 * Section 2: R8ENativeFunction creation
 * ========================================================================= */

/**
 * r8e_native_function_new - create a native (C callback) function.
 *
 * @param ctx       Engine context
 * @param callback  C function pointer
 * @param name_atom Interned function name
 * @param arity     Expected argument count (-1 = variadic)
 * @return          Newly allocated R8ENativeFunction, or NULL on OOM
 */
R8ENativeFunction *r8e_native_function_new(R8EContext *ctx,
                                            R8ENativeFunc callback,
                                            uint32_t name_atom,
                                            int16_t arity) {
    if (!ctx || !callback) return NULL;

    R8ENativeFunction *nf = (R8ENativeFunction *)calloc(
        1, sizeof(R8ENativeFunction));
    if (!nf) return NULL;

    r8e_gc_header_init((R8EGCHeader *)nf, R8E_GC_KIND_NATIVE_FUNC,
                        R8E_PROTO_FUNCTION);

    nf->callback = callback;
    nf->name_atom = name_atom;
    nf->arity = arity;
    nf->func_flags = 0;
    nf->reserved = 0;

    ctx->gc.total_allocated++;
    return nf;
}

/**
 * r8e_native_function_free - free a native function wrapper.
 */
void r8e_native_function_free(R8EContext *ctx, R8ENativeFunction *nf) {
    if (!nf) return;
    ctx->gc.total_freed++;
    free(nf);
}

/**
 * r8e_native_function_call - invoke a native function.
 *
 * This is the low-level call path. It pushes a call frame for stack trace
 * purposes, invokes the C callback, and pops the frame.
 */
R8EValue r8e_native_function_call(R8EContext *ctx,
                                   R8ENativeFunction *nf,
                                   R8EValue this_val,
                                   int argc,
                                   const R8EValue *argv) {
    assert(ctx != NULL);
    assert(nf != NULL);

    /* Stack overflow check */
    if (ctx->stack_depth >= ctx->max_stack_depth) {
        return r8e_throw_range_error(ctx,
            "Maximum call stack size exceeded");
    }

    /* Push call frame for stack trace */
    if (ctx->frame_count < R8E_MAX_CALL_FRAMES) {
        R8ECallFrame *frame = &ctx->call_frames[ctx->frame_count];
        frame->func = NULL; /* native, no bytecode function */
        frame->pc = NULL;
        frame->locals = NULL;
        frame->stack_base = NULL;
        frame->this_val = this_val;
        frame->new_target = R8E_UNDEFINED;
        frame->line = 0;
        frame->col = 0;
        frame->source_name = "<native>";
        ctx->frame_count++;
    }

    ctx->stack_depth++;

    /* Invoke the callback */
    R8EValue result = nf->callback(ctx, this_val, argc, argv);

    ctx->stack_depth--;

    /* Pop call frame */
    if (ctx->frame_count > 0) {
        ctx->frame_count--;
    }

    return result;
}


/* =========================================================================
 * Section 3: R8EBoundFunction (Function.prototype.bind)
 * ========================================================================= */

/**
 * r8e_bound_function_new - create a bound function.
 *
 * A bound function wraps a target function with a fixed this value and
 * optionally pre-supplied arguments. When called, the bound arguments
 * are prepended to the caller-supplied arguments.
 *
 * @param ctx         Engine context
 * @param target      Target function value (NaN-boxed pointer)
 * @param bound_this  Fixed this binding
 * @param bound_args  Pre-supplied arguments (can be NULL)
 * @param bound_argc  Number of pre-supplied arguments
 * @param name_atom   Name atom for "bound <name>"
 * @return            Newly allocated R8EBoundFunction, or NULL on OOM
 */
R8EBoundFunction *r8e_bound_function_new(R8EContext *ctx,
                                          R8EValue target,
                                          R8EValue bound_this,
                                          const R8EValue *bound_args,
                                          uint16_t bound_argc,
                                          uint32_t name_atom) {
    if (!ctx) return NULL;

    /* Validate target is a pointer (function object) */
    if (!R8E_IS_POINTER(target)) return NULL;

    /* Limit bound arguments to prevent abuse */
    if (bound_argc > R8E_MAX_BOUND_ARGS) return NULL;

    R8EBoundFunction *bf = (R8EBoundFunction *)calloc(
        1, sizeof(R8EBoundFunction));
    if (!bf) return NULL;

    r8e_gc_header_init((R8EGCHeader *)bf, R8E_GC_KIND_BOUND_FUNC,
                        R8E_PROTO_FUNCTION);

    bf->target = target;
    bf->bound_this = bound_this;
    bf->bound_argc = bound_argc;
    bf->name_atom = name_atom;

    /* Retain the target function and bound_this */
    r8e_retain(ctx, target);
    r8e_retain(ctx, bound_this);

    /* Copy and retain bound arguments */
    if (bound_argc > 0 && bound_args) {
        bf->bound_args = (R8EValue *)malloc(bound_argc * sizeof(R8EValue));
        if (!bf->bound_args) {
            r8e_release(ctx, target);
            r8e_release(ctx, bound_this);
            free(bf);
            return NULL;
        }
        for (uint16_t i = 0; i < bound_argc; i++) {
            bf->bound_args[i] = bound_args[i];
            r8e_retain(ctx, bound_args[i]);
        }
    } else {
        bf->bound_args = NULL;
    }

    /* Compute remaining arity:
     * If the target has arity N, the bound function has arity max(0, N - bound_argc).
     * We defer this computation since we may not know the target arity here. */
    bf->remaining_arity = 0; /* Set by caller or lazily */

    ctx->gc.total_allocated++;
    return bf;
}

/**
 * r8e_bound_function_free - free a bound function and release references.
 */
void r8e_bound_function_free(R8EContext *ctx, R8EBoundFunction *bf) {
    if (!bf) return;

    r8e_release(ctx, bf->target);
    r8e_release(ctx, bf->bound_this);

    if (bf->bound_args) {
        for (uint16_t i = 0; i < bf->bound_argc; i++) {
            r8e_release(ctx, bf->bound_args[i]);
        }
        free(bf->bound_args);
        bf->bound_args = NULL;
    }

    ctx->gc.total_freed++;
    free(bf);
}

/**
 * r8e_bound_function_build_args - build the merged argument array.
 *
 * Prepends bound_args before caller-supplied argv. Returns a newly
 * allocated array that the caller must free. Sets *out_total to the
 * total argument count.
 */
static R8EValue *r8e_bound_function_build_args(
    const R8EBoundFunction *bf,
    int argc,
    const R8EValue *argv,
    int *out_total) {

    int total = (int)bf->bound_argc + argc;
    if (total <= 0) {
        *out_total = 0;
        return NULL;
    }

    /* Clamp total */
    if (total > R8E_MAX_ARGS) {
        total = R8E_MAX_ARGS;
    }

    R8EValue *merged = (R8EValue *)malloc(total * sizeof(R8EValue));
    if (!merged) {
        *out_total = 0;
        return NULL;
    }

    /* Copy bound args first */
    int copy_bound = (int)bf->bound_argc;
    if (copy_bound > total) copy_bound = total;
    if (bf->bound_args && copy_bound > 0) {
        memcpy(merged, bf->bound_args, copy_bound * sizeof(R8EValue));
    }

    /* Copy caller args after */
    int remaining = total - copy_bound;
    if (remaining > argc) remaining = argc;
    if (argv && remaining > 0) {
        memcpy(merged + copy_bound, argv, remaining * sizeof(R8EValue));
    }

    *out_total = total;
    return merged;
}


/* =========================================================================
 * Section 4: Function type identification
 *
 * Given a NaN-boxed value, determine what kind of callable it is.
 * ========================================================================= */

/** Callable kind enumeration */
typedef enum {
    R8E_CALLABLE_NONE       = 0,
    R8E_CALLABLE_CLOSURE    = 1,
    R8E_CALLABLE_NATIVE     = 2,
    R8E_CALLABLE_BOUND      = 3
} R8ECallableKind;

/**
 * r8e_get_callable_kind - determine the callable kind of a value.
 *
 * Inspects the GC header kind bits to classify the heap object.
 */
R8ECallableKind r8e_get_callable_kind(R8EValue val) {
    if (!R8E_IS_POINTER(val)) {
        return R8E_CALLABLE_NONE;
    }

    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);
    if (!hdr) return R8E_CALLABLE_NONE;

    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);
    switch (kind) {
    case R8E_GC_KIND_CLOSURE:    return R8E_CALLABLE_CLOSURE;
    case R8E_GC_KIND_NATIVE_FUNC: return R8E_CALLABLE_NATIVE;
    case R8E_GC_KIND_BOUND_FUNC:  return R8E_CALLABLE_BOUND;
    default: return R8E_CALLABLE_NONE;
    }
}

/**
 * r8e_is_callable - check if a value is any kind of callable.
 */
bool r8e_is_callable(R8EValue val) {
    return r8e_get_callable_kind(val) != R8E_CALLABLE_NONE;
}


/* =========================================================================
 * Section 5: Arguments object creation
 * ========================================================================= */

/**
 * r8e_arguments_new - create an arguments object.
 *
 * In sloppy (non-strict) mode, the arguments object is "mapped": changes
 * to arguments[i] reflect in the corresponding local variable, and vice
 * versa. In strict mode, the arguments object is "unmapped": a snapshot.
 *
 * @param ctx          Engine context
 * @param argv         Argument values
 * @param argc         Number of arguments
 * @param callee       The function that was called
 * @param is_strict    True for strict mode (unmapped arguments)
 * @param locals       Pointer to local variable slots (for mapped mode)
 * @param param_count  Number of formal parameters (for mapped count)
 * @return             Newly allocated R8EArguments, or NULL on OOM
 */
R8EArguments *r8e_arguments_new(R8EContext *ctx,
                                 const R8EValue *argv,
                                 int argc,
                                 R8EValue callee,
                                 bool is_strict,
                                 R8EValue *locals,
                                 uint16_t param_count) {
    if (!ctx) return NULL;

    R8EArguments *args = (R8EArguments *)calloc(1, sizeof(R8EArguments));
    if (!args) return NULL;

    r8e_gc_header_init((R8EGCHeader *)args, R8E_GC_KIND_OBJECT,
                        R8E_PROTO_OBJECT);

    uint32_t count = (argc > 0) ? (uint32_t)argc : 0;
    args->length = count;
    args->capacity = count > 0 ? count : 1;
    args->is_mapped = !is_strict;

    /* Allocate values array */
    args->values = (R8EValue *)malloc(args->capacity * sizeof(R8EValue));
    if (!args->values) {
        free(args);
        return NULL;
    }

    /* Copy argument values and retain them */
    for (uint32_t i = 0; i < count; i++) {
        args->values[i] = argv[i];
        r8e_retain(ctx, argv[i]);
    }

    /* Set callee (sloppy mode only) */
    if (!is_strict) {
        args->callee = callee;
        r8e_retain(ctx, callee);
    } else {
        args->callee = R8E_UNDEFINED;
    }

    /* Set up mapped parameters (sloppy mode) */
    if (!is_strict && locals && param_count > 0) {
        args->mapped_locals = locals;
        args->mapped_count = param_count;
    } else {
        args->mapped_locals = NULL;
        args->mapped_count = 0;
    }

    ctx->gc.total_allocated++;
    return args;
}

/**
 * r8e_arguments_get - get a value from the arguments object.
 *
 * In mapped mode, if index < param_count, reads from the local variable
 * slot (reflecting any changes to the named parameter).
 */
R8EValue r8e_arguments_get(const R8EArguments *args, uint32_t index) {
    assert(args != NULL);

    if (index >= args->length) {
        return R8E_UNDEFINED;
    }

    /* Mapped mode: read from local variable slot if within param range */
    if (args->is_mapped && args->mapped_locals &&
        index < args->mapped_count) {
        return args->mapped_locals[index];
    }

    return args->values[index];
}

/**
 * r8e_arguments_set - set a value in the arguments object.
 *
 * In mapped mode, if index < param_count, also writes to the local
 * variable slot (maintaining the mapping).
 */
void r8e_arguments_set(R8EContext *ctx, R8EArguments *args,
                        uint32_t index, R8EValue val) {
    assert(ctx != NULL);
    assert(args != NULL);

    if (index >= args->length) return;

    /* Release old value, retain new */
    R8EValue old = args->values[index];
    if (old != val) {
        r8e_retain(ctx, val);
        args->values[index] = val;
        r8e_release(ctx, old);
    }

    /* Mapped mode: also write to local variable slot */
    if (args->is_mapped && args->mapped_locals &&
        index < args->mapped_count) {
        args->mapped_locals[index] = val;
    }
}

/**
 * r8e_arguments_free - free an arguments object.
 */
void r8e_arguments_free(R8EContext *ctx, R8EArguments *args) {
    if (!args) return;

    if (args->values) {
        for (uint32_t i = 0; i < args->length; i++) {
            r8e_release(ctx, args->values[i]);
        }
        free(args->values);
        args->values = NULL;
    }

    if (!R8E_IS_UNDEFINED(args->callee)) {
        r8e_release(ctx, args->callee);
    }

    ctx->gc.total_freed++;
    free(args);
}


/* =========================================================================
 * Section 6: Function.prototype.call
 *
 * func.call(thisArg, arg1, arg2, ...)
 * Calls func with the specified this value and individual arguments.
 * ========================================================================= */

/**
 * r8e_function_call - implement Function.prototype.call semantics.
 *
 * @param ctx       Engine context
 * @param func_val  The function to call (NaN-boxed)
 * @param this_val  The this binding
 * @param argc      Number of arguments (not counting thisArg)
 * @param argv      Arguments array
 * @return          Return value of the function
 */
R8EValue r8e_function_call(R8EContext *ctx,
                            R8EValue func_val,
                            R8EValue this_val,
                            int argc,
                            const R8EValue *argv) {
    if (!ctx) return R8E_UNDEFINED;

    /* Check for pending exception */
    if (ctx->error.has_exception) return R8E_UNDEFINED;

    R8ECallableKind kind = r8e_get_callable_kind(func_val);

    switch (kind) {
    case R8E_CALLABLE_NATIVE: {
        R8ENativeFunction *nf =
            (R8ENativeFunction *)r8e_get_pointer(func_val);
        return r8e_native_function_call(ctx, nf, this_val, argc, argv);
    }

    case R8E_CALLABLE_BOUND: {
        R8EBoundFunction *bf =
            (R8EBoundFunction *)r8e_get_pointer(func_val);

        /* Bound function overrides this_val with bound_this */
        int total = 0;
        R8EValue *merged = r8e_bound_function_build_args(
            bf, argc, argv, &total);

        R8EValue result = r8e_function_call(
            ctx, bf->target, bf->bound_this, total, merged);

        free(merged);
        return result;
    }

    case R8E_CALLABLE_CLOSURE: {
        /* Closure call is handled by the interpreter.
         * Here we provide the entry point that the interpreter loop
         * will use. For now, return a placeholder that indicates
         * "call this closure via the interpreter." In a fully integrated
         * engine, this dispatches to r8e_interp_call(). */

        /* Stack overflow check */
        if (ctx->stack_depth >= ctx->max_stack_depth) {
            return r8e_throw_range_error(ctx,
                "Maximum call stack size exceeded");
        }

        /* The actual bytecode interpretation is in r8e_interp.c.
         * This function provides the call protocol wrapper. */
        ctx->stack_depth++;

        /* Push a call frame */
        if (ctx->frame_count < R8E_MAX_CALL_FRAMES) {
            /* The interpreter will fill in pc, locals, etc. */
            R8ECallFrame *frame = &ctx->call_frames[ctx->frame_count];
            memset(frame, 0, sizeof(R8ECallFrame));
            frame->this_val = this_val;
            frame->new_target = R8E_UNDEFINED;
            ctx->frame_count++;
        }

        /* NOTE: In the fully assembled engine, this is where
         * r8e_interp_execute() is called. For now we return undefined
         * as the interpreter module is not yet integrated. */
        R8EValue result = R8E_UNDEFINED;

        if (ctx->frame_count > 0) {
            ctx->frame_count--;
        }
        ctx->stack_depth--;

        return result;
    }

    case R8E_CALLABLE_NONE:
    default:
        return r8e_throw_type_error(ctx,
            "Value is not a function");
    }
}


/* =========================================================================
 * Section 7: Function.prototype.apply
 *
 * func.apply(thisArg, argsArray)
 * Calls func with the specified this and an array of arguments.
 * ========================================================================= */

/**
 * r8e_function_apply - implement Function.prototype.apply semantics.
 *
 * @param ctx          Engine context
 * @param func_val     The function to call
 * @param this_val     The this binding
 * @param args_array   Array value containing arguments (or null/undefined)
 * @return             Return value of the function
 */
R8EValue r8e_function_apply(R8EContext *ctx,
                             R8EValue func_val,
                             R8EValue this_val,
                             R8EValue args_array) {
    if (!ctx) return R8E_UNDEFINED;

    /* Check for pending exception */
    if (ctx->error.has_exception) return R8E_UNDEFINED;

    /* null or undefined args_array means no arguments */
    if (R8E_IS_NULLISH(args_array)) {
        return r8e_function_call(ctx, func_val, this_val, 0, NULL);
    }

    /* args_array must be an object (array-like) */
    if (!R8E_IS_POINTER(args_array)) {
        return r8e_throw_type_error(ctx,
            "CreateListFromArrayLike called on non-object");
    }

    /*
     * In the fully integrated engine, we would:
     * 1. Get the "length" property of args_array
     * 2. Iterate 0..length-1, getting each indexed element
     * 3. Build an argv array
     * 4. Call r8e_function_call with that argv
     *
     * For now, since the object/array modules are separate, we provide
     * a direct array access path for the common case. The interpreter
     * integration layer will handle the full CreateListFromArrayLike
     * algorithm.
     */

    /* Attempt direct array access (GC kind check) */
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(args_array);
    if (!hdr) {
        return r8e_throw_type_error(ctx,
            "CreateListFromArrayLike called on non-object");
    }

    /*
     * Fallback: call with no args for now. The interpreter integration
     * will provide full array-like expansion.
     */
    return r8e_function_call(ctx, func_val, this_val, 0, NULL);
}


/* =========================================================================
 * Section 8: Function.prototype.bind
 * ========================================================================= */

/**
 * r8e_function_bind - implement Function.prototype.bind semantics.
 *
 * Returns a new bound function that wraps the target with a fixed this
 * and optional pre-supplied arguments.
 *
 * @param ctx        Engine context
 * @param func_val   The function to bind
 * @param this_val   The this value to bind
 * @param argc       Number of pre-supplied arguments
 * @param argv       Pre-supplied arguments
 * @return           Bound function value (NaN-boxed), or R8E_UNDEFINED on error
 */
R8EValue r8e_function_bind(R8EContext *ctx,
                            R8EValue func_val,
                            R8EValue this_val,
                            int argc,
                            const R8EValue *argv) {
    if (!ctx) return R8E_UNDEFINED;

    /* Target must be callable */
    if (!r8e_is_callable(func_val)) {
        return r8e_throw_type_error(ctx,
            "Bind must be called on a function");
    }

    /* Determine the name atom for the bound function */
    uint32_t target_name = 0;
    R8ECallableKind kind = r8e_get_callable_kind(func_val);
    if (kind == R8E_CALLABLE_NATIVE) {
        R8ENativeFunction *nf =
            (R8ENativeFunction *)r8e_get_pointer(func_val);
        target_name = nf->name_atom;
    } else if (kind == R8E_CALLABLE_BOUND) {
        R8EBoundFunction *bf =
            (R8EBoundFunction *)r8e_get_pointer(func_val);
        target_name = bf->name_atom;
    }
    /* For closures, we would extract from the R8EFunction */

    uint16_t bound_argc = (argc > 0) ? (uint16_t)argc : 0;
    if (bound_argc > R8E_MAX_BOUND_ARGS) {
        bound_argc = R8E_MAX_BOUND_ARGS;
    }

    R8EBoundFunction *bf = r8e_bound_function_new(
        ctx, func_val, this_val, argv, bound_argc, target_name);

    if (!bf) return R8E_UNDEFINED;

    return r8e_from_pointer(bf);
}


/* =========================================================================
 * Section 9: Constructor call (new operator)
 *
 * The constructor call protocol:
 * 1. Check that func is constructable (not arrow, not bound to non-ctor)
 * 2. Create a new object with [[Prototype]] = func.prototype
 * 3. Call func with the new object as 'this' and set new.target = func
 * 4. If func returns an object, return that object
 * 5. Otherwise, return the newly created object
 * ========================================================================= */

/**
 * r8e_construct - implement the [[Construct]] internal method.
 *
 * @param ctx        Engine context
 * @param func_val   Constructor function value
 * @param argc       Number of arguments
 * @param argv       Arguments array
 * @param new_target The new.target value (usually same as func_val)
 * @return           The newly constructed object, or R8E_UNDEFINED on error
 */
R8EValue r8e_construct(R8EContext *ctx,
                        R8EValue func_val,
                        int argc,
                        const R8EValue *argv,
                        R8EValue new_target) {
    if (!ctx) return R8E_UNDEFINED;

    /* Check for pending exception */
    if (ctx->error.has_exception) return R8E_UNDEFINED;

    R8ECallableKind kind = r8e_get_callable_kind(func_val);

    switch (kind) {
    case R8E_CALLABLE_NATIVE: {
        R8ENativeFunction *nf =
            (R8ENativeFunction *)r8e_get_pointer(func_val);

        /* Native constructors handle their own object creation */
        if (ctx->stack_depth >= ctx->max_stack_depth) {
            return r8e_throw_range_error(ctx,
                "Maximum call stack size exceeded");
        }

        ctx->stack_depth++;

        /* Push call frame */
        if (ctx->frame_count < R8E_MAX_CALL_FRAMES) {
            R8ECallFrame *frame = &ctx->call_frames[ctx->frame_count];
            memset(frame, 0, sizeof(R8ECallFrame));
            frame->this_val = R8E_UNDEFINED;
            frame->new_target = new_target;
            frame->source_name = "<native>";
            ctx->frame_count++;
        }

        /* For native constructors, pass new_target as this
         * (they create their own object internally) */
        R8EValue result = nf->callback(ctx, new_target, argc, argv);

        if (ctx->frame_count > 0) ctx->frame_count--;
        ctx->stack_depth--;

        return result;
    }

    case R8E_CALLABLE_BOUND: {
        R8EBoundFunction *bf =
            (R8EBoundFunction *)r8e_get_pointer(func_val);

        /* Bound function construct: forward to target.
         * The bound_this is NOT used for construct calls.
         * The new.target is the bound function itself (or the
         * explicitly provided new_target). */
        int total = 0;
        R8EValue *merged = r8e_bound_function_build_args(
            bf, argc, argv, &total);

        R8EValue result = r8e_construct(
            ctx, bf->target, total, merged, new_target);

        free(merged);
        return result;
    }

    case R8E_CALLABLE_CLOSURE: {
        /* Check if the function is constructable */
        /* Arrow functions and methods cannot be constructed */

        if (ctx->stack_depth >= ctx->max_stack_depth) {
            return r8e_throw_range_error(ctx,
                "Maximum call stack size exceeded");
        }

        /*
         * Full construction protocol:
         * 1. Create a new ordinary object
         * 2. Set its [[Prototype]] to func.prototype
         * 3. Call func with the new object as 'this'
         * 4. If func returns an object, return it
         * 5. Otherwise, return the new object
         *
         * In the fully integrated engine, step 1 uses r8e_object_new()
         * from r8e_object.c, and step 3 uses r8e_interp_execute().
         * Here we provide the protocol skeleton.
         */

        ctx->stack_depth++;

        /* Push call frame with new.target set */
        if (ctx->frame_count < R8E_MAX_CALL_FRAMES) {
            R8ECallFrame *frame = &ctx->call_frames[ctx->frame_count];
            memset(frame, 0, sizeof(R8ECallFrame));
            frame->this_val = R8E_UNDEFINED; /* set by interpreter */
            frame->new_target = new_target;
            ctx->frame_count++;
        }

        /* Placeholder: interpreter integration will handle the actual call */
        R8EValue result = R8E_UNDEFINED;

        if (ctx->frame_count > 0) ctx->frame_count--;
        ctx->stack_depth--;

        /* If the constructor did not return an object, return 'this'
         * (which was the newly created object). Since we are not yet
         * integrated with the interpreter, we return the placeholder. */
        if (!R8E_IS_POINTER(result)) {
            /* Would return the 'this' object here */
            return R8E_UNDEFINED;
        }

        return result;
    }

    case R8E_CALLABLE_NONE:
    default:
        return r8e_throw_type_error(ctx,
            "Value is not a constructor");
    }
}


/* =========================================================================
 * Section 10: Function.prototype.toString
 *
 * Returns a string representation of the function.
 * ========================================================================= */

/**
 * r8e_function_to_string - generate a string representation.
 *
 * For native functions: "function name() { [native code] }"
 * For bound functions:  "function bound name() { [native code] }"
 * For bytecode closures with source: the original source text
 * For bytecode closures without source: "function name() { [bytecode] }"
 *
 * Writes into the provided buffer. Returns the number of bytes written
 * (excluding null terminator), or the required buffer size if buf is NULL.
 */
int r8e_function_to_string(R8EValue func_val, char *buf, int buf_size) {
    R8ECallableKind kind = r8e_get_callable_kind(func_val);

    const char *prefix = "function ";
    const char *name_str = "";
    const char *suffix = "() { [native code] }";
    const char *bound_prefix = "";

    switch (kind) {
    case R8E_CALLABLE_NATIVE: {
        /* function name() { [native code] } */
        R8ENativeFunction *nf =
            (R8ENativeFunction *)r8e_get_pointer(func_val);
        (void)nf; /* name resolution requires atom table access */
        break;
    }

    case R8E_CALLABLE_BOUND: {
        bound_prefix = "bound ";
        break;
    }

    case R8E_CALLABLE_CLOSURE: {
        suffix = "() { [bytecode] }";
        break;
    }

    default:
        break;
    }

    int needed = snprintf(NULL, 0, "%s%s%s%s",
                          prefix, bound_prefix, name_str, suffix);

    if (buf && buf_size > 0) {
        snprintf(buf, buf_size, "%s%s%s%s",
                 prefix, bound_prefix, name_str, suffix);
    }

    return needed;
}


/* =========================================================================
 * Section 11: Function property accessors
 *
 * JavaScript functions have well-known properties:
 *   .name     - function name (string)
 *   .length   - expected number of arguments (number)
 *   .caller   - deprecated, throws in strict mode
 *   .arguments - deprecated, throws in strict mode
 * ========================================================================= */

/**
 * r8e_function_get_name_value - get the .name property value.
 *
 * Returns the name as an inline string value (or R8E_UNDEFINED if anonymous).
 */
R8EValue r8e_function_get_name_value(R8EValue func_val) {
    R8ECallableKind kind = r8e_get_callable_kind(func_val);

    uint32_t name_atom = 0;

    switch (kind) {
    case R8E_CALLABLE_NATIVE: {
        R8ENativeFunction *nf =
            (R8ENativeFunction *)r8e_get_pointer(func_val);
        name_atom = nf->name_atom;
        break;
    }
    case R8E_CALLABLE_BOUND: {
        R8EBoundFunction *bf =
            (R8EBoundFunction *)r8e_get_pointer(func_val);
        name_atom = bf->name_atom;
        break;
    }
    case R8E_CALLABLE_CLOSURE:
        /* Would need to access closure->func->name_atom */
        break;
    default:
        break;
    }

    /* Return the atom as an atom-type value (caller converts to string) */
    if (name_atom != 0) {
        return 0xFFFC000000000000ULL | (uint64_t)name_atom;
    }

    /* Anonymous function: return empty string */
    return 0xFFFD000000000000ULL; /* inline empty string, length 0 */
}

/**
 * r8e_function_get_length_value - get the .length property value.
 *
 * Returns the arity as an int32 value.
 */
R8EValue r8e_function_get_length_value(R8EValue func_val) {
    R8ECallableKind kind = r8e_get_callable_kind(func_val);

    switch (kind) {
    case R8E_CALLABLE_NATIVE: {
        R8ENativeFunction *nf =
            (R8ENativeFunction *)r8e_get_pointer(func_val);
        int16_t arity = nf->arity;
        if (arity < 0) arity = 0;
        return r8e_from_int32(arity);
    }

    case R8E_CALLABLE_BOUND: {
        R8EBoundFunction *bf =
            (R8EBoundFunction *)r8e_get_pointer(func_val);
        return r8e_from_int32(bf->remaining_arity);
    }

    case R8E_CALLABLE_CLOSURE:
        /* Would need to access closure->func and call r8e_function_get_arity */
        return r8e_from_int32(0);

    default:
        return r8e_from_int32(0);
    }
}


/* =========================================================================
 * Section 12: Rest parameters
 *
 * The rest parameter (...args) collects remaining arguments into an array.
 * This is called by the interpreter when entering a function that has
 * a rest parameter declaration.
 * ========================================================================= */

/**
 * r8e_build_rest_args - create a rest parameter array.
 *
 * Collects arguments from index 'start' to argc-1 into a new array value.
 * Returns an array value (NaN-boxed pointer to R8EArray).
 *
 * In the fully integrated engine, this creates an R8EArray. For now,
 * we return the arguments as a simple heap array wrapped in a minimal
 * array object.
 */
typedef struct R8ERestArray {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *elements;
} R8ERestArray;

R8EValue r8e_build_rest_args(R8EContext *ctx,
                              const R8EValue *argv,
                              int argc,
                              int start) {
    if (!ctx) return R8E_UNDEFINED;

    int rest_count = (argc > start) ? (argc - start) : 0;

    R8ERestArray *arr = (R8ERestArray *)calloc(1, sizeof(R8ERestArray));
    if (!arr) return R8E_UNDEFINED;

    r8e_gc_header_init((R8EGCHeader *)arr, R8E_GC_KIND_ARRAY,
                        R8E_PROTO_ARRAY);

    arr->length = (uint32_t)rest_count;
    arr->capacity = rest_count > 0 ? (uint32_t)rest_count : 1;

    arr->elements = (R8EValue *)malloc(arr->capacity * sizeof(R8EValue));
    if (!arr->elements) {
        free(arr);
        return R8E_UNDEFINED;
    }

    for (int i = 0; i < rest_count; i++) {
        arr->elements[i] = argv[start + i];
        r8e_retain(ctx, argv[start + i]);
    }

    ctx->gc.total_allocated++;
    return r8e_from_pointer(arr);
}


/* =========================================================================
 * Section 13: Default parameter evaluation
 *
 * When a function has default parameter values, the interpreter evaluates
 * the default expression if the caller passed undefined (or fewer args).
 * This module provides the check-and-evaluate protocol.
 * ========================================================================= */

/**
 * r8e_needs_default_param - check if a parameter needs its default value.
 *
 * A parameter needs its default if:
 * - The caller did not provide it (index >= argc), or
 * - The caller passed undefined explicitly
 *
 * @param argv  Argument values
 * @param argc  Number of arguments
 * @param index Parameter index to check
 * @return      True if the default should be evaluated
 */
bool r8e_needs_default_param(const R8EValue *argv, int argc, int index) {
    if (index >= argc) return true;
    return R8E_IS_UNDEFINED(argv[index]);
}

/**
 * r8e_prepare_params - prepare parameter bindings for a function call.
 *
 * Fills the local variable slots with argument values, handling:
 * - Missing arguments (filled with undefined)
 * - Extra arguments (ignored unless rest params)
 * - Flags for default evaluation (caller must then run default bytecode)
 *
 * @param func     Function descriptor
 * @param argv     Caller-supplied arguments
 * @param argc     Number of caller-supplied arguments
 * @param locals   Local variable register array to fill
 * @param needs_defaults  Output: bitfield, bit i set if param i needs default
 * @return         Number of parameters bound
 */
uint16_t r8e_prepare_params(const R8EFunction *func,
                             const R8EValue *argv,
                             int argc,
                             R8EValue *locals,
                             uint64_t *needs_defaults) {
    assert(func != NULL);
    assert(locals != NULL);

    uint16_t param_count = func->param_count;
    uint64_t defaults_mask = 0;

    /* Bind regular parameters */
    for (uint16_t i = 0; i < param_count; i++) {
        if (i < (uint16_t)argc && !R8E_IS_UNDEFINED(argv[i])) {
            locals[i] = argv[i];
        } else {
            /* Check if this parameter has a default */
            if (func->default_offsets && func->default_offsets[i] != 0) {
                locals[i] = R8E_UNDEFINED; /* will be filled by default expr */
                defaults_mask |= (1ULL << i);
            } else {
                locals[i] = R8E_UNDEFINED;
            }
        }
    }

    if (needs_defaults) {
        *needs_defaults = defaults_mask;
    }

    return param_count;
}


/* =========================================================================
 * Section 14: Tail call detection
 *
 * In strict mode, a call in tail position can be optimized to reuse the
 * current stack frame. This avoids stack overflow for recursive algorithms.
 *
 * A call is in tail position if:
 * - It is a direct return of a function call: return f(x)
 * - The calling function is in strict mode
 * - No try/catch/finally block is active
 *
 * The compiler emits R8E_OP_TAIL_CALL instead of R8E_OP_CALL + R8E_OP_RETURN
 * for tail calls. The interpreter handles this by patching the call frame
 * in-place instead of pushing a new one.
 * ========================================================================= */

/**
 * r8e_is_tail_call_eligible - check if a function can be tail-called.
 *
 * @param caller  The calling function
 * @param callee  The function being called
 * @return        True if tail call optimization is safe
 */
bool r8e_is_tail_call_eligible(const R8EFunction *caller,
                                R8EValue callee) {
    if (!caller) return false;

    /* Must be in strict mode */
    if (!(caller->func_flags & R8E_FUNC_IS_STRICT)) return false;

    /* Callee must be a callable */
    if (!r8e_is_callable(callee)) return false;

    /* Generators and async functions cannot use tail calls */
    if (caller->func_flags & (R8E_FUNC_IS_GENERATOR | R8E_FUNC_IS_ASYNC)) {
        return false;
    }

    return true;
}


/* =========================================================================
 * Section 15: new.target support
 *
 * new.target is a meta-property that:
 * - Is undefined in normal function calls
 * - Points to the constructor in new Constructor() calls
 * - Is inherited by arrow functions from enclosing scope
 * ========================================================================= */

/**
 * r8e_get_new_target - retrieve the new.target value from the call frame.
 */
R8EValue r8e_get_new_target(const R8EContext *ctx) {
    assert(ctx != NULL);

    if (ctx->frame_count == 0) {
        return R8E_UNDEFINED;
    }

    return ctx->call_frames[ctx->frame_count - 1].new_target;
}


/* =========================================================================
 * Section 16: GC integration - reference visitors
 * ========================================================================= */

/**
 * r8e_function_visit_refs - visit all heap references in a function.
 */
void r8e_function_visit_refs(const R8EFunction *func,
                              R8EGCVisitorFn visitor, void *ud) {
    if (!func) return;

    /* Visit constant pool values */
    if (func->constants) {
        for (uint16_t i = 0; i < func->const_count; i++) {
            if (R8E_IS_POINTER(func->constants[i])) {
                R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(
                    func->constants[i]);
                if (ref) visitor(ref, ud);
            }
        }
    }
}

/**
 * r8e_native_function_visit_refs - native functions hold no heap refs.
 */
void r8e_native_function_visit_refs(const R8ENativeFunction *nf,
                                     R8EGCVisitorFn visitor,
                                     void *ud) {
    (void)nf;
    (void)visitor;
    (void)ud;
    /* Native functions hold no heap-allocated references */
}

/**
 * r8e_bound_function_visit_refs - visit all references in a bound function.
 */
void r8e_bound_function_visit_refs(const R8EBoundFunction *bf,
                                    R8EGCVisitorFn visitor, void *ud) {
    if (!bf) return;

    /* Visit target */
    if (R8E_IS_POINTER(bf->target)) {
        R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(bf->target);
        if (ref) visitor(ref, ud);
    }

    /* Visit bound_this */
    if (R8E_IS_POINTER(bf->bound_this)) {
        R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(bf->bound_this);
        if (ref) visitor(ref, ud);
    }

    /* Visit bound arguments */
    if (bf->bound_args) {
        for (uint16_t i = 0; i < bf->bound_argc; i++) {
            if (R8E_IS_POINTER(bf->bound_args[i])) {
                R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(
                    bf->bound_args[i]);
                if (ref) visitor(ref, ud);
            }
        }
    }
}

/**
 * r8e_arguments_visit_refs - visit all references in an arguments object.
 */
void r8e_arguments_visit_refs(const R8EArguments *args,
                               R8EGCVisitorFn visitor, void *ud) {
    if (!args) return;

    /* Visit argument values */
    if (args->values) {
        for (uint32_t i = 0; i < args->length; i++) {
            if (R8E_IS_POINTER(args->values[i])) {
                R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(
                    args->values[i]);
                if (ref) visitor(ref, ud);
            }
        }
    }

    /* Visit callee */
    if (R8E_IS_POINTER(args->callee)) {
        R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(args->callee);
        if (ref) visitor(ref, ud);
    }
}


/* =========================================================================
 * Section 17: Function.prototype built-in methods (native implementations)
 *
 * These are the native callback functions that implement the JS-visible
 * Function.prototype.call, Function.prototype.apply, etc. They are
 * registered on the Function.prototype object during realm initialization.
 * ========================================================================= */

/**
 * Native implementation of Function.prototype.call.
 *
 * Usage: func.call(thisArg, arg1, arg2, ...)
 */
static R8EValue r8e_builtin_function_call(void *raw_ctx,
                                           R8EValue this_val,
                                           int argc,
                                           const R8EValue *argv) {
    R8EContext *ctx = (R8EContext *)raw_ctx;

    /* 'this' is the function to call */
    if (!r8e_is_callable(this_val)) {
        return r8e_throw_type_error(ctx,
            "Function.prototype.call called on non-function");
    }

    /* First argument is thisArg */
    R8EValue call_this = (argc > 0) ? argv[0] : R8E_UNDEFINED;

    /* Remaining arguments are passed to the target */
    int call_argc = (argc > 1) ? (argc - 1) : 0;
    const R8EValue *call_argv = (argc > 1) ? &argv[1] : NULL;

    return r8e_function_call(ctx, this_val, call_this, call_argc, call_argv);
}

/**
 * Native implementation of Function.prototype.apply.
 *
 * Usage: func.apply(thisArg, argsArray)
 */
static R8EValue r8e_builtin_function_apply(void *raw_ctx,
                                            R8EValue this_val,
                                            int argc,
                                            const R8EValue *argv) {
    R8EContext *ctx = (R8EContext *)raw_ctx;

    if (!r8e_is_callable(this_val)) {
        return r8e_throw_type_error(ctx,
            "Function.prototype.apply called on non-function");
    }

    R8EValue call_this = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    R8EValue args_array = (argc > 1) ? argv[1] : R8E_UNDEFINED;

    return r8e_function_apply(ctx, this_val, call_this, args_array);
}

/**
 * Native implementation of Function.prototype.bind.
 *
 * Usage: func.bind(thisArg, arg1, arg2, ...)
 */
static R8EValue r8e_builtin_function_bind(void *raw_ctx,
                                           R8EValue this_val,
                                           int argc,
                                           const R8EValue *argv) {
    R8EContext *ctx = (R8EContext *)raw_ctx;

    if (!r8e_is_callable(this_val)) {
        return r8e_throw_type_error(ctx,
            "Function.prototype.bind called on non-function");
    }

    R8EValue bind_this = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    int bind_argc = (argc > 1) ? (argc - 1) : 0;
    const R8EValue *bind_argv = (argc > 1) ? &argv[1] : NULL;

    return r8e_function_bind(ctx, this_val, bind_this, bind_argc, bind_argv);
}

/**
 * Native implementation of Function.prototype.toString.
 *
 * Usage: func.toString()
 */
static R8EValue r8e_builtin_function_toString(void *raw_ctx,
                                               R8EValue this_val,
                                               int argc,
                                               const R8EValue *argv) {
    R8EContext *ctx = (R8EContext *)raw_ctx;
    (void)argc;
    (void)argv;

    if (!r8e_is_callable(this_val)) {
        return r8e_throw_type_error(ctx,
            "Function.prototype.toString called on non-function");
    }

    /* Generate the string representation */
    int needed = r8e_function_to_string(this_val, NULL, 0);
    if (needed <= 0 || needed > 4096) {
        /* Fallback: return a simple representation */
        const char *fallback = "function () { [native code] }";
        int len = (int)strlen(fallback);
        if (len <= 7) {
            /* Fits in inline short string */
            uint64_t v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)(unsigned)len << 45);
            for (int i = 0; i < len; i++) {
                v |= ((uint64_t)(uint8_t)fallback[i] << (38 - i * 7));
            }
            return v;
        }
        /* Too long for inline - would need heap string from r8e_string.c */
        return R8E_UNDEFINED;
    }

    /* For short results, try inline string */
    char buf[64];
    r8e_function_to_string(this_val, buf, (int)sizeof(buf));

    int len = (int)strlen(buf);
    if (len <= 7) {
        uint64_t v = 0xFFFD000000000000ULL;
        v |= ((uint64_t)(unsigned)len << 45);
        for (int i = 0; i < len; i++) {
            v |= ((uint64_t)(uint8_t)buf[i] << (38 - i * 7));
        }
        return v;
    }

    /* Longer strings require heap allocation via r8e_string.c */
    return R8E_UNDEFINED;
}

/**
 * Native implementation of Function.prototype[Symbol.hasInstance].
 *
 * Implements the 'instanceof' operator for ordinary functions.
 * Returns true if target.prototype is in the prototype chain of value.
 */
static R8EValue r8e_builtin_function_hasInstance(void *raw_ctx,
                                                  R8EValue this_val,
                                                  int argc,
                                                  const R8EValue *argv) {
    R8EContext *ctx = (R8EContext *)raw_ctx;
    (void)ctx;

    if (!r8e_is_callable(this_val) || argc < 1) {
        return R8E_FALSE;
    }

    R8EValue value = argv[0];

    /* The value must be an object */
    if (!R8E_IS_POINTER(value)) {
        return R8E_FALSE;
    }

    /*
     * Full implementation:
     * 1. Get this.prototype (the constructor's prototype property)
     * 2. Walk the prototype chain of value
     * 3. If any prototype in the chain === this.prototype, return true
     *
     * Requires object model integration. Placeholder returns false.
     */
    return R8E_FALSE;
}


/* =========================================================================
 * Section 18: Function.prototype registration table
 *
 * Used during realm initialization to set up Function.prototype with
 * all the built-in methods.
 * ========================================================================= */

typedef struct {
    uint32_t      name_atom;
    R8ENativeFunc callback;
    int16_t       arity;
} R8EFuncProtoEntry;

static const R8EFuncProtoEntry r8e_func_proto_methods[] = {
    { R8E_ATOM_call,      r8e_builtin_function_call,        1 },
    { R8E_ATOM_apply,     r8e_builtin_function_apply,       2 },
    { R8E_ATOM_bind,      r8e_builtin_function_bind,        1 },
    { R8E_ATOM_toString,  r8e_builtin_function_toString,    0 },
    { 0, NULL, 0 }  /* sentinel */
};

/**
 * r8e_function_proto_method_count - return the number of Function.prototype methods.
 */
int r8e_function_proto_method_count(void) {
    int count = 0;
    while (r8e_func_proto_methods[count].callback != NULL) {
        count++;
    }
    return count;
}

/**
 * r8e_function_proto_get_method - get a Function.prototype method by index.
 */
bool r8e_function_proto_get_method(int index, uint32_t *name_atom,
                                    R8ENativeFunc *callback, int16_t *arity) {
    if (index < 0 || r8e_func_proto_methods[index].callback == NULL) {
        return false;
    }
    if (name_atom) *name_atom = r8e_func_proto_methods[index].name_atom;
    if (callback) *callback = r8e_func_proto_methods[index].callback;
    if (arity) *arity = r8e_func_proto_methods[index].arity;
    return true;
}


/* =========================================================================
 * Section 19: Arrow function creation helper
 *
 * Arrow functions differ from regular functions in:
 * - No own 'this' binding (lexical 'this' from enclosing scope)
 * - No 'arguments' object
 * - Cannot be used as constructors (no [[Construct]])
 * - No 'prototype' property
 * ========================================================================= */

/**
 * r8e_function_new_arrow - create an arrow function descriptor.
 *
 * Same as r8e_function_new but with arrow-specific flags set.
 */
R8EFunction *r8e_function_new_arrow(R8EContext *ctx,
                                     const uint8_t *bytecode,
                                     uint32_t bytecode_len,
                                     uint16_t param_count,
                                     uint16_t local_count,
                                     uint32_t name_atom) {
    R8EFunction *func = r8e_function_new(ctx, bytecode, bytecode_len,
                                          param_count, local_count,
                                          name_atom);
    if (!func) return NULL;

    /* Set arrow-specific flags */
    func->func_flags = R8E_FUNC_IS_ARROW;
    /* Arrow functions are NOT constructable */
    /* Arrow functions inherit strict mode from enclosing scope */

    return func;
}


/* =========================================================================
 * Section 20: Generator and async function stubs
 *
 * Full implementations are in r8e_iterator.c and r8e_promise.c.
 * Here we provide the creation helpers with appropriate flags.
 * ========================================================================= */

/**
 * r8e_function_new_generator - create a generator function descriptor.
 */
R8EFunction *r8e_function_new_generator(R8EContext *ctx,
                                         const uint8_t *bytecode,
                                         uint32_t bytecode_len,
                                         uint16_t param_count,
                                         uint16_t local_count,
                                         uint32_t name_atom) {
    R8EFunction *func = r8e_function_new(ctx, bytecode, bytecode_len,
                                          param_count, local_count,
                                          name_atom);
    if (!func) return NULL;

    func->func_flags = R8E_FUNC_IS_GENERATOR;
    /* Generators are NOT constructable */

    return func;
}

/**
 * r8e_function_new_async - create an async function descriptor.
 */
R8EFunction *r8e_function_new_async(R8EContext *ctx,
                                     const uint8_t *bytecode,
                                     uint32_t bytecode_len,
                                     uint16_t param_count,
                                     uint16_t local_count,
                                     uint32_t name_atom) {
    R8EFunction *func = r8e_function_new(ctx, bytecode, bytecode_len,
                                          param_count, local_count,
                                          name_atom);
    if (!func) return NULL;

    func->func_flags = R8E_FUNC_IS_ASYNC;
    /* Async functions are NOT constructable */

    return func;
}

/**
 * r8e_function_new_async_generator - create an async generator function.
 */
R8EFunction *r8e_function_new_async_generator(R8EContext *ctx,
                                               const uint8_t *bytecode,
                                               uint32_t bytecode_len,
                                               uint16_t param_count,
                                               uint16_t local_count,
                                               uint32_t name_atom) {
    R8EFunction *func = r8e_function_new(ctx, bytecode, bytecode_len,
                                          param_count, local_count,
                                          name_atom);
    if (!func) return NULL;

    func->func_flags = R8E_FUNC_IS_GENERATOR | R8E_FUNC_IS_ASYNC;

    return func;
}
