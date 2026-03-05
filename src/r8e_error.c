/*
 * r8e_error.c - Error Objects and Stack Traces
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 11 (Security Architecture) for stack trace limits.
 *
 * Architecture:
 *   - All ES2023 error types: Error, TypeError, RangeError, ReferenceError,
 *     SyntaxError, URIError, EvalError, AggregateError
 *   - Each error is a regular object with name, message, stack properties
 *   - Stack traces captured at throw time from the call frame stack
 *   - Format: "    at functionName (source:line:col)"
 *   - Pre-formatted common error messages for performance
 *   - Exception state stored in R8EContext.error
 *   - printf-style formatting for dynamic error messages
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
#include <stdarg.h>

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
#define R8E_GC_KIND_ERROR        6u

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
#define R8E_ATOM_stack              10
#define R8E_ATOM_Error              51
#define R8E_ATOM_TypeError          52
#define R8E_ATOM_RangeError         53
#define R8E_ATOM_SyntaxError        54
#define R8E_ATOM_ReferenceError     55
#define R8E_ATOM_URIError           56
#define R8E_ATOM_EvalError          57
#define R8E_ATOM_AggregateError     58

/* =========================================================================
 * Prototype IDs
 * ========================================================================= */

#define R8E_PROTO_NONE          0
#define R8E_PROTO_OBJECT        1
#define R8E_PROTO_ERROR         9
#define R8E_PROTO_TYPEERROR     10
#define R8E_PROTO_RANGEERROR    11
#define R8E_PROTO_SYNTAXERROR   12
#define R8E_PROTO_REFERENCEERROR 13
#define R8E_PROTO_URIERROR      14
#define R8E_PROTO_EVALERROR     15

/* =========================================================================
 * Error type enumeration
 * ========================================================================= */

typedef enum {
    R8E_ERROR_TYPE_ERROR          = 0,
    R8E_ERROR_TYPE_TYPE_ERROR     = 1,
    R8E_ERROR_TYPE_RANGE_ERROR    = 2,
    R8E_ERROR_TYPE_REFERENCE_ERROR = 3,
    R8E_ERROR_TYPE_SYNTAX_ERROR   = 4,
    R8E_ERROR_TYPE_URI_ERROR      = 5,
    R8E_ERROR_TYPE_EVAL_ERROR     = 6,
    R8E_ERROR_TYPE_AGGREGATE_ERROR = 7,
    R8E_ERROR_TYPE_INTERNAL       = 8,
    R8E_ERROR_TYPE_COUNT          = 9
} R8EErrorType;

/* =========================================================================
 * Stack trace configuration
 * ========================================================================= */

#define R8E_STACK_TRACE_MAX_DEPTH    10
#define R8E_STACK_TRACE_MAX_CHARS    4096
#define R8E_ERROR_MSG_MAX            1024

/* =========================================================================
 * R8EStackFrame - one frame in the stack trace
 * ========================================================================= */

typedef struct R8EStackFrame {
    const char *function_name;    /* function name (not owned) */
    const char *source_name;      /* source file name (not owned) */
    uint32_t    line;             /* source line number (1-based) */
    uint32_t    col;              /* source column number (1-based) */
} R8EStackFrame;

/* =========================================================================
 * R8EErrorObject - the JS Error object
 *
 * This is a heap-allocated object that represents an Error instance.
 * It stores the error type, message string, and captured stack trace.
 * Properties (name, message, stack) are accessible as object properties.
 * ========================================================================= */

typedef struct R8EErrorObject {
    uint32_t       flags;          /* GC header flags */
    uint32_t       proto_id;       /* error type prototype */
    R8EErrorType   error_type;     /* which Error subclass */
    char          *message;        /* heap-allocated message string */
    uint32_t       message_len;    /* message length in bytes */
    char          *stack_str;      /* pre-formatted stack trace string */
    uint32_t       stack_str_len;  /* stack trace string length */
    R8EStackFrame *frames;         /* captured stack frames */
    uint16_t       frame_count;    /* number of captured frames */
    uint16_t       frame_capacity; /* allocated frame capacity */
    /* AggregateError: array of sub-errors */
    R8EValue      *errors;         /* sub-error array (AggregateError only) */
    uint32_t       errors_count;   /* number of sub-errors */
} R8EErrorObject;

/* =========================================================================
 * R8EFunction and R8EContext (minimal, matching r8e_function.c)
 * ========================================================================= */

typedef struct R8EFunction {
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
} R8EFunction;

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
 * External function declarations
 * ========================================================================= */

extern void r8e_retain(R8EContext *ctx, R8EValue val);
extern void r8e_release(R8EContext *ctx, R8EValue val);
extern void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind,
                                uint32_t proto_id);

/* GC visitor callback type */
typedef void (*R8EGCVisitorFn)(R8EGCHeader *ref, void *user_data);


/* =========================================================================
 * Static tables: error type -> name string, prototype ID, atom ID
 * ========================================================================= */

static const char *r8e_error_type_names[R8E_ERROR_TYPE_COUNT] = {
    "Error",            /* R8E_ERROR_TYPE_ERROR */
    "TypeError",        /* R8E_ERROR_TYPE_TYPE_ERROR */
    "RangeError",       /* R8E_ERROR_TYPE_RANGE_ERROR */
    "ReferenceError",   /* R8E_ERROR_TYPE_REFERENCE_ERROR */
    "SyntaxError",      /* R8E_ERROR_TYPE_SYNTAX_ERROR */
    "URIError",         /* R8E_ERROR_TYPE_URI_ERROR */
    "EvalError",        /* R8E_ERROR_TYPE_EVAL_ERROR */
    "AggregateError",   /* R8E_ERROR_TYPE_AGGREGATE_ERROR */
    "InternalError"     /* R8E_ERROR_TYPE_INTERNAL */
};

