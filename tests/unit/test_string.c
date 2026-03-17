/*
 * test_string.c - Unit tests for r8e_string.c
 *
 * Tests string creation, inline strings, hash, character access,
 * searching, slicing, case conversion, trimming, splitting,
 * repeating, concatenation, and UTF-8 handling.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * NaN-boxing definitions
 * ------------------------------------------------------------------------- */
typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)     (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == 0xFFFDU)

static inline double r8e_get_double(R8EValue v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline R8EValue r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* -------------------------------------------------------------------------
 * Forward declarations for r8e_string.c types and functions.
 * We declare the struct locally to match the source file layout.
 * ------------------------------------------------------------------------- */

typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    char     data[];
} R8EString;

#define R8E_STR_IS_ASCII    0x01

typedef struct R8EContext R8EContext;
struct R8EContext {
    void *arena;
};

/* String creation / destruction */
extern R8EString *r8e_string_new(R8EContext *ctx, const char *data, uint32_t len);
extern R8EString *r8e_string_new_ascii(R8EContext *ctx, const char *data, uint32_t len);
extern R8EString *r8e_string_from_cstr(R8EContext *ctx, const char *cstr);
extern void       r8e_string_free(R8EContext *ctx, R8EString *s);

/* Inline strings */
extern R8EValue   r8e_inline_str_new(const char *s, uint32_t len);
extern uint32_t   r8e_inline_str_extract(R8EValue v, char *buf);
extern uint32_t   r8e_inline_str_hash(R8EValue v);

/* Value-level string creation */
extern R8EValue   r8e_value_from_string(R8EContext *ctx, const char *data, uint32_t len);

/* Local FNV-1a implementation for hash verification.
 * r8e_fnv1a is static in r8e_string.c, so we replicate it here. */
static uint32_t test_fnv1a(const char *data, uint32_t len) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Equality */
extern bool       r8e_string_equal(const R8EString *a, const R8EString *b);
extern bool       r8e_string_equal_raw(const R8EString *s, const char *data, uint32_t len);

/* Character access */
extern R8EString *r8e_string_char_at(R8EContext *ctx, R8EString *s, int32_t index);
extern int32_t    r8e_string_char_code_at(R8EContext *ctx, R8EString *s, int32_t index);

/* Searching */
extern int32_t    r8e_string_index_of(R8EContext *ctx, R8EString *s,
                                       R8EString *search, int32_t from);
extern int32_t    r8e_string_last_index_of(R8EContext *ctx, R8EString *s,
                                            R8EString *search, int32_t from);
extern bool       r8e_string_includes(R8EContext *ctx, R8EString *s,
                                       R8EString *search, int32_t from);
extern bool       r8e_string_starts_with(R8EContext *ctx, R8EString *s,
                                          R8EString *prefix, int32_t pos);
extern bool       r8e_string_ends_with(R8EContext *ctx, R8EString *s,
                                        R8EString *suffix, int32_t end_pos);

/* Transformation */
extern R8EString *r8e_string_slice(R8EContext *ctx, R8EString *s,
                                    int32_t start, int32_t end);
extern R8EString *r8e_string_to_upper(R8EContext *ctx, R8EString *s);
extern R8EString *r8e_string_to_lower(R8EContext *ctx, R8EString *s);
extern R8EString *r8e_string_trim(R8EContext *ctx, R8EString *s);
extern R8EString *r8e_string_trim_start(R8EContext *ctx, R8EString *s);
extern R8EString *r8e_string_trim_end(R8EContext *ctx, R8EString *s);

/* Building */
extern R8EString *r8e_string_concat(R8EContext *ctx, R8EString *a, R8EString *b);
extern R8EString *r8e_string_repeat(R8EContext *ctx, R8EString *s, int32_t count);

/* Split */
extern R8EString **r8e_string_split(R8EContext *ctx, R8EString *s,
                                      R8EString *separator, int32_t limit,
                                      uint32_t *out_count);

/* UTF-8 helpers */
extern int        r8e_utf8_decode(const uint8_t *bytes, uint32_t len, uint32_t *cp_out);
extern int        r8e_utf8_encode(uint32_t codepoint, uint8_t *buf);
extern uint32_t   r8e_utf8_strlen(const char *data, uint32_t byte_len);

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
#define ASSERT_EQ_STR(a, b) do { if (strcmp((a),(b)) != 0) { fprintf(stderr, "    ASSERT_EQ_STR(%s, %s): \"%s\" vs \"%s\" at %s:%d\n", #a, #b, (a), (b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_NE(a, b) do { if ((uint64_t)(a) == (uint64_t)(b)) { fprintf(stderr, "    ASSERT_NE(%s, %s) failed at %s:%d\n", #a, #b, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ(a, b) do { if ((uint64_t)(a) != (uint64_t)(b)) { fprintf(stderr, "    ASSERT_EQ(%s, %s) failed at %s:%d\n", #a, #b, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)

/* =========================================================================
 * String creation
 * ========================================================================= */

