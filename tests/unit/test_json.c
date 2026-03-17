/*
 * test_json.c - Unit tests for r8e_json.c
 *
 * Tests cover:
 *   - JSON.parse: primitives (numbers, booleans, null, strings)
 *   - JSON.parse: objects and arrays
 *   - JSON.parse: nested structures
 *   - JSON.parse: error handling (invalid JSON, depth limit)
 *   - JSON.parse: escape sequences in strings
 *   - JSON.stringify: primitives (numbers, booleans, null)
 *   - JSON.stringify: inline short strings
 *   - JSON.stringify: undefined returns undefined
 *   - JSON.parse + stringify round-trips
 *   - Edge cases: empty input, whitespace, NULL ctx
 *
 * The r8e_json.c module depends on many external functions (object model,
 * array model, string engine, atom table). This test file provides
 * minimal functional stubs so we can run integration-level tests against
 * the JSON parser and stringifier.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* =========================================================================
 * Type definitions (must match r8e_json.c layout)
 *
 * Note: r8e_json.c uses a DIFFERENT R8EContext layout from other modules.
 * It has proto_table, alloc/realloc/free function pointers, and
 * last_error/has_exception fields instead of the GC-oriented layout.
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* NaN-boxing type checks */
#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_TRUE(v)       ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)      ((v) == R8E_FALSE)
#define R8E_IS_BOOL(v)       ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_NULLISH(v)    (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))
#define R8E_IS_NUMBER(v)     (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))

/* NaN-boxing extraction / encoding */
static inline double r8e_get_double(uint64_t v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline uint64_t r8e_from_double(double d) {
    uint64_t v; memcpy(&v, &d, 8); return v;
}
static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}
static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    if (len < 0 || len > 7) return R8E_UNDEFINED;
    uint64_t v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch > 127) return R8E_UNDEFINED;
        v |= ((uint64_t)ch << (38 - i * 7));
    }
    return v;
}
static inline int r8e_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}
static inline int r8e_inline_str_decode(R8EValue v, char *buf) {
    int len = r8e_inline_str_len(v);
    for (int i = 0; i < len; i++) {
        buf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
    }
    buf[len] = '\0';
    return len;
}

/* GC header (must match r8e_json.c) */
#define R8E_GC_KIND_MASK      0x000000E0u
#define R8E_GC_KIND_SHIFT     5
#define R8E_GC_KIND_OBJECT    0u
#define R8E_GC_KIND_STRING    1u
#define R8E_GC_KIND_ARRAY     2u
#define R8E_OBJ_IS_ARRAY      0x00000080u

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* Heap string (must match r8e_json.c) */
#define R8E_STR_IS_ASCII    0x01

typedef struct R8EHeapString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    void    *offset_table;
    char     data[];
} R8EHeapString;

/* Context (must match r8e_json.c) */
#define R8E_PROTO_MAX 64

typedef struct R8EContext {
    R8EValue  proto_table[R8E_PROTO_MAX];
    uint32_t  proto_count;
    void *(*alloc)(size_t size, void *opaque);
    void *(*realloc_fn)(void *ptr, size_t size, void *opaque);
    void  (*free_fn)(void *ptr, void *opaque);
    void  *alloc_opaque;
    R8EValue last_error;
    bool     has_exception;
} R8EContext;

/* =========================================================================
 * Stub implementations for external dependencies
 *
 * r8e_json.c depends on: r8e_obj_new, r8e_obj_get, r8e_obj_set,
 * r8e_obj_has, r8e_obj_keys, r8e_arr_new, r8e_arr_length, r8e_arr_get,
 * r8e_arr_set, r8e_arr_push, r8e_string_new, r8e_string_data,
 * r8e_atom_intern_str, r8e_atom_get_str, r8e_call_fn.
 *
 * Object functions (r8e_obj_*) and r8e_string_new are linked from source
 * (r8e_object.c and r8e_string.c). Array stubs (r8e_arr_*) are provided
 * here since source uses r8e_array_* names. Other stubs (atoms, string_data,
 * call_fn) are provided here since they don't exist in linked source.
 * ========================================================================= */

/* --- Object API (linked from r8e_object.c) --- */

extern void    *r8e_obj_new(R8EContext *ctx);
extern R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key);
extern void    *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key, R8EValue val);
extern bool     r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key);
extern uint32_t r8e_obj_keys(R8EContext *ctx, void *obj, uint32_t *out_keys, uint32_t max_keys);

#define STUB_MAX_PROPS 32

/* --- Mini Array Stub --- */

#define STUB_MAX_ELEMS 64

typedef struct StubArr {
    R8EGCHeader hdr;
    R8EValue    elems[STUB_MAX_ELEMS];
    uint32_t    length;
} StubArr;

void *r8e_arr_new(R8EContext *ctx, uint32_t capacity) {
    (void)ctx; (void)capacity;
    StubArr *arr = (StubArr *)calloc(1, sizeof(StubArr));
    if (arr) {
        arr->hdr.flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT)
                         | R8E_OBJ_IS_ARRAY;
    }
    return arr;
}

uint32_t r8e_arr_length(R8EContext *ctx, void *arr) {
    (void)ctx;
    StubArr *a = (StubArr *)arr;
    return a ? a->length : 0;
}

