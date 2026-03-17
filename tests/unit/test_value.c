/*
 * test_value.c - Unit tests for r8e_value.c
 *
 * Tests NaN-boxing encoding/decoding, type checks, type conversions,
 * equality operations, and typeof.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include <limits.h>

/* -------------------------------------------------------------------------
 * Pull in the same inline definitions r8e_value.c uses
 * ------------------------------------------------------------------------- */
typedef uint64_t R8EValue;

#define R8E_TAG_INT32       0xFFF8U
#define R8E_TAG_POINTER     0xFFF9U
#define R8E_TAG_SPECIAL     0xFFFAU
#define R8E_TAG_SYMBOL      0xFFFBU
#define R8E_TAG_ATOM        0xFFFCU
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
#define R8E_IS_TRUE(v)        ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)       ((v) == R8E_FALSE)
#define R8E_IS_BOOLEAN(v)     ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)      (((v) >> 48) == R8E_TAG_SYMBOL)
#define R8E_IS_ATOM(v)        (((v) >> 48) == R8E_TAG_ATOM)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == R8E_TAG_INLINE_STR)
#define R8E_IS_NULLISH(v)     (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))

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
    if (v >= 0xFFF8000000000000ULL && v <= 0xFFFFFFFFFFFFFFFFULL)
        v = 0x7FF8000000000000ULL;
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

/* Context stub */
typedef struct R8EContext R8EContext;
struct R8EContext {
    void *arena;
    void *atom_table;
    void *global_object;
    char  error_buf[256];
    int   has_error;
};

/* Extern declarations for functions in r8e_value.c */
extern R8EValue r8e_to_number(R8EContext *ctx, R8EValue val);
extern R8EValue r8e_to_boolean(R8EValue val);
extern R8EValue r8e_to_string(R8EContext *ctx, R8EValue val);
extern R8EValue r8e_abstract_eq(R8EContext *ctx, R8EValue a, R8EValue b);
extern R8EValue r8e_strict_eq(R8EValue a, R8EValue b);
extern const char *r8e_typeof_cstring(R8EValue val);
extern R8EValue r8e_same_value(R8EValue a, R8EValue b);
extern R8EValue r8e_same_value_zero(R8EValue a, R8EValue b);
extern bool r8e_value_is_number(R8EValue v);
extern bool r8e_value_is_string(R8EValue v);
extern bool r8e_value_is_boolean(R8EValue v);
extern bool r8e_value_is_null(R8EValue v);
extern bool r8e_value_is_undefined(R8EValue v);
extern bool r8e_value_is_symbol(R8EValue v);
extern bool r8e_value_is_nullish(R8EValue v);
extern bool r8e_value_is_int32(R8EValue v);
extern bool r8e_value_is_double(R8EValue v);
extern bool r8e_value_is_inline_string(R8EValue v);

/* Test infrastructure - from test_runner.c */
extern int g_assert_fail;
#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do {                                     \
    g_assert_fail = 0; extern int g_tests_run, g_tests_passed, g_tests_failed; \
    g_tests_run++;                                              \
    printf("  %-60s ", #name);                                  \
    test_##name();                                              \
    if (g_assert_fail) { g_tests_failed++; printf("FAIL\n"); } \
    else { g_tests_passed++; printf("ok\n"); }                  \
} while (0)

#define ASSERT_TRUE(expr)  do { if (!(expr)) { fprintf(stderr, "    ASSERT_TRUE(%s) failed at %s:%d\n", #expr, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_FALSE(expr) do { if (expr) { fprintf(stderr, "    ASSERT_FALSE(%s) failed at %s:%d\n", #expr, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ(a, b)    do { if ((uint64_t)(a) != (uint64_t)(b)) { fprintf(stderr, "    ASSERT_EQ(%s, %s) failed: 0x%llx vs 0x%llx at %s:%d\n", #a, #b, (unsigned long long)(uint64_t)(a), (unsigned long long)(uint64_t)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_NE(a, b)    do { if ((uint64_t)(a) == (uint64_t)(b)) { fprintf(stderr, "    ASSERT_NE(%s, %s) failed at %s:%d\n", #a, #b, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_INT(a, b) do { if ((long long)(a) != (long long)(b)) { fprintf(stderr, "    ASSERT_EQ_INT(%s, %s): %lld vs %lld at %s:%d\n", #a, #b, (long long)(a), (long long)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_DBL(a, b, eps) do { if (fabs((double)(a)-(double)(b)) > (double)(eps)) { fprintf(stderr, "    ASSERT_EQ_DBL(%s, %s): %.17g vs %.17g at %s:%d\n", #a, #b, (double)(a), (double)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_STR(a, b) do { if (strcmp((a),(b)) != 0) { fprintf(stderr, "    ASSERT_EQ_STR(%s, %s): \"%s\" vs \"%s\" at %s:%d\n", #a, #b, (a), (b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_VALUE_EQ(a, b) do { R8EValue _a=(a),_b=(b); if (_a!=_b) { fprintf(stderr, "    ASSERT_VALUE_EQ(%s, %s): 0x%016llx vs 0x%016llx at %s:%d\n", #a, #b, (unsigned long long)_a, (unsigned long long)_b, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_NAN(v) do { R8EValue _v=(v); if (!R8E_IS_DOUBLE(_v) || !isnan(r8e_get_double(_v))) { fprintf(stderr, "    ASSERT_NAN(%s) failed at %s:%d\n", #v, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)

/* =========================================================================
 * NaN-boxing encoding/decoding roundtrips
 * ========================================================================= */