TEST(string_new_ascii) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_new(&ctx, "hello", 5);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->byte_length, 5);
    ASSERT_EQ_INT(s->char_length, 5);
    ASSERT_TRUE(s->flags & R8E_STR_IS_ASCII);
    ASSERT_EQ_STR(s->data, "hello");
    r8e_string_free(&ctx, s);
}

TEST(string_new_empty) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_new(&ctx, "", 0);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->byte_length, 0);
    ASSERT_EQ_INT(s->char_length, 0);
    ASSERT_EQ_STR(s->data, "");
    r8e_string_free(&ctx, s);
}

TEST(string_new_unicode) {
    R8EContext ctx = {0};
    /* UTF-8 for "cafe\xCC\x81" (cafe + combining accent) = 6 bytes, 5 chars */
    const char *utf8 = "caf\xC3\xA9";  /* "caf" + U+00E9 (e-acute) */
    R8EString *s = r8e_string_new(&ctx, utf8, 5);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->byte_length, 5);
    /* char_length should be 4 (caf + 1 unicode char) */
    ASSERT_EQ_INT(s->char_length, 4);
    ASSERT_FALSE(s->flags & R8E_STR_IS_ASCII);
    r8e_string_free(&ctx, s);
}

TEST(string_from_cstr) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "test");
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->byte_length, 4);
    ASSERT_EQ_STR(s->data, "test");
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Inline strings
 * ========================================================================= */

TEST(inline_str_creation) {
    R8EValue v = r8e_inline_str_new("abc", 3);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    char buf[8];
    uint32_t len = r8e_inline_str_extract(v, buf);
    ASSERT_EQ_INT(len, 3);
    ASSERT_EQ_STR(buf, "abc");
}

TEST(inline_str_empty_creation) {
    R8EValue v = r8e_inline_str_new("", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    char buf[8];
    uint32_t len = r8e_inline_str_extract(v, buf);
    ASSERT_EQ_INT(len, 0);
    ASSERT_EQ_STR(buf, "");
}

TEST(inline_str_max_length) {
    /* Maximum inline string is 6 chars per r8e_string.c can_inline check */
    R8EValue v = r8e_inline_str_new("abcdef", 6);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    char buf[8];
    uint32_t len = r8e_inline_str_extract(v, buf);
    ASSERT_EQ_INT(len, 6);
    ASSERT_EQ_STR(buf, "abcdef");
}

TEST(value_from_string_inline) {
    R8EContext ctx = {0};
    /* Short ASCII: should be inline */
    R8EValue v = r8e_value_from_string(&ctx, "hi", 2);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
}

TEST(value_from_string_heap) {
    R8EContext ctx = {0};
    /* Longer string: should be heap allocated */
    R8EValue v = r8e_value_from_string(&ctx, "hello world!", 12);
    ASSERT_TRUE(R8E_IS_POINTER(v));
    R8EString *s = (R8EString *)r8e_get_pointer(v);
    ASSERT_EQ_INT(s->byte_length, 12);
    ASSERT_EQ_STR(s->data, "hello world!");
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * FNV-1a hash consistency
 * ========================================================================= */

TEST(fnv1a_hash_consistency) {
    /* Same input should always produce same hash */
    uint32_t h1 = test_fnv1a("hello", 5);
    uint32_t h2 = test_fnv1a("hello", 5);
    ASSERT_EQ(h1, h2);

    /* Different input should (very likely) produce different hash */
    uint32_t h3 = test_fnv1a("world", 5);
    ASSERT_NE(h1, h3);

    /* Empty string has a specific hash */
    uint32_t h4 = test_fnv1a("", 0);
    ASSERT_EQ(h4, 0x811C9DC5u);  /* FNV offset basis */
}

TEST(fnv1a_hash_stored_in_string) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_new(&ctx, "test", 4);
    ASSERT_TRUE(s != NULL);
    uint32_t expected = test_fnv1a("test", 4);
    ASSERT_EQ(s->hash, expected);
    r8e_string_free(&ctx, s);
}

TEST(inline_str_hash) {
    R8EValue v = r8e_inline_str_new("abc", 3);
    uint32_t h = r8e_inline_str_hash(v);
    uint32_t expected = test_fnv1a("abc", 3);
    ASSERT_EQ(h, expected);
}

/* =========================================================================
 * charAt and charCodeAt
 * ========================================================================= */

TEST(char_at_ascii) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *ch = r8e_string_char_at(&ctx, s, 0);
    ASSERT_TRUE(ch != NULL);
    ASSERT_EQ_INT(ch->byte_length, 1);
    ASSERT_EQ_STR(ch->data, "h");
    r8e_string_free(&ctx, ch);

    ch = r8e_string_char_at(&ctx, s, 4);
    ASSERT_TRUE(ch != NULL);
    ASSERT_EQ_STR(ch->data, "o");
    r8e_string_free(&ctx, ch);

    /* Out of bounds */
    ch = r8e_string_char_at(&ctx, s, 5);
    ASSERT_TRUE(ch == NULL || ch->byte_length == 0);
    if (ch) r8e_string_free(&ctx, ch);

    r8e_string_free(&ctx, s);
}