static const uint32_t r8e_error_type_protos[R8E_ERROR_TYPE_COUNT] = {
    R8E_PROTO_ERROR,            /* Error */
    R8E_PROTO_TYPEERROR,        /* TypeError */
    R8E_PROTO_RANGEERROR,       /* RangeError */
    R8E_PROTO_REFERENCEERROR,   /* ReferenceError */
    R8E_PROTO_SYNTAXERROR,      /* SyntaxError */
    R8E_PROTO_URIERROR,         /* URIError */
    R8E_PROTO_EVALERROR,        /* EvalError */
    R8E_PROTO_ERROR,            /* AggregateError (uses Error proto) */
    R8E_PROTO_ERROR             /* InternalError (uses Error proto) */
};

static const uint32_t r8e_error_type_atoms[R8E_ERROR_TYPE_COUNT] = {
    R8E_ATOM_Error,
    R8E_ATOM_TypeError,
    R8E_ATOM_RangeError,
    R8E_ATOM_ReferenceError,
    R8E_ATOM_SyntaxError,
    R8E_ATOM_URIError,
    R8E_ATOM_EvalError,
    R8E_ATOM_AggregateError,
    R8E_ATOM_Error  /* InternalError reuses Error atom */
};


/* =========================================================================
 * Section 1: Stack trace capture
 * ========================================================================= */

/**
 * r8e_capture_stack_trace - capture the current call stack into an error.
 *
 * Walks the call frame stack from top to bottom, recording function name,
 * source file, line, and column for each frame. Stops at max_depth or
 * the bottom of the stack.
 *
 * @param ctx         Engine context
 * @param err         Error object to store frames in
 * @param max_depth   Maximum number of frames to capture (0 = use default)
 */
void r8e_capture_stack_trace(R8EContext *ctx, R8EErrorObject *err,
                              uint16_t max_depth) {
    assert(ctx != NULL);
    assert(err != NULL);

    if (max_depth == 0) {
        max_depth = R8E_STACK_TRACE_MAX_DEPTH;
    }

    uint32_t available = ctx->frame_count;
    uint16_t capture_count = (available < max_depth)
                              ? (uint16_t)available : max_depth;

    if (capture_count == 0) {
        err->frames = NULL;
        err->frame_count = 0;
        err->frame_capacity = 0;
        return;
    }

    err->frames = (R8EStackFrame *)calloc(capture_count,
                                           sizeof(R8EStackFrame));
    if (!err->frames) {
        err->frame_count = 0;
        err->frame_capacity = 0;
        return;
    }

    err->frame_capacity = capture_count;
    err->frame_count = capture_count;

    /* Walk frames from top (most recent) to bottom */
    for (uint16_t i = 0; i < capture_count; i++) {
        uint32_t frame_idx = ctx->frame_count - 1 - i;
        const R8ECallFrame *cf = &ctx->call_frames[frame_idx];

        R8EStackFrame *sf = &err->frames[i];

        /* Function name: from R8EFunction if available */
        if (cf->func && cf->func->source_name) {
            sf->source_name = cf->func->source_name;
        } else if (cf->source_name) {
            sf->source_name = cf->source_name;
        } else {
            sf->source_name = "<unknown>";
        }

        /* Function name */
        if (cf->func && cf->func->name_atom != 0) {
            /* In the fully integrated engine, we would look up the atom
             * string. For now, we use a placeholder. The atom table
             * access will be integrated later. */
            sf->function_name = "<function>";
        } else {
            sf->function_name = "<anonymous>";
        }

        sf->line = cf->line;
        sf->col = (uint32_t)cf->col;
    }
}

/**
 * r8e_format_stack_trace - format captured stack frames into a string.
 *
 * Produces the standard V8/Node.js-style stack trace format:
 *   ErrorType: message
 *       at functionName (source:line:col)
 *       at functionName (source:line:col)
 *
 * @param err          Error object with captured frames
 * @param buf          Output buffer (NULL to query required size)
 * @param buf_size     Output buffer capacity
 * @return             Number of bytes written (or required)
 */
int r8e_format_stack_trace(const R8EErrorObject *err,
                            char *buf, int buf_size) {
    assert(err != NULL);

    /* Calculate required size */
    int total = 0;
    char line_buf[256];

    /* First line: "ErrorType: message" */
    const char *type_name = "Error";
    if (err->error_type < R8E_ERROR_TYPE_COUNT) {
        type_name = r8e_error_type_names[err->error_type];
    }

    const char *msg = err->message ? err->message : "";
    int header_len;

    if (msg[0] != '\0') {
        header_len = snprintf(line_buf, sizeof(line_buf),
                              "%s: %s\n", type_name, msg);
    } else {
        header_len = snprintf(line_buf, sizeof(line_buf),
                              "%s\n", type_name);
    }

    if (buf && total + header_len < buf_size) {
        memcpy(buf + total, line_buf, header_len);
    }
    total += header_len;

    /* Stack frames */
    for (uint16_t i = 0; i < err->frame_count; i++) {
        const R8EStackFrame *sf = &err->frames[i];

        int frame_len;
        if (sf->line > 0) {
            frame_len = snprintf(line_buf, sizeof(line_buf),
                                 "    at %s (%s:%u:%u)\n",
                                 sf->function_name ? sf->function_name
                                                   : "<anonymous>",
                                 sf->source_name ? sf->source_name
                                                 : "<unknown>",
                                 sf->line, sf->col);
        } else {
            frame_len = snprintf(line_buf, sizeof(line_buf),
                                 "    at %s (%s)\n",
                                 sf->function_name ? sf->function_name
                                                   : "<anonymous>",
                                 sf->source_name ? sf->source_name
                                                 : "<unknown>");
        }

        if (buf && total + frame_len < buf_size) {
            memcpy(buf + total, line_buf, frame_len);
        }
        total += frame_len;
    }

    /* Null-terminate if buffer was provided */
    if (buf && buf_size > 0) {
        int term_pos = (total < buf_size) ? total : buf_size - 1;
        buf[term_pos] = '\0';
    }

    return total;
}


