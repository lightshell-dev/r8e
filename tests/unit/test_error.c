/*
 * test_error.c - Unit tests for r8e_error.c
 *
 * Tests cover:
 *   - Error object creation (all types)
 *   - Error type names and prototype IDs
 *   - Stack trace capture and formatting
 *   - Exception throw/get/has/clear lifecycle
 *   - Typed throw helpers (type_error, range_error, etc.)
 *   - printf-style formatted error messages
 *   - Common pre-formatted errors (not_a_function, cannot_read_prop, etc.)
 *   - AggregateError with sub-errors
 *   - Error property accessors (get_name, get_message, get_stack, etc.)
 *   - Edge cases (NULL ctx, NULL message, empty frames)
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* =========================================================================
 * Type definitions (must match r8e_error.c layout)
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

/* GC header */
#define R8E_GC_KIND_MASK         0x000000E0u
#define R8E_GC_KIND_SHIFT        5
#define R8E_GC_KIND_ERROR        6u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_PROTO_ERROR         9

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* Error type enumeration (must match r8e_error.c) */
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

/* Stack frame */
typedef struct R8EStackFrame {
    const char *function_name;
    const char *source_name;
    uint32_t    line;
    uint32_t    col;
} R8EStackFrame;

/* Error object (must match r8e_error.c) */
typedef struct R8EErrorObject {
    uint32_t       flags;
    uint32_t       proto_id;
    R8EErrorType   error_type;
    char          *message;
    uint32_t       message_len;
    char          *stack_str;
    uint32_t       stack_str_len;
    R8EStackFrame *frames;
    uint16_t       frame_count;
    uint16_t       frame_capacity;
    R8EValue      *errors;
    uint32_t       errors_count;
} R8EErrorObject;

/* Context types (must match r8e_error.c) */
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
 * External function declarations (from r8e_error.c)
 * ========================================================================= */

extern R8EErrorObject *r8e_error_object_new(R8EContext *ctx,
                                              R8EErrorType error_type,
                                              const char *message,
                                              uint32_t message_len);
extern void r8e_error_object_free(R8EContext *ctx, R8EErrorObject *err);

extern R8EValue r8e_error_new(R8EContext *ctx, R8EErrorType error_type,
                                const char *message);
extern R8EValue r8e_error_new_fmt(R8EContext *ctx, R8EErrorType error_type,
                                    const char *fmt, ...);

extern R8EValue r8e_throw(R8EContext *ctx, R8EValue value);
extern R8EValue r8e_get_exception(const R8EContext *ctx);
extern bool r8e_has_exception(const R8EContext *ctx);
extern R8EValue r8e_clear_exception(R8EContext *ctx);

extern R8EValue r8e_throw_type_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_range_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_reference_error(R8EContext *ctx,
                                            const char *fmt, ...);
extern R8EValue r8e_throw_syntax_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_uri_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_eval_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_error(R8EContext *ctx, const char *fmt, ...);
extern R8EValue r8e_throw_internal_error(R8EContext *ctx,
                                           const char *fmt, ...);

extern R8EValue r8e_aggregate_error_new(R8EContext *ctx,
                                          const char *message,
                                          const R8EValue *errors,
                                          uint32_t error_count);

extern R8EErrorType r8e_error_get_type(R8EValue error_val);
extern const char *r8e_error_get_message(R8EValue error_val);
extern const char *r8e_error_get_stack(R8EValue error_val);
extern const char *r8e_error_get_name(R8EValue error_val);
extern R8EValue r8e_error_get_name_value(R8EValue error_val);
extern uint32_t r8e_error_get_line(R8EValue error_val);
extern uint32_t r8e_error_get_column(R8EValue error_val);
extern const char *r8e_error_get_source(R8EValue error_val);

extern void r8e_capture_stack_trace(R8EContext *ctx, R8EErrorObject *err,
                                      uint16_t max_depth);
extern int r8e_format_stack_trace(const R8EErrorObject *err,
                                    char *buf, int buf_size);

extern R8EValue r8e_throw_not_a_function(R8EContext *ctx, const char *name);
extern R8EValue r8e_throw_cannot_read_prop(R8EContext *ctx,
                                             const char *prop_name,
                                             bool is_null);
extern R8EValue r8e_throw_cannot_set_prop(R8EContext *ctx,
                                            const char *prop_name,
                                            bool is_null);
extern R8EValue r8e_throw_not_defined(R8EContext *ctx, const char *name);
extern R8EValue r8e_throw_const_assign(R8EContext *ctx, const char *name);
extern R8EValue r8e_throw_invalid_lhs(R8EContext *ctx);
extern R8EValue r8e_throw_stack_overflow(R8EContext *ctx);

/* =========================================================================
 * Stubs for external functions that r8e_error.c calls
 * ========================================================================= */

void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind, uint32_t proto_id) {
    hdr->flags = (kind << R8E_GC_KIND_SHIFT) | (1u << R8E_GC_RC_INLINE_SHIFT);
    hdr->proto_id = proto_id;
}

void r8e_retain(R8EContext *ctx, R8EValue val) {
    (void)ctx; (void)val;
}