TEST(char_code_at_ascii) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "ABC");
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, 0), 65);  /* 'A' */
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, 1), 66);  /* 'B' */
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, 2), 67);  /* 'C' */
    /* Out of bounds -> -1 */
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, 3), -1);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * indexOf and lastIndexOf
 * ========================================================================= */

TEST(index_of_basic) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *needle = r8e_string_from_cstr(&ctx, "world");
    ASSERT_EQ_INT(r8e_string_index_of(&ctx, s, needle, 0), 6);
    r8e_string_free(&ctx, needle);

    needle = r8e_string_from_cstr(&ctx, "xyz");
    ASSERT_EQ_INT(r8e_string_index_of(&ctx, s, needle, 0), -1);
    r8e_string_free(&ctx, needle);

    r8e_string_free(&ctx, s);
}

TEST(last_index_of_basic) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abcabc");
    R8EString *needle = r8e_string_from_cstr(&ctx, "abc");
    ASSERT_EQ_INT(r8e_string_last_index_of(&ctx, s, needle, (int32_t)s->char_length), 3);
    r8e_string_free(&ctx, needle);
    r8e_string_free(&ctx, s);
}

TEST(includes) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *yes = r8e_string_from_cstr(&ctx, "world");
    R8EString *no = r8e_string_from_cstr(&ctx, "xyz");
    ASSERT_TRUE(r8e_string_includes(&ctx, s, yes, 0));
    ASSERT_FALSE(r8e_string_includes(&ctx, s, no, 0));
    r8e_string_free(&ctx, yes);
    r8e_string_free(&ctx, no);
    r8e_string_free(&ctx, s);
}

TEST(starts_with) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *prefix = r8e_string_from_cstr(&ctx, "hello");
    R8EString *bad = r8e_string_from_cstr(&ctx, "world");
    ASSERT_TRUE(r8e_string_starts_with(&ctx, s, prefix, 0));
    ASSERT_FALSE(r8e_string_starts_with(&ctx, s, bad, 0));
    r8e_string_free(&ctx, prefix);
    r8e_string_free(&ctx, bad);
    r8e_string_free(&ctx, s);
}

TEST(ends_with) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *suffix = r8e_string_from_cstr(&ctx, "world");
    R8EString *bad = r8e_string_from_cstr(&ctx, "hello");
    ASSERT_TRUE(r8e_string_ends_with(&ctx, s, suffix, (int32_t)s->char_length));
    ASSERT_FALSE(r8e_string_ends_with(&ctx, s, bad, (int32_t)s->char_length));
    r8e_string_free(&ctx, suffix);
    r8e_string_free(&ctx, bad);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Slice
 * ========================================================================= */

TEST(slice_positive) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *r = r8e_string_slice(&ctx, s, 0, 5);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(slice_negative) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *r = r8e_string_slice(&ctx, s, -5, (int32_t)s->char_length);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "world");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(slice_empty) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *r = r8e_string_slice(&ctx, s, 3, 3);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Case conversion
 * ========================================================================= */

TEST(to_upper_case) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *r = r8e_string_to_upper(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "HELLO");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(to_lower_case) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "HELLO");
    R8EString *r = r8e_string_to_lower(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(to_upper_mixed) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "Hello World");
    R8EString *r = r8e_string_to_upper(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "HELLO WORLD");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Trim
 * ========================================================================= */

TEST(trim) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "  hello  ");
    R8EString *r = r8e_string_trim(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(trim_start) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "  hello  ");
    R8EString *r = r8e_string_trim_start(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello  ");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(trim_end) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "  hello  ");
    R8EString *r = r8e_string_trim_end(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "  hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(trim_no_whitespace) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *r = r8e_string_trim(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Split
 * ========================================================================= */

TEST(split_simple) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "a,b,c");
    R8EString *sep = r8e_string_from_cstr(&ctx, ",");
    uint32_t count;
    R8EString **parts = r8e_string_split(&ctx, s, sep, -1, &count);
    ASSERT_TRUE(parts != NULL);
    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_STR(parts[0]->data, "a");
    ASSERT_EQ_STR(parts[1]->data, "b");
    ASSERT_EQ_STR(parts[2]->data, "c");
    for (uint32_t i = 0; i < count; i++) r8e_string_free(&ctx, parts[i]);
    free(parts);
    r8e_string_free(&ctx, sep);
    r8e_string_free(&ctx, s);
}

TEST(split_no_separator_match) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *sep = r8e_string_from_cstr(&ctx, ",");
    uint32_t count;
    R8EString **parts = r8e_string_split(&ctx, s, sep, -1, &count);
    ASSERT_TRUE(parts != NULL);
    ASSERT_EQ_INT(count, 1);
    ASSERT_EQ_STR(parts[0]->data, "hello");
    r8e_string_free(&ctx, parts[0]);
    free(parts);
    r8e_string_free(&ctx, sep);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Repeat
 * ========================================================================= */