/* =========================================================================
 * Section 2: Error object creation
 * ========================================================================= */

/**
 * r8e_error_object_new - create a new error object.
 *
 * Allocates an R8EErrorObject, copies the message string, captures
 * the stack trace, and formats the stack trace string.
 *
 * @param ctx          Engine context
 * @param error_type   Error type (R8E_ERROR_TYPE_*)
 * @param message      Error message (copied, can be NULL)
 * @param message_len  Message length (0 = use strlen)
 * @return             Newly allocated R8EErrorObject, or NULL on OOM
 */
R8EErrorObject *r8e_error_object_new(R8EContext *ctx,
                                      R8EErrorType error_type,
                                      const char *message,
                                      uint32_t message_len) {
    if (!ctx) return NULL;

    R8EErrorObject *err = (R8EErrorObject *)calloc(
        1, sizeof(R8EErrorObject));
    if (!err) return NULL;

    /* Determine prototype ID */
    uint32_t proto_id = R8E_PROTO_ERROR;
    if (error_type < R8E_ERROR_TYPE_COUNT) {
        proto_id = r8e_error_type_protos[error_type];
    }

    r8e_gc_header_init((R8EGCHeader *)err, R8E_GC_KIND_ERROR, proto_id);

    err->error_type = error_type;
    err->errors = NULL;
    err->errors_count = 0;

    /* Copy message */
    if (message) {
        if (message_len == 0) {
            message_len = (uint32_t)strlen(message);
        }
        err->message = (char *)malloc(message_len + 1);
        if (err->message) {
            memcpy(err->message, message, message_len);
            err->message[message_len] = '\0';
            err->message_len = message_len;
        }
    } else {
        err->message = NULL;
        err->message_len = 0;
    }

    /* Capture stack trace */
    r8e_capture_stack_trace(ctx, err, R8E_STACK_TRACE_MAX_DEPTH);

    /* Format stack trace string */
    int stack_size = r8e_format_stack_trace(err, NULL, 0);
    if (stack_size > 0 && stack_size < R8E_STACK_TRACE_MAX_CHARS) {
        err->stack_str = (char *)malloc(stack_size + 1);
        if (err->stack_str) {
            r8e_format_stack_trace(err, err->stack_str, stack_size + 1);
            err->stack_str_len = (uint32_t)stack_size;
        }
    }

    ctx->gc.total_allocated++;
    return err;
}

/**
 * r8e_error_object_free - free an error object.
 */
void r8e_error_object_free(R8EContext *ctx, R8EErrorObject *err) {
    if (!err) return;

    free(err->message);
    err->message = NULL;

    free(err->stack_str);
    err->stack_str = NULL;

    free(err->frames);
    err->frames = NULL;

    /* Release sub-errors (AggregateError) */
    if (err->errors) {
        for (uint32_t i = 0; i < err->errors_count; i++) {
            r8e_release(ctx, err->errors[i]);
        }
        free(err->errors);
        err->errors = NULL;
    }

    ctx->gc.total_freed++;
    free(err);
}


/* =========================================================================
 * Section 3: Error creation convenience functions
 *
 * These create an error value (NaN-boxed pointer to R8EErrorObject)
 * with the specified type and message.
 * ========================================================================= */

/**
 * r8e_error_new - create a new error value.
 *
 * @param ctx          Engine context
 * @param error_type   Error type
 * @param message      Error message
 * @return             NaN-boxed pointer to the error object
 */
R8EValue r8e_error_new(R8EContext *ctx, R8EErrorType error_type,
                        const char *message) {
    R8EErrorObject *err = r8e_error_object_new(ctx, error_type, message, 0);
    if (!err) return R8E_UNDEFINED;
    return r8e_from_pointer(err);
}

/**
 * r8e_error_new_fmt - create a new error value with printf-style formatting.
 *
 * @param ctx          Engine context
 * @param error_type   Error type
 * @param fmt          printf-style format string
 * @param ...          Format arguments
 * @return             NaN-boxed pointer to the error object
 */
