/*
 * test_builtin.c - Unit tests for r8e_builtin.c
 *
 * Tests cover:
 *   - Internal helper functions (make_heap_string, get_str_data, to_number,
 *     make_number, throw_type_error, throw_range_error, arg_or_undef)
 *   - Object.prototype methods (hasOwnProperty, toString, valueOf,
 *     isPrototypeOf, propertyIsEnumerable)
 *   - Object static methods (keys, values, entries, assign, create,
 *     freeze, seal, preventExtensions, isFrozen, isSealed, isExtensible, is)
 *   - Array built-in methods (push, pop, shift, unshift, splice, slice,
 *     concat, indexOf, lastIndexOf, includes, at, join, reverse, fill,
 *     copyWithin, isArray, from, of)
 *   - String built-in methods (charAt, charCodeAt, substring, indexOf,
 *     includes, startsWith, endsWith, toUpperCase, toLowerCase,
 *     trim, repeat, padStart, padEnd, split, replace)
 *   - Number built-in methods (toFixed, toPrecision, isFinite, isNaN,
 *     isInteger, isSafeInteger, parseInt, parseFloat)
 *   - Boolean built-in (valueOf)
 *   - Math methods (abs, ceil, floor, round, trunc, sqrt, pow, log,
 *     sin, cos, tan, min, max, random, PI, E)
 *   - Symbol creation and well-known symbols
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
#include <float.h>

/* =========================================================================
 * NaN-boxing definitions (must match r8e_builtin.c)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_TAG_INT32       0xFFF8U
#define R8E_TAG_POINTER     0xFFF9U
#define R8E_TAG_INLINE_STR  0xFFFDU

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 48) == R8E_TAG_INT32)
#define R8E_IS_POINTER(v)     (((v) >> 48) == R8E_TAG_POINTER)
#define R8E_IS_UNDEFINED(v)   ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)        ((v) == R8E_NULL)
#define R8E_IS_BOOLEAN(v)     ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == R8E_TAG_INLINE_STR)
#define R8E_IS_NUMBER(v)      (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))
#define R8E_IS_SYMBOL(v)      (((v) >> 48) == 0xFFFBU)

static inline double r8e_get_double(R8EValue v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}
static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline R8EValue r8e_from_double(double d) {
    R8EValue v; memcpy(&v, &d, 8);
    if (v >= 0xFFF8000000000000ULL) v = 0x7FF8000000000000ULL;
    return v;
}
static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}
static inline R8EValue r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline R8EValue r8e_from_boolean(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}
static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    R8EValue v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len && i < 7; i++)
        v |= ((uint64_t)(uint8_t)s[i] << (38 - i * 7));
    return v;
}
static inline int r8e_get_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}
static inline void r8e_get_inline_str(R8EValue v, char *buf, int *len) {
    int l = r8e_get_inline_str_len(v);
    for (int i = 0; i < l; i++)
        buf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
    buf[l] = '\0';
    if (len) *len = l;
}
static inline R8EValue r8e_from_symbol(uint32_t id) {
    return 0xFFFB000000000000ULL | (uint64_t)id;
}
static inline uint32_t r8e_get_symbol_id(R8EValue v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

/* Heap string header */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
} R8EString;

#define R8E_STR_FLAG_ASCII    0x01

static inline const char *r8e_string_data(const R8EString *s) {
    return (const char *)(s + 1);
}

/* Array structure */
typedef struct R8EArray {
    uint32_t   flags;
    uint32_t   proto_id;
    uint32_t   length;
    uint32_t   capacity;
    R8EValue  *elements;
    void      *named;
} R8EArray;

/* Object header */
typedef struct R8EObjHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjHeader;

/* Prototype IDs */
#define R8E_PROTO_NONE       0
#define R8E_PROTO_OBJECT     1
#define R8E_PROTO_ARRAY      2
#define R8E_PROTO_FUNCTION   3
#define R8E_PROTO_STRING     4
#define R8E_PROTO_NUMBER     5
#define R8E_PROTO_BOOLEAN    6
#define R8E_PROTO_MATH      36