R8EValue r8e_arr_get(R8EContext *ctx, void *arr, uint32_t index) {
    (void)ctx;
    StubArr *a = (StubArr *)arr;
    if (!a || index >= a->length) return R8E_UNDEFINED;
    return a->elems[index];
}

void r8e_arr_set(R8EContext *ctx, void *arr, uint32_t index, R8EValue val) {
    (void)ctx;
    StubArr *a = (StubArr *)arr;
    if (!a || index >= STUB_MAX_ELEMS) return;
    a->elems[index] = val;
    if (index >= a->length) a->length = index + 1;
}

void r8e_arr_push(R8EContext *ctx, void *arr, R8EValue val) {
    (void)ctx;
    StubArr *a = (StubArr *)arr;
    if (!a || a->length >= STUB_MAX_ELEMS) return;
    a->elems[a->length++] = val;
}

/* --- String API (r8e_string_new linked from r8e_string.c) ---
 *
 * Note: r8e_json.c declares r8e_string_new as returning R8EValue,
 * but the real implementation in r8e_string.c returns R8EString*.
 * Both are 64-bit values on this platform, so the linkage works.
 */

const char *r8e_string_data(R8EValue v, uint32_t *out_len) {
    if (!R8E_IS_POINTER(v)) return NULL;
    R8EHeapString *s = (R8EHeapString *)r8e_get_pointer(v);
    if (!s) return NULL;
    if (out_len) *out_len = s->byte_length;
    return s->data;
}

/* --- Mini Atom Table Stub ---
 *
 * Very simple: store interned strings in a flat table.
 * Atom 0 = "", atom N = N-th interned string. */

#define STUB_MAX_ATOMS 128

static struct {
    char    data[64];
    uint32_t len;
} g_atoms[STUB_MAX_ATOMS];
static uint32_t g_atom_count = 0;

static void stub_atom_reset(void) {
    g_atom_count = 0;
    memset(g_atoms, 0, sizeof(g_atoms));
}

uint32_t r8e_atom_intern_str(R8EContext *ctx, const char *str, uint32_t len) {
    (void)ctx;
    if (!str) return 0;
    if (len == 0) len = (uint32_t)strlen(str);
    /* Search existing */
    for (uint32_t i = 0; i < g_atom_count; i++) {
        if (g_atoms[i].len == len &&
            memcmp(g_atoms[i].data, str, len) == 0) {
            return i;
        }
    }
    /* Add new */
    if (g_atom_count >= STUB_MAX_ATOMS) return 0;
    uint32_t id = g_atom_count++;
    uint32_t clen = len < 63 ? len : 63;
    memcpy(g_atoms[id].data, str, clen);
    g_atoms[id].data[clen] = '\0';
    g_atoms[id].len = clen;
    return id;
}

const char *r8e_atom_get_str(R8EContext *ctx, uint32_t atom,
                              uint32_t *out_len) {
    (void)ctx;
    if (atom >= g_atom_count) {
        if (out_len) *out_len = 0;
        return "";
    }
    if (out_len) *out_len = g_atoms[atom].len;
    return g_atoms[atom].data;
}

/* --- Function Call Stub --- */

R8EValue r8e_call_fn(R8EContext *ctx, R8EValue func,
                      R8EValue this_val, int argc, const R8EValue *argv) {
    (void)ctx; (void)func; (void)this_val; (void)argc; (void)argv;
    return R8E_UNDEFINED;
}

/* =========================================================================
 * External function declarations (from r8e_json.c)
 * ========================================================================= */

extern R8EValue r8e_json_parse(R8EContext *ctx, const char *text, size_t len);
extern R8EValue r8e_json_stringify(R8EContext *ctx, R8EValue value);
extern R8EValue r8e_json_stringify_full(R8EContext *ctx, R8EValue value,
                                         R8EValue replacer, R8EValue space);

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
#define ASSERT_EQ(a, b) ASSERT_TRUE((uint64_t)(a) == (uint64_t)(b))
#define ASSERT_NE(a, b) ASSERT_TRUE((uint64_t)(a) != (uint64_t)(b))

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_DBL(a, b, eps) do { \
    double _a = (double)(a), _b = (double)(b); \
    if (fabs(_a - _b) > (eps)) { \
        printf("  FAIL: %s:%d: %.17g != %.17g\n", __FILE__, __LINE__, \
               _a, _b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        printf("  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
               _a ? _a : "(null)", _b ? _b : "(null)"); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; g_tests_run++; \
    printf("  %s... ", #fn); \
    fn(); \
    if (g_assert_fail) { g_tests_failed++; } \
    else { g_tests_passed++; printf("OK\n"); } \
} while (0)

/* =========================================================================
 * Helper: create a test context
 * ========================================================================= */

static R8EContext *make_test_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    /* No custom alloc -- r8e_json.c falls back to malloc/realloc/free */
    stub_atom_reset();
    return ctx;
}

/* Helper: decode an inline string value into a buffer */
static const char *decode_inline_str(R8EValue v, char *buf) {
    if (!R8E_IS_INLINE_STR(v)) return NULL;
    r8e_inline_str_decode(v, buf);
    return buf;
}

/* Helper: get string from value (inline or heap) */
static const char *get_str(R8EValue v, char *buf) {
    if (R8E_IS_INLINE_STR(v)) {
        r8e_inline_str_decode(v, buf);
        return buf;
    }
    if (R8E_IS_POINTER(v)) {
        uint32_t len;
        return r8e_string_data(v, &len);
    }
    return NULL;
}

/* =========================================================================
 * Tests: JSON.parse - Primitives
 * ========================================================================= */

/* Parse integer */
static void test_json_parse_integer(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "42", 0);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 42);

    free(ctx);
}