void r8e_release(R8EContext *ctx, R8EValue val) {
    (void)ctx; (void)val;
}

/* =========================================================================
 * Test Harness
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #e); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        printf("  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
               _a ? _a : "(null)", _b ? _b : "(null)"); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_STR_CONTAINS(haystack, needle) do { \
    const char *_h = (haystack), *_n = (needle); \
    if (!_h || !_n || !strstr(_h, _n)) { \
        printf("  FAIL: %s:%d: \"%s\" does not contain \"%s\"\n", \
               __FILE__, __LINE__, _h ? _h : "(null)", _n ? _n : "(null)"); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; g_tests_run++; \
    printf("  %s... ", #fn); \
    fn(); \
    if (g_assert_fail) { g_tests_failed++; } \
    else { g_tests_passed++; printf("OK\n"); } \
} while (0)

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

/* Helper: set up a fake call frame for stack trace testing */
static void push_fake_frame(R8EContext *ctx, R8EFunction *func,
                             const char *source, uint32_t line, uint16_t col) {
    if (ctx->frame_count >= R8E_MAX_CALL_FRAMES) return;
    R8ECallFrame *cf = &ctx->call_frames[ctx->frame_count++];
    memset(cf, 0, sizeof(*cf));
    cf->func = func;
    cf->source_name = source;
    cf->line = line;
    cf->col = col;
}

/* =========================================================================
 * Tests: Error Object Creation
 * ========================================================================= */

/* Create a basic Error object and verify fields */
static void test_error_create_basic(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "test error", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_ERROR);
    ASSERT_EQ_STR(err->message, "test error");
    ASSERT_EQ_INT(err->message_len, 10);

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create a TypeError and verify type */
static void test_error_create_type_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_TYPE_ERROR, "not a function", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_EQ_STR(err->message, "not a function");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create a RangeError */
static void test_error_create_range_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_RANGE_ERROR, "out of range", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_RANGE_ERROR);
    ASSERT_EQ_STR(err->message, "out of range");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create with NULL message */
static void test_error_create_null_message(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, NULL, 0);
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(err->message);
    ASSERT_EQ_INT(err->message_len, 0);

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create with explicit message length */
static void test_error_create_explicit_len(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Only take first 4 bytes of the message */
    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "test error", 4);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_STR(err->message, "test");
    ASSERT_EQ_INT(err->message_len, 4);

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* NULL context returns NULL */
static void test_error_create_null_ctx(void) {
    R8EErrorObject *err = r8e_error_object_new(
        NULL, R8E_ERROR_TYPE_ERROR, "test", 0);
    ASSERT_NULL(err);
}

/* All error types have correct names */
static void test_error_all_type_names(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    struct {
        R8EErrorType type;
        const char  *expected_name;
    } cases[] = {
        { R8E_ERROR_TYPE_ERROR,           "Error" },
        { R8E_ERROR_TYPE_TYPE_ERROR,      "TypeError" },
        { R8E_ERROR_TYPE_RANGE_ERROR,     "RangeError" },
        { R8E_ERROR_TYPE_REFERENCE_ERROR, "ReferenceError" },
        { R8E_ERROR_TYPE_SYNTAX_ERROR,    "SyntaxError" },
        { R8E_ERROR_TYPE_URI_ERROR,       "URIError" },
        { R8E_ERROR_TYPE_EVAL_ERROR,      "EvalError" },
        { R8E_ERROR_TYPE_AGGREGATE_ERROR, "AggregateError" },
        { R8E_ERROR_TYPE_INTERNAL,        "InternalError" },
    };
    int n = sizeof(cases) / sizeof(cases[0]);

    for (int i = 0; i < n; i++) {
        R8EValue val = r8e_error_new(ctx, cases[i].type, "msg");
        ASSERT_TRUE(R8E_IS_POINTER(val));
        ASSERT_EQ_STR(r8e_error_get_name(val), cases[i].expected_name);

        /* Clean up */
        R8EErrorObject *obj = (R8EErrorObject *)r8e_get_pointer(val);
        r8e_error_object_free(ctx, obj);
    }

    free(ctx);
}

/* =========================================================================
 * Tests: Error Value Creation
 * ========================================================================= */

