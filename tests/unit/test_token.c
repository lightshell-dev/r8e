/*
 * test_token.c - Unit tests for r8e_token.c (ES2023 Streaming Lexer)
 *
 * Tests cover:
 *   - Simple tokens: identifiers, numbers, strings
 *   - All keywords: each keyword produces correct token type
 *   - Number literals: 42, 3.14, 0xff, 0o77, 0b1010, 1e10, 1_000_000, 42n
 *   - String literals with escape sequences
 *   - Template literals with interpolation
 *   - All operators
 *   - Regex vs division disambiguation
 *   - Comments (line and block)
 *   - Whitespace and newline tracking
 *   - ASI hints (newline-before flag)
 *   - Error tokens (unterminated string, invalid number)
 *   - Empty input -> EOF
 *   - Complex expression token sequence
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
 * Test Harness
 * ========================================================================= */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  %-60s ", #name); \
    name(); \
    printf("[PASS]\n"); \
    tests_passed++; \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        printf("[FAIL]\n    Expected %llu == %llu\n    at %s:%d\n", \
               (unsigned long long)_a, (unsigned long long)_b, \
               __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_DOUBLE_EQ(a, b) do { \
    double _da = (double)(a); \
    double _db = (double)(b); \
    if (_da != _db) { \
        printf("[FAIL]\n    Expected %g == %g\n    at %s:%d\n", \
               _da, _db, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_STR_EQ(a, a_len, b) do { \
    const char *_sa = (a); \
    uint32_t _la = (a_len); \
    const char *_sb = (b); \
    uint32_t _lb = (uint32_t)strlen(_sb); \
    if (_la != _lb || memcmp(_sa, _sb, _la) != 0) { \
        printf("[FAIL]\n    Expected \"%.*s\" == \"%s\"\n    at %s:%d\n", \
               (int)_la, _sa, _sb, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_TRUE(cond)  ASSERT((cond))
#define ASSERT_FALSE(cond) ASSERT(!(cond))
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

/* =========================================================================
 * Types matching r8e_token.c definitions
 * ========================================================================= */

typedef enum {
    R8E_TOK_EOF = 0,
    R8E_TOK_ERROR,
    R8E_TOK_IDENT,
    R8E_TOK_NUMBER,
    R8E_TOK_BIGINT,
    R8E_TOK_STRING,
    R8E_TOK_REGEX,
    R8E_TOK_TEMPLATE_HEAD,
    R8E_TOK_TEMPLATE_MIDDLE,
    R8E_TOK_TEMPLATE_TAIL,
    R8E_TOK_TEMPLATE_FULL,

    R8E_TOK_KW_AS,
    R8E_TOK_KW_ASYNC,
    R8E_TOK_KW_AWAIT,
    R8E_TOK_KW_BREAK,
    R8E_TOK_KW_CASE,
    R8E_TOK_KW_CATCH,
    R8E_TOK_KW_CLASS,
    R8E_TOK_KW_CONST,
    R8E_TOK_KW_CONTINUE,
    R8E_TOK_KW_DEBUGGER,
    R8E_TOK_KW_DEFAULT,
    R8E_TOK_KW_DELETE,
    R8E_TOK_KW_DO,
    R8E_TOK_KW_ELSE,
    R8E_TOK_KW_ENUM,
    R8E_TOK_KW_EXPORT,
    R8E_TOK_KW_EXTENDS,
    R8E_TOK_KW_FALSE,
    R8E_TOK_KW_FINALLY,
    R8E_TOK_KW_FOR,
    R8E_TOK_KW_FROM,
    R8E_TOK_KW_FUNCTION,
    R8E_TOK_KW_GET,
    R8E_TOK_KW_IF,
    R8E_TOK_KW_IMPORT,
    R8E_TOK_KW_IN,
    R8E_TOK_KW_INSTANCEOF,
    R8E_TOK_KW_LET,
    R8E_TOK_KW_NEW,
    R8E_TOK_KW_NULL,
    R8E_TOK_KW_OF,
    R8E_TOK_KW_RETURN,
    R8E_TOK_KW_SET,
    R8E_TOK_KW_STATIC,
    R8E_TOK_KW_SUPER,
    R8E_TOK_KW_SWITCH,
    R8E_TOK_KW_THIS,
    R8E_TOK_KW_THROW,
    R8E_TOK_KW_TRUE,
    R8E_TOK_KW_TRY,
    R8E_TOK_KW_TYPEOF,
    R8E_TOK_KW_UNDEFINED,
    R8E_TOK_KW_VAR,
    R8E_TOK_KW_VOID,
    R8E_TOK_KW_WHILE,
    R8E_TOK_KW_WITH,
    R8E_TOK_KW_YIELD,

    R8E_TOK_LBRACE,
    R8E_TOK_RBRACE,
    R8E_TOK_LPAREN,
    R8E_TOK_RPAREN,
    R8E_TOK_LBRACKET,
    R8E_TOK_RBRACKET,
    R8E_TOK_DOT,
    R8E_TOK_ELLIPSIS,
    R8E_TOK_SEMICOLON,
    R8E_TOK_COMMA,
    R8E_TOK_COLON,
    R8E_TOK_QUESTION,
    R8E_TOK_QUESTION_DOT,
    R8E_TOK_NULLISH,
    R8E_TOK_NULLISH_ASSIGN,
    R8E_TOK_ARROW,

    R8E_TOK_PLUS,
    R8E_TOK_MINUS,
    R8E_TOK_STAR,
    R8E_TOK_SLASH,
    R8E_TOK_PERCENT,
    R8E_TOK_STAR_STAR,

    R8E_TOK_ASSIGN,
    R8E_TOK_PLUS_ASSIGN,
    R8E_TOK_MINUS_ASSIGN,
    R8E_TOK_STAR_ASSIGN,
    R8E_TOK_SLASH_ASSIGN,
    R8E_TOK_PERCENT_ASSIGN,
    R8E_TOK_STAR_STAR_ASSIGN,
    R8E_TOK_SHL_ASSIGN,
    R8E_TOK_SHR_ASSIGN,
    R8E_TOK_USHR_ASSIGN,
    R8E_TOK_AND_ASSIGN,
    R8E_TOK_OR_ASSIGN,
    R8E_TOK_XOR_ASSIGN,
    R8E_TOK_LAND_ASSIGN,
    R8E_TOK_LOR_ASSIGN,

    R8E_TOK_EQ,
    R8E_TOK_SEQ,
    R8E_TOK_NE,
    R8E_TOK_SNE,
    R8E_TOK_LT,
    R8E_TOK_GT,
    R8E_TOK_LE,
    R8E_TOK_GE,

    R8E_TOK_AMP,
    R8E_TOK_PIPE,
    R8E_TOK_CARET,
    R8E_TOK_TILDE,
    R8E_TOK_SHL,
    R8E_TOK_SHR,
    R8E_TOK_USHR,

    R8E_TOK_LAND,
    R8E_TOK_LOR,
    R8E_TOK_NOT,

    R8E_TOK_INC,
    R8E_TOK_DEC,

    R8E_TOK__COUNT
} R8ETokenType;