/* Parse negative integer */
static void test_json_parse_negative_int(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "-7", 0);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), -7);

    free(ctx);
}

/* Parse zero */
static void test_json_parse_zero(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "0", 0);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 0);

    free(ctx);
}

/* Parse decimal number */
static void test_json_parse_decimal(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "3.14", 0);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_EQ_DBL(r8e_get_double(v), 3.14, 1e-10);

    free(ctx);
}

/* Parse scientific notation */
static void test_json_parse_scientific(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "1.5e3", 0);
    /* 1500 fits in int32 */
    if (R8E_IS_INT32(v)) {
        ASSERT_EQ_INT(r8e_get_int32(v), 1500);
    } else {
        ASSERT_TRUE(R8E_IS_DOUBLE(v));
        ASSERT_EQ_DBL(r8e_get_double(v), 1500.0, 1e-10);
    }

    free(ctx);
}

/* Parse true */
static void test_json_parse_true(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "true", 0);
    ASSERT_EQ(v, R8E_TRUE);

    free(ctx);
}

/* Parse false */
static void test_json_parse_false(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "false", 0);
    ASSERT_EQ(v, R8E_FALSE);

    free(ctx);
}

/* Parse null */
static void test_json_parse_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "null", 0);
    ASSERT_EQ(v, R8E_NULL);

    free(ctx);
}

/* Parse short string (becomes inline) */
static void test_json_parse_short_string(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"hello\"", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    char buf[8];
    decode_inline_str(v, buf);
    ASSERT_EQ_STR(buf, "hello");

    free(ctx);
}

/* Parse long string (becomes heap) */
static void test_json_parse_long_string(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"a longer string value\"", 0);
    /* Longer than 7 chars => heap string */
    ASSERT_TRUE(R8E_IS_POINTER(v));
    uint32_t len;
    const char *data = r8e_string_data(v, &len);
    ASSERT_TRUE(data != NULL);
    ASSERT_EQ_STR(data, "a longer string value");

    /* Clean up heap string */
    free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse empty string */
static void test_json_parse_empty_string(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"\"", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    char buf[8];
    decode_inline_str(v, buf);
    ASSERT_EQ_STR(buf, "");

    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Objects and Arrays
 * ========================================================================= */

/* Parse empty object */
static void test_json_parse_empty_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    void *obj = r8e_get_pointer(v);
    ASSERT_TRUE(obj != NULL);
    uint32_t keys0[STUB_MAX_PROPS];
    ASSERT_EQ_INT(r8e_obj_keys(ctx, obj, keys0, STUB_MAX_PROPS), 0);

    free(obj);
    free(ctx);
}

/* Parse object with one property */
static void test_json_parse_simple_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{\"x\": 10}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    void *obj = r8e_get_pointer(v);
    ASSERT_TRUE(obj != NULL);
    uint32_t keys1[STUB_MAX_PROPS];
    ASSERT_EQ_INT(r8e_obj_keys(ctx, obj, keys1, STUB_MAX_PROPS), 1);

    /* Look up the "x" key via atom */
    uint32_t atom = r8e_atom_intern_str(ctx, "x", 1);
    R8EValue val = r8e_obj_get(ctx, obj, atom);
    ASSERT_TRUE(R8E_IS_INT32(val));
    ASSERT_EQ_INT(r8e_get_int32(val), 10);

    free(obj);
    free(ctx);
}

/* Parse empty array */
static void test_json_parse_empty_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    StubArr *arr = (StubArr *)r8e_get_pointer(v);
    ASSERT_TRUE(arr != NULL);
    ASSERT_EQ_INT(arr->length, 0);

    free(arr);
    free(ctx);
}

/* Parse array with elements */
static void test_json_parse_simple_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[1, 2, 3]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    StubArr *arr = (StubArr *)r8e_get_pointer(v);
    ASSERT_TRUE(arr != NULL);
    ASSERT_EQ_INT(arr->length, 3);
    ASSERT_TRUE(R8E_IS_INT32(arr->elems[0]));
    ASSERT_EQ_INT(r8e_get_int32(arr->elems[0]), 1);
    ASSERT_EQ_INT(r8e_get_int32(arr->elems[1]), 2);
    ASSERT_EQ_INT(r8e_get_int32(arr->elems[2]), 3);

    free(arr);
    free(ctx);
}

