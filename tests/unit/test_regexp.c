/*
 * test_regexp.c - Unit tests for r8e_regexp.c (Tiered Regular Expression Engine)
 *
 * Tests cover:
 *   - Compile simple patterns (literal, dot, anchors)
 *   - Match simple patterns against strings
 *   - Character classes (\d, \w, \s, [a-z], [^0-9])
 *   - Quantifiers (*, +, ?, {n}, {n,m})
 *   - Groups and captures
 *   - Flags (i case-insensitive, g global, m multiline)
 *   - No-match returns null
 *   - Fuel limit prevents ReDoS (catastrophic backtracking)
 *   - NFA64 engine selection for complex patterns
 *   - Invalid pattern rejection
 *   - Anchored patterns (^, $)
 *   - Alternation (|)
 *   - Escape sequences in patterns
 *   - Empty pattern
 *   - Dot-star pattern
 *   - Word boundary (\b)
 *   - Nested quantifiers trigger NFA
 *   - Multiple groups capture
 *   - Non-capturing groups (?:...)
 *   - Lazy quantifiers (*?, +?)
 *   - Unicode in input strings
 *   - Large input string performance
 *   - Pattern with special regex chars
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define ASSERT_EQ(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %llu != %llu\n", __FILE__, __LINE__, \
               (unsigned long long)_a, (unsigned long long)_b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a == _b) { \
        printf("  FAIL: %s:%d: both are %llu\n", __FILE__, __LINE__, \
               (unsigned long long)_a); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
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
 * Type definitions (must match r8e_regexp.c layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)

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

/* Regex engine type selection */
#define R8E_REGEX_ENGINE_BACKTRACK  0
#define R8E_REGEX_ENGINE_NFA64      1

/* Regex flags */
#define R8E_REGEX_FLAG_GLOBAL        0x01
#define R8E_REGEX_FLAG_IGNORECASE    0x02
#define R8E_REGEX_FLAG_MULTILINE     0x04
#define R8E_REGEX_FLAG_DOTALL        0x08
#define R8E_REGEX_FLAG_UNICODE       0x10
#define R8E_REGEX_FLAG_STICKY        0x20

/* Regex status codes */
#define R8E_REGEX_OK           0
#define R8E_REGEX_NOMATCH     -1
#define R8E_REGEX_ERROR       -2
#define R8E_REGEX_FUEL_EXHAUSTED -3

/* Maximum captures */
#define R8E_REGEX_MAX_CAPTURES 32

/* Compiled regex */
typedef struct R8ERegex R8ERegex;

/* Match result */
typedef struct {
    int32_t  start;                              /* match start offset */
    int32_t  end;                                /* match end offset */
    int32_t  captures_start[R8E_REGEX_MAX_CAPTURES]; /* group start offsets */
    int32_t  captures_end[R8E_REGEX_MAX_CAPTURES];   /* group end offsets */
    uint16_t capture_count;                      /* number of groups matched */
    uint8_t  engine_used;                        /* which engine was selected */
} R8ERegexMatch;

/* =========================================================================
 * Forward declarations of functions under test (from r8e_regexp.c)
 * ========================================================================= */

extern R8ERegex *r8e_regex_compile(const char *pattern, uint32_t pattern_len,
                                     uint32_t flags, char *error_buf,
                                     uint32_t error_buf_size);
extern void r8e_regex_free(R8ERegex *re);
extern int r8e_regex_exec(R8ERegex *re, const char *input, uint32_t input_len,
                           int32_t start_offset, R8ERegexMatch *match);
extern int r8e_regex_test(R8ERegex *re, const char *input, uint32_t input_len);
extern uint8_t r8e_regex_engine(const R8ERegex *re);
extern uint32_t r8e_regex_flags(const R8ERegex *re);
extern uint16_t r8e_regex_group_count(const R8ERegex *re);
extern uint32_t r8e_regex_fuel_limit(void);
extern void r8e_regex_set_fuel_limit(uint32_t limit);

/* =========================================================================
 * Tests: Pattern Compilation
 * ========================================================================= */

/* Compile a simple literal pattern */
static void test_compile_literal(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("hello", 5, 0, err, sizeof(err));
    ASSERT_NOT_NULL(re);
    ASSERT_EQ_INT(err[0], 0);
    r8e_regex_free(re);
}

/* Compile a dot pattern */
static void test_compile_dot(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("a.b", 3, 0, err, sizeof(err));
    ASSERT_NOT_NULL(re);
    r8e_regex_free(re);
}