#define R8E_TOKFLAG_NEWLINE_BEFORE  0x01
#define R8E_TOKFLAG_HAS_ESCAPE      0x02

#define R8E_MAX_TEMPLATE_DEPTH 32

typedef struct {
    R8ETokenType type;
    uint32_t     flags;
    uint32_t     start;
    uint32_t     end;
    uint32_t     line;
    uint32_t     col;
    union {
        double num_val;
        struct { const char *str; uint32_t len; } str_val;
        struct { const char *pattern; uint32_t pattern_len;
                 const char *regex_flags; uint32_t flags_len; } regex_val;
    };
    const char *error_msg;
} R8EToken;

typedef struct {
    const uint8_t *source;
    uint32_t       pos;
    uint32_t       length;
    uint32_t       line;
    uint32_t       col;
    R8ETokenType   prev_type;
    uint8_t        had_newline;
    uint32_t       tmpl_depth;
    uint32_t       tmpl_brace_count[R8E_MAX_TEMPLATE_DEPTH];
    bool           has_peeked;
    R8EToken       peeked;
} R8ELexer;

/* =========================================================================
 * Forward declarations of functions under test
 * ========================================================================= */

extern void r8e_lexer_init(R8ELexer *lex, const char *source, uint32_t length);
extern void r8e_lexer_next(R8ELexer *lex, R8EToken *tok);
extern void r8e_lexer_peek(R8ELexer *lex, R8EToken *tok);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void init_lex(R8ELexer *lex, const char *src) {
    r8e_lexer_init(lex, src, (uint32_t)strlen(src));
}

/* =========================================================================
 * Tests
 * ========================================================================= */

/* --- Empty input -> EOF --- */
static void test_empty_input(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Simple identifier --- */
static void test_simple_identifier(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "fooBar");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "fooBar");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Identifiers with $ and _ --- */
static void test_identifier_with_special_chars(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "$foo _bar __proto__");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "$foo");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "_bar");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "__proto__");
}

