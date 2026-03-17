/*
 * test_number.c - Unit tests for r8e_number.c
 *
 * Tests arithmetic with int32 fast paths, overflow to double, bitwise
 * operations, number-to-string, string-to-number, and Math methods.
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
 * NaN-boxing definitions
 * ------------------------------------------------------------------------- */
typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 48) == 0xFFF8U)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == 0xFFFDU)
#define R8E_IS_POINTER(v)     (((v) >> 48) == 0xFFF9U)

static inline double r8e_get_double(R8EValue v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
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
static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    R8EValue v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i2 = 0; i2 < len && i2 < 7; i2++)
        v |= ((uint64_t)(uint8_t)s[i2] << (38 - i2 * 7));
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
static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Heap string header for extracting number-to-string results */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
} R8EString;

static inline const char *r8e_string_data(const R8EString *s) {
    return (const char *)(s + 1);
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

/* Extern declarations for r8e_number.c functions */
extern R8EValue r8e_num_add(R8EValue a, R8EValue b);
extern R8EValue r8e_num_sub(R8EValue a, R8EValue b);
extern R8EValue r8e_num_mul(R8EValue a, R8EValue b);
extern R8EValue r8e_num_div(R8EValue a, R8EValue b);
extern R8EValue r8e_num_mod(R8EValue a, R8EValue b);
extern R8EValue r8e_num_neg(R8EValue a);
extern R8EValue r8e_num_inc(R8EValue a);
extern R8EValue r8e_num_dec(R8EValue a);

extern R8EValue r8e_num_bitand(R8EValue a, R8EValue b);
extern R8EValue r8e_num_bitor(R8EValue a, R8EValue b);
extern R8EValue r8e_num_bitxor(R8EValue a, R8EValue b);
extern R8EValue r8e_num_bitnot(R8EValue a);
extern R8EValue r8e_num_shl(R8EValue a, R8EValue b);
extern R8EValue r8e_num_shr(R8EValue a, R8EValue b);
extern R8EValue r8e_num_ushr(R8EValue a, R8EValue b);

extern R8EValue r8e_num_to_string(R8EContext *ctx, R8EValue val);
extern R8EValue r8e_string_to_number(const char *str, uint32_t len);

extern R8EValue r8e_math_abs(R8EValue v);
extern R8EValue r8e_math_floor(R8EValue v);
extern R8EValue r8e_math_ceil(R8EValue v);
extern R8EValue r8e_math_round(R8EValue v);
extern R8EValue r8e_math_trunc(R8EValue v);
extern R8EValue r8e_math_sqrt(R8EValue v);
extern R8EValue r8e_math_pow(R8EValue base, R8EValue exp);
extern R8EValue r8e_math_min(R8EValue a, R8EValue b);
extern R8EValue r8e_math_max(R8EValue a, R8EValue b);
extern R8EValue r8e_math_sign(R8EValue v);
extern R8EValue r8e_math_clz32(R8EValue v);
extern bool     r8e_is_nan(R8EValue v);
extern bool     r8e_is_finite(R8EValue v);
extern bool     r8e_is_neg_zero(R8EValue v);

/* Test infrastructure */
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
#define ASSERT_EQ_INT(a, b) do { if ((long long)(a) != (long long)(b)) { fprintf(stderr, "    ASSERT_EQ_INT(%s, %s): %lld vs %lld at %s:%d\n", #a, #b, (long long)(a), (long long)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_DBL(a, b, eps) do { if (fabs((double)(a)-(double)(b)) > (double)(eps)) { fprintf(stderr, "    ASSERT_EQ_DBL(%s, %s): %.17g vs %.17g at %s:%d\n", #a, #b, (double)(a), (double)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_STR(a, b) do { if (strcmp((a),(b)) != 0) { fprintf(stderr, "    ASSERT_EQ_STR(%s, %s): \"%s\" vs \"%s\" at %s:%d\n", #a, #b, (a), (b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_VALUE_EQ(a, b) do { R8EValue _a=(a),_b=(b); if (_a!=_b) { fprintf(stderr, "    ASSERT_VALUE_EQ(%s, %s): 0x%016llx vs 0x%016llx at %s:%d\n", #a, #b, (unsigned long long)_a, (unsigned long long)_b, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_NAN(v) do { R8EValue _v=(v); if (!R8E_IS_DOUBLE(_v) || !isnan(r8e_get_double(_v))) { fprintf(stderr, "    ASSERT_NAN(%s) failed at %s:%d\n", #v, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)

/* Helper to extract string result */
static const char *val_to_cstr(R8EValue v, char *buf) {
    if (R8E_IS_INLINE_STR(v)) {
        int len;
        r8e_get_inline_str(v, buf, &len);
        return buf;
    }
    if (R8E_IS_POINTER(v)) {
        const R8EString *s = (const R8EString *)r8e_get_pointer(v);
        return r8e_string_data(s);
    }
    return "(not a string)";
}

/* =========================================================================
 * Int32 fast path arithmetic
 * ========================================================================= */

TEST(add_int32) {
    R8EValue r = r8e_num_add(r8e_from_int32(3), r8e_from_int32(4));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 7);
}

TEST(add_int32_negative) {
    R8EValue r = r8e_num_add(r8e_from_int32(-5), r8e_from_int32(3));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -2);
}

TEST(sub_int32) {
    R8EValue r = r8e_num_sub(r8e_from_int32(10), r8e_from_int32(3));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 7);
}