TEST(double_roundtrip) {
    double vals[] = { 0.0, 1.0, -1.0, 3.14, 1e100, -1e-100, DBL_MAX, DBL_MIN };
    for (int i = 0; i < (int)(sizeof(vals)/sizeof(vals[0])); i++) {
        R8EValue v = r8e_from_double(vals[i]);
        ASSERT_TRUE(R8E_IS_DOUBLE(v));
        ASSERT_EQ_DBL(r8e_get_double(v), vals[i], 0.0);
    }
}

TEST(double_nan) {
    R8EValue v = r8e_from_double(NAN);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_TRUE(isnan(r8e_get_double(v)));
    /* NaN should be canonicalized */
    ASSERT_EQ(v, 0x7FF8000000000000ULL);
}

TEST(double_neg_zero) {
    R8EValue v = r8e_from_double(-0.0);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    double d = r8e_get_double(v);
    ASSERT_EQ_DBL(d, 0.0, 0.0);
    ASSERT_TRUE(signbit(d));
}

TEST(double_infinity) {
    R8EValue vinf = r8e_from_double(INFINITY);
    ASSERT_TRUE(R8E_IS_DOUBLE(vinf));
    ASSERT_TRUE(isinf(r8e_get_double(vinf)));
    ASSERT_TRUE(r8e_get_double(vinf) > 0);

    R8EValue vninf = r8e_from_double(-INFINITY);
    ASSERT_TRUE(R8E_IS_DOUBLE(vninf));
    ASSERT_TRUE(isinf(r8e_get_double(vninf)));
    ASSERT_TRUE(r8e_get_double(vninf) < 0);
}

TEST(int32_roundtrip) {
    int32_t vals[] = { 0, 1, -1, 42, -42, INT32_MAX, INT32_MIN, 1000000 };
    for (int i = 0; i < (int)(sizeof(vals)/sizeof(vals[0])); i++) {
        R8EValue v = r8e_from_int32(vals[i]);
        ASSERT_TRUE(R8E_IS_INT32(v));
        ASSERT_FALSE(R8E_IS_DOUBLE(v));
        ASSERT_EQ_INT(r8e_get_int32(v), vals[i]);
    }
}

TEST(pointer_roundtrip) {
    /* Use a stack variable address as a valid pointer */
    int dummy;
    void *ptr = &dummy;
    R8EValue v = r8e_from_pointer(ptr);
    ASSERT_TRUE(R8E_IS_POINTER(v));
    ASSERT_FALSE(R8E_IS_DOUBLE(v));
    ASSERT_FALSE(R8E_IS_INT32(v));
    ASSERT_EQ((uintptr_t)r8e_get_pointer(v), (uintptr_t)ptr);
}

TEST(bool_encoding) {
    R8EValue vt = r8e_from_boolean(true);
    R8EValue vf = r8e_from_boolean(false);
    ASSERT_VALUE_EQ(vt, R8E_TRUE);
    ASSERT_VALUE_EQ(vf, R8E_FALSE);
    ASSERT_TRUE(R8E_IS_BOOLEAN(vt));
    ASSERT_TRUE(R8E_IS_BOOLEAN(vf));
    ASSERT_TRUE(R8E_IS_TRUE(vt));
    ASSERT_TRUE(R8E_IS_FALSE(vf));
    ASSERT_FALSE(R8E_IS_DOUBLE(vt));
    ASSERT_FALSE(R8E_IS_INT32(vt));
}