/* Parse mixed-type array */
static void test_json_parse_mixed_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[1, true, null, \"hi\"]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    StubArr *arr = (StubArr *)r8e_get_pointer(v);
    ASSERT_TRUE(arr != NULL);
    ASSERT_EQ_INT(arr->length, 4);

    ASSERT_TRUE(R8E_IS_INT32(arr->elems[0]));
    ASSERT_EQ_INT(r8e_get_int32(arr->elems[0]), 1);
    ASSERT_EQ(arr->elems[1], R8E_TRUE);
    ASSERT_EQ(arr->elems[2], R8E_NULL);
    /* "hi" is short enough for inline string */
    ASSERT_TRUE(R8E_IS_INLINE_STR(arr->elems[3]));

    free(arr);
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Error Handling
 * ========================================================================= */

/* Parse invalid JSON */
static void test_json_parse_invalid(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{bad}", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* Parse trailing garbage */
static void test_json_parse_trailing_garbage(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "42 extra", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* Parse empty input */
static void test_json_parse_empty_input(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);

    free(ctx);
}

/* Parse NULL text */
static void test_json_parse_null_text(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, NULL, 0);
    ASSERT_EQ(v, R8E_UNDEFINED);

    free(ctx);
}

/* Parse NULL context */
static void test_json_parse_null_ctx(void) {
    R8EValue v = r8e_json_parse(NULL, "42", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
}

/* Parse whitespace only */
static void test_json_parse_whitespace_only(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "   \t\n  ", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);

    free(ctx);
}

/* Parse unterminated string */
static void test_json_parse_unterminated_string(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"hello", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* Parse with leading/trailing whitespace (should succeed) */
static void test_json_parse_surrounding_whitespace(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "  42  ", 0);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 42);

    free(ctx);
}

/* =========================================================================
 * Tests: JSON.stringify - Primitives
 * ========================================================================= */

/* Stringify integer */
static void test_json_stringify_integer(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_stringify(ctx, r8e_from_int32(42));
    char buf[16];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "42");

    /* Free heap string if applicable */
    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Stringify negative integer */
static void test_json_stringify_negative_int(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_stringify(ctx, r8e_from_int32(-99));
    char buf[16];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "-99");

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Stringify true */
static void test_json_stringify_true(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_stringify(ctx, R8E_TRUE);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "true");

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Stringify false */
static void test_json_stringify_false(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_stringify(ctx, R8E_FALSE);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "false");

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Stringify null */
static void test_json_stringify_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_stringify(ctx, R8E_NULL);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "null");

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Stringify undefined returns undefined */
static void test_json_stringify_undefined(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_stringify(ctx, R8E_UNDEFINED);
    ASSERT_EQ(v, R8E_UNDEFINED);

    free(ctx);
}

/* Stringify NULL context */
static void test_json_stringify_null_ctx(void) {
    R8EValue v = r8e_json_stringify(NULL, r8e_from_int32(1));
    ASSERT_EQ(v, R8E_UNDEFINED);
}

/* =========================================================================
 * Tests: JSON.parse + stringify consistency
 * ========================================================================= */

/* Parse then stringify an integer */
static void test_json_roundtrip_int(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue parsed = r8e_json_parse(ctx, "123", 0);
    ASSERT_TRUE(R8E_IS_INT32(parsed));

    R8EValue stringified = r8e_json_stringify(ctx, parsed);
    char buf[16];
    const char *s = get_str(stringified, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "123");

    if (R8E_IS_POINTER(stringified)) free(r8e_get_pointer(stringified));
    free(ctx);
}

/* Parse then stringify booleans */
static void test_json_roundtrip_bool(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue t = r8e_json_parse(ctx, "true", 0);
    ASSERT_EQ(t, R8E_TRUE);

    R8EValue s1 = r8e_json_stringify(ctx, t);
    char buf[8];
    ASSERT_EQ_STR(get_str(s1, buf), "true");

    R8EValue f = r8e_json_parse(ctx, "false", 0);
    ASSERT_EQ(f, R8E_FALSE);

    R8EValue s2 = r8e_json_stringify(ctx, f);
    ASSERT_EQ_STR(get_str(s2, buf), "false");

    if (R8E_IS_POINTER(s1)) free(r8e_get_pointer(s1));
    if (R8E_IS_POINTER(s2)) free(r8e_get_pointer(s2));
    free(ctx);
}

/* Parse then stringify null */
static void test_json_roundtrip_null(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue parsed = r8e_json_parse(ctx, "null", 0);
    ASSERT_EQ(parsed, R8E_NULL);

    R8EValue s = r8e_json_stringify(ctx, parsed);
    char buf[8];
    ASSERT_EQ_STR(get_str(s, buf), "null");

    if (R8E_IS_POINTER(s)) free(r8e_get_pointer(s));
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - String Escape Sequences
 * ========================================================================= */

/* Parse string with newline escape */
static void test_json_parse_escape_newline(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"a\\nb\"", 0);
    /* "a\nb" is 3 chars, fits inline */
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT((int)strlen(s), 3);
    ASSERT_TRUE(s[0] == 'a');
    ASSERT_TRUE(s[1] == '\n');
    ASSERT_TRUE(s[2] == 'b');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse string with tab escape */
static void test_json_parse_escape_tab(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"a\\tb\"", 0);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s[1] == '\t');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse string with escaped quotes */
static void test_json_parse_escape_quote(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"he said \\\"hi\\\"\"", 0);
    /* "he said \"hi\"" = 13 chars, heap string */
    ASSERT_TRUE(R8E_IS_POINTER(v));
    uint32_t len;
    const char *data = r8e_string_data(v, &len);
    ASSERT_TRUE(data != NULL);
    ASSERT_EQ_STR(data, "he said \"hi\"");

    free(r8e_get_pointer(v));
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - explicit length parameter
 * ========================================================================= */