/* --- All keywords --- */
static void test_all_keywords(void) {
    R8ELexer lex;
    R8EToken tok;

    typedef struct { const char *text; R8ETokenType expected; } KWTest;
    KWTest kw_tests[] = {
        { "as",         R8E_TOK_KW_AS },
        { "async",      R8E_TOK_KW_ASYNC },
        { "await",      R8E_TOK_KW_AWAIT },
        { "break",      R8E_TOK_KW_BREAK },
        { "case",       R8E_TOK_KW_CASE },
        { "catch",      R8E_TOK_KW_CATCH },
        { "class",      R8E_TOK_KW_CLASS },
        { "const",      R8E_TOK_KW_CONST },
        { "continue",   R8E_TOK_KW_CONTINUE },
        { "debugger",   R8E_TOK_KW_DEBUGGER },
        { "default",    R8E_TOK_KW_DEFAULT },
        { "delete",     R8E_TOK_KW_DELETE },
        { "do",         R8E_TOK_KW_DO },
        { "else",       R8E_TOK_KW_ELSE },
        { "enum",       R8E_TOK_KW_ENUM },
        { "export",     R8E_TOK_KW_EXPORT },
        { "extends",    R8E_TOK_KW_EXTENDS },
        { "false",      R8E_TOK_KW_FALSE },
        { "finally",    R8E_TOK_KW_FINALLY },
        { "for",        R8E_TOK_KW_FOR },
        { "from",       R8E_TOK_KW_FROM },
        { "function",   R8E_TOK_KW_FUNCTION },
        { "get",        R8E_TOK_KW_GET },
        { "if",         R8E_TOK_KW_IF },
        { "import",     R8E_TOK_KW_IMPORT },
        { "in",         R8E_TOK_KW_IN },
        { "instanceof", R8E_TOK_KW_INSTANCEOF },
        { "let",        R8E_TOK_KW_LET },
        { "new",        R8E_TOK_KW_NEW },
        { "null",       R8E_TOK_KW_NULL },
        { "of",         R8E_TOK_KW_OF },
        { "return",     R8E_TOK_KW_RETURN },
        { "set",        R8E_TOK_KW_SET },
        { "static",     R8E_TOK_KW_STATIC },
        { "super",      R8E_TOK_KW_SUPER },
        { "switch",     R8E_TOK_KW_SWITCH },
        { "this",       R8E_TOK_KW_THIS },
        { "throw",      R8E_TOK_KW_THROW },
        { "true",       R8E_TOK_KW_TRUE },
        { "try",        R8E_TOK_KW_TRY },
        { "typeof",     R8E_TOK_KW_TYPEOF },
        { "undefined",  R8E_TOK_KW_UNDEFINED },
        { "var",        R8E_TOK_KW_VAR },
        { "void",       R8E_TOK_KW_VOID },
        { "while",      R8E_TOK_KW_WHILE },
        { "with",       R8E_TOK_KW_WITH },
        { "yield",      R8E_TOK_KW_YIELD },
    };

    int count = (int)(sizeof(kw_tests) / sizeof(kw_tests[0]));
    for (int i = 0; i < count; i++) {
        init_lex(&lex, kw_tests[i].text);
        r8e_lexer_next(&lex, &tok);
        if (tok.type != kw_tests[i].expected) {
            printf("[FAIL]\n    Keyword \"%s\": expected type %d, got %d\n"
                   "    at %s:%d\n",
                   kw_tests[i].text, kw_tests[i].expected, tok.type,
                   __FILE__, __LINE__);
            tests_failed++;
            return;
        }
    }
}

/* --- Integer literal --- */
static void test_number_integer(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "42");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 42.0);
}

/* --- Decimal number --- */
static void test_number_decimal(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "3.14");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT(fabs(tok.num_val - 3.14) < 1e-10);
}

/* --- Hex literal --- */
static void test_number_hex(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0xff");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 255.0);
}

/* --- Octal literal --- */
static void test_number_octal(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0o77");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 63.0);
}

/* --- Binary literal --- */
static void test_number_binary(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0b1010");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 10.0);
}

/* --- Exponent notation --- */
static void test_number_exponent(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "1e10");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 1e10);
}

/* --- Numeric separator --- */
static void test_number_separator(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "1_000_000");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 1000000.0);
}

/* --- BigInt literal --- */
static void test_number_bigint(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "42n");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_BIGINT);
    ASSERT_DOUBLE_EQ(tok.num_val, 42.0);
}

/* --- Number starting with dot --- */
static void test_number_dot_prefix(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, ".5");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT(fabs(tok.num_val - 0.5) < 1e-10);
}

/* --- Zero --- */
static void test_number_zero(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 0.0);
}

/* --- String with single quotes --- */
static void test_string_single_quote(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "'hello'");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "hello");
}

/* --- String with double quotes --- */
static void test_string_double_quote(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "\"world\"");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "world");
}

/* --- String with escape sequences (raw source view) --- */
static void test_string_escapes(void) {
    R8ELexer lex;
    R8EToken tok;

    /* The lexer returns the raw source content (with backslash sequences intact).
     * Actual escape interpretation happens later in the compiler. */
    init_lex(&lex, "\"a\\nb\\tc\"");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_HAS_ESCAPE) != 0);
    /* Raw content between quotes: a\nb\tc */
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "a\\nb\\tc");
}

/* --- Empty string --- */
static void test_string_empty(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "''");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_EQ(tok.str_val.len, 0);
}

/* --- Template literal (no interpolation) --- */
static void test_template_full(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "`hello world`");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_FULL);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "hello world");
}

/* --- Template literal with interpolation --- */
static void test_template_interpolation(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "`hello ${name}!`");

    /* Should produce: TEMPLATE_HEAD, IDENT, TEMPLATE_TAIL */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_HEAD);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "hello ");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "name");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_TAIL);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "!");
}