TEST(null_undefined_encoding) {
    ASSERT_TRUE(R8E_IS_NULL(R8E_NULL));
    ASSERT_TRUE(R8E_IS_UNDEFINED(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_NULL(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_UNDEFINED(R8E_NULL));
    ASSERT_TRUE(R8E_IS_NULLISH(R8E_NULL));
    ASSERT_TRUE(R8E_IS_NULLISH(R8E_UNDEFINED));
    ASSERT_FALSE(R8E_IS_NULLISH(R8E_TRUE));
    ASSERT_FALSE(R8E_IS_NULLISH(r8e_from_int32(0)));
}

/* =========================================================================
 * Inline short strings
 * ========================================================================= */

TEST(inline_str_empty) {
    R8EValue v = r8e_from_inline_str("", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    ASSERT_EQ_INT(r8e_get_inline_str_len(v), 0);
    char buf[8];
    int len;
    r8e_get_inline_str(v, buf, &len);
    ASSERT_EQ_INT(len, 0);
    ASSERT_EQ_STR(buf, "");
}

TEST(inline_str_short) {
    const char *strings[] = { "a", "hi", "foo", "test", "hello", "abcdef", "1234567" };
    int lengths[] = { 1, 2, 3, 4, 5, 6, 7 };
    for (int i = 0; i < 7; i++) {
        R8EValue v = r8e_from_inline_str(strings[i], lengths[i]);
        ASSERT_TRUE(R8E_IS_INLINE_STR(v));
        ASSERT_EQ_INT(r8e_get_inline_str_len(v), lengths[i]);
        char buf[8];
        int len;
        r8e_get_inline_str(v, buf, &len);
        ASSERT_EQ_INT(len, lengths[i]);
        ASSERT_EQ_STR(buf, strings[i]);
    }
}

TEST(inline_str_identity) {
    /* Same string should produce same value */
    R8EValue a = r8e_from_inline_str("foo", 3);
    R8EValue b = r8e_from_inline_str("foo", 3);
    ASSERT_VALUE_EQ(a, b);

    /* Different strings should produce different values */
    R8EValue c = r8e_from_inline_str("bar", 3);
    ASSERT_NE(a, c);
}

/* =========================================================================
 * Type-check function tests
 * ========================================================================= */

TEST(type_check_functions) {
    ASSERT_TRUE(r8e_value_is_number(r8e_from_int32(42)));
    ASSERT_TRUE(r8e_value_is_number(r8e_from_double(3.14)));
    ASSERT_FALSE(r8e_value_is_number(R8E_NULL));
    ASSERT_FALSE(r8e_value_is_number(R8E_UNDEFINED));

    ASSERT_TRUE(r8e_value_is_boolean(R8E_TRUE));
    ASSERT_TRUE(r8e_value_is_boolean(R8E_FALSE));
    ASSERT_FALSE(r8e_value_is_boolean(r8e_from_int32(1)));

    ASSERT_TRUE(r8e_value_is_null(R8E_NULL));
    ASSERT_FALSE(r8e_value_is_null(R8E_UNDEFINED));

    ASSERT_TRUE(r8e_value_is_undefined(R8E_UNDEFINED));
    ASSERT_FALSE(r8e_value_is_undefined(R8E_NULL));

    ASSERT_TRUE(r8e_value_is_nullish(R8E_NULL));
    ASSERT_TRUE(r8e_value_is_nullish(R8E_UNDEFINED));
    ASSERT_FALSE(r8e_value_is_nullish(R8E_FALSE));

    ASSERT_TRUE(r8e_value_is_int32(r8e_from_int32(0)));
    ASSERT_FALSE(r8e_value_is_int32(r8e_from_double(0.0)));

    ASSERT_TRUE(r8e_value_is_double(r8e_from_double(1.5)));
    ASSERT_FALSE(r8e_value_is_double(r8e_from_int32(1)));

    ASSERT_TRUE(r8e_value_is_inline_string(r8e_from_inline_str("x", 1)));
    ASSERT_FALSE(r8e_value_is_inline_string(r8e_from_int32(0)));
}

/* =========================================================================
 * ToNumber conversion
 * ========================================================================= */

TEST(to_number_primitives) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* undefined -> NaN */
    ASSERT_NAN(r8e_to_number(&ctx, R8E_UNDEFINED));

    /* null -> 0 */
    R8EValue n = r8e_to_number(&ctx, R8E_NULL);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 0);

    /* true -> 1 */
    n = r8e_to_number(&ctx, R8E_TRUE);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 1);

    /* false -> 0 */
    n = r8e_to_number(&ctx, R8E_FALSE);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 0);
}

TEST(to_number_strings) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* "42" -> 42 */
    R8EValue s42 = r8e_from_inline_str("42", 2);
    R8EValue n42 = r8e_to_number(&ctx, s42);
    ASSERT_TRUE(R8E_IS_INT32(n42));
    ASSERT_EQ_INT(r8e_get_int32(n42), 42);

    /* "" -> 0 */
    R8EValue sempty = r8e_from_inline_str("", 0);
    R8EValue n0 = r8e_to_number(&ctx, sempty);
    ASSERT_TRUE(R8E_IS_INT32(n0));
    ASSERT_EQ_INT(r8e_get_int32(n0), 0);

    /* "abc" -> NaN */
    R8EValue sabc = r8e_from_inline_str("abc", 3);
    ASSERT_NAN(r8e_to_number(&ctx, sabc));

    /* "3.14" -> 3.14 */
    R8EValue spi = r8e_from_inline_str("3.14", 4);
    R8EValue npi = r8e_to_number(&ctx, spi);
    ASSERT_TRUE(R8E_IS_DOUBLE(npi));
    ASSERT_EQ_DBL(r8e_get_double(npi), 3.14, 1e-15);
}

TEST(to_number_passthrough) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue i = r8e_from_int32(7);
    ASSERT_VALUE_EQ(r8e_to_number(&ctx, i), i);

    R8EValue d = r8e_from_double(2.718);
    ASSERT_VALUE_EQ(r8e_to_number(&ctx, d), d);
}

/* =========================================================================
 * ToBoolean conversion
 * ========================================================================= */

TEST(to_boolean_falsy) {
    ASSERT_VALUE_EQ(r8e_to_boolean(R8E_UNDEFINED), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(R8E_NULL), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(R8E_FALSE), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_int32(0)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_double(0.0)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_double(-0.0)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_double(NAN)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_inline_str("", 0)), R8E_FALSE);
}

TEST(to_boolean_truthy) {
    ASSERT_VALUE_EQ(r8e_to_boolean(R8E_TRUE), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_int32(1)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_int32(-1)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_double(0.1)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_double(INFINITY)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_inline_str("x", 1)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_inline_str("false", 5)), R8E_TRUE);
}

/* =========================================================================
 * ToString conversion
 * ========================================================================= */

TEST(to_string_primitives) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* null -> "null" */
    R8EValue s = r8e_to_string(&ctx, R8E_NULL);
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    char buf[8]; int len;
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "null");

    /* true -> "true" */
    s = r8e_to_string(&ctx, R8E_TRUE);
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "true");

    /* false -> "false" */
    s = r8e_to_string(&ctx, R8E_FALSE);
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "false");
}

TEST(to_string_numbers) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 42 -> "42" */
    R8EValue s = r8e_to_string(&ctx, r8e_from_int32(42));
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    char buf[8]; int len;
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "42");

    /* 0 -> "0" */
    s = r8e_to_string(&ctx, r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "0");
}

/* =========================================================================
 * Abstract Equality (==)
 * ========================================================================= */

TEST(abstract_eq_null_undefined) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, R8E_UNDEFINED), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_UNDEFINED, R8E_NULL), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, R8E_NULL), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_UNDEFINED, R8E_UNDEFINED), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, R8E_FALSE), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, r8e_from_int32(0)), R8E_FALSE);
}