TEST(repeat_zero) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abc");
    R8EString *r = r8e_string_repeat(&ctx, s, 0);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(repeat_one) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abc");
    R8EString *r = r8e_string_repeat(&ctx, s, 1);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "abc");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(repeat_three) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "ab");
    R8EString *r = r8e_string_repeat(&ctx, s, 3);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "ababab");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Concat
 * ========================================================================= */

TEST(concat_basic) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "hello");
    R8EString *b = r8e_string_from_cstr(&ctx, " world");
    R8EString *r = r8e_string_concat(&ctx, a, b);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello world");
    ASSERT_EQ_INT(r->byte_length, 11);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
}

TEST(concat_with_empty) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "hello");
    R8EString *b = r8e_string_from_cstr(&ctx, "");
    R8EString *r = r8e_string_concat(&ctx, a, b);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
}

/* =========================================================================
 * String equality
 * ========================================================================= */

TEST(string_equality) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "hello");
    R8EString *b = r8e_string_from_cstr(&ctx, "hello");
    R8EString *c = r8e_string_from_cstr(&ctx, "world");
    ASSERT_TRUE(r8e_string_equal(a, b));
    ASSERT_FALSE(r8e_string_equal(a, c));
    ASSERT_TRUE(r8e_string_equal_raw(a, "hello", 5));
    ASSERT_FALSE(r8e_string_equal_raw(a, "world", 5));
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
    r8e_string_free(&ctx, c);
}

/* =========================================================================
 * UTF-8 helpers
 * ========================================================================= */

TEST(utf8_decode_ascii) {
    const uint8_t data[] = "A";
    uint32_t cp;
    int n = r8e_utf8_decode(data, 1, &cp);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(cp, 'A');
}

TEST(utf8_decode_two_byte) {
    /* U+00E9 (e-acute) = 0xC3 0xA9 */
    const uint8_t data[] = {0xC3, 0xA9};
    uint32_t cp;
    int n = r8e_utf8_decode(data, 2, &cp);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(cp, 0xE9);
}

TEST(utf8_decode_three_byte) {
    /* U+4E16 (Chinese: world) = 0xE4 0xB8 0x96 */
    const uint8_t data[] = {0xE4, 0xB8, 0x96};
    uint32_t cp;
    int n = r8e_utf8_decode(data, 3, &cp);
    ASSERT_EQ_INT(n, 3);
    ASSERT_EQ_INT(cp, 0x4E16);
}

TEST(utf8_decode_four_byte) {
    /* U+1F600 (grinning face emoji) = 0xF0 0x9F 0x98 0x80 */
    const uint8_t data[] = {0xF0, 0x9F, 0x98, 0x80};
    uint32_t cp;
    int n = r8e_utf8_decode(data, 4, &cp);
    ASSERT_EQ_INT(n, 4);
    ASSERT_EQ_INT(cp, 0x1F600);
}

TEST(utf8_encode_roundtrip) {
    uint32_t codepoints[] = { 'A', 0xE9, 0x4E16, 0x1F600 };
    int expected_lens[] = { 1, 2, 3, 4 };

    for (int i = 0; i < 4; i++) {
        uint8_t buf[4];
        int n = r8e_utf8_encode(codepoints[i], buf);
        ASSERT_EQ_INT(n, expected_lens[i]);

        /* Decode back */
        uint32_t cp;
        int n2 = r8e_utf8_decode(buf, (uint32_t)n, &cp);
        ASSERT_EQ_INT(n2, n);
        ASSERT_EQ_INT(cp, codepoints[i]);
    }
}

TEST(utf8_strlen) {
    /* Pure ASCII: byte_len == char_len */
    ASSERT_EQ_INT(r8e_utf8_strlen("hello", 5), 5);

    /* "caf\xC3\xA9" = 4 characters in 5 bytes */
    ASSERT_EQ_INT(r8e_utf8_strlen("caf\xC3\xA9", 5), 4);

    /* Empty */
    ASSERT_EQ_INT(r8e_utf8_strlen("", 0), 0);
}

TEST(utf8_string_char_code_at_unicode) {
    R8EContext ctx = {0};
    /* "caf\xC3\xA9" = caf + U+00E9 */
    R8EString *s = r8e_string_new(&ctx, "caf\xC3\xA9", 5);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, 0), 'c');
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, 3), 0xE9);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended string creation
 * ========================================================================= */

TEST(string_new_ascii_flag) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_new(&ctx, "hello world", 11);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s->flags & R8E_STR_IS_ASCII);
    ASSERT_EQ_INT(s->byte_length, 11);
    ASSERT_EQ_INT(s->char_length, 11);
    r8e_string_free(&ctx, s);
}

TEST(string_new_long) {
    R8EContext ctx = {0};
    /* Create a string longer than inline threshold */
    const char *long_str = "this is a fairly long string that definitely goes to heap";
    R8EString *s = r8e_string_from_cstr(&ctx, long_str);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->byte_length, (uint32_t)strlen(long_str));
    ASSERT_EQ_STR(s->data, long_str);
    r8e_string_free(&ctx, s);
}