/* r8e_error_new returns a NaN-boxed pointer */
static void test_error_new_returns_pointer(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "hello");
    ASSERT_TRUE(R8E_IS_POINTER(val));

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_STR(err->message, "hello");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_error_new_fmt formats the message */
static void test_error_new_fmt(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new_fmt(ctx, R8E_ERROR_TYPE_TYPE_ERROR,
                                      "%s is not a %s", "foo", "number");
    ASSERT_TRUE(R8E_IS_POINTER(val));

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_STR(err->message, "foo is not a number");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * Tests: Exception Lifecycle (throw/has/get/clear)
 * ========================================================================= */

/* Initially no exception is pending */
static void test_exception_initially_none(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    ASSERT_FALSE(r8e_has_exception(ctx));
    ASSERT_EQ(r8e_get_exception(ctx), R8E_UNDEFINED);

    free(ctx);
}

/* Throw sets exception */
static void test_exception_throw(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_from_int32(42);
    R8EValue ret = r8e_throw(ctx, val);
    ASSERT_EQ(ret, R8E_UNDEFINED);  /* throw returns undefined */
    ASSERT_TRUE(r8e_has_exception(ctx));
    ASSERT_EQ(r8e_get_exception(ctx), val);

    free(ctx);
}

/* Clear exception returns the value and resets state */
static void test_exception_clear(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_from_int32(99);
    r8e_throw(ctx, val);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue cleared = r8e_clear_exception(ctx);
    ASSERT_EQ(cleared, val);
    ASSERT_FALSE(r8e_has_exception(ctx));
    ASSERT_EQ(r8e_get_exception(ctx), R8E_UNDEFINED);

    free(ctx);
}

/* Clear when no exception returns undefined */
static void test_exception_clear_when_none(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cleared = r8e_clear_exception(ctx);
    ASSERT_EQ(cleared, R8E_UNDEFINED);

    free(ctx);
}

/* Throwing twice replaces the exception */
static void test_exception_throw_twice(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw(ctx, r8e_from_int32(1));
    r8e_throw(ctx, r8e_from_int32(2));
    ASSERT_TRUE(r8e_has_exception(ctx));
    ASSERT_EQ(r8e_get_exception(ctx), r8e_from_int32(2));

    free(ctx);
}

/* Throw with NULL ctx does not crash */
static void test_exception_throw_null_ctx(void) {
    R8EValue ret = r8e_throw(NULL, r8e_from_int32(1));
    ASSERT_EQ(ret, R8E_UNDEFINED);
    ASSERT_FALSE(r8e_has_exception(NULL));
}

/* =========================================================================
 * Tests: Typed Throw Helpers
 * ========================================================================= */

/* r8e_throw_type_error sets a TypeError exception */
static void test_throw_type_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_type_error(ctx, "X is not a function");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "TypeError");
    ASSERT_EQ_STR(r8e_error_get_message(exc), "X is not a function");

    /* Clean up */
    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_range_error sets a RangeError exception */
static void test_throw_range_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_range_error(ctx, "Index %d out of range", 42);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_RANGE_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "RangeError");
    ASSERT_EQ_STR(r8e_error_get_message(exc), "Index 42 out of range");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_reference_error */
static void test_throw_reference_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_reference_error(ctx, "x is not defined");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_REFERENCE_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "ReferenceError");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_syntax_error */
static void test_throw_syntax_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_syntax_error(ctx, "Unexpected token '}'");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_SYNTAX_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "SyntaxError");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * Tests: Stack Trace Capture and Formatting
 * ========================================================================= */

/* Stack trace with no frames */
static void test_stack_trace_empty(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "test", 0);
    ASSERT_NOT_NULL(err);
    /* No frames in context, so no stack frames captured */
    ASSERT_EQ_INT(err->frame_count, 0);

    /* Format should just have the error header */
    char buf[256];
    int len = r8e_format_stack_trace(err, buf, sizeof(buf));
    ASSERT_TRUE(len > 0);
    ASSERT_STR_CONTAINS(buf, "Error: test");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Stack trace with frames */
static void test_stack_trace_with_frames(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Set up fake call stack */
    R8EFunction func1 = {0};
    func1.name_atom = 42;  /* non-zero = named function */
    func1.source_name = "app.js";

    R8EFunction func2 = {0};
    func2.name_atom = 0;   /* zero = anonymous */
    func2.source_name = "lib.js";

    push_fake_frame(ctx, &func1, "app.js", 10, 5);
    push_fake_frame(ctx, &func2, "lib.js", 20, 15);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_TYPE_ERROR, "bad value", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->frame_count, 2);

    /* Frames are captured top-to-bottom (most recent first) */
    ASSERT_EQ_INT(err->frames[0].line, 20);
    ASSERT_EQ_INT(err->frames[1].line, 10);

    /* Format stack trace */
    char buf[512];
    int len = r8e_format_stack_trace(err, buf, sizeof(buf));
    ASSERT_TRUE(len > 0);
    ASSERT_STR_CONTAINS(buf, "TypeError: bad value");
    ASSERT_STR_CONTAINS(buf, "at ");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Stack trace formatting: query required size with NULL buffer */
static void test_stack_trace_query_size(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "hello", 0);
    ASSERT_NOT_NULL(err);

    int required = r8e_format_stack_trace(err, NULL, 0);
    ASSERT_TRUE(required > 0);

    /* Allocate exact size and format */
    char *buf = (char *)malloc(required + 1);
    ASSERT_NOT_NULL(buf);
    r8e_format_stack_trace(err, buf, required + 1);
    ASSERT_STR_CONTAINS(buf, "Error: hello");

    free(buf);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * Tests: Error Property Accessors
 * ========================================================================= */

/* r8e_error_get_type returns correct type */
static void test_error_get_type(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_SYNTAX_ERROR, "bad");
    ASSERT_EQ_INT(r8e_error_get_type(val), R8E_ERROR_TYPE_SYNTAX_ERROR);

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_error_get_message returns message string */
static void test_error_get_message(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "hello world");
    ASSERT_EQ_STR(r8e_error_get_message(val), "hello world");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_error_get_message returns "" for non-pointer */