R8EValue r8e_error_new_fmt(R8EContext *ctx, R8EErrorType error_type,
                            const char *fmt, ...) {
    if (!ctx || !fmt) return R8E_UNDEFINED;

    char buf[R8E_ERROR_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    buf[len] = '\0';

    return r8e_error_new(ctx, error_type, buf);
}


/* =========================================================================
 * Section 4: Error throwing functions
 *
 * These create an error and set it as the pending exception in the context.
 * They return R8E_UNDEFINED for easy use in return statements:
 *   return r8e_throw_type_error(ctx, "X is not a function");
 * ========================================================================= */

/**
 * r8e_throw - set a value as the current pending exception.
 *
 * Any JS value can be thrown (not just error objects). This is the
 * low-level throw mechanism used by the THROW opcode.
 *
 * @param ctx    Engine context
 * @param value  The value to throw
 * @return       R8E_UNDEFINED (for return convenience)
 */
R8EValue r8e_throw(R8EContext *ctx, R8EValue value) {
    if (!ctx) return R8E_UNDEFINED;

    /* If an exception is already pending, release it first */
    if (ctx->error.has_exception) {
        r8e_release(ctx, ctx->error.exception);
    }

    ctx->error.exception = value;
    ctx->error.has_exception = true;

    /* Retain the exception value */
    r8e_retain(ctx, value);

    return R8E_UNDEFINED;
}

/**
 * r8e_get_exception - get the pending exception value.
 *
 * Returns R8E_UNDEFINED if no exception is pending.
 */
R8EValue r8e_get_exception(const R8EContext *ctx) {
    if (!ctx || !ctx->error.has_exception) return R8E_UNDEFINED;
    return ctx->error.exception;
}

/**
 * r8e_has_exception - check if an exception is pending.
 */
bool r8e_has_exception(const R8EContext *ctx) {
    if (!ctx) return false;
    return ctx->error.has_exception;
}

/**
 * r8e_clear_exception - clear the pending exception.
 *
 * Returns the exception value that was cleared (caller must manage RC).
 */
R8EValue r8e_clear_exception(R8EContext *ctx) {
    if (!ctx || !ctx->error.has_exception) return R8E_UNDEFINED;

    R8EValue exc = ctx->error.exception;
    ctx->error.exception = R8E_UNDEFINED;
    ctx->error.has_exception = false;
    ctx->error.error_line = 0;
    ctx->error.error_col = 0;
    ctx->error.error_file = NULL;

    /* Do NOT release here; the caller takes ownership */
    return exc;
}

/**
 * r8e_throw_error_internal - common implementation for typed throw functions.
 */
static R8EValue r8e_throw_error_internal(R8EContext *ctx,
                                          R8EErrorType type,
                                          const char *fmt,
                                          va_list args) {
    if (!ctx) return R8E_UNDEFINED;

    char buf[R8E_ERROR_MSG_MAX];
    int len = 0;

    if (fmt) {
        len = vsnprintf(buf, sizeof(buf), fmt, args);
        if (len < 0) len = 0;
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        buf[len] = '\0';
    } else {
        buf[0] = '\0';
    }

    R8EValue err_val = r8e_error_new(ctx, type, buf);

    /* Record source location from current call frame */
    if (ctx->frame_count > 0) {
        const R8ECallFrame *cf = &ctx->call_frames[ctx->frame_count - 1];
        ctx->error.error_line = (uint16_t)cf->line;
        ctx->error.error_col = cf->col;
        ctx->error.error_file = cf->source_name;
    }

    return r8e_throw(ctx, err_val);
}

/**
 * r8e_throw_type_error - throw a TypeError.
 */
R8EValue r8e_throw_type_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_TYPE_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_range_error - throw a RangeError.
 */
R8EValue r8e_throw_range_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_RANGE_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_reference_error - throw a ReferenceError.
 */
R8EValue r8e_throw_reference_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_REFERENCE_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_syntax_error - throw a SyntaxError.
 */
R8EValue r8e_throw_syntax_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_SYNTAX_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_uri_error - throw a URIError.
 */
R8EValue r8e_throw_uri_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_URI_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_eval_error - throw an EvalError.
 */
R8EValue r8e_throw_eval_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_EVAL_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_error - throw a generic Error.
 */
R8EValue r8e_throw_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_ERROR, fmt, args);
    va_end(args);
    return result;
}

/**
 * r8e_throw_internal_error - throw an internal engine error.
 */
R8EValue r8e_throw_internal_error(R8EContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    R8EValue result = r8e_throw_error_internal(
        ctx, R8E_ERROR_TYPE_INTERNAL, fmt, args);
    va_end(args);
    return result;
}


/* =========================================================================
 * Section 5: AggregateError creation
 *
 * AggregateError is used by Promise.any() to report multiple rejections.
 * It has an additional 'errors' property containing an array of sub-errors.
 * ========================================================================= */

/**
 * r8e_aggregate_error_new - create an AggregateError.
 *
 * @param ctx          Engine context
 * @param message      Error message
 * @param errors       Array of sub-error values
 * @param error_count  Number of sub-errors
 * @return             NaN-boxed pointer to the AggregateError object
 */
R8EValue r8e_aggregate_error_new(R8EContext *ctx,
                                  const char *message,
                                  const R8EValue *errors,
                                  uint32_t error_count) {
    if (!ctx) return R8E_UNDEFINED;

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_AGGREGATE_ERROR, message, 0);
    if (!err) return R8E_UNDEFINED;

    /* Copy and retain sub-errors */
    if (error_count > 0 && errors) {
        err->errors = (R8EValue *)malloc(error_count * sizeof(R8EValue));
        if (err->errors) {
            for (uint32_t i = 0; i < error_count; i++) {
                err->errors[i] = errors[i];
                r8e_retain(ctx, errors[i]);
            }
            err->errors_count = error_count;
        }
    }

    return r8e_from_pointer(err);
}


/* =========================================================================
 * Section 6: Error property access
 *
 * Error objects have three standard properties:
 *   .name     - the error type name (e.g., "TypeError")
 *   .message  - the error message string
 *   .stack    - the formatted stack trace string
 * ========================================================================= */

/**
 * r8e_error_get_type - return the error type of an error object.
 */
R8EErrorType r8e_error_get_type(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return R8E_ERROR_TYPE_ERROR;

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err) return R8E_ERROR_TYPE_ERROR;

    return err->error_type;
}

/**
 * r8e_error_get_message - return the error message as a C string.
 *
 * Returns a pointer to the internal message buffer. The caller must not
 * free or modify it. Returns "" for errors with no message.
 */
const char *r8e_error_get_message(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return "";

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err || !err->message) return "";

    return err->message;
}

/**
 * r8e_error_get_stack - return the formatted stack trace as a C string.
 *
 * Returns a pointer to the internal stack trace buffer. The caller must
 * not free or modify it. Returns "" if no stack trace was captured.
 */
const char *r8e_error_get_stack(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return "";

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err || !err->stack_str) return "";

    return err->stack_str;
}

/**
 * r8e_error_get_name - return the error type name as a C string.
 *
 * Returns "Error", "TypeError", etc.
 */
const char *r8e_error_get_name(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return "Error";

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err) return "Error";

    if (err->error_type < R8E_ERROR_TYPE_COUNT) {
        return r8e_error_type_names[err->error_type];
    }
    return "Error";
}

/**
 * r8e_error_get_name_value - return the error name as a NaN-boxed value.
 *
 * Returns an atom-type value for the error type name.
 */
R8EValue r8e_error_get_name_value(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) {
        return 0xFFFC000000000000ULL | (uint64_t)R8E_ATOM_Error;
    }

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err) {
        return 0xFFFC000000000000ULL | (uint64_t)R8E_ATOM_Error;
    }

    uint32_t atom = R8E_ATOM_Error;
    if (err->error_type < R8E_ERROR_TYPE_COUNT) {
        atom = r8e_error_type_atoms[err->error_type];
    }

    return 0xFFFC000000000000ULL | (uint64_t)atom;
}