TEST(string_new_single_char) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_new(&ctx, "x", 1);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->byte_length, 1);
    ASSERT_EQ_INT(s->char_length, 1);
    ASSERT_EQ_STR(s->data, "x");
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended inline string tests
 * ========================================================================= */

TEST(inline_str_each_length) {
    /* Test encoding/decoding for lengths 1-6 individually */
    const char *strs[] = {"A", "AB", "ABC", "ABCD", "ABCDE", "ABCDEF"};
    for (int i = 0; i < 6; i++) {
        R8EValue v = r8e_inline_str_new(strs[i], (uint32_t)(i + 1));
        ASSERT_TRUE(R8E_IS_INLINE_STR(v));
        char buf[8];
        uint32_t len = r8e_inline_str_extract(v, buf);
        ASSERT_EQ_INT(len, (uint32_t)(i + 1));
        ASSERT_EQ_STR(buf, strs[i]);
    }
}

TEST(inline_str_digits) {
    R8EValue v = r8e_inline_str_new("123456", 6);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
    char buf[8];
    uint32_t len = r8e_inline_str_extract(v, buf);
    ASSERT_EQ_INT(len, 6);
    ASSERT_EQ_STR(buf, "123456");
}

TEST(inline_str_hash_consistency) {
    /* Same string should always produce same hash */
    R8EValue v1 = r8e_inline_str_new("abc", 3);
    R8EValue v2 = r8e_inline_str_new("abc", 3);
    ASSERT_EQ(r8e_inline_str_hash(v1), r8e_inline_str_hash(v2));

    /* Different strings should (very likely) produce different hash */
    R8EValue v3 = r8e_inline_str_new("xyz", 3);
    ASSERT_NE(r8e_inline_str_hash(v1), r8e_inline_str_hash(v3));
}

TEST(inline_str_empty_hash) {
    R8EValue v = r8e_inline_str_new("", 0);
    uint32_t h = r8e_inline_str_hash(v);
    /* FNV-1a of empty string = offset basis */
    ASSERT_EQ(h, 0x811C9DC5u);
}

/* =========================================================================
 * Extended value_from_string
 * ========================================================================= */

TEST(value_from_string_empty) {
    R8EContext ctx = {0};
    R8EValue v = r8e_value_from_string(&ctx, "", 0);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v));
}

TEST(value_from_string_boundary) {
    R8EContext ctx = {0};
    /* 6-char ASCII should be inline */
    R8EValue v6 = r8e_value_from_string(&ctx, "abcdef", 6);
    ASSERT_TRUE(R8E_IS_INLINE_STR(v6));

    /* 7+ chars should go to heap */
    R8EValue v7 = r8e_value_from_string(&ctx, "abcdefg", 7);
    ASSERT_TRUE(R8E_IS_POINTER(v7));
    R8EString *s = (R8EString *)r8e_get_pointer(v7);
    ASSERT_EQ_STR(s->data, "abcdefg");
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended charAt and charCodeAt
 * ========================================================================= */

TEST(char_at_negative_index) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *ch = r8e_string_char_at(&ctx, s, -1);
    ASSERT_TRUE(ch == NULL || ch->byte_length == 0);
    if (ch) r8e_string_free(&ctx, ch);
    r8e_string_free(&ctx, s);
}

TEST(char_code_at_negative) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    ASSERT_EQ_INT(r8e_string_char_code_at(&ctx, s, -1), -1);
    r8e_string_free(&ctx, s);
}

TEST(char_at_each_position) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abcde");
    const char *expected = "abcde";
    for (int i = 0; i < 5; i++) {
        R8EString *ch = r8e_string_char_at(&ctx, s, i);
        ASSERT_TRUE(ch != NULL);
        ASSERT_EQ_INT(ch->byte_length, 1);
        ASSERT_TRUE(ch->data[0] == expected[i]);
        r8e_string_free(&ctx, ch);
    }
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended indexOf / lastIndexOf
 * ========================================================================= */

TEST(index_of_from_position) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abcabc");
    R8EString *needle = r8e_string_from_cstr(&ctx, "abc");

    ASSERT_EQ_INT(r8e_string_index_of(&ctx, s, needle, 0), 0);
    ASSERT_EQ_INT(r8e_string_index_of(&ctx, s, needle, 1), 3);
    ASSERT_EQ_INT(r8e_string_index_of(&ctx, s, needle, 4), -1);

    r8e_string_free(&ctx, needle);
    r8e_string_free(&ctx, s);
}

TEST(index_of_empty_needle) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *empty = r8e_string_from_cstr(&ctx, "");

    /* Empty needle should be found at position 0 */
    ASSERT_EQ_INT(r8e_string_index_of(&ctx, s, empty, 0), 0);

    r8e_string_free(&ctx, empty);
    r8e_string_free(&ctx, s);
}