static void test_error_get_message_non_pointer(void) {
    ASSERT_EQ_STR(r8e_error_get_message(R8E_UNDEFINED), "");
    ASSERT_EQ_STR(r8e_error_get_message(r8e_from_int32(42)), "");
}

/* r8e_error_get_name for non-pointer returns "Error" */
static void test_error_get_name_non_pointer(void) {
    ASSERT_EQ_STR(r8e_error_get_name(R8E_UNDEFINED), "Error");
    ASSERT_EQ_STR(r8e_error_get_name(R8E_NULL), "Error");
}

/* r8e_error_get_stack returns the stack string */
static void test_error_get_stack(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EFunction func = {0};
    func.name_atom = 1;
    func.source_name = "test.js";
    push_fake_frame(ctx, &func, "test.js", 5, 3);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "oops");
    const char *stack = r8e_error_get_stack(val);
    ASSERT_NOT_NULL(stack);
    ASSERT_TRUE(strlen(stack) > 0);
    ASSERT_STR_CONTAINS(stack, "Error: oops");
    ASSERT_STR_CONTAINS(stack, "at ");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_error_get_stack returns "" for non-pointer */
static void test_error_get_stack_non_pointer(void) {
    ASSERT_EQ_STR(r8e_error_get_stack(R8E_UNDEFINED), "");
}

/* r8e_error_get_line returns line from first frame */
static void test_error_get_line(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EFunction func = {0};
    func.source_name = "app.js";
    push_fake_frame(ctx, &func, "app.js", 42, 10);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "msg");
    ASSERT_EQ_INT(r8e_error_get_line(val), 42);
    ASSERT_EQ_INT(r8e_error_get_column(val), 10);
    ASSERT_EQ_STR(r8e_error_get_source(val), "app.js");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_error_get_line returns 0 for no frames */
static void test_error_get_line_no_frames(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "msg");
    ASSERT_EQ_INT(r8e_error_get_line(val), 0);
    ASSERT_EQ_INT(r8e_error_get_column(val), 0);
    ASSERT_NULL(r8e_error_get_source(val));

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(val);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * Tests: AggregateError
 * ========================================================================= */

/* Create AggregateError with sub-errors */
static void test_aggregate_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue sub1 = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "err1");
    R8EValue sub2 = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "err2");
    R8EValue subs[] = { sub1, sub2 };

    R8EValue agg = r8e_aggregate_error_new(ctx, "All failed", subs, 2);
    ASSERT_TRUE(R8E_IS_POINTER(agg));

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(agg);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_AGGREGATE_ERROR);
    ASSERT_EQ_STR(err->message, "All failed");
    ASSERT_EQ_INT(err->errors_count, 2);
    ASSERT_NOT_NULL(err->errors);
    ASSERT_EQ(err->errors[0], sub1);
    ASSERT_EQ(err->errors[1], sub2);

    ASSERT_EQ_STR(r8e_error_get_name(agg), "AggregateError");

    /* Clean up sub-errors first */
    R8EErrorObject *s1 = (R8EErrorObject *)r8e_get_pointer(sub1);
    R8EErrorObject *s2 = (R8EErrorObject *)r8e_get_pointer(sub2);
    r8e_error_object_free(ctx, err);
    r8e_error_object_free(ctx, s1);
    r8e_error_object_free(ctx, s2);
    free(ctx);
}

/* AggregateError with zero sub-errors */
static void test_aggregate_error_empty(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue agg = r8e_aggregate_error_new(ctx, "none", NULL, 0);
    ASSERT_TRUE(R8E_IS_POINTER(agg));

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(agg);
    ASSERT_EQ_INT(err->errors_count, 0);
    ASSERT_NULL(err->errors);

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * Tests: Common Pre-formatted Error Helpers
 * ========================================================================= */

/* r8e_throw_not_a_function with name */
static void test_throw_not_a_function(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_not_a_function(ctx, "myFunc");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "myFunc");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "not a function");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_not_a_function without name */
static void test_throw_not_a_function_no_name(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_not_a_function(ctx, NULL);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "not a function");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_cannot_read_prop with null */
static void test_throw_cannot_read_prop_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_cannot_read_prop(ctx, "foo", true);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "null");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "foo");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_cannot_read_prop with undefined */
static void test_throw_cannot_read_prop_undefined(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_cannot_read_prop(ctx, "bar", false);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "undefined");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "bar");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_not_defined */
static void test_throw_not_defined(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_not_defined(ctx, "myVar");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_REFERENCE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "myVar");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "not defined");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_const_assign */
static void test_throw_const_assign(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_const_assign(ctx, "MAX");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "constant");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "MAX");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_stack_overflow */
static void test_throw_stack_overflow(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_stack_overflow(ctx);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_RANGE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "call stack");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_invalid_lhs */
static void test_throw_invalid_lhs(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_invalid_lhs(ctx);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_REFERENCE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "left-hand side");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * Tests: GC Integration
 * ========================================================================= */