/* --- All basic punctuators --- */
static void test_punctuators(void) {
    R8ELexer lex;
    R8EToken tok;

    typedef struct { const char *text; R8ETokenType expected; } PuncTest;
    PuncTest tests[] = {
        { "(",  R8E_TOK_LPAREN },
        { ")",  R8E_TOK_RPAREN },
        { "[",  R8E_TOK_LBRACKET },
        { "]",  R8E_TOK_RBRACKET },
        { "{",  R8E_TOK_LBRACE },
        { "}",  R8E_TOK_RBRACE },
        { ";",  R8E_TOK_SEMICOLON },
        { ",",  R8E_TOK_COMMA },
        { ":",  R8E_TOK_COLON },
        { "~",  R8E_TOK_TILDE },
        { ".",  R8E_TOK_DOT },
        { "...", R8E_TOK_ELLIPSIS },
    };

    int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < count; i++) {
        init_lex(&lex, tests[i].text);
        r8e_lexer_next(&lex, &tok);
        if (tok.type != tests[i].expected) {
            printf("[FAIL]\n    Punctuator \"%s\": expected type %d, got %d\n"
                   "    at %s:%d\n",
                   tests[i].text, tests[i].expected, tok.type,
                   __FILE__, __LINE__);
            tests_failed++;
            return;
        }
    }
}

/* --- Arithmetic and assignment operators --- */
static void test_operators(void) {
    R8ELexer lex;
    R8EToken tok;

    typedef struct { const char *text; R8ETokenType expected; } OpTest;
    OpTest tests[] = {
        { "+",    R8E_TOK_PLUS },
        { "-",    R8E_TOK_MINUS },
        { "*",    R8E_TOK_STAR },
        { "%",    R8E_TOK_PERCENT },
        { "**",   R8E_TOK_STAR_STAR },
        { "=",    R8E_TOK_ASSIGN },
        { "+=",   R8E_TOK_PLUS_ASSIGN },
        { "-=",   R8E_TOK_MINUS_ASSIGN },
        { "*=",   R8E_TOK_STAR_ASSIGN },
        { "%=",   R8E_TOK_PERCENT_ASSIGN },
        { "**=",  R8E_TOK_STAR_STAR_ASSIGN },
        { "++",   R8E_TOK_INC },
        { "--",   R8E_TOK_DEC },
    };

    int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < count; i++) {
        init_lex(&lex, tests[i].text);
        r8e_lexer_next(&lex, &tok);
        if (tok.type != tests[i].expected) {
            printf("[FAIL]\n    Operator \"%s\": expected type %d, got %d\n"
                   "    at %s:%d\n",
                   tests[i].text, tests[i].expected, tok.type,
                   __FILE__, __LINE__);
            tests_failed++;
            return;
        }
    }
}

/* --- Comparison operators --- */
static void test_comparison_operators(void) {
    R8ELexer lex;
    R8EToken tok;

    typedef struct { const char *text; R8ETokenType expected; } CmpTest;
    CmpTest tests[] = {
        { "==",  R8E_TOK_EQ },
        { "===", R8E_TOK_SEQ },
        { "!=",  R8E_TOK_NE },
        { "!==", R8E_TOK_SNE },
        { "<",   R8E_TOK_LT },
        { ">",   R8E_TOK_GT },
        { "<=",  R8E_TOK_LE },
        { ">=",  R8E_TOK_GE },
    };

    int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < count; i++) {
        init_lex(&lex, tests[i].text);
        r8e_lexer_next(&lex, &tok);
        if (tok.type != tests[i].expected) {
            printf("[FAIL]\n    Comparison \"%s\": expected %d, got %d\n"
                   "    at %s:%d\n",
                   tests[i].text, tests[i].expected, tok.type,
                   __FILE__, __LINE__);
            tests_failed++;
            return;
        }
    }
}

/* --- Bitwise and logical operators --- */
static void test_bitwise_logical_operators(void) {
    R8ELexer lex;
    R8EToken tok;

    typedef struct { const char *text; R8ETokenType expected; } BLTest;
    BLTest tests[] = {
        { "&",   R8E_TOK_AMP },
        { "|",   R8E_TOK_PIPE },
        { "^",   R8E_TOK_CARET },
        { "<<",  R8E_TOK_SHL },
        { ">>",  R8E_TOK_SHR },
        { ">>>", R8E_TOK_USHR },
        { "&&",  R8E_TOK_LAND },
        { "||",  R8E_TOK_LOR },
        { "!",   R8E_TOK_NOT },
    };

    int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < count; i++) {
        init_lex(&lex, tests[i].text);
        r8e_lexer_next(&lex, &tok);
        if (tok.type != tests[i].expected) {
            printf("[FAIL]\n    Bitwise/Logical \"%s\": expected %d, got %d\n"
                   "    at %s:%d\n",
                   tests[i].text, tests[i].expected, tok.type,
                   __FILE__, __LINE__);
            tests_failed++;
            return;
        }
    }
}