TEST(last_index_of_multiple) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abababab");
    R8EString *needle = r8e_string_from_cstr(&ctx, "ab");

    /* Last occurrence */
    ASSERT_EQ_INT(r8e_string_last_index_of(&ctx, s, needle, (int32_t)s->char_length), 6);

    r8e_string_free(&ctx, needle);
    r8e_string_free(&ctx, s);
}

TEST(includes_from_position) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *needle = r8e_string_from_cstr(&ctx, "hello");

    ASSERT_TRUE(r8e_string_includes(&ctx, s, needle, 0));
    ASSERT_FALSE(r8e_string_includes(&ctx, s, needle, 1));

    r8e_string_free(&ctx, needle);
    r8e_string_free(&ctx, s);
}

TEST(starts_with_position) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *prefix = r8e_string_from_cstr(&ctx, "world");

    ASSERT_FALSE(r8e_string_starts_with(&ctx, s, prefix, 0));
    ASSERT_TRUE(r8e_string_starts_with(&ctx, s, prefix, 6));

    r8e_string_free(&ctx, prefix);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended slice
 * ========================================================================= */

TEST(slice_entire_string) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *r = r8e_string_slice(&ctx, s, 0, (int32_t)s->char_length);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(slice_middle) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    R8EString *r = r8e_string_slice(&ctx, s, 3, 8);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "lo wo");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(slice_both_negative) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello world");
    /* slice(-5, -1) extracts "worl" */
    R8EString *r = r8e_string_slice(&ctx, s, -5, -1);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "worl");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(slice_start_beyond_end) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    /* start > end produces empty string */
    R8EString *r = r8e_string_slice(&ctx, s, 3, 1);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended case conversion
 * ========================================================================= */

TEST(to_lower_mixed_case) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "Hello World 123!");
    R8EString *r = r8e_string_to_lower(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello world 123!");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(to_upper_digits_unchanged) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "abc123def");
    R8EString *r = r8e_string_to_upper(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "ABC123DEF");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(to_lower_empty) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "");
    R8EString *r = r8e_string_to_lower(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(to_upper_empty) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "");
    R8EString *r = r8e_string_to_upper(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended trim
 * ========================================================================= */

TEST(trim_tabs_newlines) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "\t\nhello\r\n");
    R8EString *r = r8e_string_trim(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(trim_all_whitespace) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "   \t\n  ");
    R8EString *r = r8e_string_trim(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(trim_start_only_leading) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *r = r8e_string_trim_start(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(trim_end_only_trailing) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    R8EString *r = r8e_string_trim_end(&ctx, s);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_STR(r->data, "hello");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended split
 * ========================================================================= */

TEST(split_with_limit) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "a,b,c,d,e");
    R8EString *sep = r8e_string_from_cstr(&ctx, ",");
    uint32_t count;
    R8EString **parts = r8e_string_split(&ctx, s, sep, 3, &count);
    ASSERT_TRUE(parts != NULL);
    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_STR(parts[0]->data, "a");
    ASSERT_EQ_STR(parts[1]->data, "b");
    ASSERT_EQ_STR(parts[2]->data, "c");
    for (uint32_t i = 0; i < count; i++) r8e_string_free(&ctx, parts[i]);
    free(parts);
    r8e_string_free(&ctx, sep);
    r8e_string_free(&ctx, s);
}

TEST(split_multi_char_separator) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "a::b::c");
    R8EString *sep = r8e_string_from_cstr(&ctx, "::");
    uint32_t count;
    R8EString **parts = r8e_string_split(&ctx, s, sep, -1, &count);
    ASSERT_TRUE(parts != NULL);
    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_STR(parts[0]->data, "a");
    ASSERT_EQ_STR(parts[1]->data, "b");
    ASSERT_EQ_STR(parts[2]->data, "c");
    for (uint32_t i = 0; i < count; i++) r8e_string_free(&ctx, parts[i]);
    free(parts);
    r8e_string_free(&ctx, sep);
    r8e_string_free(&ctx, s);
}

TEST(split_adjacent_separators) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "a,,b");
    R8EString *sep = r8e_string_from_cstr(&ctx, ",");
    uint32_t count;
    R8EString **parts = r8e_string_split(&ctx, s, sep, -1, &count);
    ASSERT_TRUE(parts != NULL);
    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_STR(parts[0]->data, "a");
    ASSERT_EQ_STR(parts[1]->data, "");
    ASSERT_EQ_STR(parts[2]->data, "b");
    for (uint32_t i = 0; i < count; i++) r8e_string_free(&ctx, parts[i]);
    free(parts);
    r8e_string_free(&ctx, sep);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended repeat
 * ========================================================================= */

TEST(repeat_large) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "ab");
    R8EString *r = r8e_string_repeat(&ctx, s, 10);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 20);
    ASSERT_EQ_STR(r->data, "abababababababababab");
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

TEST(repeat_empty_string) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "");
    R8EString *r = r8e_string_repeat(&ctx, s, 5);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, s);
}

/* =========================================================================
 * Extended concat
 * ========================================================================= */