TEST(abstract_eq_number_string) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 0 == false -> true (false coerces to 0) */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, r8e_from_int32(0), R8E_FALSE), R8E_TRUE);

    /* "" == 0 -> true ("" coerces to 0) */
    R8EValue empty = r8e_from_inline_str("", 0);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, empty, r8e_from_int32(0)), R8E_TRUE);

    /* "42" == 42 -> true */
    R8EValue s42 = r8e_from_inline_str("42", 2);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, s42, r8e_from_int32(42)), R8E_TRUE);
}

/* =========================================================================
 * Strict Equality (===)
 * ========================================================================= */

TEST(strict_eq_nan) {
    /* NaN !== NaN */
    R8EValue nan1 = r8e_from_double(NAN);
    R8EValue nan2 = r8e_from_double(NAN);
    ASSERT_VALUE_EQ(r8e_strict_eq(nan1, nan2), R8E_FALSE);
}

TEST(strict_eq_zero) {
    /* +0 === -0 */
    R8EValue pz = r8e_from_double(0.0);
    R8EValue nz = r8e_from_double(-0.0);
    ASSERT_VALUE_EQ(r8e_strict_eq(pz, nz), R8E_TRUE);

    /* int 0 === double 0.0 */
    R8EValue iz = r8e_from_int32(0);
    ASSERT_VALUE_EQ(r8e_strict_eq(iz, pz), R8E_TRUE);
}

TEST(strict_eq_type_mismatch) {
    /* Different types -> false */
    ASSERT_VALUE_EQ(r8e_strict_eq(r8e_from_int32(1), R8E_TRUE), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_strict_eq(R8E_NULL, R8E_UNDEFINED), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_strict_eq(r8e_from_inline_str("1", 1), r8e_from_int32(1)), R8E_FALSE);
}