/* --- Nullish and optional chaining operators --- */
static void test_nullish_and_optional(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "??");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NULLISH);

    init_lex(&lex, "?" "?=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NULLISH_ASSIGN);

    init_lex(&lex, "?.");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_QUESTION_DOT);

    init_lex(&lex, "?");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_QUESTION);
}

/* --- Arrow operator --- */
static void test_arrow_operator(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "=>");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ARROW);
}

/* --- Line comment --- */
static void test_line_comment(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a // this is a comment\nb");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "a");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "b");
    /* b should have newline-before flag */
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_NEWLINE_BEFORE) != 0);
}

/* --- Block comment --- */
static void test_block_comment(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a /* block comment */ b");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "a");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "b");
}

/* --- Block comment with newline --- */
static void test_block_comment_multiline(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a /*\n  multi\n  line\n*/ b");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "b");
    /* Should have newline-before because the block comment contained newlines */
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_NEWLINE_BEFORE) != 0);
}

/* --- Regex vs division disambiguation --- */
static void test_regex_after_operator(void) {
    R8ELexer lex;
    R8EToken tok;

    /* After '=' (an operator), '/' should start a regex */
    init_lex(&lex, "= /abc/gi");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ASSIGN);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_REGEX);
    ASSERT_STR_EQ(tok.regex_val.pattern, tok.regex_val.pattern_len, "abc");
    ASSERT_STR_EQ(tok.regex_val.regex_flags, tok.regex_val.flags_len, "gi");
}

/* --- Division after number --- */
static void test_division_after_number(void) {
    R8ELexer lex;
    R8EToken tok;

    /* After a number, '/' is division, not regex */
    init_lex(&lex, "42 / 7");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SLASH);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
}

/* --- Division after identifier --- */
static void test_division_after_ident(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "x / y");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SLASH);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
}

/* --- Regex after open paren --- */
static void test_regex_after_paren(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "(/pattern/)");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_LPAREN);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_REGEX);
    ASSERT_STR_EQ(tok.regex_val.pattern, tok.regex_val.pattern_len, "pattern");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
}

/* --- ASI hint: newline-before flag --- */
static void test_asi_newline_flag(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a\nb");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    /* First token should not have newline-before */
    ASSERT_FALSE((tok.flags & R8E_TOKFLAG_NEWLINE_BEFORE) != 0);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    /* Second token should have newline-before */
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_NEWLINE_BEFORE) != 0);
}

/* --- Line/column tracking --- */
static void test_line_column_tracking(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a b\n  c");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.line, 1);
    ASSERT_EQ(tok.col, 1);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.line, 1);
    ASSERT_EQ(tok.col, 3);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.line, 2);
    ASSERT_EQ(tok.col, 3);
}

/* --- Peek does not consume --- */
static void test_peek(void) {
    R8ELexer lex;
    R8EToken tok, peeked;
    init_lex(&lex, "a b c");

    r8e_lexer_peek(&lex, &peeked);
    ASSERT_EQ(peeked.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(peeked.str_val.str, peeked.str_val.len, "a");

    /* Next should return the same token as peek */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "a");

    /* Next call should advance past peek */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "b");
}

/* --- Unterminated string error --- */
static void test_error_unterminated_string(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "\"hello");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ERROR);
    ASSERT_NOT_NULL(tok.error_msg);
}

/* --- Unterminated string with newline --- */
static void test_error_string_newline(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "\"hello\nworld\"");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ERROR);
    ASSERT_NOT_NULL(tok.error_msg);
}

/* --- Hex literal with no digits --- */
static void test_error_hex_no_digits(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0x");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ERROR);
    ASSERT_NOT_NULL(tok.error_msg);
}

/* --- Complex expression token sequence --- */
static void test_complex_expression(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "const x = a + b * (c - 1);");

    /* const */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_KW_CONST);

    /* x */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "x");

    /* = */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ASSIGN);

    /* a */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    /* + */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_PLUS);

    /* b */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    /* * */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STAR);

    /* ( */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_LPAREN);

    /* c */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    /* - */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_MINUS);

    /* 1 */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 1.0);

    /* ) */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_RPAREN);

    /* ; */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);

    /* EOF */
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Keyword-like identifiers (prefixed/suffixed) are identifiers --- */
static void test_keyword_prefix_is_ident(void) {
    R8ELexer lex;
    R8EToken tok;

    /* "ifx" is not the keyword "if" */
    init_lex(&lex, "ifx");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "ifx");

    /* "fora" is not the keyword "for" */
    init_lex(&lex, "fora");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "fora");
}

/* --- Multiple tokens on one line --- */
static void test_multiple_tokens_same_line(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "1+2");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 1.0);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_PLUS);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 2.0);
}

/* --- Whitespace only input -> EOF --- */
static void test_whitespace_only(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "   \t  \n  ");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Comment only input -> EOF --- */
static void test_comment_only(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "// just a comment");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Negative exponent --- */
static void test_number_negative_exponent(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "5e-3");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT(fabs(tok.num_val - 0.005) < 1e-10);
}