TEST(mul_int32) {
    R8EValue r = r8e_num_mul(r8e_from_int32(6), r8e_from_int32(7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

/* =========================================================================
 * Overflow to double
 * ========================================================================= */

TEST(add_overflow) {
    R8EValue r = r8e_num_add(r8e_from_int32(INT32_MAX), r8e_from_int32(1));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), (double)INT32_MAX + 1.0, 0.0);
}

TEST(sub_overflow) {
    R8EValue r = r8e_num_sub(r8e_from_int32(INT32_MIN), r8e_from_int32(1));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), (double)INT32_MIN - 1.0, 0.0);
}

TEST(mul_overflow) {
    R8EValue r = r8e_num_mul(r8e_from_int32(INT32_MAX), r8e_from_int32(2));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), (double)INT32_MAX * 2.0, 0.0);
}

/* =========================================================================
 * Division
 * ========================================================================= */

TEST(div_exact) {
    R8EValue r = r8e_num_div(r8e_from_int32(10), r8e_from_int32(2));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 5);
}

TEST(div_non_exact) {
    R8EValue r = r8e_num_div(r8e_from_int32(7), r8e_from_int32(2));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 3.5, 0.0);
}

TEST(div_by_zero) {
    R8EValue r1 = r8e_num_div(r8e_from_int32(5), r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_DOUBLE(r1));
    ASSERT_TRUE(isinf(r8e_get_double(r1)));
    ASSERT_TRUE(r8e_get_double(r1) > 0);

    R8EValue r2 = r8e_num_div(r8e_from_int32(-5), r8e_from_int32(0));
    ASSERT_TRUE(isinf(r8e_get_double(r2)));
    ASSERT_TRUE(r8e_get_double(r2) < 0);

    R8EValue r3 = r8e_num_div(r8e_from_int32(0), r8e_from_int32(0));
    ASSERT_NAN(r3);
}

/* =========================================================================
 * Modulo
 * ========================================================================= */

TEST(mod_positive) {
    R8EValue r = r8e_num_mod(r8e_from_int32(7), r8e_from_int32(3));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1);
}

TEST(mod_negative) {
    R8EValue r = r8e_num_mod(r8e_from_int32(-7), r8e_from_int32(3));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -1);
}

TEST(mod_neg_zero) {
    /* -7 % 7 = -0 */
    R8EValue r = r8e_num_mod(r8e_from_int32(-7), r8e_from_int32(7));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(mod_by_zero) {
    ASSERT_NAN(r8e_num_mod(r8e_from_int32(5), r8e_from_int32(0)));
}

/* =========================================================================
 * Negation and increment/decrement
 * ========================================================================= */

TEST(neg_int32) {
    R8EValue r = r8e_num_neg(r8e_from_int32(5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -5);
}

TEST(neg_zero_produces_neg_zero) {
    R8EValue r = r8e_num_neg(r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(inc_int32) {
    R8EValue r = r8e_num_inc(r8e_from_int32(41));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

TEST(inc_overflow) {
    R8EValue r = r8e_num_inc(r8e_from_int32(INT32_MAX));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), (double)INT32_MAX + 1.0, 0.0);
}

TEST(dec_int32) {
    R8EValue r = r8e_num_dec(r8e_from_int32(43));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

TEST(dec_underflow) {
    R8EValue r = r8e_num_dec(r8e_from_int32(INT32_MIN));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), (double)INT32_MIN - 1.0, 0.0);
}

/* =========================================================================
 * Bitwise operations
 * ========================================================================= */

TEST(bitand) {
    R8EValue r = r8e_num_bitand(r8e_from_int32(0xFF), r8e_from_int32(0x0F));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0x0F);
}

TEST(bitor) {
    R8EValue r = r8e_num_bitor(r8e_from_int32(0xF0), r8e_from_int32(0x0F));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0xFF);
}

TEST(bitxor) {
    R8EValue r = r8e_num_bitxor(r8e_from_int32(0xFF), r8e_from_int32(0x0F));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0xF0);
}

TEST(bitnot) {
    R8EValue r = r8e_num_bitnot(r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -1);
}

TEST(shl) {
    R8EValue r = r8e_num_shl(r8e_from_int32(1), r8e_from_int32(10));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1024);
}

TEST(shr) {
    R8EValue r = r8e_num_shr(r8e_from_int32(-1), r8e_from_int32(1));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -1);  /* sign-extending */
}

TEST(ushr) {
    R8EValue r = r8e_num_ushr(r8e_from_int32(-1), r8e_from_int32(0));
    /* -1 unsigned = 0xFFFFFFFF = 4294967295, which exceeds INT32_MAX */
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 4294967295.0, 0.0);
}

TEST(shift_mask) {
    /* Shift count is masked to 0-31 bits */
    R8EValue r = r8e_num_shl(r8e_from_int32(1), r8e_from_int32(32));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1);  /* 32 & 0x1F = 0 */
}

/* =========================================================================
 * Number to string
 * ========================================================================= */

TEST(num_to_string_integers) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    char buf[32];

    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_int32(0)), buf), "0");
    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_int32(1)), buf), "1");
    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_int32(-1)), buf), "-1");
    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_int32(42)), buf), "42");
}

TEST(num_to_string_special) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    char buf[32];

    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_double(NAN)), buf), "NaN");
    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_double(INFINITY)), buf), "Infinity");
    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_double(-INFINITY)), buf), "-Infinity");
}

TEST(num_to_string_neg_zero) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    char buf[32];

    /* -0 -> "0" per ES2023 */
    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_double(-0.0)), buf), "0");
}

TEST(num_to_string_decimal) {
    R8EContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    char buf[32];

    ASSERT_EQ_STR(val_to_cstr(r8e_num_to_string(&ctx, r8e_from_double(3.14)), buf), "3.14");
}