/**
 * r8e_error_get_line - return the source line where the error occurred.
 */
uint32_t r8e_error_get_line(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return 0;

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err || err->frame_count == 0) return 0;

    return err->frames[0].line;
}

/**
 * r8e_error_get_column - return the source column where the error occurred.
 */
uint32_t r8e_error_get_column(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return 0;

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err || err->frame_count == 0) return 0;

    return err->frames[0].col;
}

/**
 * r8e_error_get_source - return the source file where the error occurred.
 */
const char *r8e_error_get_source(R8EValue error_val) {
    if (!R8E_IS_POINTER(error_val)) return NULL;

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(error_val);
    if (!err || err->frame_count == 0) return NULL;

    return err->frames[0].source_name;
}


/* =========================================================================
 * Section 7: Common pre-formatted error messages
 *
 * These functions create common error messages with proper formatting.
 * Using dedicated functions avoids repeated format string parsing and
 * ensures consistent error messages across the engine.
 * ========================================================================= */

/**
 * r8e_throw_not_a_function - "X is not a function"
 */
R8EValue r8e_throw_not_a_function(R8EContext *ctx, const char *name) {
    if (name && name[0] != '\0') {
        return r8e_throw_type_error(ctx, "%s is not a function", name);
    }
    return r8e_throw_type_error(ctx, "Value is not a function");
}

/**
 * r8e_throw_cannot_read_prop - "Cannot read properties of null/undefined"
 */
R8EValue r8e_throw_cannot_read_prop(R8EContext *ctx,
                                     const char *prop_name,
                                     bool is_null) {
    const char *type_str = is_null ? "null" : "undefined";

    if (prop_name && prop_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "Cannot read properties of %s (reading '%s')",
            type_str, prop_name);
    }
    return r8e_throw_type_error(ctx,
        "Cannot read properties of %s", type_str);
}

/**
 * r8e_throw_cannot_set_prop - "Cannot set properties of null/undefined"
 */
R8EValue r8e_throw_cannot_set_prop(R8EContext *ctx,
                                    const char *prop_name,
                                    bool is_null) {
    const char *type_str = is_null ? "null" : "undefined";

    if (prop_name && prop_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "Cannot set properties of %s (setting '%s')",
            type_str, prop_name);
    }
    return r8e_throw_type_error(ctx,
        "Cannot set properties of %s", type_str);
}

/**
 * r8e_throw_not_defined - "X is not defined"
 */
R8EValue r8e_throw_not_defined(R8EContext *ctx, const char *name) {
    if (name && name[0] != '\0') {
        return r8e_throw_reference_error(ctx, "%s is not defined", name);
    }
    return r8e_throw_reference_error(ctx, "Variable is not defined");
}

/**
 * r8e_throw_const_assign - "Assignment to constant variable"
 */
R8EValue r8e_throw_const_assign(R8EContext *ctx, const char *name) {
    if (name && name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "Assignment to constant variable '%s'", name);
    }
    return r8e_throw_type_error(ctx,
        "Assignment to constant variable");
}

/**
 * r8e_throw_invalid_lhs - "Invalid left-hand side in assignment"
 */
R8EValue r8e_throw_invalid_lhs(R8EContext *ctx) {
    return r8e_throw_reference_error(ctx,
        "Invalid left-hand side in assignment");
}

/**
 * r8e_throw_stack_overflow - "Maximum call stack size exceeded"
 */
R8EValue r8e_throw_stack_overflow(R8EContext *ctx) {
    return r8e_throw_range_error(ctx,
        "Maximum call stack size exceeded");
}

/**
 * r8e_throw_not_iterable - "X is not iterable"
 */
R8EValue r8e_throw_not_iterable(R8EContext *ctx, const char *type_name) {
    if (type_name && type_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "%s is not iterable", type_name);
    }
    return r8e_throw_type_error(ctx, "Value is not iterable");
}

/**
 * r8e_throw_not_an_object - "X is not an object"
 */
R8EValue r8e_throw_not_an_object(R8EContext *ctx, const char *type_name) {
    if (type_name && type_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "%s is not an object", type_name);
    }
    return r8e_throw_type_error(ctx, "Value is not an object");
}

/**
 * r8e_throw_readonly - "Cannot assign to read only property 'X'"
 */
R8EValue r8e_throw_readonly(R8EContext *ctx, const char *prop_name) {
    if (prop_name && prop_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "Cannot assign to read only property '%s'", prop_name);
    }
    return r8e_throw_type_error(ctx,
        "Cannot assign to read only property");
}

/**
 * r8e_throw_not_extensible - "Cannot add property X, object is not extensible"
 */
R8EValue r8e_throw_not_extensible(R8EContext *ctx, const char *prop_name) {
    if (prop_name && prop_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "Cannot add property '%s', object is not extensible",
            prop_name);
    }
    return r8e_throw_type_error(ctx,
        "Cannot add property, object is not extensible");
}

/**
 * r8e_throw_redeclaration - "Identifier 'X' has already been declared"
 */
R8EValue r8e_throw_redeclaration(R8EContext *ctx, const char *name) {
    if (name && name[0] != '\0') {
        return r8e_throw_syntax_error(ctx,
            "Identifier '%s' has already been declared", name);
    }
    return r8e_throw_syntax_error(ctx,
        "Identifier has already been declared");
}

/**
 * r8e_throw_uninitialized - "Cannot access 'X' before initialization"
 */
R8EValue r8e_throw_uninitialized(R8EContext *ctx, const char *name) {
    if (name && name[0] != '\0') {
        return r8e_throw_reference_error(ctx,
            "Cannot access '%s' before initialization", name);
    }
    return r8e_throw_reference_error(ctx,
        "Cannot access variable before initialization");
}