TEST(concat_both_empty) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "");
    R8EString *b = r8e_string_from_cstr(&ctx, "");
    R8EString *r = r8e_string_concat(&ctx, a, b);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->byte_length, 0);
    r8e_string_free(&ctx, r);
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
}

TEST(concat_multiple) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "foo");
    R8EString *b = r8e_string_from_cstr(&ctx, "bar");
    R8EString *c = r8e_string_from_cstr(&ctx, "baz");
    R8EString *ab = r8e_string_concat(&ctx, a, b);
    R8EString *abc = r8e_string_concat(&ctx, ab, c);
    ASSERT_TRUE(abc != NULL);
    ASSERT_EQ_STR(abc->data, "foobarbaz");
    r8e_string_free(&ctx, abc);
    r8e_string_free(&ctx, ab);
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
    r8e_string_free(&ctx, c);
}

/* =========================================================================
 * Extended string equality
 * ========================================================================= */

TEST(string_equal_different_length) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "hello");
    R8EString *b = r8e_string_from_cstr(&ctx, "hell");
    ASSERT_FALSE(r8e_string_equal(a, b));
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
}

TEST(string_equal_raw_mismatch) {
    R8EContext ctx = {0};
    R8EString *s = r8e_string_from_cstr(&ctx, "hello");
    ASSERT_FALSE(r8e_string_equal_raw(s, "hell", 4));
    ASSERT_FALSE(r8e_string_equal_raw(s, "hellx", 5));
    r8e_string_free(&ctx, s);
}

TEST(string_equal_empty) {
    R8EContext ctx = {0};
    R8EString *a = r8e_string_from_cstr(&ctx, "");
    R8EString *b = r8e_string_from_cstr(&ctx, "");
    ASSERT_TRUE(r8e_string_equal(a, b));
    r8e_string_free(&ctx, a);
    r8e_string_free(&ctx, b);
}

/* =========================================================================
 * Extended UTF-8 tests
 * ========================================================================= */

TEST(utf8_decode_invalid_short) {
    /* Incomplete sequence: only lead byte of 2-byte */
    const uint8_t data[] = {0xC3};
    uint32_t cp;
    int n = r8e_utf8_decode(data, 1, &cp);
    /* Should return error or partial decode */
    ASSERT_TRUE(n <= 0 || n == 1);
}

TEST(utf8_encode_boundary) {
    /* Test encoding at boundary of each byte count */
    uint8_t buf[4];

    /* U+007F = 1 byte max */
    int n = r8e_utf8_encode(0x7F, buf);
    ASSERT_EQ_INT(n, 1);

    /* U+0080 = 2 byte min */
    n = r8e_utf8_encode(0x80, buf);
    ASSERT_EQ_INT(n, 2);

    /* U+07FF = 2 byte max */
    n = r8e_utf8_encode(0x7FF, buf);
    ASSERT_EQ_INT(n, 2);

    /* U+0800 = 3 byte min */
    n = r8e_utf8_encode(0x800, buf);
    ASSERT_EQ_INT(n, 3);

    /* U+FFFF = 3 byte max */
    n = r8e_utf8_encode(0xFFFF, buf);
    ASSERT_EQ_INT(n, 3);

    /* U+10000 = 4 byte min */
    n = r8e_utf8_encode(0x10000, buf);
    ASSERT_EQ_INT(n, 4);

    /* U+10FFFF = 4 byte max (highest valid Unicode) */
    n = r8e_utf8_encode(0x10FFFF, buf);
    ASSERT_EQ_INT(n, 4);
}

TEST(utf8_strlen_mixed) {
    /* Mix of 1-byte and multi-byte characters */
    /* "a" + U+00E9 (2 bytes) + "b" = 4 bytes, 3 chars */
    ASSERT_EQ_INT(r8e_utf8_strlen("a\xC3\xA9" "b", 4), 3);
}

TEST(utf8_strlen_emoji) {
    /* U+1F600 = 4 bytes, 1 char */
    const char emoji[] = "\xF0\x9F\x98\x80";
    ASSERT_EQ_INT(r8e_utf8_strlen(emoji, 4), 1);
}

/* =========================================================================
 * Hash collision resistance
 * ========================================================================= */

TEST(hash_different_strings) {
    /* Verify that many similar short strings produce different hashes */
    R8EContext ctx = {0};
    uint32_t hashes[26];
    char buf[2] = {0, 0};
    for (int i = 0; i < 26; i++) {
        buf[0] = (char)('a' + i);
        R8EString *s = r8e_string_new(&ctx, buf, 1);
        hashes[i] = s->hash;
        r8e_string_free(&ctx, s);
    }
    /* Check that at least 20 of 26 are unique (collision-free for single chars) */
    int unique = 0;
    for (int i = 0; i < 26; i++) {
        bool is_unique = true;
        for (int j = 0; j < i; j++) {
            if (hashes[i] == hashes[j]) { is_unique = false; break; }
        }
        if (is_unique) unique++;
    }
    ASSERT_TRUE(unique >= 20);
}