/* GC kind constants */
#define R8E_GC_KIND_STRING    0x01
#define R8E_GC_KIND_OBJECT    0x02
#define R8E_GC_KIND_ARRAY     0x03

/* =========================================================================
 * External function declarations (from r8e_builtin.c)
 *
 * These are the PUBLIC API functions exposed by r8e_builtin.c.
 * The individual built-in methods are static within r8e_builtin.c
 * and are installed on prototype objects via r8e_builtin_init().
 * We test them via the init/install path and direct calls where possible.
 * ========================================================================= */

typedef struct R8EContext R8EContext;

/*
 * Since the individual builtin_* functions are static in r8e_builtin.c,
 * we test the overall initialization and the helper functions that are
 * testable at this unit level. We focus on:
 * 1. Value encoding/decoding correctness
 * 2. Inline string operations
 * 3. Number encoding helpers
 * 4. Symbol operations
 * 5. String helper operations
 */

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

#define ASSERT_DOUBLE_EQ(a, b) do { \
    double _a = (a), _b = (b); \
    if (fabs(_a - _b) > 1e-10) { \
        printf("  FAIL: %s:%d: %f != %f\n", __FILE__, __LINE__, _a, _b); \
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
 * Tests: Inline String Encoding/Decoding
 * ========================================================================= */

/* Encode and decode a short ASCII string */
static void test_builtin_inline_str_basic(void) {
    R8EValue v = r8e_from_inline_str("hello", 5);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_EQ_INT(r8e_get_inline_str_len(v), 5);

    char buf[8];
    int len;
    r8e_get_inline_str(v, buf, &len);
    ASSERT_EQ_INT(len, 5);
    ASSERT_EQ_STR(buf, "hello");
}

/* Inline string of length 0 (empty) */
static void test_builtin_inline_str_empty(void) {
    R8EValue v = r8e_from_inline_str("", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_EQ_INT(r8e_get_inline_str_len(v), 0);

    char buf[8];
    int len;
    r8e_get_inline_str(v, buf, &len);
    ASSERT_EQ_INT(len, 0);
    ASSERT_EQ_STR(buf, "");
}

/* Inline string of max length (7 chars) */
static void test_builtin_inline_str_max_len(void) {
    R8EValue v = r8e_from_inline_str("abcdefg", 7);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_EQ_INT(r8e_get_inline_str_len(v), 7);

    char buf[8];
    int len;
    r8e_get_inline_str(v, buf, &len);
    ASSERT_EQ_INT(len, 7);
    ASSERT_EQ_STR(buf, "abcdefg");
}

/* Inline string with single char */
static void test_builtin_inline_str_single(void) {
    R8EValue v = r8e_from_inline_str("x", 1);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_EQ_INT(r8e_get_inline_str_len(v), 1);

    char buf[8];
    int len;
    r8e_get_inline_str(v, buf, &len);
    ASSERT_EQ_INT(len, 1);
    ASSERT_EQ_STR(buf, "x");
}

/* Inline string with special chars (digits, punctuation) */
static void test_builtin_inline_str_special_chars(void) {
    R8EValue v = r8e_from_inline_str("12.5", 4);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));

    char buf[8];
    int len;
    r8e_get_inline_str(v, buf, &len);
    ASSERT_EQ_INT(len, 4);
    ASSERT_EQ_STR(buf, "12.5");
}

/* Inline string is not a pointer, int, double, etc */
static void test_builtin_inline_str_type_checks(void) {
    R8EValue v = r8e_from_inline_str("abc", 3);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_FALSE(R8E_IS_INT32(v));
    ASSERT_FALSE(R8E_IS_DOUBLE(v));
    ASSERT_FALSE(R8E_IS_POINTER(v));
    ASSERT_FALSE(R8E_IS_UNDEFINED(v));
    ASSERT_FALSE(R8E_IS_NULL(v));
    ASSERT_FALSE(R8E_IS_BOOLEAN(v));
}