/* Parse with explicit length truncates input */
static void test_json_parse_explicit_len(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    /* Only parse the first 2 chars: "42" from "42extra" */
    R8EValue v = r8e_json_parse(ctx, "42extra", 2);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 42);

    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Nested Structures
 * ========================================================================= */

/* Parse a nested object */
static void test_json_parse_nested_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{\"a\": {\"b\": 2}}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    void *outer = r8e_get_pointer(v);
    ASSERT_TRUE(outer != NULL);
    uint32_t outer_keys[STUB_MAX_PROPS];
    ASSERT_EQ_INT(r8e_obj_keys(ctx, outer, outer_keys, STUB_MAX_PROPS), 1);

    uint32_t atom_a = r8e_atom_intern_str(ctx, "a", 1);
    R8EValue inner_val = r8e_obj_get(ctx, outer, atom_a);
    ASSERT_TRUE(R8E_IS_POINTER(inner_val));

    void *inner = r8e_get_pointer(inner_val);
    ASSERT_TRUE(inner != NULL);
    uint32_t inner_keys[STUB_MAX_PROPS];
    ASSERT_EQ_INT(r8e_obj_keys(ctx, inner, inner_keys, STUB_MAX_PROPS), 1);

    uint32_t atom_b = r8e_atom_intern_str(ctx, "b", 1);
    R8EValue b_val = r8e_obj_get(ctx, inner, atom_b);
    ASSERT_TRUE(R8E_IS_INT32(b_val));
    ASSERT_EQ_INT(r8e_get_int32(b_val), 2);

    free(inner);
    free(outer);
    free(ctx);
}

/* Parse array of arrays */
static void test_json_parse_nested_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[[1, 2], [3, 4]]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    StubArr *outer = (StubArr *)r8e_get_pointer(v);
    ASSERT_TRUE(outer != NULL);
    ASSERT_EQ_INT(outer->length, 2);

    /* First inner array */
    ASSERT_TRUE(R8E_IS_POINTER(outer->elems[0]));
    StubArr *inner0 = (StubArr *)r8e_get_pointer(outer->elems[0]);
    ASSERT_EQ_INT(inner0->length, 2);
    ASSERT_EQ_INT(r8e_get_int32(inner0->elems[0]), 1);
    ASSERT_EQ_INT(r8e_get_int32(inner0->elems[1]), 2);

    /* Second inner array */
    ASSERT_TRUE(R8E_IS_POINTER(outer->elems[1]));
    StubArr *inner1 = (StubArr *)r8e_get_pointer(outer->elems[1]);
    ASSERT_EQ_INT(inner1->length, 2);
    ASSERT_EQ_INT(r8e_get_int32(inner1->elems[0]), 3);
    ASSERT_EQ_INT(r8e_get_int32(inner1->elems[1]), 4);

    free(inner0);
    free(inner1);
    free(outer);
    free(ctx);
}

/* Parse object with array value */
static void test_json_parse_object_with_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{\"items\": [1, 2, 3]}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    void *obj = r8e_get_pointer(v);
    ASSERT_TRUE(obj != NULL);
    uint32_t obj_keys[STUB_MAX_PROPS];
    ASSERT_EQ_INT(r8e_obj_keys(ctx, obj, obj_keys, STUB_MAX_PROPS), 1);

    uint32_t atom_items = r8e_atom_intern_str(ctx, "items", 5);
    R8EValue arr_val = r8e_obj_get(ctx, obj, atom_items);
    ASSERT_TRUE(R8E_IS_POINTER(arr_val));

    StubArr *arr = (StubArr *)r8e_get_pointer(arr_val);
    ASSERT_EQ_INT(arr->length, 3);

    free(arr);
    free(obj);
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Error Edge Cases
 * ========================================================================= */

/* Parse trailing comma in object (invalid JSON) */
static void test_json_parse_trailing_comma_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{\"a\": 1,}", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* Parse trailing comma in array (invalid JSON) */
static void test_json_parse_trailing_comma_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[1, 2,]", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* Parse duplicate keys in object (last one wins per spec) */
static void test_json_parse_duplicate_keys(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{\"a\": 1, \"a\": 2}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    void *obj = r8e_get_pointer(v);
    ASSERT_TRUE(obj != NULL);

    uint32_t atom_a = r8e_atom_intern_str(ctx, "a", 1);
    R8EValue val = r8e_obj_get(ctx, obj, atom_a);
    ASSERT_TRUE(R8E_IS_INT32(val));
    /* Last value wins */
    ASSERT_EQ_INT(r8e_get_int32(val), 2);

    free(obj);
    free(ctx);
}

/* Parse leading zeros in numbers (invalid JSON) */
static void test_json_parse_leading_zeros(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "007", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Number Edge Cases
 * ========================================================================= */

/* Parse negative zero */
static void test_json_parse_negative_zero(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "-0", 0);
    /* -0 may be stored as int32(0) or double(-0.0) depending on impl */
    ASSERT_TRUE(R8E_IS_INT32(v) || R8E_IS_DOUBLE(v));

    free(ctx);
}

/* Parse large integer that overflows int32 */
static void test_json_parse_large_number(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "9999999999", 0);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_EQ_DBL(r8e_get_double(v), 9999999999.0, 1.0);

    free(ctx);
}

/* Parse negative exponent */
static void test_json_parse_negative_exponent(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "1e-3", 0);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_EQ_DBL(r8e_get_double(v), 0.001, 1e-10);

    free(ctx);
}

/* Parse scientific notation with capital E */
static void test_json_parse_scientific_capital_e(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "2.5E2", 0);
    /* 250 fits in int32 */
    if (R8E_IS_INT32(v)) {
        ASSERT_EQ_INT(r8e_get_int32(v), 250);
    } else {
        ASSERT_TRUE(R8E_IS_DOUBLE(v));
        ASSERT_EQ_DBL(r8e_get_double(v), 250.0, 1e-10);
    }

    free(ctx);
}