/* =========================================================================
 * Test suite entry point
 * ========================================================================= */

void test_suite_string(void) {
    /* String creation */
    RUN_TEST(string_new_ascii);
    RUN_TEST(string_new_empty);
    RUN_TEST(string_new_unicode);
    RUN_TEST(string_from_cstr);

    /* Inline strings */
    RUN_TEST(inline_str_creation);
    RUN_TEST(inline_str_empty_creation);
    RUN_TEST(inline_str_max_length);
    RUN_TEST(value_from_string_inline);
    RUN_TEST(value_from_string_heap);

    /* Hash */
    RUN_TEST(fnv1a_hash_consistency);
    RUN_TEST(fnv1a_hash_stored_in_string);
    RUN_TEST(inline_str_hash);

    /* Character access */
    RUN_TEST(char_at_ascii);
    RUN_TEST(char_code_at_ascii);

    /* Searching */
    RUN_TEST(index_of_basic);
    RUN_TEST(last_index_of_basic);
    RUN_TEST(includes);
    RUN_TEST(starts_with);
    RUN_TEST(ends_with);

    /* Slice */
    RUN_TEST(slice_positive);
    RUN_TEST(slice_negative);
    RUN_TEST(slice_empty);

    /* Case conversion */
    RUN_TEST(to_upper_case);
    RUN_TEST(to_lower_case);
    RUN_TEST(to_upper_mixed);

    /* Trim */
    RUN_TEST(trim);
    RUN_TEST(trim_start);
    RUN_TEST(trim_end);
    RUN_TEST(trim_no_whitespace);

    /* Split */
    RUN_TEST(split_simple);
    RUN_TEST(split_no_separator_match);

    /* Repeat */
    RUN_TEST(repeat_zero);
    RUN_TEST(repeat_one);
    RUN_TEST(repeat_three);

    /* Concat */
    RUN_TEST(concat_basic);
    RUN_TEST(concat_with_empty);

    /* Equality */
    RUN_TEST(string_equality);

    /* UTF-8 */
    RUN_TEST(utf8_decode_ascii);
    RUN_TEST(utf8_decode_two_byte);
    RUN_TEST(utf8_decode_three_byte);
    RUN_TEST(utf8_decode_four_byte);
    RUN_TEST(utf8_encode_roundtrip);
    RUN_TEST(utf8_strlen);
    RUN_TEST(utf8_string_char_code_at_unicode);

    /* Extended string creation */
    RUN_TEST(string_new_ascii_flag);
    RUN_TEST(string_new_long);
    RUN_TEST(string_new_single_char);

    /* Extended inline strings */
    RUN_TEST(inline_str_each_length);
    RUN_TEST(inline_str_digits);
    RUN_TEST(inline_str_hash_consistency);
    RUN_TEST(inline_str_empty_hash);

    /* Extended value_from_string */
    RUN_TEST(value_from_string_empty);
    RUN_TEST(value_from_string_boundary);

    /* Extended charAt/charCodeAt */
    RUN_TEST(char_at_negative_index);
    RUN_TEST(char_code_at_negative);
    RUN_TEST(char_at_each_position);

    /* Extended indexOf/lastIndexOf */
    RUN_TEST(index_of_from_position);
    RUN_TEST(index_of_empty_needle);
    RUN_TEST(last_index_of_multiple);
    RUN_TEST(includes_from_position);
    RUN_TEST(starts_with_position);

    /* Extended slice */
    RUN_TEST(slice_entire_string);
    RUN_TEST(slice_middle);
    RUN_TEST(slice_both_negative);
    RUN_TEST(slice_start_beyond_end);

    /* Extended case conversion */
    RUN_TEST(to_lower_mixed_case);
    RUN_TEST(to_upper_digits_unchanged);
    RUN_TEST(to_lower_empty);
    RUN_TEST(to_upper_empty);

    /* Extended trim */
    RUN_TEST(trim_tabs_newlines);
    RUN_TEST(trim_all_whitespace);
    RUN_TEST(trim_start_only_leading);
    RUN_TEST(trim_end_only_trailing);

    /* Extended split */
    RUN_TEST(split_with_limit);
    RUN_TEST(split_multi_char_separator);
    RUN_TEST(split_adjacent_separators);

    /* Extended repeat */
    RUN_TEST(repeat_large);
    RUN_TEST(repeat_empty_string);

    /* Extended concat */
    RUN_TEST(concat_both_empty);
    RUN_TEST(concat_multiple);

    /* Extended equality */
    RUN_TEST(string_equal_different_length);
    RUN_TEST(string_equal_raw_mismatch);
    RUN_TEST(string_equal_empty);

    /* Extended UTF-8 */
    RUN_TEST(utf8_decode_invalid_short);
    RUN_TEST(utf8_encode_boundary);
    RUN_TEST(utf8_strlen_mixed);
    RUN_TEST(utf8_strlen_emoji);

    /* Hash collision resistance */
    RUN_TEST(hash_different_strings);
}