TEST(strict_eq_same_type) {
    /* Same type, same value */
    ASSERT_VALUE_EQ(r8e_strict_eq(r8e_from_int32(42), r8e_from_int32(42)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_strict_eq(R8E_TRUE, R8E_TRUE), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_strict_eq(R8E_FALSE, R8E_FALSE), R8E_TRUE);

    /* Same type, different value */
    ASSERT_VALUE_EQ(r8e_strict_eq(r8e_from_int32(1), r8e_from_int32(2)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_strict_eq(R8E_TRUE, R8E_FALSE), R8E_FALSE);
}

/* =========================================================================
 * SameValue and SameValueZero
 * ========================================================================= */

TEST(same_value) {
    /* NaN is SameValue as NaN */
    R8EValue nan = r8e_from_double(NAN);
    ASSERT_VALUE_EQ(r8e_same_value(nan, nan), R8E_TRUE);

    /* +0 is NOT SameValue as -0 */
    ASSERT_VALUE_EQ(r8e_same_value(r8e_from_double(0.0), r8e_from_double(-0.0)), R8E_FALSE);
}

TEST(same_value_zero) {
    /* NaN is SameValueZero as NaN */
    R8EValue nan = r8e_from_double(NAN);
    ASSERT_VALUE_EQ(r8e_same_value_zero(nan, nan), R8E_TRUE);

    /* +0 IS SameValueZero as -0 */
    ASSERT_VALUE_EQ(r8e_same_value_zero(r8e_from_double(0.0), r8e_from_double(-0.0)), R8E_TRUE);
}

/* =========================================================================
 * typeof operator
 * ========================================================================= */

TEST(typeof_values) {
    ASSERT_EQ_STR(r8e_typeof_cstring(R8E_UNDEFINED), "undefined");
    ASSERT_EQ_STR(r8e_typeof_cstring(R8E_NULL), "object");  /* typeof null === "object" */
    ASSERT_EQ_STR(r8e_typeof_cstring(R8E_TRUE), "boolean");
    ASSERT_EQ_STR(r8e_typeof_cstring(R8E_FALSE), "boolean");
    ASSERT_EQ_STR(r8e_typeof_cstring(r8e_from_int32(42)), "number");
    ASSERT_EQ_STR(r8e_typeof_cstring(r8e_from_double(3.14)), "number");
    ASSERT_EQ_STR(r8e_typeof_cstring(r8e_from_double(NAN)), "number");
    ASSERT_EQ_STR(r8e_typeof_cstring(r8e_from_double(INFINITY)), "number");
    ASSERT_EQ_STR(r8e_typeof_cstring(r8e_from_inline_str("hi", 2)), "string");
}

/* =========================================================================
 * Additional NaN-boxing edge cases
 * ========================================================================= */

TEST(double_max_safe_integer) {
    /* MAX_SAFE_INTEGER = 2^53 - 1 = 9007199254740991 */
    double max_safe = 9007199254740991.0;
    R8EValue v = r8e_from_double(max_safe);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_EQ_DBL(r8e_get_double(v), max_safe, 0.0);

    /* MIN_SAFE_INTEGER = -(2^53 - 1) */
    double min_safe = -9007199254740991.0;
    R8EValue v2 = r8e_from_double(min_safe);
    ASSERT_TRUE(R8E_IS_DOUBLE(v2));
    ASSERT_EQ_DBL(r8e_get_double(v2), min_safe, 0.0);
}

TEST(double_subnormal) {
    /* Subnormal (denormalized) doubles should roundtrip */
    double subnorm = 5e-324; /* smallest positive subnormal */
    R8EValue v = r8e_from_double(subnorm);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_EQ_DBL(r8e_get_double(v), subnorm, 0.0);
}

TEST(double_negative_nan_canonicalized) {
    /* Negative NaN patterns should be canonicalized */
    /* A negative NaN has bit pattern >= 0xFFF8... which collides with tags */
    uint64_t neg_nan_pattern = 0xFFF8000000000001ULL;
    double d;
    memcpy(&d, &neg_nan_pattern, 8);
    R8EValue v = r8e_from_double(d);
    /* Should be canonicalized to positive NaN */
    ASSERT_EQ(v, 0x7FF8000000000000ULL);
    ASSERT_TRUE(R8E_IS_DOUBLE(v));
    ASSERT_TRUE(isnan(r8e_get_double(v)));
}

TEST(int32_boundary_values) {
    /* INT32_MAX and INT32_MIN boundary checks */
    R8EValue vmax = r8e_from_int32(INT32_MAX);
    ASSERT_TRUE(R8E_IS_INT32(vmax));
    ASSERT_EQ_INT(r8e_get_int32(vmax), INT32_MAX);
    ASSERT_FALSE(R8E_IS_DOUBLE(vmax));

    R8EValue vmin = r8e_from_int32(INT32_MIN);
    ASSERT_TRUE(R8E_IS_INT32(vmin));
    ASSERT_EQ_INT(r8e_get_int32(vmin), INT32_MIN);
    ASSERT_FALSE(R8E_IS_DOUBLE(vmin));

    /* Adjacent values */
    R8EValue v1 = r8e_from_int32(INT32_MAX - 1);
    ASSERT_EQ_INT(r8e_get_int32(v1), INT32_MAX - 1);
    R8EValue v2 = r8e_from_int32(INT32_MIN + 1);
    ASSERT_EQ_INT(r8e_get_int32(v2), INT32_MIN + 1);
}

TEST(pointer_null) {
    /* NULL pointer should still encode/decode correctly */
    R8EValue v = r8e_from_pointer(NULL);
    ASSERT_TRUE(R8E_IS_POINTER(v));
    ASSERT_TRUE(r8e_get_pointer(v) == NULL);
}

TEST(symbol_encoding) {
    /* Symbol values should be correctly identified */
    R8EValue sym = 0xFFFB000000000001ULL;
    ASSERT_TRUE(R8E_IS_SYMBOL(sym));
    ASSERT_FALSE(R8E_IS_DOUBLE(sym));
    ASSERT_FALSE(R8E_IS_INT32(sym));
    ASSERT_FALSE(R8E_IS_POINTER(sym));
    ASSERT_FALSE(R8E_IS_BOOLEAN(sym));
    ASSERT_FALSE(R8E_IS_INLINE_STR(sym));
}

TEST(atom_encoding) {
    /* Atom values should be correctly identified */
    R8EValue atom = 0xFFFC000000000042ULL;
    ASSERT_TRUE(R8E_IS_ATOM(atom));
    ASSERT_FALSE(R8E_IS_DOUBLE(atom));
    ASSERT_FALSE(R8E_IS_INT32(atom));
    ASSERT_FALSE(R8E_IS_SYMBOL(atom));
}

/* =========================================================================
 * Inline string edge cases
 * ========================================================================= */

TEST(inline_str_all_lengths) {
    /* Test every valid length from 0 to 7 */
    const char *test_strings[] = {
        "", "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg"
    };
    for (int len = 0; len <= 7; len++) {
        R8EValue v = r8e_from_inline_str(test_strings[len], len);
        ASSERT_TRUE(R8E_IS_INLINE_STR(v));
        ASSERT_EQ_INT(r8e_get_inline_str_len(v), len);
        char buf[8];
        int out_len;
        r8e_get_inline_str(v, buf, &out_len);
        ASSERT_EQ_INT(out_len, len);
        for (int i = 0; i < len; i++) {
            ASSERT_TRUE(buf[i] == test_strings[len][i]);
        }
    }
}

TEST(inline_str_special_chars) {
    /* Test ASCII special characters (digits, punctuation) */
    R8EValue v1 = r8e_from_inline_str("0", 1);
    char buf[8]; int len;
    r8e_get_inline_str(v1, buf, &len);
    ASSERT_EQ_STR(buf, "0");

    R8EValue v2 = r8e_from_inline_str(" !@#$%", 6);
    r8e_get_inline_str(v2, buf, &len);
    ASSERT_EQ_INT(len, 6);
    ASSERT_EQ_STR(buf, " !@#$%");

    R8EValue v3 = r8e_from_inline_str("~{}", 3);
    r8e_get_inline_str(v3, buf, &len);
    ASSERT_EQ_STR(buf, "~{}");
}

TEST(inline_str_null_len_param) {
    /* Test r8e_get_inline_str with NULL len parameter */
    R8EValue v = r8e_from_inline_str("hi", 2);
    char buf[8];
    r8e_get_inline_str(v, buf, NULL);
    ASSERT_EQ_STR(buf, "hi");
}

/* =========================================================================
 * Extended type-check function tests
 * ========================================================================= */

TEST(type_check_symbol) {
    R8EValue sym = 0xFFFB000000000001ULL;
    ASSERT_TRUE(r8e_value_is_symbol(sym));
    ASSERT_FALSE(r8e_value_is_number(sym));
    ASSERT_FALSE(r8e_value_is_boolean(sym));
    ASSERT_FALSE(r8e_value_is_null(sym));
    ASSERT_FALSE(r8e_value_is_undefined(sym));
    ASSERT_FALSE(r8e_value_is_nullish(sym));
    ASSERT_FALSE(r8e_value_is_inline_string(sym));
}

TEST(type_check_nan_infinity) {
    /* NaN and Infinity are numbers */
    ASSERT_TRUE(r8e_value_is_number(r8e_from_double(NAN)));
    ASSERT_TRUE(r8e_value_is_number(r8e_from_double(INFINITY)));
    ASSERT_TRUE(r8e_value_is_number(r8e_from_double(-INFINITY)));
    ASSERT_TRUE(r8e_value_is_double(r8e_from_double(NAN)));
    ASSERT_TRUE(r8e_value_is_double(r8e_from_double(INFINITY)));
    ASSERT_FALSE(r8e_value_is_int32(r8e_from_double(NAN)));
}

/* =========================================================================
 * Extended ToNumber conversion
 * ========================================================================= */

TEST(to_number_whitespace_strings) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* "  42  " (whitespace around number) -> 42 */
    R8EValue s = r8e_from_inline_str("  42  ", 6);
    R8EValue n = r8e_to_number(&ctx, s);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 42);

    /* "   " (only whitespace) -> 0 */
    R8EValue ws = r8e_from_inline_str("   ", 3);
    R8EValue nws = r8e_to_number(&ctx, ws);
    ASSERT_TRUE(R8E_IS_INT32(nws));
    ASSERT_EQ_INT(r8e_get_int32(nws), 0);
}