/* Parse explicit positive exponent */
static void test_json_parse_positive_exponent(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "5e+2", 0);
    if (R8E_IS_INT32(v)) {
        ASSERT_EQ_INT(r8e_get_int32(v), 500);
    } else {
        ASSERT_TRUE(R8E_IS_DOUBLE(v));
        ASSERT_EQ_DBL(r8e_get_double(v), 500.0, 1e-10);
    }

    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Escape Sequences (additional)
 * ========================================================================= */

/* Parse \uXXXX unicode escape */
static void test_json_parse_unicode_escape(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    /* \u0041 = 'A' */
    R8EValue v = r8e_json_parse(ctx, "\"\\u0041\"", 0);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "A");

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse backslash escape */
static void test_json_parse_escape_backslash(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"a\\\\b\"", 0);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT((int)strlen(s), 3);
    ASSERT_TRUE(s[0] == 'a');
    ASSERT_TRUE(s[1] == '\\');
    ASSERT_TRUE(s[2] == 'b');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse forward slash escape */
static void test_json_parse_escape_slash(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"a\\/b\"", 0);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT((int)strlen(s), 3);
    ASSERT_TRUE(s[0] == 'a');
    ASSERT_TRUE(s[1] == '/');
    ASSERT_TRUE(s[2] == 'b');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse \b (backspace) and \f (form feed) escapes */
static void test_json_parse_escape_bf(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"\\b\\f\"", 0);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT((int)strlen(s), 2);
    ASSERT_TRUE(s[0] == '\b');
    ASSERT_TRUE(s[1] == '\f');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Parse \r (carriage return) */
static void test_json_parse_escape_cr(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "\"a\\rb\"", 0);
    char buf[8];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s[1] == '\r');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.stringify - Objects and Arrays
 * ========================================================================= */

/* Stringify an object */
static void test_json_stringify_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    /* Build an object with one property */
    void *obj = r8e_obj_new(ctx);
    ASSERT_TRUE(obj != NULL);
    uint32_t key_atom = r8e_atom_intern_str(ctx, "x", 1);
    r8e_obj_set(ctx, obj, key_atom, r8e_from_int32(10));

    R8EValue sv = r8e_json_stringify(ctx, r8e_from_pointer(obj));
    ASSERT_TRUE(R8E_IS_POINTER(sv) || R8E_IS_INLINE_STR(sv));

    char buf[64];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    /* Should contain "x" and 10 */
    ASSERT_TRUE(strstr(s, "\"x\"") != NULL);
    ASSERT_TRUE(strstr(s, "10") != NULL);

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(obj);
    free(ctx);
}

/* Stringify an array */
static void test_json_stringify_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    StubArr *arr = (StubArr *)r8e_arr_new(ctx, 4);
    ASSERT_TRUE(arr != NULL);
    r8e_arr_push(ctx, arr, r8e_from_int32(1));
    r8e_arr_push(ctx, arr, r8e_from_int32(2));
    r8e_arr_push(ctx, arr, r8e_from_int32(3));

    R8EValue sv = r8e_json_stringify(ctx, r8e_from_pointer(arr));
    ASSERT_TRUE(R8E_IS_POINTER(sv) || R8E_IS_INLINE_STR(sv));

    char buf[64];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "[1,2,3]");

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(arr);
    free(ctx);
}

/* Stringify string value */
static void test_json_stringify_string(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue str_val = r8e_from_inline_str("hi", 2);
    R8EValue sv = r8e_json_stringify(ctx, str_val);

    char buf[16];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "\"hi\"");

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(ctx);
}

/* Stringify double value */
static void test_json_stringify_double(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue dval = r8e_from_double(3.14);
    R8EValue sv = r8e_json_stringify(ctx, dval);

    char buf[32];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    /* Should contain "3.14" or similar representation */
    ASSERT_TRUE(strstr(s, "3.14") != NULL);

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(ctx);
}

/* Stringify zero */
static void test_json_stringify_zero(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue sv = r8e_json_stringify(ctx, r8e_from_int32(0));
    char buf[16];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "0");

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.stringify_full - Space parameter
 * ========================================================================= */