/* =========================================================================
 * String to number
 * ========================================================================= */

TEST(string_to_number_int) {
    R8EValue r = r8e_string_to_number("42", 2);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

TEST(string_to_number_decimal) {
    R8EValue r = r8e_string_to_number("3.14", 4);
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 3.14, 1e-15);
}

TEST(string_to_number_hex) {
    R8EValue r = r8e_string_to_number("0xff", 4);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 255);
}

TEST(string_to_number_binary) {
    R8EValue r = r8e_string_to_number("0b1010", 6);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 10);
}

TEST(string_to_number_octal) {
    R8EValue r = r8e_string_to_number("0o77", 4);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 63);
}

TEST(string_to_number_infinity) {
    R8EValue r = r8e_string_to_number("Infinity", 8);
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(isinf(r8e_get_double(r)));
    ASSERT_TRUE(r8e_get_double(r) > 0);
}

TEST(string_to_number_empty) {
    R8EValue r = r8e_string_to_number("", 0);
    ASSERT_NAN(r);
}

TEST(string_to_number_invalid) {
    ASSERT_NAN(r8e_string_to_number("abc", 3));
    ASSERT_NAN(r8e_string_to_number("0xZZ", 4));
}

TEST(string_to_number_negative) {
    R8EValue r = r8e_string_to_number("-42", 3);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -42);
}

/* =========================================================================
 * Math methods
 * ========================================================================= */

TEST(math_abs) {
    R8EValue r = r8e_math_abs(r8e_from_int32(-5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 5);

    r = r8e_math_abs(r8e_from_int32(5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 5);

    r = r8e_math_abs(r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);

    /* abs(INT32_MIN) overflows to double */
    r = r8e_math_abs(r8e_from_int32(INT32_MIN));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 2147483648.0, 0.0);
}

TEST(math_floor) {
    ASSERT_EQ_INT(r8e_get_int32(r8e_math_floor(r8e_from_int32(5))), 5);

    R8EValue r = r8e_math_floor(r8e_from_double(3.7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 3);

    r = r8e_math_floor(r8e_from_double(-3.2));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -4);
}

TEST(math_ceil) {
    R8EValue r = r8e_math_ceil(r8e_from_double(3.2));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 4);

    r = r8e_math_ceil(r8e_from_double(-3.7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -3);
}

TEST(math_round) {
    R8EValue r = r8e_math_round(r8e_from_double(3.5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 4);

    r = r8e_math_round(r8e_from_double(3.4));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 3);
}

TEST(math_sqrt) {
    R8EValue r = r8e_math_sqrt(r8e_from_int32(9));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 3);

    r = r8e_math_sqrt(r8e_from_double(2.0));
    ASSERT_EQ_DBL(r8e_get_double(r), sqrt(2.0), 1e-15);

    /* sqrt(-1) = NaN */
    ASSERT_NAN(r8e_math_sqrt(r8e_from_int32(-1)));
}

TEST(math_pow) {
    R8EValue r = r8e_math_pow(r8e_from_int32(2), r8e_from_int32(10));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1024);

    /* anything ** 0 = 1 */
    r = r8e_math_pow(r8e_from_double(NAN), r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1);
}

TEST(math_min_max) {
    R8EValue r = r8e_math_min(r8e_from_int32(3), r8e_from_int32(7));
    ASSERT_EQ_INT(r8e_get_int32(r), 3);

    r = r8e_math_max(r8e_from_int32(3), r8e_from_int32(7));
    ASSERT_EQ_INT(r8e_get_int32(r), 7);

    /* NaN propagation */
    ASSERT_NAN(r8e_math_min(r8e_from_double(NAN), r8e_from_int32(1)));
    ASSERT_NAN(r8e_math_max(r8e_from_int32(1), r8e_from_double(NAN)));
}

TEST(math_sign) {
    R8EValue r = r8e_math_sign(r8e_from_int32(42));
    ASSERT_EQ_INT(r8e_get_int32(r), 1);

    r = r8e_math_sign(r8e_from_int32(-42));
    ASSERT_EQ_INT(r8e_get_int32(r), -1);

    r = r8e_math_sign(r8e_from_int32(0));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);

    ASSERT_NAN(r8e_math_sign(r8e_from_double(NAN)));
}