TEST(to_number_hex_string) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue s = r8e_from_inline_str("0xff", 4);
    R8EValue n = r8e_to_number(&ctx, s);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 255);
}

TEST(to_number_binary_string) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue s = r8e_from_inline_str("0b1010", 6);
    R8EValue n = r8e_to_number(&ctx, s);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 10);
}

TEST(to_number_octal_string) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue s = r8e_from_inline_str("0o77", 4);
    R8EValue n = r8e_to_number(&ctx, s);
    ASSERT_TRUE(R8E_IS_INT32(n));
    ASSERT_EQ_INT(r8e_get_int32(n), 63);
}

TEST(to_number_infinity_strings) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* "+Infinity" and "-Infinity" are > 7 chars so won't be inline strings,
     * but "Infinity" fits as an inline str concept... Actually "Infinity" is
     * 8 chars, too long for inline. Let's test what we can. */

    /* Test negative number string */
    R8EValue s = r8e_from_inline_str("-3.14", 5);
    R8EValue n = r8e_to_number(&ctx, s);
    ASSERT_TRUE(R8E_IS_DOUBLE(n));
    ASSERT_EQ_DBL(r8e_get_double(n), -3.14, 1e-15);
}

TEST(to_number_symbol_error) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue sym = 0xFFFB000000000001ULL;
    R8EValue n = r8e_to_number(&ctx, sym);
    ASSERT_TRUE(R8E_IS_DOUBLE(n));
    ASSERT_TRUE(isnan(r8e_get_double(n)));
    ASSERT_TRUE(ctx.has_error);
}

/* =========================================================================
 * Extended ToBoolean conversion
 * ========================================================================= */

TEST(to_boolean_negative_infinity) {
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_double(-INFINITY)), R8E_TRUE);
}

TEST(to_boolean_symbol) {
    /* Symbols are always truthy */
    R8EValue sym = 0xFFFB000000000001ULL;
    ASSERT_VALUE_EQ(r8e_to_boolean(sym), R8E_TRUE);
}

TEST(to_boolean_int32_edge) {
    /* INT32_MIN and INT32_MAX should be truthy */
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_int32(INT32_MIN)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_to_boolean(r8e_from_int32(INT32_MAX)), R8E_TRUE);
}

/* =========================================================================
 * Extended ToString conversion
 * ========================================================================= */

TEST(to_string_undefined) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* undefined -> "undefined" (9 chars = heap string) */
    R8EValue s = r8e_to_string(&ctx, R8E_UNDEFINED);
    /* "undefined" is > 7 chars, so it goes to heap */
    ASSERT_TRUE(R8E_IS_POINTER(s) || R8E_IS_INLINE_STR(s));
}

TEST(to_string_int_zero) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue s = r8e_to_string(&ctx, r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    char buf[8]; int len;
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "0");
}

TEST(to_string_negative_int) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue s = r8e_to_string(&ctx, r8e_from_int32(-1));
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    char buf[8]; int len;
    r8e_get_inline_str(s, buf, &len);
    ASSERT_EQ_STR(buf, "-1");
}

TEST(to_string_string_passthrough) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue v = r8e_from_inline_str("hello", 5);
    R8EValue s = r8e_to_string(&ctx, v);
    ASSERT_VALUE_EQ(s, v);
}

/* =========================================================================
 * ToInt32 and ToUint32 conversions
 * ========================================================================= */

extern R8EValue r8e_to_int32(R8EContext *ctx, R8EValue val);
extern R8EValue r8e_to_uint32(R8EContext *ctx, R8EValue val);

TEST(to_int32_passthrough) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue v = r8e_from_int32(42);
    R8EValue r = r8e_to_int32(&ctx, v);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

TEST(to_int32_from_double) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue r = r8e_to_int32(&ctx, r8e_from_double(3.7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 3);
}

TEST(to_int32_nan) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue r = r8e_to_int32(&ctx, r8e_from_double(NAN));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);
}

TEST(to_int32_infinity) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue r = r8e_to_int32(&ctx, r8e_from_double(INFINITY));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);
}

TEST(to_int32_large_double) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 2^32 + 5 should wrap to 5 */
    R8EValue r = r8e_to_int32(&ctx, r8e_from_double(4294967301.0));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 5);
}

TEST(to_uint32_negative) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* -1 as uint32 = 4294967295 -> stored as double since > INT32_MAX */
    R8EValue r = r8e_to_uint32(&ctx, r8e_from_int32(-1));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 4294967295.0, 0.0);
}