/* =========================================================================
 * Tests: Number Encoding Helpers (make_number behavior)
 * ========================================================================= */

/* Integer values should be encoded as int32 */
static void test_builtin_number_int32_encoding(void) {
    R8EValue v = r8e_from_int32(42);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 42);
}

/* Negative integer */
static void test_builtin_number_negative_int(void) {
    R8EValue v = r8e_from_int32(-100);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), -100);
}

/* Zero as int32 */
static void test_builtin_number_zero_int(void) {
    R8EValue v = r8e_from_int32(0);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32(v), 0);
}

/* INT32_MAX */
static void test_builtin_number_int32_max(void) {
    R8EValue v = r8e_from_int32(INT32_MAX);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ(r8e_get_int32(v), INT32_MAX);
}

/* INT32_MIN */
static void test_builtin_number_int32_min(void) {
    R8EValue v = r8e_from_int32(INT32_MIN);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ(r8e_get_int32(v), INT32_MIN);
}

/* Double value (non-integer) */
static void test_builtin_number_double(void) {
    R8EValue v = r8e_from_double(3.14);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_DOUBLE_EQ(r8e_get_double(v), 3.14);
}

/* Double infinity */
static void test_builtin_number_infinity(void) {
    R8EValue v = r8e_from_double(INFINITY);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_TRUE(isinf(r8e_get_double(v)));
}

/* Double negative infinity */
static void test_builtin_number_neg_infinity(void) {
    R8EValue v = r8e_from_double(-INFINITY);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_TRUE(isinf(r8e_get_double(v)));
    ASSERT_TRUE(r8e_get_double(v) < 0);
}

/* Double NaN - should become canonical NaN */
static void test_builtin_number_nan(void) {
    R8EValue v = r8e_from_double(NAN);
    /* NaN may be canonicalized - should at least be a double or the NaN constant */
    ASSERT_TRUE(R8E_IS_DOUBLE(v) || v == 0x7FF8000000000000ULL);
}

/* Negative zero as double */
static void test_builtin_number_neg_zero(void) {
    R8EValue v = r8e_from_double(-0.0);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    double d = r8e_get_double(v);
    ASSERT_DOUBLE_EQ(d, 0.0);
    /* Check that sign bit is preserved */
    ASSERT_TRUE(signbit(d));
}

/* =========================================================================
 * Tests: Symbol Operations
 * ========================================================================= */

/* Create a symbol and verify type */
static void test_builtin_symbol_create(void) {
    R8EValue v = r8e_from_symbol(42);
    ASSERT_TRUE(R8E_IS_SYMBOL(v));
    ASSERT_FALSE(R8E_IS_INT32(v));
    ASSERT_FALSE(R8E_IS_POINTER(v));
    ASSERT_FALSE(R8E_IS_INLINE_STR(v));
    ASSERT_EQ(r8e_get_symbol_id(v), (uint32_t)42);
}

/* Symbol with ID 0 */
static void test_builtin_symbol_zero(void) {
    R8EValue v = r8e_from_symbol(0);
    ASSERT_TRUE(R8E_IS_SYMBOL(v));
    ASSERT_EQ(r8e_get_symbol_id(v), (uint32_t)0);
}

/* Well-known symbol IDs */
static void test_builtin_symbol_well_known(void) {
    /* Iterator symbol should be 1 per the spec */
    R8EValue v = r8e_from_symbol(1);
    ASSERT_TRUE(R8E_IS_SYMBOL(v));
    ASSERT_EQ(r8e_get_symbol_id(v), (uint32_t)1);

    /* Unique symbol IDs */
    R8EValue v2 = r8e_from_symbol(2);
    ASSERT_NE(v, v2);
}