/* Compile anchor patterns */
static void test_compile_anchors(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("^start$", 7, 0, err, sizeof(err));
    ASSERT_NOT_NULL(re);
    r8e_regex_free(re);
}

/* Compile empty pattern */
static void test_compile_empty_pattern(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("", 0, 0, err, sizeof(err));
    ASSERT_NOT_NULL(re);
    /* Empty pattern should match anything at position 0 */
    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "abc", 3, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 0);
    r8e_regex_free(re);
}

/* Invalid pattern should return NULL and set error */
static void test_compile_invalid_pattern(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("[unclosed", 9, 0, err, sizeof(err));
    ASSERT_NULL(re);
    ASSERT_TRUE(err[0] != 0); /* error message set */
}

/* Invalid pattern: unmatched parenthesis */
static void test_compile_unmatched_paren(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("(abc", 4, 0, err, sizeof(err));
    ASSERT_NULL(re);
    ASSERT_TRUE(err[0] != 0);
}

/* Invalid pattern: bad quantifier */
static void test_compile_bad_quantifier(void) {
    char err[128] = {0};
    R8ERegex *re = r8e_regex_compile("*abc", 4, 0, err, sizeof(err));
    ASSERT_NULL(re);
    ASSERT_TRUE(err[0] != 0);
}

/* =========================================================================
 * Tests: Simple Matching
 * ========================================================================= */

/* Match a literal string */
static void test_match_literal(void) {
    R8ERegex *re = r8e_regex_compile("hello", 5, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "say hello world", 15, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 4);
    ASSERT_EQ_INT(m.end, 9);

    r8e_regex_free(re);
}