TEST(to_uint32_positive) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue r = r8e_to_uint32(&ctx, r8e_from_int32(42));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

/* =========================================================================
 * Extended Abstract Equality (==)
 * ========================================================================= */

TEST(abstract_eq_bool_coercion) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* true == 1 -> true (true coerces to 1) */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_TRUE, r8e_from_int32(1)), R8E_TRUE);

    /* false == 0 -> true */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_FALSE, r8e_from_int32(0)), R8E_TRUE);

    /* true == 2 -> false */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_TRUE, r8e_from_int32(2)), R8E_FALSE);

    /* false == "" -> true (both coerce to 0) */
    R8EValue empty = r8e_from_inline_str("", 0);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_FALSE, empty), R8E_TRUE);
}

TEST(abstract_eq_same_type) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Same type delegates to strict_eq */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, r8e_from_int32(42), r8e_from_int32(42)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, r8e_from_int32(1), r8e_from_int32(2)), R8E_FALSE);

    /* NaN == NaN -> false (strict eq) */
    R8EValue nan = r8e_from_double(NAN);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, nan, nan), R8E_FALSE);
}

TEST(abstract_eq_string_number) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* "0" == 0 -> true */
    R8EValue s0 = r8e_from_inline_str("0", 1);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, s0, r8e_from_int32(0)), R8E_TRUE);

    /* "1" == 1 -> true */
    R8EValue s1 = r8e_from_inline_str("1", 1);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, s1, r8e_from_int32(1)), R8E_TRUE);

    /* Number == String (reversed order) */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, r8e_from_int32(1), s1), R8E_TRUE);
}

TEST(abstract_eq_null_not_equal_to_other) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* null is only == undefined, not == 0, false, "", etc. */
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, r8e_from_int32(0)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, R8E_FALSE), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_NULL, r8e_from_inline_str("", 0)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_abstract_eq(&ctx, R8E_UNDEFINED, r8e_from_int32(0)), R8E_FALSE);
}

/* =========================================================================
 * Extended Strict Equality (===)
 * ========================================================================= */

TEST(strict_eq_inline_strings) {
    /* Same inline strings -> true */
    R8EValue a = r8e_from_inline_str("hello", 5);
    R8EValue b = r8e_from_inline_str("hello", 5);
    ASSERT_VALUE_EQ(r8e_strict_eq(a, b), R8E_TRUE);

    /* Different inline strings -> false */
    R8EValue c = r8e_from_inline_str("world", 5);
    ASSERT_VALUE_EQ(r8e_strict_eq(a, c), R8E_FALSE);
}

TEST(strict_eq_int_double_same_value) {
    /* int32(5) === double(5.0) -> true (both are number type) */
    ASSERT_VALUE_EQ(r8e_strict_eq(r8e_from_int32(5), r8e_from_double(5.0)), R8E_TRUE);
}

TEST(strict_eq_both_undefined) {
    ASSERT_VALUE_EQ(r8e_strict_eq(R8E_UNDEFINED, R8E_UNDEFINED), R8E_TRUE);
}

TEST(strict_eq_both_null) {
    ASSERT_VALUE_EQ(r8e_strict_eq(R8E_NULL, R8E_NULL), R8E_TRUE);
}

/* =========================================================================
 * Extended SameValue and SameValueZero
 * ========================================================================= */

TEST(same_value_int_vs_double) {
    /* int32(0) vs double(0.0) -> true (SameValue, both positive zero) */
    ASSERT_VALUE_EQ(r8e_same_value(r8e_from_int32(0), r8e_from_double(0.0)), R8E_TRUE);

    /* int32(0) vs double(-0.0) -> false (SameValue distinguishes +0 and -0) */
    ASSERT_VALUE_EQ(r8e_same_value(r8e_from_int32(0), r8e_from_double(-0.0)), R8E_FALSE);
}

TEST(same_value_zero_int_vs_double) {
    /* int32(0) vs double(-0.0) -> true (SameValueZero treats them as equal) */
    ASSERT_VALUE_EQ(r8e_same_value_zero(r8e_from_int32(0), r8e_from_double(-0.0)), R8E_TRUE);
}

TEST(same_value_different_types) {
    /* Different types -> false */
    ASSERT_VALUE_EQ(r8e_same_value(r8e_from_int32(1), R8E_TRUE), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_same_value(R8E_NULL, R8E_UNDEFINED), R8E_FALSE);
}

/* =========================================================================
 * typeof extended
 * ========================================================================= */

TEST(typeof_symbol) {
    R8EValue sym = 0xFFFB000000000001ULL;
    ASSERT_EQ_STR(r8e_typeof_cstring(sym), "symbol");
}

TEST(typeof_neg_zero) {
    /* typeof -0 === "number" */
    ASSERT_EQ_STR(r8e_typeof_cstring(r8e_from_double(-0.0)), "number");
}

/* =========================================================================
 * Debug printing: r8e_value_to_cstring
 * ========================================================================= */

extern const char *r8e_value_to_cstring(R8EContext *ctx, R8EValue val);

TEST(value_to_cstring_primitives) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, R8E_UNDEFINED), "undefined");
    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, R8E_NULL), "null");
    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, R8E_TRUE), "true");
    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, R8E_FALSE), "false");
    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, r8e_from_int32(42)), "42");
}

TEST(value_to_cstring_special_numbers) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, r8e_from_double(NAN)), "NaN");
    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, r8e_from_double(-0.0)), "-0");
}

TEST(value_to_cstring_inline_str) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    R8EValue v = r8e_from_inline_str("hello", 5);
    ASSERT_EQ_STR(r8e_value_to_cstring(&ctx, v), "hello");
}