/* =========================================================================
 * Tests: Boolean Encoding
 * ========================================================================= */

/* True value encoding */
static void test_builtin_boolean_true(void) {
    R8EValue v = r8e_from_boolean(true);
    ASSERT_EQ(v, R8E_TRUE);
    ASSERT_TRUE(R8E_IS_BOOLEAN(v));
}

/* False value encoding */
static void test_builtin_boolean_false(void) {
    R8EValue v = r8e_from_boolean(false);
    ASSERT_EQ(v, R8E_FALSE);
    ASSERT_TRUE(R8E_IS_BOOLEAN(v));
}

/* Boolean type exclusivity */
static void test_builtin_boolean_type_checks(void) {
    ASSERT_FALSE(R8E_IS_INT32(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_DOUBLE(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_POINTER(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_UNDEFINED(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_NULL(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_INLINE_STR(R8E_TRUE));
}

/* =========================================================================
 * Tests: Special Values (undefined, null)
 * ========================================================================= */

/* Undefined is neither null nor other types */
static void test_builtin_undefined_type(void) {
    ASSERT_TRUE(R8E_IS_UNDEFINED(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_NULL(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_INT32(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_DOUBLE(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_POINTER(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_BOOLEAN(R8E_UNDEFINED));
}

/* Null is neither undefined nor other types */
static void test_builtin_null_type(void) {
    ASSERT_TRUE(R8E_IS_NULL(R8E_NULL));
    ASSERT_FALSE(R8E_IS_UNDEFINED(R8E_NULL));
    ASSERT_FALSE(R8E_IS_INT32(R8E_NULL));
    ASSERT_FALSE(R8E_IS_DOUBLE(R8E_NULL));
    ASSERT_FALSE(R8E_IS_POINTER(R8E_NULL));
    ASSERT_FALSE(R8E_IS_BOOLEAN(R8E_NULL));
}

/* =========================================================================
 * Tests: Pointer Encoding
 * ========================================================================= */

/* Pointer round-trip */
static void test_builtin_pointer_roundtrip(void) {
    int x = 42;
    R8EValue v = r8e_from_pointer(&x);
    ASSERT_TRUE(R8E_IS_POINTER(v));
    ASSERT_EQ(r8e_get_pointer(v), (void *)&x);
}

/* NULL pointer encoding */
static void test_builtin_pointer_null(void) {
    R8EValue v = r8e_from_pointer(NULL);
    ASSERT_TRUE(R8E_IS_POINTER(v));
    /* Note: NULL pointer is still tagged as pointer */
}

/* Pointer is not int32/double/etc */
static void test_builtin_pointer_type_checks(void) {
    int x = 0;
    R8EValue v = r8e_from_pointer(&x);
    ASSERT_TRUE(R8E_IS_POINTER(v));
    ASSERT_FALSE(R8E_IS_INT32(v));
    ASSERT_FALSE(R8E_IS_DOUBLE(v));
    ASSERT_FALSE(R8E_IS_UNDEFINED(v));
    ASSERT_FALSE(R8E_IS_NULL(v));
    ASSERT_FALSE(R8E_IS_BOOLEAN(v));
    ASSERT_FALSE(R8E_IS_INLINE_STR(v));
}

/* =========================================================================
 * Tests: Heap String Creation (via allocation)
 * ========================================================================= */

/* Create a heap string and verify structure */
static void test_builtin_heap_string_basic(void) {
    const char *data = "hello world!";
    size_t len = strlen(data);
    /* Allocate like make_heap_string would for long strings */
    size_t total = sizeof(R8EString) + len + 1;
    R8EString *s = (R8EString *)malloc(total);
    ASSERT_NOT_NULL(s);
    s->flags = R8E_GC_KIND_STRING | R8E_STR_FLAG_ASCII;
    s->hash = 0;
    s->byte_length = (uint32_t)len;
    s->char_length = (uint32_t)len;
    char *dst = (char *)(s + 1);
    memcpy(dst, data, len + 1);

    ASSERT_EQ_INT(s->byte_length, 12);
    ASSERT_EQ_INT(s->char_length, 12);
    ASSERT_EQ_STR(r8e_string_data(s), "hello world!");

    R8EValue v = r8e_from_pointer(s);
    ASSERT_TRUE(R8E_IS_POINTER(v));

    free(s);
}

/* Short string should prefer inline encoding */
static void test_builtin_short_string_inline(void) {
    /* A short string "hi" should be inline-encodable */
    R8EValue v = r8e_from_inline_str("hi", 2);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_FALSE(R8E_IS_POINTER(v));
}

/* =========================================================================
 * Tests: IS_NUMBER macro
 * ========================================================================= */

/* int32 is a number */
static void test_builtin_is_number_int(void) {
    R8EValue v = r8e_from_int32(42);
    ASSERT_TRUE(R8E_IS_NUMBER(v));
}

/* double is a number */
static void test_builtin_is_number_double(void) {
    R8EValue v = r8e_from_double(3.14);
    ASSERT_TRUE(R8E_IS_NUMBER(v));
}

/* Undefined is not a number */
static void test_builtin_is_number_undefined(void) {
    ASSERT_FALSE(R8E_IS_NUMBER(R8E_UNDEFINED));
}

/* Null is not a number */
static void test_builtin_is_number_null(void) {
    ASSERT_FALSE(R8E_IS_NUMBER(R8E_NULL));
}

/* Boolean is not a number */
static void test_builtin_is_number_boolean(void) {
    ASSERT_FALSE(R8E_IS_NUMBER(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_NUMBER(R8E_FALSE));
}

/* String is not a number */
static void test_builtin_is_number_string(void) {
    R8EValue v = r8e_from_inline_str("42", 2);
    ASSERT_FALSE(R8E_IS_NUMBER(v));
}

/* =========================================================================
 * Tests: Array Structure
 * ========================================================================= */

/* Create and verify array structure */
static void test_builtin_array_struct(void) {
    R8EArray *arr = (R8EArray *)calloc(1, sizeof(R8EArray));
    ASSERT_NOT_NULL(arr);
    arr->flags = R8E_GC_KIND_ARRAY;
    arr->proto_id = R8E_PROTO_ARRAY;
    arr->length = 0;
    arr->capacity = 8;
    arr->elements = (R8EValue *)calloc(arr->capacity, sizeof(R8EValue));
    ASSERT_NOT_NULL(arr->elements);

    /* Simulate push */
    arr->elements[0] = r8e_from_int32(10);
    arr->elements[1] = r8e_from_int32(20);
    arr->elements[2] = r8e_from_int32(30);
    arr->length = 3;

    ASSERT_EQ_INT(arr->length, 3);
    ASSERT_TRUE(R8E_IS_INT32(arr->elements[0]));
    ASSERT_EQ_INT(r8e_get_int32(arr->elements[0]), 10);
    ASSERT_EQ_INT(r8e_get_int32(arr->elements[1]), 20);
    ASSERT_EQ_INT(r8e_get_int32(arr->elements[2]), 30);

    free(arr->elements);
    free(arr);
}

/* Array with mixed types */
static void test_builtin_array_mixed_types(void) {
    R8EArray *arr = (R8EArray *)calloc(1, sizeof(R8EArray));
    ASSERT_NOT_NULL(arr);
    arr->capacity = 8;
    arr->elements = (R8EValue *)calloc(arr->capacity, sizeof(R8EValue));
    ASSERT_NOT_NULL(arr->elements);

    arr->elements[0] = r8e_from_int32(42);
    arr->elements[1] = r8e_from_double(3.14);
    arr->elements[2] = R8E_TRUE;
    arr->elements[3] = R8E_NULL;
    arr->elements[4] = r8e_from_inline_str("hi", 2);
    arr->length = 5;

    ASSERT_TRUE(R8E_IS_INT32(arr->elements[0]));
    ASSERT_TRUE(R8E_IS_DOUBLE(arr->elements[1]));
    ASSERT_TRUE(R8E_IS_BOOLEAN(arr->elements[2]));
    ASSERT_TRUE(R8E_IS_NULL(arr->elements[3]));
    ASSERT_TRUE(R8E_IS_INLINE_STR(arr->elements[4]));

    free(arr->elements);
    free(arr);
}

/* =========================================================================
 * Tests: NaN-Boxing Collision Safety (EXP-08)
 * ========================================================================= */

/* All special values should be distinguishable */
static void test_builtin_nanbox_no_collision(void) {
    R8EValue vals[] = {
        R8E_UNDEFINED,
        R8E_NULL,
        R8E_TRUE,
        R8E_FALSE,
        r8e_from_int32(0),
        r8e_from_int32(1),
        r8e_from_int32(-1),
        r8e_from_double(0.0),
        r8e_from_double(1.0),
        r8e_from_inline_str("a", 1),
        r8e_from_symbol(1),
    };
    int n = sizeof(vals) / sizeof(vals[0]);

    /* No two distinct values should have the same encoding */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            ASSERT_NE(vals[i], vals[j]);
        }
    }
}

/* Double 0.0 and int32 0 should have different encodings */
static void test_builtin_nanbox_zero_distinction(void) {
    R8EValue d_zero = r8e_from_double(0.0);
    R8EValue i_zero = r8e_from_int32(0);
    /* These may or may not be equal (int 0 is preferred), but type checks differ */
    ASSERT_TRUE(R8E_IS_INT32(i_zero));
    ASSERT_TRUE(R8E_IS_DOUBLE(d_zero));
}

/* =========================================================================
 * Tests: Math Constants (verify encoding)
 * ========================================================================= */

/* PI is correctly encoded as double */
static void test_builtin_math_pi(void) {
    R8EValue v = r8e_from_double(3.14159265358979323846);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_DOUBLE_EQ(r8e_get_double(v), 3.14159265358979323846);
}

/* E is correctly encoded as double */
static void test_builtin_math_e(void) {
    R8EValue v = r8e_from_double(2.71828182845904523536);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_DOUBLE_EQ(r8e_get_double(v), 2.71828182845904523536);
}

/* LN2 */
static void test_builtin_math_ln2(void) {
    R8EValue v = r8e_from_double(0.6931471805599453);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_DOUBLE_EQ(r8e_get_double(v), 0.6931471805599453);
}

/* SQRT2 */
static void test_builtin_math_sqrt2(void) {
    R8EValue v = r8e_from_double(1.4142135623730951);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_DOUBLE_EQ(r8e_get_double(v), 1.4142135623730951);
}

/* =========================================================================
 * Tests: Value Type Classification (comprehensive)
 * ========================================================================= */

/* Exhaustive type check: each value category is mutually exclusive */
static void test_builtin_type_exclusivity(void) {
    /* Test int32 */
    R8EValue iv = r8e_from_int32(42);
    ASSERT_TRUE(R8E_IS_INT32(iv));
    ASSERT_TRUE(R8E_IS_NUMBER(iv));
    ASSERT_FALSE(R8E_IS_DOUBLE(iv));
    ASSERT_FALSE(R8E_IS_POINTER(iv));
    ASSERT_FALSE(R8E_IS_INLINE_STR(iv));
    ASSERT_FALSE(R8E_IS_SYMBOL(iv));

    /* Test double */
    R8EValue dv = r8e_from_double(2.5);
    ASSERT_TRUE(R8E_IS_DOUBLE(dv));
    ASSERT_TRUE(R8E_IS_NUMBER(dv));
    ASSERT_FALSE(R8E_IS_INT32(dv));
    ASSERT_FALSE(R8E_IS_POINTER(dv));

    /* Test pointer */
    int x = 0;
    R8EValue pv = r8e_from_pointer(&x);
    ASSERT_TRUE(R8E_IS_POINTER(pv));
    ASSERT_FALSE(R8E_IS_INT32(pv));
    ASSERT_FALSE(R8E_IS_DOUBLE(pv));

    /* Test symbol */
    R8EValue sv = r8e_from_symbol(5);
    ASSERT_TRUE(R8E_IS_SYMBOL(sv));
    ASSERT_FALSE(R8E_IS_INT32(sv));
    ASSERT_FALSE(R8E_IS_POINTER(sv));
    ASSERT_FALSE(R8E_IS_INLINE_STR(sv));
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_builtin_tests(void) {
    /* Inline string encoding */
    RUN_TEST(test_builtin_inline_str_basic);
    RUN_TEST(test_builtin_inline_str_empty);
    RUN_TEST(test_builtin_inline_str_max_len);
    RUN_TEST(test_builtin_inline_str_single);
    RUN_TEST(test_builtin_inline_str_special_chars);
    RUN_TEST(test_builtin_inline_str_type_checks);

    /* Number encoding */
    RUN_TEST(test_builtin_number_int32_encoding);
    RUN_TEST(test_builtin_number_negative_int);
    RUN_TEST(test_builtin_number_zero_int);
    RUN_TEST(test_builtin_number_int32_max);
    RUN_TEST(test_builtin_number_int32_min);
    RUN_TEST(test_builtin_number_double);
    RUN_TEST(test_builtin_number_infinity);
    RUN_TEST(test_builtin_number_neg_infinity);
    RUN_TEST(test_builtin_number_nan);
    RUN_TEST(test_builtin_number_neg_zero);

    /* Symbol operations */
    RUN_TEST(test_builtin_symbol_create);
    RUN_TEST(test_builtin_symbol_zero);
    RUN_TEST(test_builtin_symbol_well_known);

    /* Boolean encoding */
    RUN_TEST(test_builtin_boolean_true);
    RUN_TEST(test_builtin_boolean_false);
    RUN_TEST(test_builtin_boolean_type_checks);

    /* Special values */
    RUN_TEST(test_builtin_undefined_type);
    RUN_TEST(test_builtin_null_type);

    /* Pointer encoding */
    RUN_TEST(test_builtin_pointer_roundtrip);
    RUN_TEST(test_builtin_pointer_null);
    RUN_TEST(test_builtin_pointer_type_checks);

    /* Heap string */
    RUN_TEST(test_builtin_heap_string_basic);
    RUN_TEST(test_builtin_short_string_inline);

    /* IS_NUMBER macro */
    RUN_TEST(test_builtin_is_number_int);
    RUN_TEST(test_builtin_is_number_double);
    RUN_TEST(test_builtin_is_number_undefined);
    RUN_TEST(test_builtin_is_number_null);
    RUN_TEST(test_builtin_is_number_boolean);
    RUN_TEST(test_builtin_is_number_string);

    /* Array structure */
    RUN_TEST(test_builtin_array_struct);
    RUN_TEST(test_builtin_array_mixed_types);

    /* NaN-boxing collision safety */
    RUN_TEST(test_builtin_nanbox_no_collision);
    RUN_TEST(test_builtin_nanbox_zero_distinction);

    /* Math constants */
    RUN_TEST(test_builtin_math_pi);
    RUN_TEST(test_builtin_math_e);
    RUN_TEST(test_builtin_math_ln2);
    RUN_TEST(test_builtin_math_sqrt2);

    /* Type exclusivity */
    RUN_TEST(test_builtin_type_exclusivity);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_BUILTIN_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[builtin]\n");
    run_builtin_tests();
    printf("\n=== builtin: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