/**
 * r8e_throw_not_a_constructor - "X is not a constructor"
 */
R8EValue r8e_throw_not_a_constructor(R8EContext *ctx, const char *name) {
    if (name && name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "%s is not a constructor", name);
    }
    return r8e_throw_type_error(ctx, "Value is not a constructor");
}

/**
 * r8e_throw_invalid_array_length - "Invalid array length"
 */
R8EValue r8e_throw_invalid_array_length(R8EContext *ctx) {
    return r8e_throw_range_error(ctx, "Invalid array length");
}

/**
 * r8e_throw_cannot_convert_to_primitive -
 *   "Cannot convert object to primitive value"
 */
R8EValue r8e_throw_cannot_convert_to_primitive(R8EContext *ctx) {
    return r8e_throw_type_error(ctx,
        "Cannot convert object to primitive value");
}

/**
 * r8e_throw_super_not_constructor -
 *   "Super constructor null/undefined is not a constructor"
 */
R8EValue r8e_throw_super_not_constructor(R8EContext *ctx, bool is_null) {
    const char *type_str = is_null ? "null" : "undefined";
    return r8e_throw_type_error(ctx,
        "Super constructor %s is not a constructor", type_str);
}

/**
 * r8e_throw_delete_super_prop - "Cannot delete super property"
 */
R8EValue r8e_throw_delete_super_prop(R8EContext *ctx) {
    return r8e_throw_reference_error(ctx,
        "Unsupported reference to 'super'");
}

/**
 * r8e_throw_not_configurable - "Cannot redefine property 'X'"
 */
R8EValue r8e_throw_not_configurable(R8EContext *ctx,
                                     const char *prop_name) {
    if (prop_name && prop_name[0] != '\0') {
        return r8e_throw_type_error(ctx,
            "Cannot redefine property '%s'", prop_name);
    }
    return r8e_throw_type_error(ctx, "Cannot redefine property");
}


/* =========================================================================
 * Section 8: Error.prototype built-in methods
 * ========================================================================= */

typedef R8EValue (*R8ENativeFunc_ctx)(void *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv);

/**
 * Native implementation of Error.prototype.toString.
 *
 * Returns "ErrorType: message" or just "ErrorType" if no message.
 */
static R8EValue r8e_builtin_error_toString(void *raw_ctx,
                                            R8EValue this_val,
                                            int argc,
                                            const R8EValue *argv) {
    (void)argc;
    (void)argv;
    R8EContext *ctx = (R8EContext *)raw_ctx;
    (void)ctx;

    if (!R8E_IS_POINTER(this_val)) {
        return R8E_UNDEFINED;
    }

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(this_val);
    if (!err) return R8E_UNDEFINED;

    /* Get the type name */
    const char *type_name = "Error";
    if (err->error_type < R8E_ERROR_TYPE_COUNT) {
        type_name = r8e_error_type_names[err->error_type];
    }

    /* If message is empty, return just the type name */
    if (!err->message || err->message[0] == '\0') {
        int len = (int)strlen(type_name);
        if (len <= 7) {
            /* Fits in inline short string */
            uint64_t v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)(unsigned)len << 45);
            for (int i = 0; i < len; i++) {
                v |= ((uint64_t)(uint8_t)type_name[i] << (38 - i * 7));
            }
            return v;
        }
        /* Need heap string; return atom reference as fallback */
        if (err->error_type < R8E_ERROR_TYPE_COUNT) {
            return 0xFFFC000000000000ULL |
                   (uint64_t)r8e_error_type_atoms[err->error_type];
        }
        return R8E_UNDEFINED;
    }

    /* "ErrorType: message" - likely too long for inline string */
    /* In the fully integrated engine, this creates a heap string via
     * r8e_string.c. For now, return the atom for the error type. */
    if (err->error_type < R8E_ERROR_TYPE_COUNT) {
        return 0xFFFC000000000000ULL |
               (uint64_t)r8e_error_type_atoms[err->error_type];
    }

    return R8E_UNDEFINED;
}


/* =========================================================================
 * Section 9: Error constructor natives
 *
 * These implement the Error(), TypeError(), etc. constructors that
 * can be called with new or without.
 * ========================================================================= */

/**
 * Common error constructor implementation.
 * Works for all error subtypes.
 */
static R8EValue r8e_error_constructor_common(void *raw_ctx,
                                              R8EValue this_val,
                                              int argc,
                                              const R8EValue *argv,
                                              R8EErrorType type) {
    R8EContext *ctx = (R8EContext *)raw_ctx;
    (void)this_val;

    const char *message = NULL;
    char inline_buf[8];

    /* Extract message from first argument */
    if (argc > 0 && !R8E_IS_UNDEFINED(argv[0])) {
        /* Check for inline string */
        if (R8E_IS_INLINE_STR(argv[0])) {
            int len = (int)((argv[0] >> 45) & 0x7);
            for (int i = 0; i < len && i < 7; i++) {
                inline_buf[i] = (char)((argv[0] >> (38 - i * 7)) & 0x7F);
            }
            inline_buf[len] = '\0';
            message = inline_buf;
        }
        /* For heap strings, would need r8e_string.c integration */
    }

    return r8e_error_new(ctx, type, message);
}

/**
 * Native Error constructor: new Error(message)
 */
static R8EValue r8e_builtin_error_ctor(void *raw_ctx,
                                        R8EValue this_val,
                                        int argc,
                                        const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_ERROR);
}

/**
 * Native TypeError constructor: new TypeError(message)
 */
static R8EValue r8e_builtin_type_error_ctor(void *raw_ctx,
                                             R8EValue this_val,
                                             int argc,
                                             const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_TYPE_ERROR);
}

/**
 * Native RangeError constructor: new RangeError(message)
 */
static R8EValue r8e_builtin_range_error_ctor(void *raw_ctx,
                                              R8EValue this_val,
                                              int argc,
                                              const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_RANGE_ERROR);
}