TEST(math_trunc) {
    R8EValue r = r8e_math_trunc(r8e_from_double(3.7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 3);

    r = r8e_math_trunc(r8e_from_double(-3.7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -3);
}

TEST(math_clz32) {
    R8EValue r = r8e_math_clz32(r8e_from_int32(0));
    ASSERT_EQ_INT(r8e_get_int32(r), 32);

    r = r8e_math_clz32(r8e_from_int32(1));
    ASSERT_EQ_INT(r8e_get_int32(r), 31);

    r = r8e_math_clz32(r8e_from_int32(0x80000000));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);
}

/* =========================================================================
 * Special value helpers
 * ========================================================================= */

TEST(special_value_checks) {
    ASSERT_TRUE(r8e_is_nan(r8e_from_double(NAN)));
    ASSERT_FALSE(r8e_is_nan(r8e_from_int32(0)));
    ASSERT_FALSE(r8e_is_nan(r8e_from_double(0.0)));

    ASSERT_TRUE(r8e_is_finite(r8e_from_int32(42)));
    ASSERT_TRUE(r8e_is_finite(r8e_from_double(3.14)));
    ASSERT_FALSE(r8e_is_finite(r8e_from_double(INFINITY)));
    ASSERT_FALSE(r8e_is_finite(r8e_from_double(NAN)));

    ASSERT_TRUE(r8e_is_neg_zero(r8e_from_double(-0.0)));
    ASSERT_FALSE(r8e_is_neg_zero(r8e_from_double(0.0)));
    ASSERT_FALSE(r8e_is_neg_zero(r8e_from_int32(0)));
}

/* =========================================================================
 * Extended arithmetic edge cases
 * ========================================================================= */

TEST(add_double_double) {
    R8EValue r = r8e_num_add(r8e_from_double(1.5), r8e_from_double(2.5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 4);
}

TEST(add_int_double) {
    R8EValue r = r8e_num_add(r8e_from_int32(1), r8e_from_double(2.5));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 3.5, 0.0);
}

TEST(add_nan_propagation) {
    R8EValue nan = r8e_from_double(NAN);
    ASSERT_NAN(r8e_num_add(nan, r8e_from_int32(1)));
    ASSERT_NAN(r8e_num_add(r8e_from_int32(1), nan));
}

TEST(add_infinity) {
    R8EValue inf = r8e_from_double(INFINITY);
    R8EValue ninf = r8e_from_double(-INFINITY);

    /* Infinity + 1 = Infinity */
    R8EValue r = r8e_num_add(inf, r8e_from_int32(1));
    ASSERT_TRUE(isinf(r8e_get_double(r)));

    /* Infinity + (-Infinity) = NaN */
    ASSERT_NAN(r8e_num_add(inf, ninf));
}

TEST(sub_to_zero) {
    R8EValue r = r8e_num_sub(r8e_from_int32(5), r8e_from_int32(5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);
}

TEST(mul_by_zero_neg) {
    /* 0 * -1 = -0 */
    R8EValue r = r8e_num_mul(r8e_from_int32(0), r8e_from_int32(-1));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(mul_neg_by_zero) {
    /* -1 * 0 = -0 */
    R8EValue r = r8e_num_mul(r8e_from_int32(-1), r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(div_int32_min_by_neg1) {
    /* INT32_MIN / -1 overflows to double */
    R8EValue r = r8e_num_div(r8e_from_int32(INT32_MIN), r8e_from_int32(-1));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 2147483648.0, 0.0);
}

TEST(div_neg_zero_result) {
    /* -1 / positive produces -0 when result is 0 */
    R8EValue r = r8e_num_div(r8e_from_int32(0), r8e_from_int32(-5));
    /* 0 / -5 = -0 */
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(mod_int32_min_neg1) {
    /* INT32_MIN % -1 = 0 (avoid UB) */
    R8EValue r = r8e_num_mod(r8e_from_int32(INT32_MIN), r8e_from_int32(-1));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 0);
}

TEST(mod_double_operands) {
    R8EValue r = r8e_num_mod(r8e_from_double(5.5), r8e_from_double(2.0));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 1.5, 1e-15);
}

/* =========================================================================
 * Exponentiation
 * ========================================================================= */

extern R8EValue r8e_num_pow(R8EValue a, R8EValue b);

TEST(pow_basic) {
    R8EValue r = r8e_num_pow(r8e_from_int32(2), r8e_from_int32(3));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 8);
}

TEST(pow_nan_exponent) {
    ASSERT_NAN(r8e_num_pow(r8e_from_int32(2), r8e_from_double(NAN)));
}

TEST(pow_zero_exponent) {
    /* anything ** 0 = 1, including NaN */
    R8EValue r = r8e_num_pow(r8e_from_double(NAN), r8e_from_int32(0));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1);
}

TEST(pow_nan_base) {
    ASSERT_NAN(r8e_num_pow(r8e_from_double(NAN), r8e_from_int32(2)));
}

TEST(pow_fractional) {
    R8EValue r = r8e_num_pow(r8e_from_int32(4), r8e_from_double(0.5));
    ASSERT_EQ_DBL(r8e_get_double(r), 2.0, 1e-15);
}

/* =========================================================================
 * Negation edge cases
 * ========================================================================= */

TEST(neg_int32_min) {
    /* -INT32_MIN overflows, goes to double */
    R8EValue r = r8e_num_neg(r8e_from_int32(INT32_MIN));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 2147483648.0, 0.0);
}

TEST(neg_double) {
    R8EValue r = r8e_num_neg(r8e_from_double(3.14));
    ASSERT_EQ_DBL(r8e_get_double(r), -3.14, 1e-15);
}

/* =========================================================================
 * Comparison operations
 * ========================================================================= */

extern R8EValue r8e_num_lt(R8EValue a, R8EValue b);
extern R8EValue r8e_num_le(R8EValue a, R8EValue b);
extern R8EValue r8e_num_gt(R8EValue a, R8EValue b);
extern R8EValue r8e_num_ge(R8EValue a, R8EValue b);