/* Error creation increments total_allocated */
static void test_error_gc_alloc_count(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    uint32_t before = ctx->gc.total_allocated;
    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "test", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(ctx->gc.total_allocated, before + 1);

    r8e_error_object_free(ctx, err);
    ASSERT_EQ_INT(ctx->gc.total_freed, 1);

    free(ctx);
}

/* =========================================================================
 * NEW: Additional Typed Throw Helpers (URI, Eval, Internal, generic Error)
 * ========================================================================= */

/* r8e_throw_uri_error */
static void test_throw_uri_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_uri_error(ctx, "URI malformed: %s", "%%%");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_URI_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "URIError");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "URI malformed");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_eval_error */
static void test_throw_eval_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_eval_error(ctx, "eval is not supported");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_EVAL_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "EvalError");
    ASSERT_EQ_STR(r8e_error_get_message(exc), "eval is not supported");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_internal_error */
static void test_throw_internal_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_internal_error(ctx, "OOM at line %d", 42);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_INTERNAL);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "InternalError");
    ASSERT_EQ_STR(r8e_error_get_message(exc), "OOM at line 42");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_error (generic Error) */
static void test_throw_generic_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_error(ctx, "something went wrong");
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_ERROR);
    ASSERT_EQ_STR(r8e_error_get_name(exc), "Error");
    ASSERT_EQ_STR(r8e_error_get_message(exc), "something went wrong");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: Error Name Value (atom-based)
 * ========================================================================= */

/* r8e_error_get_name_value for non-pointer returns Error atom */
static void test_error_get_name_value_non_pointer(void) {
    R8EValue nv = r8e_error_get_name_value(R8E_UNDEFINED);
    /* Should be an atom-tagged value */
    ASSERT_TRUE((nv >> 48) == 0xFFFCU);
}

/* r8e_error_get_name_value for a TypeError */
static void test_error_get_name_value_type_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new(ctx, R8E_ERROR_TYPE_TYPE_ERROR, "msg");
    ASSERT_TRUE(R8E_IS_POINTER(val));

    R8EValue nv = r8e_error_get_name_value(val);
    /* Should be an atom-tagged value */
    ASSERT_TRUE((nv >> 48) == 0xFFFCU);

    /* Create a separate plain Error to compare atoms */
    R8EValue val2 = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "msg2");
    ASSERT_TRUE(R8E_IS_POINTER(val2));
    R8EValue error_nv = r8e_error_get_name_value(val2);
    /* At minimum, both should be valid atom values */
    ASSERT_TRUE((error_nv >> 48) == 0xFFFCU);

    /* Clean up both error objects */
    R8EErrorObject *err1 = (R8EErrorObject *)r8e_get_pointer(val);
    R8EErrorObject *err2 = (R8EErrorObject *)r8e_get_pointer(val2);
    r8e_error_object_free(ctx, err1);
    r8e_error_object_free(ctx, err2);
    free(ctx);
}

/* =========================================================================
 * NEW: Cannot Set Property Errors
 * ========================================================================= */

/* r8e_throw_cannot_set_prop with null */
static void test_throw_cannot_set_prop_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_cannot_set_prop(ctx, "x", true);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "null");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "x");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "set");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_cannot_set_prop with undefined */
static void test_throw_cannot_set_prop_undefined(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_cannot_set_prop(ctx, "y", false);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "undefined");
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "y");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_cannot_set_prop with no property name */
static void test_throw_cannot_set_prop_no_name(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_cannot_set_prop(ctx, NULL, true);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "null");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: Stack Trace Edge Cases
 * ========================================================================= */

/* Stack trace max depth limiting: more frames than max_depth */
static void test_stack_trace_max_depth(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Push 10 fake frames */
    R8EFunction funcs[10];
    memset(funcs, 0, sizeof(funcs));
    for (int i = 0; i < 10; i++) {
        funcs[i].source_name = "deep.js";
        funcs[i].name_atom = (uint32_t)(i + 1);
        push_fake_frame(ctx, &funcs[i], "deep.js", (uint32_t)(i + 1), 1);
    }

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "deep", 0);
    ASSERT_NOT_NULL(err);
    /* Should have captured frames (up to the max depth or all 10) */
    ASSERT_TRUE(err->frame_count > 0);
    ASSERT_TRUE(err->frame_count <= 10);

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Explicit capture with depth = 2 */
static void test_stack_trace_explicit_depth(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Push 5 fake frames */
    R8EFunction funcs[5];
    memset(funcs, 0, sizeof(funcs));
    for (int i = 0; i < 5; i++) {
        funcs[i].source_name = "app.js";
        push_fake_frame(ctx, &funcs[i], "app.js", (uint32_t)(i * 10), 0);
    }

    R8EErrorObject *err = (R8EErrorObject *)calloc(1, sizeof(R8EErrorObject));
    ASSERT_NOT_NULL(err);
    err->error_type = R8E_ERROR_TYPE_ERROR;

    r8e_capture_stack_trace(ctx, err, 2);
    ASSERT_EQ_INT(err->frame_count, 2);
    /* Most recent frame first (frame_count-1 = index 4, then 3) */
    ASSERT_EQ_INT(err->frames[0].line, 40);
    ASSERT_EQ_INT(err->frames[1].line, 30);

    free(err->frames);
    free(err);
    free(ctx);
}