TEST(value_to_cstring_null_ctx) {
    /* Should work with NULL ctx using static buffer */
    ASSERT_EQ_STR(r8e_value_to_cstring(NULL, R8E_NULL), "null");
    ASSERT_EQ_STR(r8e_value_to_cstring(NULL, r8e_from_int32(7)), "7");
}

/* =========================================================================
 * No-collision sanity: distinct types have distinct bit patterns
 * ========================================================================= */

TEST(no_tag_collisions) {
    R8EValue vals[] = {
        r8e_from_double(1.0),
        r8e_from_int32(1),
        R8E_UNDEFINED,
        R8E_NULL,
        R8E_TRUE,
        R8E_FALSE,
        r8e_from_inline_str("a", 1),
    };
    int n = (int)(sizeof(vals)/sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            ASSERT_NE(vals[i], vals[j]);
        }
    }
}

/* =========================================================================
 * Test suite entry point
 * ========================================================================= */

void test_suite_value(void) {
    /* Encoding/decoding roundtrips */
    RUN_TEST(double_roundtrip);
    RUN_TEST(double_nan);
    RUN_TEST(double_neg_zero);
    RUN_TEST(double_infinity);
    RUN_TEST(int32_roundtrip);
    RUN_TEST(pointer_roundtrip);
    RUN_TEST(bool_encoding);
    RUN_TEST(null_undefined_encoding);

    /* Inline short strings */
    RUN_TEST(inline_str_empty);
    RUN_TEST(inline_str_short);
    RUN_TEST(inline_str_identity);

    /* Type-check functions */
    RUN_TEST(type_check_functions);

    /* ToNumber */
    RUN_TEST(to_number_primitives);
    RUN_TEST(to_number_strings);
    RUN_TEST(to_number_passthrough);

    /* ToBoolean */
    RUN_TEST(to_boolean_falsy);
    RUN_TEST(to_boolean_truthy);

    /* ToString */
    RUN_TEST(to_string_primitives);
    RUN_TEST(to_string_numbers);

    /* Abstract Equality */
    RUN_TEST(abstract_eq_null_undefined);
    RUN_TEST(abstract_eq_number_string);

    /* Strict Equality */
    RUN_TEST(strict_eq_nan);
    RUN_TEST(strict_eq_zero);
    RUN_TEST(strict_eq_type_mismatch);
    RUN_TEST(strict_eq_same_type);

    /* SameValue / SameValueZero */
    RUN_TEST(same_value);
    RUN_TEST(same_value_zero);

    /* typeof */
    RUN_TEST(typeof_values);

    /* No collisions */
    RUN_TEST(no_tag_collisions);

    /* Additional NaN-boxing edge cases */
    RUN_TEST(double_max_safe_integer);
    RUN_TEST(double_subnormal);
    RUN_TEST(double_negative_nan_canonicalized);
    RUN_TEST(int32_boundary_values);
    RUN_TEST(pointer_null);
    RUN_TEST(symbol_encoding);
    RUN_TEST(atom_encoding);

    /* Inline string edge cases */
    RUN_TEST(inline_str_all_lengths);
    RUN_TEST(inline_str_special_chars);
    RUN_TEST(inline_str_null_len_param);

    /* Extended type checks */
    RUN_TEST(type_check_symbol);
    RUN_TEST(type_check_nan_infinity);

    /* Extended ToNumber */
    RUN_TEST(to_number_whitespace_strings);
    RUN_TEST(to_number_hex_string);
    RUN_TEST(to_number_binary_string);
    RUN_TEST(to_number_octal_string);
    RUN_TEST(to_number_infinity_strings);
    RUN_TEST(to_number_symbol_error);

    /* Extended ToBoolean */
    RUN_TEST(to_boolean_negative_infinity);
    RUN_TEST(to_boolean_symbol);
    RUN_TEST(to_boolean_int32_edge);

    /* Extended ToString */
    RUN_TEST(to_string_undefined);
    RUN_TEST(to_string_int_zero);
    RUN_TEST(to_string_negative_int);
    RUN_TEST(to_string_string_passthrough);

    /* ToInt32 / ToUint32 */
    RUN_TEST(to_int32_passthrough);
    RUN_TEST(to_int32_from_double);
    RUN_TEST(to_int32_nan);
    RUN_TEST(to_int32_infinity);
    RUN_TEST(to_int32_large_double);
    RUN_TEST(to_uint32_negative);
    RUN_TEST(to_uint32_positive);

    /* Extended Abstract Equality */
    RUN_TEST(abstract_eq_bool_coercion);
    RUN_TEST(abstract_eq_same_type);
    RUN_TEST(abstract_eq_string_number);
    RUN_TEST(abstract_eq_null_not_equal_to_other);

    /* Extended Strict Equality */
    RUN_TEST(strict_eq_inline_strings);
    RUN_TEST(strict_eq_int_double_same_value);
    RUN_TEST(strict_eq_both_undefined);
    RUN_TEST(strict_eq_both_null);

    /* Extended SameValue / SameValueZero */
    RUN_TEST(same_value_int_vs_double);
    RUN_TEST(same_value_zero_int_vs_double);
    RUN_TEST(same_value_different_types);

    /* Extended typeof */
    RUN_TEST(typeof_symbol);
    RUN_TEST(typeof_neg_zero);

    /* Debug printing */
    RUN_TEST(value_to_cstring_primitives);
    RUN_TEST(value_to_cstring_special_numbers);
    RUN_TEST(value_to_cstring_inline_str);
    RUN_TEST(value_to_cstring_null_ctx);
}