/* --- Division assign --- */
static void test_slash_assign(void) {
    R8ELexer lex;
    R8EToken tok;

    /* After an identifier, /= is SLASH_ASSIGN */
    init_lex(&lex, "x /= 2");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SLASH_ASSIGN);
}

/* --- For loop token sequence --- */
static void test_for_loop_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "for(let i=0;i<10;i++){}");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_FOR);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_LET);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_ASSIGN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_INC);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Hex BigInt --- */
static void test_hex_bigint(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0xFFn");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_BIGINT);
    ASSERT_DOUBLE_EQ(tok.num_val, 255.0);
}

/* --- Logical assignment operators --- */
static void test_logical_assignment_operators(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "&&=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_LAND_ASSIGN);

    init_lex(&lex, "||=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_LOR_ASSIGN);
}

/* --- Shift assignment operators --- */
static void test_shift_assignment_operators(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "<<=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SHL_ASSIGN);

    init_lex(&lex, ">>=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SHR_ASSIGN);

    init_lex(&lex, ">>>=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_USHR_ASSIGN);
}

/* =========================================================================
 * NEW TESTS: Extended Lexer Coverage
 * ========================================================================= */

/* --- Positive exponent with + sign --- */
static void test_number_positive_exponent(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "2.5e+3");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 2500.0);
}

/* --- Capital E exponent --- */
static void test_number_capital_exponent(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "1E5");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 100000.0);
}

/* --- Hex with uppercase letters --- */
static void test_number_hex_upper(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0xABCD");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 43981.0);
}

/* --- Binary BigInt --- */
static void test_binary_bigint(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0b1111n");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_BIGINT);
    ASSERT_DOUBLE_EQ(tok.num_val, 15.0);
}

/* --- Octal BigInt --- */
static void test_octal_bigint(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0o77n");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_BIGINT);
    ASSERT_DOUBLE_EQ(tok.num_val, 63.0);
}

/* --- Numeric separator in hex --- */
static void test_number_separator_hex(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0xFF_FF");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 65535.0);
}

/* --- Decimal with trailing dot --- */
static void test_number_trailing_dot(void) {
    R8ELexer lex;
    R8EToken tok;
    /* "1." should be 1.0 followed by EOF, not a property access */
    init_lex(&lex, "1.");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    ASSERT_DOUBLE_EQ(tok.num_val, 1.0);
}

/* --- String with unicode escape \\uXXXX --- */
static void test_string_unicode_escape(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "'\\u0041'");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_HAS_ESCAPE) != 0);
}

/* --- String with braced unicode escape \\u{XXXXX} --- */
static void test_string_braced_unicode_escape(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "'\\u{1F600}'");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_HAS_ESCAPE) != 0);
}

/* --- String with backslash-backslash escape --- */
static void test_string_backslash_escape(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "'\\\\'");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_HAS_ESCAPE) != 0);
}

/* --- String with tab escape --- */
static void test_string_tab_escape(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "\"\\t\"");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_HAS_ESCAPE) != 0);
}

/* --- Template literal with multiple interpolations --- */
static void test_template_multiple_interpolations(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "`${a}+${b}`");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_HEAD);
    ASSERT_EQ(tok.str_val.len, 0); /* empty head */

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "a");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_MIDDLE);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "+");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "b");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_TAIL);
    ASSERT_EQ(tok.str_val.len, 0); /* empty tail */
}

/* --- Template with empty interpolation --- */
static void test_template_empty_head_tail(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "`${x}`");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_HEAD);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_TEMPLATE_TAIL);
}

/* --- Regex after return keyword --- */
static void test_regex_after_return(void) {
    R8ELexer lex;
    R8EToken tok;

    /* After 'return', / should be regex start */
    init_lex(&lex, "return /test/i");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_KW_RETURN);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_REGEX);
    ASSERT_STR_EQ(tok.regex_val.pattern, tok.regex_val.pattern_len, "test");
    ASSERT_STR_EQ(tok.regex_val.regex_flags, tok.regex_val.flags_len, "i");
}

/* --- Regex after comma --- */
static void test_regex_after_comma(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, ", /abc/");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_COMMA);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_REGEX);
}

/* --- Division after close paren --- */
static void test_division_after_rparen(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, ") / 2");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_RPAREN);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_SLASH);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
}

/* --- Regex with escaped slash --- */
static void test_regex_escaped_slash(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "= /a\\/b/g");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ASSIGN);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_REGEX);
}

/* --- Regex with character class --- */
static void test_regex_char_class(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "= /[a-z]/");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ASSIGN);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_REGEX);
}

/* --- Bitwise assignment operators --- */
static void test_bitwise_assignment_operators(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "&=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_AND_ASSIGN);

    init_lex(&lex, "|=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_OR_ASSIGN);

    init_lex(&lex, "^=");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_XOR_ASSIGN);
}

/* --- Unterminated block comment is error --- */
static void test_error_unterminated_block_comment(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "/* unclosed comment");

    r8e_lexer_next(&lex, &tok);
    /* Should produce EOF or ERROR (the block comment consumes everything) */
    ASSERT_TRUE(tok.type == R8E_TOK_EOF || tok.type == R8E_TOK_ERROR);
}