/* Stringify with space parameter for indentation */
static void test_json_stringify_with_space(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    StubArr *arr = (StubArr *)r8e_arr_new(ctx, 4);
    ASSERT_TRUE(arr != NULL);
    r8e_arr_push(ctx, arr, r8e_from_int32(1));
    r8e_arr_push(ctx, arr, r8e_from_int32(2));

    /* space = 2 (as int32) */
    R8EValue sv = r8e_json_stringify_full(ctx, r8e_from_pointer(arr),
                                            R8E_UNDEFINED, r8e_from_int32(2));
    ASSERT_TRUE(R8E_IS_POINTER(sv) || R8E_IS_INLINE_STR(sv));

    char buf[128];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    /* With space=2, should contain newlines and indentation */
    ASSERT_TRUE(strstr(s, "\n") != NULL);

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(arr);
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse + stringify round-trips (extended)
 * ========================================================================= */

/* Round-trip a decimal number */
static void test_json_roundtrip_decimal(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue parsed = r8e_json_parse(ctx, "3.14", 0);
    ASSERT_TRUE(R8E_IS_DOUBLE(parsed));

    R8EValue sv = r8e_json_stringify(ctx, parsed);
    char buf[32];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(strstr(s, "3.14") != NULL);

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(ctx);
}

/* Round-trip a short string */
static void test_json_roundtrip_string(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue parsed = r8e_json_parse(ctx, "\"abc\"", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(parsed));

    R8EValue sv = r8e_json_stringify(ctx, parsed);
    char buf[16];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "\"abc\"");

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(ctx);
}

/* Round-trip empty array */
static void test_json_roundtrip_empty_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue parsed = r8e_json_parse(ctx, "[]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(parsed));

    R8EValue sv = r8e_json_stringify(ctx, parsed);
    char buf[16];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "[]");

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(r8e_get_pointer(parsed));
    free(ctx);
}

/* Round-trip empty object */
static void test_json_roundtrip_empty_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue parsed = r8e_json_parse(ctx, "{}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(parsed));

    R8EValue sv = r8e_json_stringify(ctx, parsed);
    char buf[16];
    const char *s = get_str(sv, buf);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_STR(s, "{}");

    if (R8E_IS_POINTER(sv)) free(r8e_get_pointer(sv));
    free(r8e_get_pointer(parsed));
    free(ctx);
}

/* =========================================================================
 * Tests: JSON.parse - Multi-property objects
 * ========================================================================= */

/* Parse object with multiple properties */
static void test_json_parse_multi_prop_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx,
        "{\"a\": 1, \"b\": true, \"c\": null}", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    void *obj = r8e_get_pointer(v);
    ASSERT_TRUE(obj != NULL);
    uint32_t mp_keys[STUB_MAX_PROPS];
    ASSERT_EQ_INT(r8e_obj_keys(ctx, obj, mp_keys, STUB_MAX_PROPS), 3);

    uint32_t atom_a = r8e_atom_intern_str(ctx, "a", 1);
    uint32_t atom_b = r8e_atom_intern_str(ctx, "b", 1);
    uint32_t atom_c = r8e_atom_intern_str(ctx, "c", 1);

    ASSERT_TRUE(R8E_IS_INT32(r8e_obj_get(ctx, obj, atom_a)));
    ASSERT_EQ_INT(r8e_get_int32(r8e_obj_get(ctx, obj, atom_a)), 1);
    ASSERT_EQ(r8e_obj_get(ctx, obj, atom_b), R8E_TRUE);
    ASSERT_EQ(r8e_obj_get(ctx, obj, atom_c), R8E_NULL);

    free(obj);
    free(ctx);
}

/* Parse boolean array */
static void test_json_parse_bool_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[true, false, true]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    StubArr *arr = (StubArr *)r8e_get_pointer(v);
    ASSERT_TRUE(arr != NULL);
    ASSERT_EQ_INT(arr->length, 3);
    ASSERT_EQ(arr->elems[0], R8E_TRUE);
    ASSERT_EQ(arr->elems[1], R8E_FALSE);
    ASSERT_EQ(arr->elems[2], R8E_TRUE);

    free(arr);
    free(ctx);
}

/* Parse singleton array */
static void test_json_parse_singleton_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[null]", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    StubArr *arr = (StubArr *)r8e_get_pointer(v);
    ASSERT_TRUE(arr != NULL);
    ASSERT_EQ_INT(arr->length, 1);
    ASSERT_EQ(arr->elems[0], R8E_NULL);

    free(arr);
    free(ctx);
}

/* Parse string with all escape types */
static void test_json_parse_all_escapes(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    /* "a\n\t\r\b\f\\\"\/" */
    R8EValue v = r8e_json_parse(ctx,
        "\"a\\n\\t\\r\\b\\f\\\\\\\"\\/\"", 0);
    ASSERT_TRUE(R8E_IS_POINTER(v) || R8E_IS_INLINE_STR(v));

    char buf[32];
    const char *s = get_str(v, buf);
    ASSERT_TRUE(s != NULL);
    /* Should be 9 chars: a \n \t \r \b \f \ " / */
    ASSERT_EQ_INT((int)strlen(s), 9);
    ASSERT_TRUE(s[0] == 'a');
    ASSERT_TRUE(s[1] == '\n');
    ASSERT_TRUE(s[2] == '\t');
    ASSERT_TRUE(s[3] == '\r');
    ASSERT_TRUE(s[4] == '\b');
    ASSERT_TRUE(s[5] == '\f');
    ASSERT_TRUE(s[6] == '\\');
    ASSERT_TRUE(s[7] == '"');
    ASSERT_TRUE(s[8] == '/');

    if (R8E_IS_POINTER(v)) free(r8e_get_pointer(v));
    free(ctx);
}