TEST(comparison_int32) {
    ASSERT_VALUE_EQ(r8e_num_lt(r8e_from_int32(3), r8e_from_int32(5)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_num_lt(r8e_from_int32(5), r8e_from_int32(3)), R8E_FALSE);
    ASSERT_VALUE_EQ(r8e_num_lt(r8e_from_int32(3), r8e_from_int32(3)), R8E_FALSE);

    ASSERT_VALUE_EQ(r8e_num_le(r8e_from_int32(3), r8e_from_int32(3)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_num_le(r8e_from_int32(4), r8e_from_int32(3)), R8E_FALSE);

    ASSERT_VALUE_EQ(r8e_num_gt(r8e_from_int32(5), r8e_from_int32(3)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_num_gt(r8e_from_int32(3), r8e_from_int32(5)), R8E_FALSE);

    ASSERT_VALUE_EQ(r8e_num_ge(r8e_from_int32(3), r8e_from_int32(3)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_num_ge(r8e_from_int32(2), r8e_from_int32(3)), R8E_FALSE);
}

TEST(comparison_nan_undefined) {
    /* NaN comparisons return undefined */
    R8EValue nan = r8e_from_double(NAN);
    ASSERT_VALUE_EQ(r8e_num_lt(nan, r8e_from_int32(1)), R8E_UNDEFINED);
    ASSERT_VALUE_EQ(r8e_num_gt(r8e_from_int32(1), nan), R8E_UNDEFINED);
    ASSERT_VALUE_EQ(r8e_num_le(nan, nan), R8E_UNDEFINED);
    ASSERT_VALUE_EQ(r8e_num_ge(nan, nan), R8E_UNDEFINED);
}

TEST(comparison_double) {
    ASSERT_VALUE_EQ(r8e_num_lt(r8e_from_double(1.5), r8e_from_double(2.5)), R8E_TRUE);
    ASSERT_VALUE_EQ(r8e_num_gt(r8e_from_double(2.5), r8e_from_double(1.5)), R8E_TRUE);
}

/* =========================================================================
 * Bitwise with double operands
 * ========================================================================= */

TEST(bitand_double_operands) {
    /* Doubles are ToInt32'd before bitwise ops */
    R8EValue r = r8e_num_bitand(r8e_from_double(5.9), r8e_from_double(3.1));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 5 & 3);
}

TEST(bitor_double_operands) {
    R8EValue r = r8e_num_bitor(r8e_from_double(5.9), r8e_from_double(3.1));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 5 | 3);
}

TEST(bitnot_double) {
    R8EValue r = r8e_num_bitnot(r8e_from_double(5.7));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), ~5);
}

TEST(shl_double_operands) {
    R8EValue r = r8e_num_shl(r8e_from_double(1.0), r8e_from_double(4.0));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 16);
}

TEST(ushr_small_result) {
    /* Unsigned right shift with result fitting in int32 */
    R8EValue r = r8e_num_ushr(r8e_from_int32(16), r8e_from_int32(2));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 4);
}

/* =========================================================================
 * Extended Math methods
 * ========================================================================= */

extern R8EValue r8e_math_log(R8EValue v);
extern R8EValue r8e_math_log2(R8EValue v);
extern R8EValue r8e_math_log10(R8EValue v);
extern R8EValue r8e_math_sin(R8EValue v);
extern R8EValue r8e_math_cos(R8EValue v);
extern R8EValue r8e_math_tan(R8EValue v);
extern R8EValue r8e_math_atan(R8EValue v);
extern R8EValue r8e_math_atan2(R8EValue y, R8EValue x);
extern R8EValue r8e_math_asin(R8EValue v);
extern R8EValue r8e_math_acos(R8EValue v);
extern R8EValue r8e_math_exp(R8EValue v);
extern R8EValue r8e_math_cbrt(R8EValue v);
extern R8EValue r8e_math_hypot(R8EValue a, R8EValue b);
extern R8EValue r8e_math_fround(R8EValue v);
extern R8EValue r8e_math_imul(R8EValue a, R8EValue b);
extern R8EValue r8e_math_log1p(R8EValue v);
extern R8EValue r8e_math_expm1(R8EValue v);
extern R8EValue r8e_math_sinh(R8EValue v);
extern R8EValue r8e_math_cosh(R8EValue v);
extern R8EValue r8e_math_tanh(R8EValue v);
extern R8EValue r8e_math_asinh(R8EValue v);
extern R8EValue r8e_math_acosh(R8EValue v);
extern R8EValue r8e_math_atanh(R8EValue v);