/* Format stack trace with tiny buffer causes truncation */
static void test_stack_trace_small_buffer(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EFunction func = {0};
    func.source_name = "main.js";
    push_fake_frame(ctx, &func, "main.js", 100, 5);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "truncation test", 0);
    ASSERT_NOT_NULL(err);

    /* Format into a very small buffer */
    char small_buf[10];
    int required = r8e_format_stack_trace(err, small_buf, sizeof(small_buf));
    ASSERT_TRUE(required > (int)sizeof(small_buf));
    /* Buffer should be null-terminated */
    ASSERT_TRUE(small_buf[sizeof(small_buf) - 1] == '\0');

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Multiple stack frames formatting */
static void test_stack_trace_multiple_frames_format(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EFunction func1 = {0};
    func1.name_atom = 1;
    func1.source_name = "a.js";
    R8EFunction func2 = {0};
    func2.name_atom = 2;
    func2.source_name = "b.js";
    R8EFunction func3 = {0};
    func3.name_atom = 3;
    func3.source_name = "c.js";

    push_fake_frame(ctx, &func1, "a.js", 10, 1);
    push_fake_frame(ctx, &func2, "b.js", 20, 2);
    push_fake_frame(ctx, &func3, "c.js", 30, 3);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_TYPE_ERROR, "multi frame", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->frame_count, 3);

    char buf[1024];
    int len = r8e_format_stack_trace(err, buf, sizeof(buf));
    ASSERT_TRUE(len > 0);
    ASSERT_STR_CONTAINS(buf, "TypeError: multi frame");
    /* All three frames should appear */
    ASSERT_STR_CONTAINS(buf, "a.js");
    ASSERT_STR_CONTAINS(buf, "b.js");
    ASSERT_STR_CONTAINS(buf, "c.js");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Frame with line=0 uses different format (no line:col) */
static void test_stack_trace_zero_line_frame(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EFunction func = {0};
    func.source_name = "native.c";
    push_fake_frame(ctx, &func, "native.c", 0, 0);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "native", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->frame_count, 1);

    char buf[512];
    r8e_format_stack_trace(err, buf, sizeof(buf));
    /* Should use the format without line:col */
    ASSERT_STR_CONTAINS(buf, "at ");
    ASSERT_STR_CONTAINS(buf, "native.c");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: Error Free Edge Cases
 * ========================================================================= */

/* r8e_error_object_free with NULL is safe */
static void test_error_free_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_error_object_free(ctx, NULL);
    /* No crash */

    free(ctx);
}

/* Error with empty message string */
static void test_error_empty_message(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_STR(err->message, "");
    ASSERT_EQ_INT(err->message_len, 0);

    /* Stack trace should just have "Error" without ": " separator when
     * message is empty */
    char buf[256];
    r8e_format_stack_trace(err, buf, sizeof(buf));
    ASSERT_STR_CONTAINS(buf, "Error");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: Error Property Accessors on Edge Cases
 * ========================================================================= */

/* r8e_error_get_line for non-pointer returns 0 */
static void test_error_get_line_non_pointer(void) {
    ASSERT_EQ_INT(r8e_error_get_line(R8E_UNDEFINED), 0);
    ASSERT_EQ_INT(r8e_error_get_line(r8e_from_int32(42)), 0);
}

/* r8e_error_get_column for non-pointer returns 0 */
static void test_error_get_column_non_pointer(void) {
    ASSERT_EQ_INT(r8e_error_get_column(R8E_UNDEFINED), 0);
    ASSERT_EQ_INT(r8e_error_get_column(r8e_from_int32(42)), 0);
}

/* r8e_error_get_source for non-pointer returns NULL */
static void test_error_get_source_non_pointer(void) {
    ASSERT_NULL(r8e_error_get_source(R8E_UNDEFINED));
    ASSERT_NULL(r8e_error_get_source(r8e_from_int32(42)));
}

/* r8e_error_get_type for non-pointer defaults to ERROR */
static void test_error_get_type_non_pointer(void) {
    ASSERT_EQ_INT(r8e_error_get_type(R8E_UNDEFINED), R8E_ERROR_TYPE_ERROR);
    ASSERT_EQ_INT(r8e_error_get_type(R8E_NULL), R8E_ERROR_TYPE_ERROR);
    ASSERT_EQ_INT(r8e_error_get_type(r8e_from_int32(0)), R8E_ERROR_TYPE_ERROR);
}

/* =========================================================================
 * NEW: Throw with format args containing multiple types
 * ========================================================================= */

/* Type error with integer format */
static void test_throw_type_error_fmt_int(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_type_error(ctx, "Expected %d args, got %d", 3, 1);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_STR(r8e_error_get_message(exc), "Expected 3 args, got 1");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: r8e_error_new_fmt edge cases
 * ========================================================================= */

/* r8e_error_new_fmt with NULL ctx returns UNDEFINED */
static void test_error_new_fmt_null_ctx(void) {
    R8EValue val = r8e_error_new_fmt(NULL, R8E_ERROR_TYPE_ERROR, "test");
    ASSERT_EQ(val, R8E_UNDEFINED);
}

/* r8e_error_new_fmt with NULL fmt returns UNDEFINED */
static void test_error_new_fmt_null_fmt(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_error_new_fmt(ctx, R8E_ERROR_TYPE_ERROR, NULL);
    ASSERT_EQ(val, R8E_UNDEFINED);

    free(ctx);
}

/* =========================================================================
 * NEW: Exception lifecycle edge cases
 * ========================================================================= */

/* Throw an error object (not just a primitive) */
static void test_exception_throw_error_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue err_val = r8e_error_new(ctx, R8E_ERROR_TYPE_ERROR, "real error");
    r8e_throw(ctx, err_val);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc));
    ASSERT_EQ_STR(r8e_error_get_message(exc), "real error");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Clear exception returns value for ownership transfer */