/**
 * Native ReferenceError constructor: new ReferenceError(message)
 */
static R8EValue r8e_builtin_reference_error_ctor(void *raw_ctx,
                                                  R8EValue this_val,
                                                  int argc,
                                                  const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_REFERENCE_ERROR);
}

/**
 * Native SyntaxError constructor: new SyntaxError(message)
 */
static R8EValue r8e_builtin_syntax_error_ctor(void *raw_ctx,
                                               R8EValue this_val,
                                               int argc,
                                               const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_SYNTAX_ERROR);
}

/**
 * Native URIError constructor: new URIError(message)
 */
static R8EValue r8e_builtin_uri_error_ctor(void *raw_ctx,
                                            R8EValue this_val,
                                            int argc,
                                            const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_URI_ERROR);
}

/**
 * Native EvalError constructor: new EvalError(message)
 */
static R8EValue r8e_builtin_eval_error_ctor(void *raw_ctx,
                                             R8EValue this_val,
                                             int argc,
                                             const R8EValue *argv) {
    return r8e_error_constructor_common(
        raw_ctx, this_val, argc, argv, R8E_ERROR_TYPE_EVAL_ERROR);
}

/**
 * Native AggregateError constructor: new AggregateError(errors, message)
 */
static R8EValue r8e_builtin_aggregate_error_ctor(void *raw_ctx,
                                                  R8EValue this_val,
                                                  int argc,
                                                  const R8EValue *argv) {
    R8EContext *ctx = (R8EContext *)raw_ctx;
    (void)this_val;

    const char *message = NULL;
    char inline_buf[8];

    /* Second argument is the message */
    if (argc > 1 && !R8E_IS_UNDEFINED(argv[1])) {
        if (R8E_IS_INLINE_STR(argv[1])) {
            int len = (int)((argv[1] >> 45) & 0x7);
            for (int i = 0; i < len && i < 7; i++) {
                inline_buf[i] = (char)((argv[1] >> (38 - i * 7)) & 0x7F);
            }
            inline_buf[len] = '\0';
            message = inline_buf;
        }
    }

    /* First argument is the errors iterable.
     * In the fully integrated engine, we would iterate over it.
     * For now, create an AggregateError with no sub-errors. */
    return r8e_aggregate_error_new(ctx, message, NULL, 0);
}


/* =========================================================================
 * Section 10: Error constructor registration table
 * ========================================================================= */

typedef R8EValue (*R8ENativeFunc_raw)(void *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv);

typedef struct {
    uint32_t         name_atom;
    uint32_t         proto_id;
    R8ENativeFunc_raw constructor;
} R8EErrorCtorEntry;

static const R8EErrorCtorEntry r8e_error_constructors[] = {
    { R8E_ATOM_Error,          R8E_PROTO_ERROR,
      r8e_builtin_error_ctor },
    { R8E_ATOM_TypeError,      R8E_PROTO_TYPEERROR,
      r8e_builtin_type_error_ctor },
    { R8E_ATOM_RangeError,     R8E_PROTO_RANGEERROR,
      r8e_builtin_range_error_ctor },
    { R8E_ATOM_ReferenceError, R8E_PROTO_REFERENCEERROR,
      r8e_builtin_reference_error_ctor },
    { R8E_ATOM_SyntaxError,    R8E_PROTO_SYNTAXERROR,
      r8e_builtin_syntax_error_ctor },
    { R8E_ATOM_URIError,       R8E_PROTO_URIERROR,
      r8e_builtin_uri_error_ctor },
    { R8E_ATOM_EvalError,      R8E_PROTO_EVALERROR,
      r8e_builtin_eval_error_ctor },
    { R8E_ATOM_AggregateError, R8E_PROTO_ERROR,
      r8e_builtin_aggregate_error_ctor },
    { 0, 0, NULL } /* sentinel */
};

/**
 * r8e_error_constructor_count - return the number of error constructors.
 */
int r8e_error_constructor_count(void) {
    int count = 0;
    while (r8e_error_constructors[count].constructor != NULL) {
        count++;
    }
    return count;
}

/**
 * r8e_error_constructor_get - get an error constructor entry by index.
 */
bool r8e_error_constructor_get(int index, uint32_t *name_atom,
                                uint32_t *proto_id,
                                R8ENativeFunc_raw *constructor) {
    if (index < 0 || r8e_error_constructors[index].constructor == NULL) {
        return false;
    }
    if (name_atom) *name_atom = r8e_error_constructors[index].name_atom;
    if (proto_id) *proto_id = r8e_error_constructors[index].proto_id;
    if (constructor) *constructor = r8e_error_constructors[index].constructor;
    return true;
}


/* =========================================================================
 * Section 11: GC integration
 * ========================================================================= */

/**
 * r8e_error_visit_refs - visit all heap references in an error object.
 */
void r8e_error_visit_refs(const R8EErrorObject *err,
                           R8EGCVisitorFn visitor, void *ud) {
    if (!err) return;

    /* Visit sub-errors (AggregateError) */
    if (err->errors) {
        for (uint32_t i = 0; i < err->errors_count; i++) {
            if (R8E_IS_POINTER(err->errors[i])) {
                R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(
                    err->errors[i]);
                if (ref) visitor(ref, ud);
            }
        }
    }
}

/**
 * r8e_error_is_error - check if a NaN-boxed value is an error object.
 *
 * Inspects the GC header kind bits.
 */
bool r8e_error_is_error(R8EValue val) {
    if (!R8E_IS_POINTER(val)) return false;

    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);
    if (!hdr) return false;

    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);
    return kind == R8E_GC_KIND_ERROR;
}


/* =========================================================================
 * Section 12: Exception handling helpers for try/catch/finally
 *
 * These functions are used by the interpreter when processing
 * TRY_PUSH, TRY_POP, CATCH, FINALLY, and THROW opcodes.
 * ========================================================================= */