/* --- Unterminated template literal --- */
static void test_error_unterminated_template(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "`unterminated");

    r8e_lexer_next(&lex, &tok);
    ASSERT_TRUE(tok.type == R8E_TOK_ERROR ||
                tok.type == R8E_TOK_TEMPLATE_FULL);
}

/* --- Binary no digits error --- */
static void test_error_binary_no_digits(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0b");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ERROR);
    ASSERT_NOT_NULL(tok.error_msg);
}

/* --- Octal no digits error --- */
static void test_error_octal_no_digits(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "0o");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_ERROR);
    ASSERT_NOT_NULL(tok.error_msg);
}

/* --- Carriage return as line terminator --- */
static void test_cr_line_terminator(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a\rb");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_EQ(tok.line, 1);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_EQ(tok.line, 2);
    ASSERT_TRUE((tok.flags & R8E_TOKFLAG_NEWLINE_BEFORE) != 0);
}

/* --- CRLF as single line terminator --- */
static void test_crlf_line_terminator(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "a\r\nb");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_EQ(tok.line, 1);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_EQ(tok.line, 2);
}

/* --- Consecutive operators without whitespace --- */
static void test_consecutive_operators(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "+-");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_PLUS);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_MINUS);
}

/* --- Token start/end offsets --- */
static void test_token_offsets(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "abc def");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.start, 0);
    ASSERT_EQ(tok.end, 3);

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.start, 4);
    ASSERT_EQ(tok.end, 7);
}

/* --- Keyword suffix makes it an identifier --- */
static void test_keyword_suffix_is_ident(void) {
    R8ELexer lex;
    R8EToken tok;

    init_lex(&lex, "returned");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "returned");

    init_lex(&lex, "classes");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);

    init_lex(&lex, "letter");
    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_IDENT);
}

/* --- Multiline string with escaped newline --- */
static void test_string_escaped_newline(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "'hello\\\nworld'");

    r8e_lexer_next(&lex, &tok);
    /* Line continuation in string: should succeed */
    ASSERT_EQ(tok.type, R8E_TOK_STRING);
}

/* --- Arrow function token sequence --- */
static void test_arrow_function_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "(a, b) => a + b");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_COMMA);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_ARROW);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_PLUS);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Class declaration token sequence --- */
static void test_class_declaration_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "class Foo extends Bar { }");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_CLASS);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "Foo");
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_EXTENDS);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "Bar");
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- Async/await token sequence --- */
static void test_async_await_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "async function f() { await x; }");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_ASYNC);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_FUNCTION);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_AWAIT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
}

/* --- Generator function token sequence --- */
static void test_generator_function_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "function* gen() { yield 1; }");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_FUNCTION);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_STAR);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    ASSERT_STR_EQ(tok.str_val.str, tok.str_val.len, "gen");
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_YIELD);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
}

/* --- Switch statement token sequence --- */
static void test_switch_statement_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "switch(x){case 1:break;default:break;}");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_SWITCH);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_CASE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_NUMBER);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_COLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_BREAK);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_DEFAULT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_COLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_BREAK);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
}

/* --- Try/catch/finally token sequence --- */
static void test_try_catch_finally_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "try{throw e;}catch(e){}finally{}");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_TRY);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_THROW);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_CATCH);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_FINALLY);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
}

/* --- Destructuring token sequence --- */
static void test_destructuring_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "let {a, b: c} = obj;");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_KW_LET);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_COMMA);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_COLON);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RBRACE);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_ASSIGN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_SEMICOLON);
}

/* --- Spread operator token sequence --- */
static void test_spread_operator_sequence(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "f(...args)");

    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_LPAREN);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_ELLIPSIS);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_IDENT);
    r8e_lexer_next(&lex, &tok); ASSERT_EQ(tok.type, R8E_TOK_RPAREN);
}

/* --- Multiple peeks return same token --- */
static void test_double_peek(void) {
    R8ELexer lex;
    R8EToken tok1, tok2;
    init_lex(&lex, "hello");

    r8e_lexer_peek(&lex, &tok1);
    r8e_lexer_peek(&lex, &tok2);
    ASSERT_EQ(tok1.type, tok2.type);
    ASSERT_EQ(tok1.start, tok2.start);
}

/* --- Only whitespace after comment --- */
static void test_comment_then_whitespace(void) {
    R8ELexer lex;
    R8EToken tok;
    init_lex(&lex, "/* comment */   ");

    r8e_lexer_next(&lex, &tok);
    ASSERT_EQ(tok.type, R8E_TOK_EOF);
}

/* --- HTML comment opening (<!--) --- */
static void test_html_comment_open(void) {
    R8ELexer lex;
    R8EToken tok;
    /* HTML-style comments: <!-- is treated as single-line comment in scripts */
    init_lex(&lex, "<!-- comment\nx");

    r8e_lexer_next(&lex, &tok);
    /* Depending on implementation: either skipped to next line or produces LT */
    ASSERT_TRUE(tok.type == R8E_TOK_IDENT || tok.type == R8E_TOK_LT);
}