static void test_exception_clear_returns_value(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_from_int32(777);
    r8e_throw(ctx, val);

    R8EValue cleared = r8e_clear_exception(ctx);
    ASSERT_EQ(cleared, val);
    /* After clear, getting exception returns undefined */
    ASSERT_EQ(r8e_get_exception(ctx), R8E_UNDEFINED);
    ASSERT_FALSE(r8e_has_exception(ctx));

    free(ctx);
}

/* =========================================================================
 * NEW: AggregateError edge cases
 * ========================================================================= */

/* AggregateError with NULL ctx */
static void test_aggregate_error_null_ctx(void) {
    R8EValue agg = r8e_aggregate_error_new(NULL, "fail", NULL, 0);
    ASSERT_EQ(agg, R8E_UNDEFINED);
}

/* AggregateError with NULL message */
static void test_aggregate_error_null_message(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue agg = r8e_aggregate_error_new(ctx, NULL, NULL, 0);
    ASSERT_TRUE(R8E_IS_POINTER(agg));

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(agg);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_AGGREGATE_ERROR);
    ASSERT_NULL(err->message);

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: Pre-formatted error helpers edge cases
 * ========================================================================= */

/* r8e_throw_cannot_read_prop with no property name */
static void test_throw_cannot_read_prop_no_name(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_cannot_read_prop(ctx, NULL, true);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "null");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_not_defined with NULL name */
static void test_throw_not_defined_null_name(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_not_defined(ctx, NULL);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_REFERENCE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "not defined");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* r8e_throw_const_assign with NULL name */
static void test_throw_const_assign_null_name(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_throw_const_assign(ctx, NULL);
    ASSERT_TRUE(r8e_has_exception(ctx));

    R8EValue exc = r8e_get_exception(ctx);
    ASSERT_EQ_INT(r8e_error_get_type(exc), R8E_ERROR_TYPE_TYPE_ERROR);
    ASSERT_STR_CONTAINS(r8e_error_get_message(exc), "constant");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(exc);
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: ReferenceError creation
 * ========================================================================= */

/* Create a ReferenceError object directly */
static void test_error_create_reference_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_REFERENCE_ERROR, "x not defined", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_REFERENCE_ERROR);
    ASSERT_EQ_STR(err->message, "x not defined");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create a SyntaxError object directly */
static void test_error_create_syntax_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_SYNTAX_ERROR, "unexpected }", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_SYNTAX_ERROR);
    ASSERT_EQ_STR(err->message, "unexpected }");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create a URIError object directly */
static void test_error_create_uri_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_URI_ERROR, "malformed URI", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_URI_ERROR);
    ASSERT_EQ_STR(err->message, "malformed URI");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Create an InternalError object directly */
static void test_error_create_internal_error(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *err = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_INTERNAL, "internal fault", 0);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(err->error_type, R8E_ERROR_TYPE_INTERNAL);
    ASSERT_EQ_STR(err->message, "internal fault");

    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* =========================================================================
 * NEW: Error source location from call frame
 * ========================================================================= */

/* Typed throw records source location from current frame */
static void test_throw_records_source_location(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EFunction func = {0};
    func.source_name = "module.js";
    push_fake_frame(ctx, &func, "module.js", 55, 12);

    r8e_throw_type_error(ctx, "bad thing");
    ASSERT_TRUE(r8e_has_exception(ctx));

    /* Error state should record the source location */
    ASSERT_EQ_INT(ctx->error.error_line, 55);
    ASSERT_EQ_INT(ctx->error.error_col, 12);
    ASSERT_EQ_STR(ctx->error.error_file, "module.js");

    R8EErrorObject *err = (R8EErrorObject *)r8e_get_pointer(
        r8e_get_exception(ctx));
    r8e_clear_exception(ctx);
    r8e_error_object_free(ctx, err);
    free(ctx);
}