/**
 * R8ETryHandler - a single try/catch/finally handler on the handler stack.
 *
 * Each TRY_PUSH opcode creates one of these. TRY_POP removes it.
 * When an exception is thrown, the interpreter searches the handler
 * stack from top to bottom for the innermost enclosing handler.
 */
typedef struct R8ETryHandler {
    uint32_t catch_offset;     /* bytecode offset of catch block (0 = none) */
    uint32_t finally_offset;   /* bytecode offset of finally block (0 = none) */
    uint32_t stack_depth;      /* operand stack depth when TRY_PUSH was executed */
    uint32_t frame_depth;      /* call frame depth when TRY_PUSH was executed */
} R8ETryHandler;

#define R8E_MAX_TRY_HANDLERS 32

/**
 * R8ETryStack - stack of try/catch/finally handlers.
 *
 * This is part of the interpreter state, managed per call frame.
 */
typedef struct R8ETryStack {
    R8ETryHandler handlers[R8E_MAX_TRY_HANDLERS];
    uint32_t      count;
} R8ETryStack;

/**
 * r8e_try_stack_init - initialize a try handler stack.
 */
void r8e_try_stack_init(R8ETryStack *ts) {
    assert(ts != NULL);
    ts->count = 0;
}

/**
 * r8e_try_stack_push - push a new try/catch handler.
 *
 * @param ts              Try handler stack
 * @param catch_offset    Bytecode offset of catch block
 * @param finally_offset  Bytecode offset of finally block
 * @param stack_depth     Current operand stack depth
 * @param frame_depth     Current call frame depth
 * @return                True on success, false if stack is full
 */
bool r8e_try_stack_push(R8ETryStack *ts,
                         uint32_t catch_offset,
                         uint32_t finally_offset,
                         uint32_t stack_depth,
                         uint32_t frame_depth) {
    assert(ts != NULL);

    if (ts->count >= R8E_MAX_TRY_HANDLERS) {
        return false;
    }

    R8ETryHandler *h = &ts->handlers[ts->count];
    h->catch_offset = catch_offset;
    h->finally_offset = finally_offset;
    h->stack_depth = stack_depth;
    h->frame_depth = frame_depth;
    ts->count++;

    return true;
}

/**
 * r8e_try_stack_pop - pop the top try/catch handler.
 */
bool r8e_try_stack_pop(R8ETryStack *ts) {
    assert(ts != NULL);

    if (ts->count == 0) {
        return false;
    }

    ts->count--;
    return true;
}

/**
 * r8e_try_stack_top - get the top handler without popping.
 */
const R8ETryHandler *r8e_try_stack_top(const R8ETryStack *ts) {
    assert(ts != NULL);

    if (ts->count == 0) {
        return NULL;
    }

    return &ts->handlers[ts->count - 1];
}

/**
 * r8e_try_stack_has_handler - check if there is an active handler.
 */
bool r8e_try_stack_has_handler(const R8ETryStack *ts) {
    assert(ts != NULL);
    return ts->count > 0;
}

/**
 * r8e_try_stack_find_handler - find the handler for the current exception.
 *
 * Searches from the top of the handler stack for a handler whose
 * frame_depth matches or is above the current depth.
 *
 * @param ts           Try handler stack
 * @param frame_depth  Current call frame depth
 * @return             Pointer to the handler, or NULL if none found
 */
const R8ETryHandler *r8e_try_stack_find_handler(const R8ETryStack *ts,
                                                 uint32_t frame_depth) {
    assert(ts != NULL);

    for (int i = (int)ts->count - 1; i >= 0; i--) {
        if (ts->handlers[i].frame_depth <= frame_depth) {
            return &ts->handlers[i];
        }
    }

    return NULL;
}


/* =========================================================================
 * Section 13: Error object property resolution
 *
 * When JS code accesses error.name, error.message, or error.stack,
 * the property lookup must be intercepted to return the correct value.
 * These functions are called by the object model's property lookup
 * when the object is detected to be an error.
 * ========================================================================= */

/**
 * r8e_error_get_prop - get a property from an error object.
 *
 * Handles the well-known error properties (name, message, stack).
 * For other properties, falls through to the normal object model.
 *
 * @param err        Error object
 * @param prop_atom  Property name atom
 * @return           Property value, or R8E_UNDEFINED if not found
 */
R8EValue r8e_error_get_prop(const R8EErrorObject *err, uint32_t prop_atom) {
    if (!err) return R8E_UNDEFINED;

    switch (prop_atom) {
    case R8E_ATOM_name:
        /* Return the error type name as an atom value */
        if (err->error_type < R8E_ERROR_TYPE_COUNT) {
            return 0xFFFC000000000000ULL |
                   (uint64_t)r8e_error_type_atoms[err->error_type];
        }
        return 0xFFFC000000000000ULL | (uint64_t)R8E_ATOM_Error;

    case R8E_ATOM_message:
        /* Return message as inline string if short enough */
        if (!err->message || err->message_len == 0) {
            /* Empty string */
            return 0xFFFD000000000000ULL;
        }
        if (err->message_len <= 7) {
            /* Try inline encoding */
            uint64_t v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)(unsigned)err->message_len << 45);
            bool ascii_ok = true;
            for (uint32_t i = 0; i < err->message_len; i++) {
                uint8_t ch = (uint8_t)err->message[i];
                if (ch > 127) { ascii_ok = false; break; }
                v |= ((uint64_t)ch << (38 - i * 7));
            }
            if (ascii_ok) return v;
        }
        /* For longer messages, would return a heap string pointer.
         * Requires r8e_string.c integration. */
        return R8E_UNDEFINED;

    case R8E_ATOM_stack:
        /* Stack trace is typically too long for inline string.
         * Return R8E_UNDEFINED until string module integration. */
        return R8E_UNDEFINED;

    case R8E_ATOM_constructor:
        /* Would return the constructor function for this error type.
         * Requires realm prototype table access. */
        return R8E_UNDEFINED;

    default:
        return R8E_UNDEFINED;
    }
}