/* Truncated JSON: unterminated object */
static void test_json_parse_truncated_object(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "{\"a\": 1", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* Truncated JSON: unterminated array */
static void test_json_parse_truncated_array(void) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_TRUE(ctx != NULL);

    R8EValue v = r8e_json_parse(ctx, "[1, 2", 0);
    ASSERT_EQ(v, R8E_UNDEFINED);
    ASSERT_TRUE(ctx->has_exception);

    free(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_json_tests(void) {
    /* Parse primitives */
    RUN_TEST(test_json_parse_integer);
    RUN_TEST(test_json_parse_negative_int);
    RUN_TEST(test_json_parse_zero);
    RUN_TEST(test_json_parse_decimal);
    RUN_TEST(test_json_parse_scientific);
    RUN_TEST(test_json_parse_true);
    RUN_TEST(test_json_parse_false);
    RUN_TEST(test_json_parse_null);
    RUN_TEST(test_json_parse_short_string);
    RUN_TEST(test_json_parse_long_string);
    RUN_TEST(test_json_parse_empty_string);

    /* Parse objects and arrays */
    RUN_TEST(test_json_parse_empty_object);
    RUN_TEST(test_json_parse_simple_object);
    RUN_TEST(test_json_parse_empty_array);
    RUN_TEST(test_json_parse_simple_array);
    RUN_TEST(test_json_parse_mixed_array);

    /* Parse error handling */
    RUN_TEST(test_json_parse_invalid);
    RUN_TEST(test_json_parse_trailing_garbage);
    RUN_TEST(test_json_parse_empty_input);
    RUN_TEST(test_json_parse_null_text);
    RUN_TEST(test_json_parse_null_ctx);
    RUN_TEST(test_json_parse_whitespace_only);
    RUN_TEST(test_json_parse_unterminated_string);
    RUN_TEST(test_json_parse_surrounding_whitespace);

    /* Stringify primitives */
    RUN_TEST(test_json_stringify_integer);
    RUN_TEST(test_json_stringify_negative_int);
    RUN_TEST(test_json_stringify_true);
    RUN_TEST(test_json_stringify_false);
    RUN_TEST(test_json_stringify_null);
    RUN_TEST(test_json_stringify_undefined);
    RUN_TEST(test_json_stringify_null_ctx);

    /* Round-trips */
    RUN_TEST(test_json_roundtrip_int);
    RUN_TEST(test_json_roundtrip_bool);
    RUN_TEST(test_json_roundtrip_null);

    /* Escape sequences */
    RUN_TEST(test_json_parse_escape_newline);
    RUN_TEST(test_json_parse_escape_tab);
    RUN_TEST(test_json_parse_escape_quote);

    /* Explicit length */
    RUN_TEST(test_json_parse_explicit_len);

    /* Nested structures */
    RUN_TEST(test_json_parse_nested_object);
    RUN_TEST(test_json_parse_nested_array);
    RUN_TEST(test_json_parse_object_with_array);

    /* Error edge cases */
    RUN_TEST(test_json_parse_trailing_comma_object);
    RUN_TEST(test_json_parse_trailing_comma_array);
    RUN_TEST(test_json_parse_duplicate_keys);
    RUN_TEST(test_json_parse_leading_zeros);

    /* Number edge cases */
    RUN_TEST(test_json_parse_negative_zero);
    RUN_TEST(test_json_parse_large_number);
    RUN_TEST(test_json_parse_negative_exponent);
    RUN_TEST(test_json_parse_scientific_capital_e);
    RUN_TEST(test_json_parse_positive_exponent);

    /* Additional escape sequences */
    RUN_TEST(test_json_parse_unicode_escape);
    RUN_TEST(test_json_parse_escape_backslash);
    RUN_TEST(test_json_parse_escape_slash);
    RUN_TEST(test_json_parse_escape_bf);
    RUN_TEST(test_json_parse_escape_cr);

    /* Stringify objects and arrays */
    RUN_TEST(test_json_stringify_object);
    RUN_TEST(test_json_stringify_array);
    RUN_TEST(test_json_stringify_string);
    RUN_TEST(test_json_stringify_double);
    RUN_TEST(test_json_stringify_zero);

    /* Stringify with space */
    RUN_TEST(test_json_stringify_with_space);

    /* Extended round-trips */
    RUN_TEST(test_json_roundtrip_decimal);
    RUN_TEST(test_json_roundtrip_string);
    RUN_TEST(test_json_roundtrip_empty_array);
    RUN_TEST(test_json_roundtrip_empty_object);

    /* Multi-property objects */
    RUN_TEST(test_json_parse_multi_prop_object);

    /* Boolean and singleton arrays */
    RUN_TEST(test_json_parse_bool_array);
    RUN_TEST(test_json_parse_singleton_array);

    /* All escape types */
    RUN_TEST(test_json_parse_all_escapes);

    /* Truncated JSON */
    RUN_TEST(test_json_parse_truncated_object);
    RUN_TEST(test_json_parse_truncated_array);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_JSON_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[json]\n");
    run_json_tests();
    printf("\n=== json: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