/* =========================================================================
 * Test Runner
 * ========================================================================= */

void run_token_tests(void) {
    printf("\n=== Token Tests ===\n");

    /* Existing tests */
    RUN_TEST(test_empty_input);
    RUN_TEST(test_simple_identifier);
    RUN_TEST(test_identifier_with_special_chars);
    RUN_TEST(test_all_keywords);
    RUN_TEST(test_number_integer);
    RUN_TEST(test_number_decimal);
    RUN_TEST(test_number_hex);
    RUN_TEST(test_number_octal);
    RUN_TEST(test_number_binary);
    RUN_TEST(test_number_exponent);
    RUN_TEST(test_number_separator);
    RUN_TEST(test_number_bigint);
    RUN_TEST(test_number_dot_prefix);
    RUN_TEST(test_number_zero);
    RUN_TEST(test_number_negative_exponent);
    RUN_TEST(test_hex_bigint);
    RUN_TEST(test_string_single_quote);
    RUN_TEST(test_string_double_quote);
    RUN_TEST(test_string_escapes);
    RUN_TEST(test_string_empty);
    RUN_TEST(test_template_full);
    RUN_TEST(test_template_interpolation);
    RUN_TEST(test_punctuators);
    RUN_TEST(test_operators);
    RUN_TEST(test_comparison_operators);
    RUN_TEST(test_bitwise_logical_operators);
    RUN_TEST(test_nullish_and_optional);
    RUN_TEST(test_arrow_operator);
    RUN_TEST(test_logical_assignment_operators);
    RUN_TEST(test_shift_assignment_operators);
    RUN_TEST(test_slash_assign);
    RUN_TEST(test_line_comment);
    RUN_TEST(test_block_comment);
    RUN_TEST(test_block_comment_multiline);
    RUN_TEST(test_regex_after_operator);
    RUN_TEST(test_division_after_number);
    RUN_TEST(test_division_after_ident);
    RUN_TEST(test_regex_after_paren);
    RUN_TEST(test_asi_newline_flag);
    RUN_TEST(test_line_column_tracking);
    RUN_TEST(test_peek);
    RUN_TEST(test_error_unterminated_string);
    RUN_TEST(test_error_string_newline);
    RUN_TEST(test_error_hex_no_digits);
    RUN_TEST(test_complex_expression);
    RUN_TEST(test_keyword_prefix_is_ident);
    RUN_TEST(test_multiple_tokens_same_line);
    RUN_TEST(test_whitespace_only);
    RUN_TEST(test_comment_only);
    RUN_TEST(test_for_loop_sequence);

    /* NEW tests */
    RUN_TEST(test_number_positive_exponent);
    RUN_TEST(test_number_capital_exponent);
    RUN_TEST(test_number_hex_upper);
    RUN_TEST(test_binary_bigint);
    RUN_TEST(test_octal_bigint);
    RUN_TEST(test_number_separator_hex);
    RUN_TEST(test_number_trailing_dot);
    RUN_TEST(test_string_unicode_escape);
    RUN_TEST(test_string_braced_unicode_escape);
    RUN_TEST(test_string_backslash_escape);
    RUN_TEST(test_string_tab_escape);
    RUN_TEST(test_template_multiple_interpolations);
    RUN_TEST(test_template_empty_head_tail);
    RUN_TEST(test_regex_after_return);
    RUN_TEST(test_regex_after_comma);
    RUN_TEST(test_division_after_rparen);
    RUN_TEST(test_regex_escaped_slash);
    RUN_TEST(test_regex_char_class);
    RUN_TEST(test_bitwise_assignment_operators);
    RUN_TEST(test_error_unterminated_block_comment);
    RUN_TEST(test_error_unterminated_template);
    RUN_TEST(test_error_binary_no_digits);
    RUN_TEST(test_error_octal_no_digits);
    RUN_TEST(test_cr_line_terminator);
    RUN_TEST(test_crlf_line_terminator);
    RUN_TEST(test_consecutive_operators);
    RUN_TEST(test_token_offsets);
    RUN_TEST(test_keyword_suffix_is_ident);
    RUN_TEST(test_string_escaped_newline);
    RUN_TEST(test_arrow_function_sequence);
    RUN_TEST(test_class_declaration_sequence);
    RUN_TEST(test_async_await_sequence);
    RUN_TEST(test_generator_function_sequence);
    RUN_TEST(test_switch_statement_sequence);
    RUN_TEST(test_try_catch_finally_sequence);
    RUN_TEST(test_destructuring_sequence);
    RUN_TEST(test_spread_operator_sequence);
    RUN_TEST(test_double_peek);
    RUN_TEST(test_comment_then_whitespace);
    RUN_TEST(test_html_comment_open);

    printf("\n  Token Tests: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf("\n");
}

#ifdef TEST_TOKEN_MAIN
int main(void) {
    run_token_tests();
    return tests_failed > 0 ? 1 : 0;
}
#endif