/* Multiple GC alloc/free tracking across several errors */
static void test_error_gc_multiple(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EErrorObject *e1 = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_ERROR, "e1", 0);
    R8EErrorObject *e2 = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_TYPE_ERROR, "e2", 0);
    R8EErrorObject *e3 = r8e_error_object_new(
        ctx, R8E_ERROR_TYPE_RANGE_ERROR, "e3", 0);
    ASSERT_EQ_INT(ctx->gc.total_allocated, 3);

    r8e_error_object_free(ctx, e1);
    ASSERT_EQ_INT(ctx->gc.total_freed, 1);
    r8e_error_object_free(ctx, e2);
    ASSERT_EQ_INT(ctx->gc.total_freed, 2);
    r8e_error_object_free(ctx, e3);
    ASSERT_EQ_INT(ctx->gc.total_freed, 3);

    free(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_error_tests(void) {
    /* Creation */
    RUN_TEST(test_error_create_basic);
    RUN_TEST(test_error_create_type_error);
    RUN_TEST(test_error_create_range_error);
    RUN_TEST(test_error_create_null_message);
    RUN_TEST(test_error_create_explicit_len);
    RUN_TEST(test_error_create_null_ctx);
    RUN_TEST(test_error_all_type_names);
    RUN_TEST(test_error_create_reference_error);
    RUN_TEST(test_error_create_syntax_error);
    RUN_TEST(test_error_create_uri_error);
    RUN_TEST(test_error_create_internal_error);
    RUN_TEST(test_error_empty_message);

    /* Error values */
    RUN_TEST(test_error_new_returns_pointer);
    RUN_TEST(test_error_new_fmt);
    RUN_TEST(test_error_new_fmt_null_ctx);
    RUN_TEST(test_error_new_fmt_null_fmt);

    /* Exception lifecycle */
    RUN_TEST(test_exception_initially_none);
    RUN_TEST(test_exception_throw);
    RUN_TEST(test_exception_clear);
    RUN_TEST(test_exception_clear_when_none);
    RUN_TEST(test_exception_throw_twice);
    RUN_TEST(test_exception_throw_null_ctx);
    RUN_TEST(test_exception_throw_error_object);
    RUN_TEST(test_exception_clear_returns_value);

    /* Typed throw helpers */
    RUN_TEST(test_throw_type_error);
    RUN_TEST(test_throw_range_error);
    RUN_TEST(test_throw_reference_error);
    RUN_TEST(test_throw_syntax_error);
    RUN_TEST(test_throw_uri_error);
    RUN_TEST(test_throw_eval_error);
    RUN_TEST(test_throw_internal_error);
    RUN_TEST(test_throw_generic_error);
    RUN_TEST(test_throw_type_error_fmt_int);

    /* Stack traces */
    RUN_TEST(test_stack_trace_empty);
    RUN_TEST(test_stack_trace_with_frames);
    RUN_TEST(test_stack_trace_query_size);
    RUN_TEST(test_stack_trace_max_depth);
    RUN_TEST(test_stack_trace_explicit_depth);
    RUN_TEST(test_stack_trace_small_buffer);
    RUN_TEST(test_stack_trace_multiple_frames_format);
    RUN_TEST(test_stack_trace_zero_line_frame);

    /* Property accessors */
    RUN_TEST(test_error_get_type);
    RUN_TEST(test_error_get_message);
    RUN_TEST(test_error_get_message_non_pointer);
    RUN_TEST(test_error_get_name_non_pointer);
    RUN_TEST(test_error_get_stack);
    RUN_TEST(test_error_get_stack_non_pointer);
    RUN_TEST(test_error_get_line);
    RUN_TEST(test_error_get_line_no_frames);
    RUN_TEST(test_error_get_line_non_pointer);
    RUN_TEST(test_error_get_column_non_pointer);
    RUN_TEST(test_error_get_source_non_pointer);
    RUN_TEST(test_error_get_type_non_pointer);
    RUN_TEST(test_error_get_name_value_non_pointer);
    RUN_TEST(test_error_get_name_value_type_error);

    /* AggregateError */
    RUN_TEST(test_aggregate_error);
    RUN_TEST(test_aggregate_error_empty);
    RUN_TEST(test_aggregate_error_null_ctx);
    RUN_TEST(test_aggregate_error_null_message);

    /* Common error helpers */
    RUN_TEST(test_throw_not_a_function);
    RUN_TEST(test_throw_not_a_function_no_name);
    RUN_TEST(test_throw_cannot_read_prop_null);
    RUN_TEST(test_throw_cannot_read_prop_undefined);
    RUN_TEST(test_throw_cannot_read_prop_no_name);
    RUN_TEST(test_throw_cannot_set_prop_null);
    RUN_TEST(test_throw_cannot_set_prop_undefined);
    RUN_TEST(test_throw_cannot_set_prop_no_name);
    RUN_TEST(test_throw_not_defined);
    RUN_TEST(test_throw_not_defined_null_name);
    RUN_TEST(test_throw_const_assign);
    RUN_TEST(test_throw_const_assign_null_name);
    RUN_TEST(test_throw_stack_overflow);
    RUN_TEST(test_throw_invalid_lhs);

    /* Source location recording */
    RUN_TEST(test_throw_records_source_location);

    /* GC integration */
    RUN_TEST(test_error_gc_alloc_count);
    RUN_TEST(test_error_gc_multiple);

    /* Free edge cases */
    RUN_TEST(test_error_free_null);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_ERROR_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[error]\n");
    run_error_tests();
    printf("\n=== error: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