TEST(math_log) {
    R8EValue r = r8e_math_log(r8e_from_double(1.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    r = r8e_math_log(r8e_from_double(M_E));
    ASSERT_EQ_DBL(r8e_get_double(r), 1.0, 1e-15);

    /* log(0) = -Infinity */
    r = r8e_math_log(r8e_from_int32(0));
    ASSERT_TRUE(isinf(r8e_get_double(r)));
    ASSERT_TRUE(r8e_get_double(r) < 0);

    /* log(-1) = NaN */
    ASSERT_NAN(r8e_math_log(r8e_from_int32(-1)));
}

TEST(math_log2) {
    R8EValue r = r8e_math_log2(r8e_from_int32(8));
    ASSERT_EQ_DBL(r8e_get_double(r), 3.0, 1e-15);

    ASSERT_NAN(r8e_math_log2(r8e_from_int32(-1)));
}

TEST(math_log10) {
    R8EValue r = r8e_math_log10(r8e_from_int32(100));
    ASSERT_EQ_DBL(r8e_get_double(r), 2.0, 1e-15);

    ASSERT_NAN(r8e_math_log10(r8e_from_int32(-1)));
}

TEST(math_sin_cos_tan) {
    R8EValue r = r8e_math_sin(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    r = r8e_math_cos(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 1.0, 1e-15);

    r = r8e_math_tan(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);
}

TEST(math_asin_acos) {
    R8EValue r = r8e_math_asin(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    r = r8e_math_acos(r8e_from_double(1.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    /* Out of range */
    ASSERT_NAN(r8e_math_asin(r8e_from_double(2.0)));
    ASSERT_NAN(r8e_math_acos(r8e_from_double(2.0)));
}

TEST(math_atan_atan2) {
    R8EValue r = r8e_math_atan(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    r = r8e_math_atan2(r8e_from_double(1.0), r8e_from_double(1.0));
    ASSERT_EQ_DBL(r8e_get_double(r), M_PI / 4.0, 1e-15);
}

TEST(math_exp) {
    R8EValue r = r8e_math_exp(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 1.0, 1e-15);

    r = r8e_math_exp(r8e_from_double(1.0));
    ASSERT_EQ_DBL(r8e_get_double(r), M_E, 1e-10);
}

TEST(math_cbrt) {
    R8EValue r = r8e_math_cbrt(r8e_from_int32(27));
    ASSERT_EQ_DBL(r8e_get_double(r), 3.0, 1e-15);

    r = r8e_math_cbrt(r8e_from_int32(-8));
    ASSERT_EQ_DBL(r8e_get_double(r), -2.0, 1e-15);
}

TEST(math_hypot) {
    R8EValue r = r8e_math_hypot(r8e_from_int32(3), r8e_from_int32(4));
    ASSERT_EQ_DBL(r8e_get_double(r), 5.0, 1e-15);
}

TEST(math_fround) {
    R8EValue r = r8e_math_fround(r8e_from_double(1.337));
    float f = 1.337f;
    ASSERT_EQ_DBL(r8e_get_double(r), (double)f, 1e-15);
}

TEST(math_imul) {
    R8EValue r = r8e_math_imul(r8e_from_int32(3), r8e_from_int32(4));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 12);

    /* Overflow wraps to 32-bit */
    r = r8e_math_imul(r8e_from_int32(0xFFFFFFFF), r8e_from_int32(5));
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -5);
}

TEST(math_log1p) {
    R8EValue r = r8e_math_log1p(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    /* log1p(-1) = -Infinity */
    r = r8e_math_log1p(r8e_from_int32(-1));
    ASSERT_TRUE(isinf(r8e_get_double(r)));

    ASSERT_NAN(r8e_math_log1p(r8e_from_int32(-2)));
}

TEST(math_expm1) {
    R8EValue r = r8e_math_expm1(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);
}

TEST(math_sinh_cosh_tanh) {
    R8EValue r = r8e_math_sinh(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    r = r8e_math_cosh(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 1.0, 1e-15);

    r = r8e_math_tanh(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);
}

TEST(math_asinh_acosh_atanh) {
    R8EValue r = r8e_math_asinh(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    r = r8e_math_acosh(r8e_from_double(1.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    /* acosh(x < 1) = NaN */
    ASSERT_NAN(r8e_math_acosh(r8e_from_double(0.5)));

    r = r8e_math_atanh(r8e_from_double(0.0));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.0, 1e-15);

    /* atanh(1) = Infinity */
    r = r8e_math_atanh(r8e_from_double(1.0));
    ASSERT_TRUE(isinf(r8e_get_double(r)));

    ASSERT_NAN(r8e_math_atanh(r8e_from_double(2.0)));
}

TEST(math_round_neg_half) {
    /* Math.round(-0.5) = -0 per ES2023 */
    R8EValue r = r8e_math_round(r8e_from_double(-0.5));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(math_trunc_nan) {
    ASSERT_NAN(r8e_math_trunc(r8e_from_double(NAN)));
}

TEST(math_floor_infinity) {
    R8EValue r = r8e_math_floor(r8e_from_double(INFINITY));
    ASSERT_TRUE(isinf(r8e_get_double(r)));
}

TEST(math_ceil_neg_zero) {
    R8EValue r = r8e_math_ceil(r8e_from_double(-0.5));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    /* ceil(-0.5) = -0 per IEEE */
    double d = r8e_get_double(r);
    ASSERT_EQ_DBL(d, 0.0, 0.0);
}

TEST(math_min_max_zero) {
    /* Math.min(+0, -0) = -0 */
    R8EValue r = r8e_math_min(r8e_from_double(0.0), r8e_from_double(-0.0));
    ASSERT_TRUE(r8e_is_neg_zero(r));

    /* Math.max(+0, -0) = +0 */
    r = r8e_math_max(r8e_from_double(-0.0), r8e_from_double(0.0));
    ASSERT_FALSE(r8e_is_neg_zero(r));
}

/* =========================================================================
 * Extended special value helpers
 * ========================================================================= */

extern bool r8e_is_integer(R8EValue v);
extern bool r8e_is_safe_integer(R8EValue v);

TEST(is_integer) {
    ASSERT_TRUE(r8e_is_integer(r8e_from_int32(0)));
    ASSERT_TRUE(r8e_is_integer(r8e_from_int32(42)));
    ASSERT_TRUE(r8e_is_integer(r8e_from_double(5.0)));
    ASSERT_FALSE(r8e_is_integer(r8e_from_double(5.5)));
    ASSERT_FALSE(r8e_is_integer(r8e_from_double(NAN)));
    ASSERT_FALSE(r8e_is_integer(r8e_from_double(INFINITY)));
}

TEST(is_safe_integer) {
    ASSERT_TRUE(r8e_is_safe_integer(r8e_from_int32(0)));
    ASSERT_TRUE(r8e_is_safe_integer(r8e_from_int32(INT32_MAX)));
    ASSERT_TRUE(r8e_is_safe_integer(r8e_from_double(9007199254740991.0)));
    ASSERT_FALSE(r8e_is_safe_integer(r8e_from_double(9007199254740992.0)));
    ASSERT_FALSE(r8e_is_safe_integer(r8e_from_double(5.5)));
    ASSERT_FALSE(r8e_is_safe_integer(r8e_from_double(INFINITY)));
}

TEST(is_nan_non_number) {
    /* Non-number values should return false for is_nan */
    ASSERT_FALSE(r8e_is_nan(R8E_UNDEFINED));
    ASSERT_FALSE(r8e_is_nan(R8E_NULL));
    ASSERT_FALSE(r8e_is_nan(R8E_TRUE));
}

TEST(is_finite_non_number) {
    /* Non-number values should return false for is_finite */
    ASSERT_FALSE(r8e_is_finite(R8E_UNDEFINED));
    ASSERT_FALSE(r8e_is_finite(R8E_NULL));
}

/* =========================================================================
 * Extended string to number
 * ========================================================================= */

TEST(string_to_number_whitespace) {
    /* Leading/trailing whitespace should be stripped */
    R8EValue r = r8e_string_to_number("  42  ", 6);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);

    /* Only whitespace -> NaN (string_to_number is like parseFloat, not ToNumber) */
    r = r8e_string_to_number("   ", 3);
    ASSERT_NAN(r);
}

TEST(string_to_number_scientific) {
    R8EValue r = r8e_string_to_number("1e3", 3);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 1000);
}

TEST(string_to_number_neg_infinity) {
    R8EValue r = r8e_string_to_number("-Infinity", 9);
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(isinf(r8e_get_double(r)));
    ASSERT_TRUE(r8e_get_double(r) < 0);
}

TEST(string_to_number_null_input) {
    ASSERT_NAN(r8e_string_to_number(NULL, 0));
}

TEST(string_to_number_leading_dot) {
    R8EValue r = r8e_string_to_number(".5", 2);
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_EQ_DBL(r8e_get_double(r), 0.5, 1e-15);
}

/* =========================================================================
 * parseInt
 * ========================================================================= */

extern R8EValue r8e_parse_int(const char *str, uint32_t len, int radix);

TEST(parse_int_decimal) {
    R8EValue r = r8e_parse_int("42", 2, 10);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 42);
}

TEST(parse_int_hex_auto) {
    R8EValue r = r8e_parse_int("0xff", 4, 0);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 255);
}

TEST(parse_int_binary_auto) {
    R8EValue r = r8e_parse_int("0b1010", 6, 0);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 10);
}

TEST(parse_int_radix_16) {
    R8EValue r = r8e_parse_int("ff", 2, 16);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 255);
}

TEST(parse_int_invalid_radix) {
    ASSERT_NAN(r8e_parse_int("42", 2, 1));
    ASSERT_NAN(r8e_parse_int("42", 2, 37));
}

TEST(parse_int_negative) {
    R8EValue r = r8e_parse_int("-10", 3, 10);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), -10);
}

TEST(parse_int_stops_at_invalid) {
    /* parseInt("123abc", 10) -> 123 */
    R8EValue r = r8e_parse_int("123abc", 6, 10);
    ASSERT_TRUE(R8E_IS_INT32(r));
    ASSERT_EQ_INT(r8e_get_int32(r), 123);
}

TEST(parse_int_null_input) {
    ASSERT_NAN(r8e_parse_int(NULL, 0, 10));
}

/* =========================================================================
 * Math constants
 * ========================================================================= */

extern R8EValue r8e_math_e(void);
extern R8EValue r8e_math_pi(void);
extern R8EValue r8e_math_ln2(void);
extern R8EValue r8e_math_sqrt2(void);

TEST(math_constants) {
    ASSERT_EQ_DBL(r8e_get_double(r8e_math_e()), 2.718281828459045, 1e-15);
    ASSERT_EQ_DBL(r8e_get_double(r8e_math_pi()), 3.141592653589793, 1e-15);
    ASSERT_EQ_DBL(r8e_get_double(r8e_math_ln2()), 0.6931471805599453, 1e-15);
    ASSERT_EQ_DBL(r8e_get_double(r8e_math_sqrt2()), 1.4142135623730951, 1e-15);
}

/* =========================================================================
 * Number constants
 * ========================================================================= */

extern R8EValue r8e_number_max_safe_integer(void);
extern R8EValue r8e_number_min_safe_integer(void);
extern R8EValue r8e_number_epsilon(void);

TEST(number_constants) {
    ASSERT_EQ_DBL(r8e_get_double(r8e_number_max_safe_integer()), 9007199254740991.0, 0.0);
    ASSERT_EQ_DBL(r8e_get_double(r8e_number_min_safe_integer()), -9007199254740991.0, 0.0);
    ASSERT_EQ_DBL(r8e_get_double(r8e_number_epsilon()), DBL_EPSILON, 0.0);
}

/* =========================================================================
 * sign of -0 in operations
 * ========================================================================= */

TEST(sign_neg_zero) {
    /* sign(-0) should return -0 */
    R8EValue r = r8e_math_sign(r8e_from_double(-0.0));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_TRUE(r8e_is_neg_zero(r));
}

TEST(sign_pos_zero) {
    /* sign(+0) should return +0 */
    R8EValue r = r8e_math_sign(r8e_from_double(0.0));
    ASSERT_TRUE(R8E_IS_DOUBLE(r));
    ASSERT_FALSE(r8e_is_neg_zero(r));
}

/* =========================================================================
 * Test suite entry point
 * ========================================================================= */

void test_suite_number(void) {
    /* Int32 fast path */
    RUN_TEST(add_int32);
    RUN_TEST(add_int32_negative);
    RUN_TEST(sub_int32);
    RUN_TEST(mul_int32);

    /* Overflow */
    RUN_TEST(add_overflow);
    RUN_TEST(sub_overflow);
    RUN_TEST(mul_overflow);

    /* Division */
    RUN_TEST(div_exact);
    RUN_TEST(div_non_exact);
    RUN_TEST(div_by_zero);

    /* Modulo */
    RUN_TEST(mod_positive);
    RUN_TEST(mod_negative);
    RUN_TEST(mod_neg_zero);
    RUN_TEST(mod_by_zero);

    /* Unary */
    RUN_TEST(neg_int32);
    RUN_TEST(neg_zero_produces_neg_zero);
    RUN_TEST(inc_int32);
    RUN_TEST(inc_overflow);
    RUN_TEST(dec_int32);
    RUN_TEST(dec_underflow);

    /* Bitwise */
    RUN_TEST(bitand);
    RUN_TEST(bitor);
    RUN_TEST(bitxor);
    RUN_TEST(bitnot);
    RUN_TEST(shl);
    RUN_TEST(shr);
    RUN_TEST(ushr);
    RUN_TEST(shift_mask);

    /* Number to string */
    RUN_TEST(num_to_string_integers);
    RUN_TEST(num_to_string_special);
    RUN_TEST(num_to_string_neg_zero);
    RUN_TEST(num_to_string_decimal);

    /* String to number */
    RUN_TEST(string_to_number_int);
    RUN_TEST(string_to_number_decimal);
    RUN_TEST(string_to_number_hex);
    RUN_TEST(string_to_number_binary);
    RUN_TEST(string_to_number_octal);
    RUN_TEST(string_to_number_infinity);
    RUN_TEST(string_to_number_empty);
    RUN_TEST(string_to_number_invalid);
    RUN_TEST(string_to_number_negative);

    /* Math methods */
    RUN_TEST(math_abs);
    RUN_TEST(math_floor);
    RUN_TEST(math_ceil);
    RUN_TEST(math_round);
    RUN_TEST(math_sqrt);
    RUN_TEST(math_pow);
    RUN_TEST(math_min_max);
    RUN_TEST(math_sign);
    RUN_TEST(math_trunc);
    RUN_TEST(math_clz32);

    /* Special value checks */
    RUN_TEST(special_value_checks);

    /* Extended arithmetic edge cases */
    RUN_TEST(add_double_double);
    RUN_TEST(add_int_double);
    RUN_TEST(add_nan_propagation);
    RUN_TEST(add_infinity);
    RUN_TEST(sub_to_zero);
    RUN_TEST(mul_by_zero_neg);
    RUN_TEST(mul_neg_by_zero);
    RUN_TEST(div_int32_min_by_neg1);
    RUN_TEST(div_neg_zero_result);
    RUN_TEST(mod_int32_min_neg1);
    RUN_TEST(mod_double_operands);

    /* Exponentiation */
    RUN_TEST(pow_basic);
    RUN_TEST(pow_nan_exponent);
    RUN_TEST(pow_zero_exponent);
    RUN_TEST(pow_nan_base);
    RUN_TEST(pow_fractional);

    /* Negation edge cases */
    RUN_TEST(neg_int32_min);
    RUN_TEST(neg_double);

    /* Comparison operations */
    RUN_TEST(comparison_int32);
    RUN_TEST(comparison_nan_undefined);
    RUN_TEST(comparison_double);

    /* Bitwise with doubles */
    RUN_TEST(bitand_double_operands);
    RUN_TEST(bitor_double_operands);
    RUN_TEST(bitnot_double);
    RUN_TEST(shl_double_operands);
    RUN_TEST(ushr_small_result);

    /* Extended Math methods */
    RUN_TEST(math_log);
    RUN_TEST(math_log2);
    RUN_TEST(math_log10);
    RUN_TEST(math_sin_cos_tan);
    RUN_TEST(math_asin_acos);
    RUN_TEST(math_atan_atan2);
    RUN_TEST(math_exp);
    RUN_TEST(math_cbrt);
    RUN_TEST(math_hypot);
    RUN_TEST(math_fround);
    RUN_TEST(math_imul);
    RUN_TEST(math_log1p);
    RUN_TEST(math_expm1);
    RUN_TEST(math_sinh_cosh_tanh);
    RUN_TEST(math_asinh_acosh_atanh);
    RUN_TEST(math_round_neg_half);
    RUN_TEST(math_trunc_nan);
    RUN_TEST(math_floor_infinity);
    RUN_TEST(math_ceil_neg_zero);
    RUN_TEST(math_min_max_zero);

    /* Extended special values */
    RUN_TEST(is_integer);
    RUN_TEST(is_safe_integer);
    RUN_TEST(is_nan_non_number);
    RUN_TEST(is_finite_non_number);

    /* Extended string to number */
    RUN_TEST(string_to_number_whitespace);
    RUN_TEST(string_to_number_scientific);
    RUN_TEST(string_to_number_neg_infinity);
    RUN_TEST(string_to_number_null_input);
    RUN_TEST(string_to_number_leading_dot);

    /* parseInt */
    RUN_TEST(parse_int_decimal);
    RUN_TEST(parse_int_hex_auto);
    RUN_TEST(parse_int_binary_auto);
    RUN_TEST(parse_int_radix_16);
    RUN_TEST(parse_int_invalid_radix);
    RUN_TEST(parse_int_negative);
    RUN_TEST(parse_int_stops_at_invalid);
    RUN_TEST(parse_int_null_input);

    /* Math constants */
    RUN_TEST(math_constants);

    /* Number constants */
    RUN_TEST(number_constants);

    /* Sign of zero */
    RUN_TEST(sign_neg_zero);
    RUN_TEST(sign_pos_zero);
}