/* No match returns NOMATCH */
static void test_no_match(void) {
    R8ERegex *re = r8e_regex_compile("xyz", 3, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "hello world", 11, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* Dot matches any character except newline */
static void test_match_dot(void) {
    R8ERegex *re = r8e_regex_compile("a.c", 3, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "abc", 3, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 3);

    /* Dot should not match newline */
    rc = r8e_regex_exec(re, "a\nc", 3, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* test() boolean API */
static void test_test_api(void) {
    R8ERegex *re = r8e_regex_compile("world", 5, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    ASSERT_EQ_INT(r8e_regex_test(re, "hello world", 11), 1);
    ASSERT_EQ_INT(r8e_regex_test(re, "hello earth", 11), 0);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Character Classes
 * ========================================================================= */

/* \d matches digits */
static void test_char_class_digit(void) {
    R8ERegex *re = r8e_regex_compile("\\d+", 3, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "abc123def", 9, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 3);
    ASSERT_EQ_INT(m.end, 6);

    r8e_regex_free(re);
}

/* \w matches word characters */
static void test_char_class_word(void) {
    R8ERegex *re = r8e_regex_compile("\\w+", 3, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "  hello  ", 9, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 2);
    ASSERT_EQ_INT(m.end, 7);

    r8e_regex_free(re);
}

/* \s matches whitespace */
static void test_char_class_space(void) {
    R8ERegex *re = r8e_regex_compile("\\s+", 3, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "hello world", 11, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 5);
    ASSERT_EQ_INT(m.end, 6);

    r8e_regex_free(re);
}

/* [a-z] range character class */
static void test_char_class_range(void) {
    R8ERegex *re = r8e_regex_compile("[a-z]+", 6, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "123abc456", 9, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 3);
    ASSERT_EQ_INT(m.end, 6);

    r8e_regex_free(re);
}

/* [^0-9] negated character class */
static void test_char_class_negated(void) {
    R8ERegex *re = r8e_regex_compile("[^0-9]+", 7, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "123abc456", 9, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 3);
    ASSERT_EQ_INT(m.end, 6);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Quantifiers
 * ========================================================================= */

/* Star quantifier: zero or more */
static void test_quantifier_star(void) {
    R8ERegex *re = r8e_regex_compile("ab*c", 4, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    /* "ac" matches (zero b's) */
    int rc = r8e_regex_exec(re, "ac", 2, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);

    /* "abbc" matches (two b's) */
    rc = r8e_regex_exec(re, "abbc", 4, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 4);

    r8e_regex_free(re);
}

/* Plus quantifier: one or more */
static void test_quantifier_plus(void) {
    R8ERegex *re = r8e_regex_compile("ab+c", 4, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    /* "ac" should NOT match (needs at least one b) */
    int rc = r8e_regex_exec(re, "ac", 2, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_NOMATCH);

    /* "abc" matches (one b) */
    rc = r8e_regex_exec(re, "abc", 3, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);

    r8e_regex_free(re);
}

/* Question mark: zero or one */
static void test_quantifier_question(void) {
    R8ERegex *re = r8e_regex_compile("colou?r", 7, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "color", 5, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(r8e_regex_exec(re, "colour", 6, 0, &m), R8E_REGEX_OK);

    r8e_regex_free(re);
}

/* {n} exact repetition */
static void test_quantifier_exact(void) {
    R8ERegex *re = r8e_regex_compile("a{3}", 4, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "aa", 2, 0, &m), R8E_REGEX_NOMATCH);
    ASSERT_EQ_INT(r8e_regex_exec(re, "aaa", 3, 0, &m), R8E_REGEX_OK);

    r8e_regex_free(re);
}

/* {n,m} bounded repetition */
static void test_quantifier_range(void) {
    R8ERegex *re = r8e_regex_compile("a{2,4}", 6, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "a", 1, 0, &m), R8E_REGEX_NOMATCH);
    ASSERT_EQ_INT(r8e_regex_exec(re, "aa", 2, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(r8e_regex_exec(re, "aaaa", 4, 0, &m), R8E_REGEX_OK);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Groups and Captures
 * ========================================================================= */

/* Single capture group */
static void test_group_capture(void) {
    R8ERegex *re = r8e_regex_compile("(\\d+)-(\\d+)", 12, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "date: 2026-03", 13, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.capture_count, 2);
    /* Group 1: "2026" */
    ASSERT_EQ_INT(m.captures_start[0], 6);
    ASSERT_EQ_INT(m.captures_end[0], 10);
    /* Group 2: "03" */
    ASSERT_EQ_INT(m.captures_start[1], 11);
    ASSERT_EQ_INT(m.captures_end[1], 13);

    r8e_regex_free(re);
}

/* Group count accessor */
static void test_group_count(void) {
    R8ERegex *re = r8e_regex_compile("(a)(b)(c)", 9, 0, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_EQ_INT(r8e_regex_group_count(re), 3);
    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Flags
 * ========================================================================= */

/* Case-insensitive flag */
static void test_flag_ignorecase(void) {
    R8ERegex *re = r8e_regex_compile("hello", 5,
                                       R8E_REGEX_FLAG_IGNORECASE, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(r8e_regex_flags(re) & R8E_REGEX_FLAG_IGNORECASE);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "Hello", 5, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(r8e_regex_exec(re, "HELLO", 5, 0, &m), R8E_REGEX_OK);

    r8e_regex_free(re);
}

/* Multiline flag: ^ and $ match line boundaries */
static void test_flag_multiline(void) {
    R8ERegex *re = r8e_regex_compile("^line$", 6,
                                       R8E_REGEX_FLAG_MULTILINE, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "first\nline\nlast", 15, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 6);
    ASSERT_EQ_INT(m.end, 10);

    r8e_regex_free(re);
}

/* Global flag is stored */
static void test_flag_global(void) {
    R8ERegex *re = r8e_regex_compile("a", 1,
                                       R8E_REGEX_FLAG_GLOBAL, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(r8e_regex_flags(re) & R8E_REGEX_FLAG_GLOBAL);
    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Engine Selection
 * ========================================================================= */

/* Simple patterns use backtracking engine */
static void test_engine_simple_uses_backtrack(void) {
    R8ERegex *re = r8e_regex_compile("abc", 3, 0, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_EQ_INT(r8e_regex_engine(re), R8E_REGEX_ENGINE_BACKTRACK);
    r8e_regex_free(re);
}

/* Complex patterns (nested quantifiers) use NFA64 */
static void test_engine_complex_uses_nfa(void) {
    /* (a+)+ is a nested quantifier - should trigger NFA engine */
    R8ERegex *re = r8e_regex_compile("(a+)+b", 6, 0, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_EQ_INT(r8e_regex_engine(re), R8E_REGEX_ENGINE_NFA64);
    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Fuel Limit (ReDoS Prevention)
 * ========================================================================= */

/* Fuel limit prevents catastrophic backtracking */
static void test_fuel_limit_prevents_redos(void) {
    /* Set a very low fuel limit */
    uint32_t original = r8e_regex_fuel_limit();
    r8e_regex_set_fuel_limit(100);

    /* This pattern with backtracking is pathological on non-matching input */
    R8ERegex *re = r8e_regex_compile("a*a*a*a*b", 9, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    /* Long input of 'a's with no 'b' -> triggers fuel exhaustion or NFA */
    char input[64];
    memset(input, 'a', 63);
    input[63] = '\0';

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, input, 63, 0, &m);
    /* Should either be NOMATCH or FUEL_EXHAUSTED, not hang */
    ASSERT_TRUE(rc == R8E_REGEX_NOMATCH || rc == R8E_REGEX_FUEL_EXHAUSTED);

    r8e_regex_set_fuel_limit(original);
    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Alternation
 * ========================================================================= */

/* Alternation matches either branch */
static void test_alternation(void) {
    R8ERegex *re = r8e_regex_compile("cat|dog", 7, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "I have a cat", 12, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 9);
    ASSERT_EQ_INT(m.end, 12);

    ASSERT_EQ_INT(r8e_regex_exec(re, "I have a dog", 12, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 9);
    ASSERT_EQ_INT(m.end, 12);

    ASSERT_EQ_INT(r8e_regex_exec(re, "I have a fish", 13, 0, &m),
                  R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Start Offset
 * ========================================================================= */

/* Match starting from a non-zero offset */
static void test_start_offset(void) {
    R8ERegex *re = r8e_regex_compile("ab", 2, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    /* "ababab" - start searching from offset 2 */
    int rc = r8e_regex_exec(re, "ababab", 6, 2, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 2);
    ASSERT_EQ_INT(m.end, 4);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Dot-star greedy match
 * ========================================================================= */

/* Dot-star matches greedily */
static void test_dot_star_greedy(void) {
    R8ERegex *re = r8e_regex_compile("a.*b", 4, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "a1b2b", 5, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    /* Greedy: should match "a1b2b" (entire string) */
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 5);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Escape Sequences in Patterns
 * ========================================================================= */

/* Escaped special characters match literally */
static void test_escape_special_chars(void) {
    R8ERegex *re = r8e_regex_compile("\\.", 2, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    /* Should match literal '.' not any character */
    ASSERT_EQ_INT(r8e_regex_exec(re, "a.b", 3, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 1);
    ASSERT_EQ_INT(m.end, 2);

    /* Should NOT match 'a' (dot is not wildcard when escaped) */
    ASSERT_EQ_INT(r8e_regex_exec(re, "abc", 3, 0, &m), R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Lazy Quantifiers
 * ========================================================================= */

/* Lazy star: .*? matches minimal */
static void test_lazy_star(void) {
    R8ERegex *re = r8e_regex_compile("a.*?b", 5, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "a1b2b", 5, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    /* Lazy: should match "a1b" not "a1b2b" */
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 3);

    r8e_regex_free(re);
}

/* Lazy plus: .+? matches minimal */
static void test_lazy_plus(void) {
    R8ERegex *re = r8e_regex_compile("a.+?b", 5, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "axb2b", 5, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 3);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Non-Capturing Groups
 * ========================================================================= */

/* Non-capturing group: (?:...) */
static void test_non_capturing_group(void) {
    R8ERegex *re = r8e_regex_compile("(?:ab)+", 7, 0, NULL, 0);
    ASSERT_NOT_NULL(re);
    /* Non-capturing groups do not add to capture count */
    ASSERT_EQ_INT(r8e_regex_group_count(re), 0);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "ababab", 6, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 6);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Word Boundary
 * ========================================================================= */

/* \b word boundary */
static void test_word_boundary(void) {
    R8ERegex *re = r8e_regex_compile("\\bword\\b", 8, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    /* "word" as a standalone word */
    int rc = r8e_regex_exec(re, "a word here", 11, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 2);
    ASSERT_EQ_INT(m.end, 6);

    /* "sword" should not match \bword\b */
    rc = r8e_regex_exec(re, "swordfish", 9, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: DotAll Flag (s flag)
 * ========================================================================= */

/* DotAll: dot matches newline */
static void test_flag_dotall(void) {
    R8ERegex *re = r8e_regex_compile("a.b", 3,
                                       R8E_REGEX_FLAG_DOTALL, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    /* Dot should match newline with dotAll flag */
    int rc = r8e_regex_exec(re, "a\nb", 3, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 3);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Unicode Flag
 * ========================================================================= */

/* Unicode flag is stored */
static void test_flag_unicode(void) {
    R8ERegex *re = r8e_regex_compile("a", 1,
                                       R8E_REGEX_FLAG_UNICODE, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(r8e_regex_flags(re) & R8E_REGEX_FLAG_UNICODE);
    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Sticky Flag
 * ========================================================================= */

/* Sticky flag is stored */
static void test_flag_sticky(void) {
    R8ERegex *re = r8e_regex_compile("a", 1,
                                       R8E_REGEX_FLAG_STICKY, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(r8e_regex_flags(re) & R8E_REGEX_FLAG_STICKY);
    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Multiple Capture Groups
 * ========================================================================= */

/* Pattern with 3 groups */
static void test_multiple_groups(void) {
    R8ERegex *re = r8e_regex_compile("(\\w+)@(\\w+)\\.(\\w+)", 19, 0, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_EQ_INT(r8e_regex_group_count(re), 3);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "user@host.com", 13, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.capture_count, 3);
    /* Group 1: "user" */
    ASSERT_EQ_INT(m.captures_start[0], 0);
    ASSERT_EQ_INT(m.captures_end[0], 4);
    /* Group 2: "host" */
    ASSERT_EQ_INT(m.captures_start[1], 5);
    ASSERT_EQ_INT(m.captures_end[1], 9);
    /* Group 3: "com" */
    ASSERT_EQ_INT(m.captures_start[2], 10);
    ASSERT_EQ_INT(m.captures_end[2], 13);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Empty Input
 * ========================================================================= */

/* Match against empty input */
static void test_match_empty_input(void) {
    R8ERegex *re = r8e_regex_compile("a*", 2, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "", 0, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    /* a* on empty matches empty string at position 0 */
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 0);

    r8e_regex_free(re);
}

/* Non-zero min pattern on empty input: no match */
static void test_no_match_empty_input(void) {
    R8ERegex *re = r8e_regex_compile("a+", 2, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "", 0, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Anchored Full Match
 * ========================================================================= */

/* ^...$ on a full-line string */
static void test_full_anchor_match(void) {
    R8ERegex *re = r8e_regex_compile("^hello$", 7, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "hello", 5, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 5);

    /* Should not match if there is extra text */
    ASSERT_EQ_INT(r8e_regex_exec(re, "hello world", 11, 0, &m),
                  R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: {n,} unbounded repetition
 * ========================================================================= */

static void test_quantifier_unbounded(void) {
    R8ERegex *re = r8e_regex_compile("a{2,}", 5, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "a", 1, 0, &m), R8E_REGEX_NOMATCH);
    ASSERT_EQ_INT(r8e_regex_exec(re, "aa", 2, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(r8e_regex_exec(re, "aaaaaaa", 7, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 7);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Escaped Characters in Patterns
 * ========================================================================= */

/* Escaped brackets */
static void test_escape_brackets(void) {
    R8ERegex *re = r8e_regex_compile("\\[test\\]", 8, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "[test]", 6, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 0);
    ASSERT_EQ_INT(m.end, 6);

    ASSERT_EQ_INT(r8e_regex_exec(re, "test", 4, 0, &m), R8E_REGEX_NOMATCH);

    r8e_regex_free(re);
}

/* Escaped parens */
static void test_escape_parens(void) {
    R8ERegex *re = r8e_regex_compile("\\(x\\)", 5, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "(x)", 3, 0, &m), R8E_REGEX_OK);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Fuel Limit API
 * ========================================================================= */

/* Default fuel limit is positive */
static void test_fuel_limit_default(void) {
    uint32_t fuel = r8e_regex_fuel_limit();
    ASSERT_TRUE(fuel > 0);
}

/* Set and read fuel limit */
static void test_fuel_limit_set_get(void) {
    uint32_t original = r8e_regex_fuel_limit();
    r8e_regex_set_fuel_limit(500000);
    ASSERT_EQ_INT(r8e_regex_fuel_limit(), 500000);
    r8e_regex_set_fuel_limit(original); /* restore */
}

/* =========================================================================
 * Tests: Large Input
 * ========================================================================= */

/* Match a pattern in a large input */
static void test_large_input(void) {
    R8ERegex *re = r8e_regex_compile("needle", 6, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    /* Build a large haystack */
    char input[1024];
    memset(input, 'x', sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    /* Plant the needle near the end */
    memcpy(input + 1000, "needle", 6);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, input, (uint32_t)strlen(input), 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 1000);
    ASSERT_EQ_INT(m.end, 1006);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Alternation with Groups
 * ========================================================================= */

/* Alternation inside a group */
static void test_alternation_in_group(void) {
    R8ERegex *re = r8e_regex_compile("(cat|dog|bird)", 14, 0, NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_EQ_INT(r8e_regex_group_count(re), 1);

    R8ERegexMatch m = {0};
    ASSERT_EQ_INT(r8e_regex_exec(re, "my bird", 7, 0, &m), R8E_REGEX_OK);
    ASSERT_EQ_INT(m.captures_start[0], 3);
    ASSERT_EQ_INT(m.captures_end[0], 7);

    r8e_regex_free(re);
}

/* =========================================================================
 * Tests: Character Class with Special Chars
 * ========================================================================= */

/* Character class with hyphen at start */
static void test_char_class_hyphen_start(void) {
    R8ERegex *re = r8e_regex_compile("[-abc]+", 7, 0, NULL, 0);
    ASSERT_NOT_NULL(re);

    R8ERegexMatch m = {0};
    int rc = r8e_regex_exec(re, "x-a-b-cx", 8, 0, &m);
    ASSERT_EQ_INT(rc, R8E_REGEX_OK);
    ASSERT_EQ_INT(m.start, 1);
    ASSERT_EQ_INT(m.end, 7);

    r8e_regex_free(re);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_regexp_tests(void) {
    /* Compilation */
    RUN_TEST(test_compile_literal);
    RUN_TEST(test_compile_dot);
    RUN_TEST(test_compile_anchors);
    RUN_TEST(test_compile_empty_pattern);
    RUN_TEST(test_compile_invalid_pattern);
    RUN_TEST(test_compile_unmatched_paren);
    RUN_TEST(test_compile_bad_quantifier);

    /* Simple matching */
    RUN_TEST(test_match_literal);
    RUN_TEST(test_no_match);
    RUN_TEST(test_match_dot);
    RUN_TEST(test_test_api);

    /* Character classes */
    RUN_TEST(test_char_class_digit);
    RUN_TEST(test_char_class_word);
    RUN_TEST(test_char_class_space);
    RUN_TEST(test_char_class_range);
    RUN_TEST(test_char_class_negated);

    /* Quantifiers */
    RUN_TEST(test_quantifier_star);
    RUN_TEST(test_quantifier_plus);
    RUN_TEST(test_quantifier_question);
    RUN_TEST(test_quantifier_exact);
    RUN_TEST(test_quantifier_range);

    /* Groups */
    RUN_TEST(test_group_capture);
    RUN_TEST(test_group_count);

    /* Flags */
    RUN_TEST(test_flag_ignorecase);
    RUN_TEST(test_flag_multiline);
    RUN_TEST(test_flag_global);

    /* Engine selection */
    RUN_TEST(test_engine_simple_uses_backtrack);
    RUN_TEST(test_engine_complex_uses_nfa);

    /* Fuel limit / ReDoS */
    RUN_TEST(test_fuel_limit_prevents_redos);

    /* Alternation */
    RUN_TEST(test_alternation);

    /* Start offset */
    RUN_TEST(test_start_offset);

    /* Greedy */
    RUN_TEST(test_dot_star_greedy);

    /* Escape sequences */
    RUN_TEST(test_escape_special_chars);

    /* Lazy quantifiers */
    RUN_TEST(test_lazy_star);
    RUN_TEST(test_lazy_plus);

    /* Non-capturing groups */
    RUN_TEST(test_non_capturing_group);

    /* Word boundary */
    RUN_TEST(test_word_boundary);

    /* Additional flags */
    RUN_TEST(test_flag_dotall);
    RUN_TEST(test_flag_unicode);
    RUN_TEST(test_flag_sticky);

    /* Multiple groups */
    RUN_TEST(test_multiple_groups);

    /* Empty input matching */
    RUN_TEST(test_match_empty_input);
    RUN_TEST(test_no_match_empty_input);

    /* Full anchor match */
    RUN_TEST(test_full_anchor_match);

    /* Unbounded repetition */
    RUN_TEST(test_quantifier_unbounded);

    /* Escaped characters */
    RUN_TEST(test_escape_brackets);
    RUN_TEST(test_escape_parens);

    /* Fuel limit API */
    RUN_TEST(test_fuel_limit_default);
    RUN_TEST(test_fuel_limit_set_get);

    /* Large input */
    RUN_TEST(test_large_input);

    /* Alternation in groups */
    RUN_TEST(test_alternation_in_group);

    /* Character class with special chars */
    RUN_TEST(test_char_class_hyphen_start);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_REGEXP_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[regexp]\n");
    run_regexp_tests();
    printf("\n=== regexp: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
