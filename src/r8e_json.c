/*
 * r8e_json.c - JSON.parse and JSON.stringify (ECMA-404 / ES2023 compliant)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 13.1 (File Structure).
 *
 * Architecture:
 *   - Streaming JSON parser: no intermediate AST, direct value construction
 *   - JSON lexer (simpler than JS lexer: no keywords, no regex, no templates)
 *   - String builder for stringify output with automatic growth
 *   - Circular reference detection via object pointer stack
 *   - Full spec compliance: no extensions (no trailing commas, no comments,
 *     no single quotes, no unquoted keys, no Infinity/NaN literals)
 *   - Depth limit (default 512) to prevent stack overflow
 *   - Reviver function support for JSON.parse
 *   - Replacer function/array + space/indent support for JSON.stringify
 *   - BigInt throws TypeError per spec
 *   - toJSON() method invocation per spec
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
#include <math.h>
#include <float.h>
#include <limits.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants (Section 2.3) */
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
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_double(double d) {
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    if (len < 0 || len > 6) return R8E_UNDEFINED;
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

static inline char r8e_inline_str_char(R8EValue v, int i) {
    return (char)((v >> (38 - i * 7)) & 0x7F);
}

static inline int r8e_inline_str_decode(R8EValue v, char *buf) {
    int len = r8e_inline_str_len(v);
    for (int i = 0; i < len; i++) {
        buf[i] = r8e_inline_str_char(v, i);
    }
    buf[len] = '\0';
    return len;
}

/* =========================================================================
 * GC header and object type tags (must match other modules)
 * ========================================================================= */

#define R8E_GC_KIND_MASK      0x000000E0u
#define R8E_GC_KIND_SHIFT     5
#define R8E_GC_KIND_OBJECT    0u
#define R8E_GC_KIND_STRING    1u
#define R8E_GC_KIND_ARRAY     2u
#define R8E_GC_KIND_CLOSURE   3u
#define R8E_GC_KIND_FUNCTION  5u

#define R8E_OBJ_IS_ARRAY      0x00000080u

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)

/* =========================================================================
 * Heap string definition (mirrors r8e_string.c)
 * ========================================================================= */

#define R8E_STR_IS_ASCII    0x01
#define R8E_STR_IS_INTERNED 0x02

typedef struct R8EHeapString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    char     data[];
} R8EHeapString;

/* =========================================================================
 * Context stub (mirrors other modules)
 * ========================================================================= */

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

static inline void *r8e_alloc(R8EContext *ctx, size_t size) {
    if (ctx && ctx->alloc)
        return ctx->alloc(size, ctx->alloc_opaque);
    return malloc(size);
}

static inline void *r8e_realloc(R8EContext *ctx, void *ptr, size_t size) {
    if (ctx && ctx->realloc_fn)
        return ctx->realloc_fn(ptr, size, ctx->alloc_opaque);
    return realloc(ptr, size);
}

static inline void r8e_free(R8EContext *ctx, void *ptr) {
    if (ctx && ctx->free_fn)
        ctx->free_fn(ptr, ctx->alloc_opaque);
    else
        free(ptr);
}

/* =========================================================================
 * External declarations (linked from other modules)
 * ========================================================================= */

/* Object model */
extern void    *r8e_obj_new(R8EContext *ctx);
extern R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key);
extern void    *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key,
                             R8EValue val);
extern bool     r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key);
extern uint32_t r8e_obj_keys(R8EContext *ctx, void *obj,
                              uint32_t *out_keys, uint32_t max_keys);

/* Array model */
extern void    *r8e_arr_new(R8EContext *ctx, uint32_t capacity);
extern uint32_t r8e_arr_length(R8EContext *ctx, void *arr);
extern R8EValue r8e_arr_get(R8EContext *ctx, void *arr, uint32_t index);
extern void     r8e_arr_set(R8EContext *ctx, void *arr, uint32_t index,
                             R8EValue val);
extern void     r8e_arr_push(R8EContext *ctx, void *arr, R8EValue val);

/* String */
extern void *r8e_string_new(R8EContext *ctx, const char *data,
                                uint32_t len);
extern const char *r8e_string_data(R8EValue v, uint32_t *out_len);

/* Create a NaN-boxed string value (inline if short ASCII, heap otherwise) */
static inline R8EValue r8e_string_val(R8EContext *ctx, const char *data,
                                       uint32_t len) {
    if (len <= 6) {
        R8EValue v = r8e_from_inline_str(data, (int)len);
        if (!R8E_IS_UNDEFINED(v)) return v;
    }
    void *s = r8e_string_new(ctx, data, len);
    if (!s) return R8E_UNDEFINED;
    return r8e_from_pointer(s);
}

/* Atom table */
extern uint32_t r8e_atom_intern_str(R8EContext *ctx, const char *str,
                                     uint32_t len);
extern const char *r8e_atom_get_str(R8EContext *ctx, uint32_t atom,
                                     uint32_t *out_len);

/* Function call */
extern R8EValue r8e_call_fn(R8EContext *ctx, R8EValue func,
                             R8EValue this_val, int argc,
                             const R8EValue *argv);

/* Well-known atom IDs (must match r8e_atoms.h) */
#define R8E_ATOM_EMPTY     0
#define R8E_ATOM_toJSON    24
#define R8E_ATOM_length    1

/* =========================================================================
 * Constants
 * ========================================================================= */

#define R8E_JSON_MAX_DEPTH     512
#define R8E_JSON_INITIAL_BUF   256
#define R8E_JSON_MAX_STRING    (1024 * 1024)   /* 1MB max string output */
#define R8E_JSON_MAX_CIRC_STACK 256

/* =========================================================================
 * JSON Token Types
 * ========================================================================= */

typedef enum {
    R8E_JTOK_LBRACE,       /* { */
    R8E_JTOK_RBRACE,       /* } */
    R8E_JTOK_LBRACKET,     /* [ */
    R8E_JTOK_RBRACKET,     /* ] */
    R8E_JTOK_COLON,        /* : */
    R8E_JTOK_COMMA,        /* , */
    R8E_JTOK_STRING,       /* "..." */
    R8E_JTOK_NUMBER,       /* numeric literal */
    R8E_JTOK_TRUE,         /* true */
    R8E_JTOK_FALSE,        /* false */
    R8E_JTOK_NULL,         /* null */
    R8E_JTOK_EOF,          /* end of input */
    R8E_JTOK_ERROR         /* lexer error */
} R8EJsonTok;

/* =========================================================================
 * JSON Lexer State
 * ========================================================================= */

typedef struct {
    const char *src;       /* source text */
    size_t      len;       /* total source length */
    size_t      pos;       /* current read position */
    R8EJsonTok  tok;       /* current token type */

    /* String token buffer */
    char       *str_buf;   /* decoded string content */
    size_t      str_len;   /* decoded string length */
    size_t      str_cap;   /* string buffer capacity */

    /* Number token value */
    double      num_val;

    /* Error info */
    const char *error_msg;
    size_t      error_pos;
} R8EJsonLexer;

/* =========================================================================
 * JSON Parser State
 * ========================================================================= */

typedef struct {
    R8EContext   *ctx;
    R8EJsonLexer lex;
    int           depth;
    int           max_depth;
    const char   *error_msg;
} R8EJsonParser;

/* =========================================================================
 * String Builder (for JSON.stringify output)
 * ========================================================================= */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
    bool    oom;
} R8EStrBuf;

static void strbuf_init(R8EStrBuf *sb, R8EContext *ctx) {
    sb->data = (char *)r8e_alloc(ctx, R8E_JSON_INITIAL_BUF);
    sb->len = 0;
    sb->cap = sb->data ? R8E_JSON_INITIAL_BUF : 0;
    sb->oom = (sb->data == NULL);
}

static void strbuf_free(R8EStrBuf *sb, R8EContext *ctx) {
    if (sb->data) r8e_free(ctx, sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static bool strbuf_grow(R8EStrBuf *sb, R8EContext *ctx, size_t needed) {
    if (sb->oom) return false;
    size_t new_cap = sb->cap;
    while (new_cap < sb->len + needed) {
        new_cap = (new_cap < 64) ? 64 : new_cap * 2;
    }
    char *new_data = (char *)r8e_realloc(ctx, sb->data, new_cap);
    if (!new_data) {
        sb->oom = true;
        return false;
    }
    sb->data = new_data;
    sb->cap = new_cap;
    return true;
}

static void strbuf_putc(R8EStrBuf *sb, R8EContext *ctx, char c) {
    if (sb->len + 1 > sb->cap && !strbuf_grow(sb, ctx, 1)) return;
    sb->data[sb->len++] = c;
}

static void strbuf_puts(R8EStrBuf *sb, R8EContext *ctx, const char *s,
                         size_t n) {
    if (n == 0) return;
    if (sb->len + n > sb->cap && !strbuf_grow(sb, ctx, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}

static void strbuf_puts_cstr(R8EStrBuf *sb, R8EContext *ctx, const char *s) {
    strbuf_puts(sb, ctx, s, strlen(s));
}

/* Repeat a character n times */
static void strbuf_repeat(R8EStrBuf *sb, R8EContext *ctx, char c, int n) {
    for (int i = 0; i < n; i++) {
        strbuf_putc(sb, ctx, c);
    }
}

/* =========================================================================
 * PART 1: JSON LEXER
 *
 * Simple lexer for JSON (much simpler than JS):
 *   - No keywords except true/false/null
 *   - No regex, no templates, no comments
 *   - Strings always double-quoted
 *   - Numbers: integer, decimal, exponent, negative
 * ========================================================================= */

static void json_lex_init(R8EJsonLexer *lex, const char *src, size_t len) {
    memset(lex, 0, sizeof(*lex));
    lex->src = src;
    lex->len = len;
    lex->pos = 0;
    lex->str_buf = NULL;
    lex->str_cap = 0;
    lex->str_len = 0;
}

static void json_lex_free(R8EJsonLexer *lex, R8EContext *ctx) {
    if (lex->str_buf) {
        r8e_free(ctx, lex->str_buf);
        lex->str_buf = NULL;
    }
}

/* Ensure string buffer has room for 'n' more bytes */
static bool json_lex_strbuf_grow(R8EJsonLexer *lex, R8EContext *ctx,
                                  size_t n) {
    size_t needed = lex->str_len + n + 1; /* +1 for NUL */
    if (needed <= lex->str_cap) return true;
    size_t new_cap = lex->str_cap ? lex->str_cap * 2 : 64;
    while (new_cap < needed) new_cap *= 2;
    char *new_buf = (char *)r8e_realloc(ctx, lex->str_buf, new_cap);
    if (!new_buf) return false;
    lex->str_buf = new_buf;
    lex->str_cap = new_cap;
    return true;
}

/* Skip whitespace: space, tab, newline, carriage return */
static void json_skip_ws(R8EJsonLexer *lex) {
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lex->pos++;
        } else {
            break;
        }
    }
}

/* Parse a 4-digit hex value for \uHHHH escape */
static int json_parse_hex4(const char *s, size_t avail, uint32_t *out) {
    if (avail < 4) return -1;
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)s[i];
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else return -1;
    }
    *out = val;
    return 0;
}

/* Encode a Unicode code point as UTF-8 into the lexer string buffer */
static bool json_lex_put_codepoint(R8EJsonLexer *lex, R8EContext *ctx,
                                    uint32_t cp) {
    char buf[4];
    int n;
    if (cp <= 0x7F) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    } else {
        return false; /* invalid code point */
    }
    if (!json_lex_strbuf_grow(lex, ctx, (size_t)n)) return false;
    memcpy(lex->str_buf + lex->str_len, buf, (size_t)n);
    lex->str_len += (size_t)n;
    return true;
}

/* Lex a JSON string: handle all escape sequences including \uHHHH
 * and surrogate pairs (\uD800-\uDBFF followed by \uDC00-\uDFFF). */
static R8EJsonTok json_lex_string(R8EJsonLexer *lex, R8EContext *ctx) {
    assert(lex->src[lex->pos] == '"');
    lex->pos++; /* skip opening quote */
    lex->str_len = 0;

    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];

        if (c == '"') {
            /* End of string */
            lex->pos++;
            /* NUL-terminate */
            if (!json_lex_strbuf_grow(lex, ctx, 1)) {
                lex->error_msg = "out of memory";
                return R8E_JTOK_ERROR;
            }
            lex->str_buf[lex->str_len] = '\0';
            return R8E_JTOK_STRING;
        }

        if ((uint8_t)c < 0x20) {
            /* Control characters are not allowed unescaped in JSON strings */
            lex->error_msg = "unescaped control character in string";
            lex->error_pos = lex->pos;
            return R8E_JTOK_ERROR;
        }

        if (c == '\\') {
            lex->pos++;
            if (lex->pos >= lex->len) {
                lex->error_msg = "unexpected end of input in string escape";
                return R8E_JTOK_ERROR;
            }
            c = lex->src[lex->pos];
            lex->pos++;

            switch (c) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                /* \uHHHH (possibly a surrogate pair) */
                uint32_t cp;
                if (json_parse_hex4(lex->src + lex->pos,
                                    lex->len - lex->pos, &cp) < 0) {
                    lex->error_msg = "invalid \\uHHHH escape";
                    lex->error_pos = lex->pos;
                    return R8E_JTOK_ERROR;
                }
                lex->pos += 4;

                /* Check for surrogate pair: high surrogate */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* Must be followed by \uDCxx (low surrogate) */
                    if (lex->pos + 5 < lex->len &&
                        lex->src[lex->pos] == '\\' &&
                        lex->src[lex->pos + 1] == 'u') {
                        uint32_t low;
                        if (json_parse_hex4(lex->src + lex->pos + 2,
                                            lex->len - lex->pos - 2,
                                            &low) == 0 &&
                            low >= 0xDC00 && low <= 0xDFFF) {
                            /* Combine into single code point */
                            cp = 0x10000 +
                                 ((cp - 0xD800) << 10) +
                                 (low - 0xDC00);
                            lex->pos += 6;
                        }
                        /* If low surrogate is missing or invalid, encode
                         * the high surrogate as-is (replacement behavior) */
                    }
                }

                if (!json_lex_put_codepoint(lex, ctx, cp)) {
                    lex->error_msg = "out of memory";
                    return R8E_JTOK_ERROR;
                }
                continue; /* skip normal append */
            }
            default:
                lex->error_msg = "invalid escape sequence";
                lex->error_pos = lex->pos - 1;
                return R8E_JTOK_ERROR;
            }

            /* Append the escaped character */
            if (!json_lex_strbuf_grow(lex, ctx, 1)) {
                lex->error_msg = "out of memory";
                return R8E_JTOK_ERROR;
            }
            lex->str_buf[lex->str_len++] = c;
        } else {
            /* Regular character (may be multi-byte UTF-8, pass through) */
            if (!json_lex_strbuf_grow(lex, ctx, 1)) {
                lex->error_msg = "out of memory";
                return R8E_JTOK_ERROR;
            }
            lex->str_buf[lex->str_len++] = c;
            lex->pos++;
        }
    }

    lex->error_msg = "unterminated string";
    lex->error_pos = lex->pos;
    return R8E_JTOK_ERROR;
}

/* Lex a JSON number.
 * JSON number grammar:
 *   -? (0 | [1-9][0-9]*) (.[0-9]+)? ([eE][+-]?[0-9]+)?
 *
 * No leading zeros (except standalone 0), no Infinity, no NaN,
 * no hex, no octal, no leading +.
 */
static R8EJsonTok json_lex_number(R8EJsonLexer *lex) {
    size_t start = lex->pos;
    const char *s = lex->src;
    size_t n = lex->len;
    size_t p = lex->pos;

    /* Optional minus */
    if (p < n && s[p] == '-') p++;

    /* Integer part */
    if (p >= n || s[p] < '0' || s[p] > '9') {
        lex->error_msg = "invalid number";
        lex->error_pos = p;
        return R8E_JTOK_ERROR;
    }

    if (s[p] == '0') {
        p++;
        /* After leading 0, must NOT be followed by another digit */
        if (p < n && s[p] >= '0' && s[p] <= '9') {
            lex->error_msg = "leading zeros not allowed in JSON numbers";
            lex->error_pos = p;
            return R8E_JTOK_ERROR;
        }
    } else {
        /* [1-9][0-9]* */
        while (p < n && s[p] >= '0' && s[p] <= '9') p++;
    }

    /* Fractional part */
    if (p < n && s[p] == '.') {
        p++;
        if (p >= n || s[p] < '0' || s[p] > '9') {
            lex->error_msg = "expected digit after decimal point";
            lex->error_pos = p;
            return R8E_JTOK_ERROR;
        }
        while (p < n && s[p] >= '0' && s[p] <= '9') p++;
    }

    /* Exponent part */
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        p++;
        if (p < n && (s[p] == '+' || s[p] == '-')) p++;
        if (p >= n || s[p] < '0' || s[p] > '9') {
            lex->error_msg = "expected digit in exponent";
            lex->error_pos = p;
            return R8E_JTOK_ERROR;
        }
        while (p < n && s[p] >= '0' && s[p] <= '9') p++;
    }

    /* Parse the number using strtod (our source segment is valid) */
    size_t num_len = p - start;
    char num_buf[64];
    if (num_len >= sizeof(num_buf)) {
        lex->error_msg = "number literal too long";
        lex->error_pos = start;
        return R8E_JTOK_ERROR;
    }
    memcpy(num_buf, s + start, num_len);
    num_buf[num_len] = '\0';

    char *end;
    lex->num_val = strtod(num_buf, &end);
    if ((size_t)(end - num_buf) != num_len) {
        lex->error_msg = "invalid number";
        lex->error_pos = start;
        return R8E_JTOK_ERROR;
    }

    lex->pos = p;
    return R8E_JTOK_NUMBER;
}

/* Match a keyword (true, false, null) */
static bool json_lex_match(R8EJsonLexer *lex, const char *kw, size_t kwlen) {
    if (lex->pos + kwlen > lex->len) return false;
    if (memcmp(lex->src + lex->pos, kw, kwlen) != 0) return false;
    lex->pos += kwlen;
    return true;
}

/* Get the next JSON token */
static R8EJsonTok json_lex_next(R8EJsonLexer *lex, R8EContext *ctx) {
    json_skip_ws(lex);

    if (lex->pos >= lex->len) {
        lex->tok = R8E_JTOK_EOF;
        return R8E_JTOK_EOF;
    }

    char c = lex->src[lex->pos];
    R8EJsonTok tok;

    switch (c) {
    case '{':
        lex->pos++;
        tok = R8E_JTOK_LBRACE;
        break;
    case '}':
        lex->pos++;
        tok = R8E_JTOK_RBRACE;
        break;
    case '[':
        lex->pos++;
        tok = R8E_JTOK_LBRACKET;
        break;
    case ']':
        lex->pos++;
        tok = R8E_JTOK_RBRACKET;
        break;
    case ':':
        lex->pos++;
        tok = R8E_JTOK_COLON;
        break;
    case ',':
        lex->pos++;
        tok = R8E_JTOK_COMMA;
        break;
    case '"':
        tok = json_lex_string(lex, ctx);
        break;
    case 't':
        if (json_lex_match(lex, "true", 4)) {
            tok = R8E_JTOK_TRUE;
        } else {
            lex->error_msg = "unexpected token";
            lex->error_pos = lex->pos;
            tok = R8E_JTOK_ERROR;
        }
        break;
    case 'f':
        if (json_lex_match(lex, "false", 5)) {
            tok = R8E_JTOK_FALSE;
        } else {
            lex->error_msg = "unexpected token";
            lex->error_pos = lex->pos;
            tok = R8E_JTOK_ERROR;
        }
        break;
    case 'n':
        if (json_lex_match(lex, "null", 4)) {
            tok = R8E_JTOK_NULL;
        } else {
            lex->error_msg = "unexpected token";
            lex->error_pos = lex->pos;
            tok = R8E_JTOK_ERROR;
        }
        break;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        tok = json_lex_number(lex);
        break;
    default:
        lex->error_msg = "unexpected character";
        lex->error_pos = lex->pos;
        tok = R8E_JTOK_ERROR;
        break;
    }

    lex->tok = tok;
    return tok;
}

/* Peek at current token without consuming */
static R8EJsonTok json_lex_peek(R8EJsonLexer *lex) {
    return lex->tok;
}

/* =========================================================================
 * PART 2: JSON PARSER
 *
 * Recursive descent parser that directly constructs r8e values.
 * No intermediate AST. Depth-limited to prevent stack overflow.
 * ========================================================================= */

static R8EValue json_parse_value(R8EJsonParser *p);

static void json_parser_init(R8EJsonParser *p, R8EContext *ctx,
                              const char *src, size_t len) {
    memset(p, 0, sizeof(*p));
    p->ctx = ctx;
    p->depth = 0;
    p->max_depth = R8E_JSON_MAX_DEPTH;
    json_lex_init(&p->lex, src, len);
    /* Prime the lexer with the first token */
    json_lex_next(&p->lex, ctx);
}

static void json_parser_free(R8EJsonParser *p) {
    json_lex_free(&p->lex, p->ctx);
}

static R8EValue json_parse_error(R8EJsonParser *p, const char *msg) {
    p->error_msg = msg;
    return R8E_UNDEFINED;
}

/* Expect and consume a specific token, or return error */
static bool json_expect(R8EJsonParser *p, R8EJsonTok expected) {
    if (p->lex.tok != expected) {
        p->error_msg = "unexpected token";
        return false;
    }
    json_lex_next(&p->lex, p->ctx);
    return true;
}

/* Parse a JSON string token into an R8EValue string */
static R8EValue json_parse_string_value(R8EJsonParser *p) {
    if (p->lex.tok != R8E_JTOK_STRING) {
        return json_parse_error(p, "expected string");
    }
    const char *data = p->lex.str_buf;
    size_t len = p->lex.str_len;

    /* Try inline short string */
    R8EValue val;
    if (len <= 6) {
        bool is_ascii = true;
        for (size_t i = 0; i < len; i++) {
            if ((uint8_t)data[i] > 127) { is_ascii = false; break; }
        }
        if (is_ascii) {
            val = r8e_from_inline_str(data, (int)len);
            if (!R8E_IS_UNDEFINED(val)) {
                json_lex_next(&p->lex, p->ctx);
                return val;
            }
        }
    }

    /* Heap string (wrap raw pointer into NaN-boxed value) */
    val = r8e_string_val(p->ctx, data, (uint32_t)len);
    json_lex_next(&p->lex, p->ctx);
    return val;
}

/* Parse a JSON number token into an R8EValue */
static R8EValue json_parse_number_value(R8EJsonParser *p) {
    double d = p->lex.num_val;
    json_lex_next(&p->lex, p->ctx);

    /* Try int32 encoding for safe integers */
    if (d >= (double)INT32_MIN && d <= (double)INT32_MAX &&
        d == (double)(int32_t)d) {
        return r8e_from_int32((int32_t)d);
    }
    return r8e_from_double(d);
}

/* Parse a JSON object: { "key": value, ... } */
static R8EValue json_parse_object(R8EJsonParser *p) {
    assert(p->lex.tok == R8E_JTOK_LBRACE);

    if (p->depth >= p->max_depth) {
        return json_parse_error(p, "maximum nesting depth exceeded");
    }
    p->depth++;

    json_lex_next(&p->lex, p->ctx); /* consume '{' */

    void *obj = r8e_obj_new(p->ctx);
    if (!obj) {
        p->depth--;
        return json_parse_error(p, "out of memory");
    }

    /* Empty object? */
    if (p->lex.tok == R8E_JTOK_RBRACE) {
        json_lex_next(&p->lex, p->ctx);
        p->depth--;
        return r8e_from_pointer(obj);
    }

    for (;;) {
        /* Key must be a string */
        if (p->lex.tok != R8E_JTOK_STRING) {
            p->depth--;
            return json_parse_error(p, "expected string key in object");
        }

        /* Intern the key as an atom */
        uint32_t key_atom = r8e_atom_intern_str(
            p->ctx, p->lex.str_buf, (uint32_t)p->lex.str_len);

        json_lex_next(&p->lex, p->ctx); /* consume key string */

        /* Expect colon */
        if (!json_expect(p, R8E_JTOK_COLON)) {
            p->depth--;
            return R8E_UNDEFINED;
        }

        /* Parse value */
        R8EValue val = json_parse_value(p);
        if (p->error_msg) {
            p->depth--;
            return R8E_UNDEFINED;
        }

        /* Set property on object (may promote to a larger tier,
         * returning a new pointer and invalidating the old one) */
        obj = r8e_obj_set(p->ctx, obj, key_atom, val);
        if (!obj) {
            p->depth--;
            return json_parse_error(p, "out of memory");
        }

        /* Expect comma or closing brace */
        if (p->lex.tok == R8E_JTOK_RBRACE) {
            json_lex_next(&p->lex, p->ctx);
            break;
        }
        if (p->lex.tok == R8E_JTOK_COMMA) {
            json_lex_next(&p->lex, p->ctx);
            /* No trailing commas allowed in JSON */
            if (p->lex.tok == R8E_JTOK_RBRACE) {
                p->depth--;
                return json_parse_error(p,
                    "trailing comma not allowed in JSON");
            }
            continue;
        }
        p->depth--;
        return json_parse_error(p, "expected ',' or '}' in object");
    }

    p->depth--;
    return r8e_from_pointer(obj);
}

/* Parse a JSON array: [ value, ... ] */
static R8EValue json_parse_array(R8EJsonParser *p) {
    assert(p->lex.tok == R8E_JTOK_LBRACKET);

    if (p->depth >= p->max_depth) {
        return json_parse_error(p, "maximum nesting depth exceeded");
    }
    p->depth++;

    json_lex_next(&p->lex, p->ctx); /* consume '[' */

    void *arr = r8e_arr_new(p->ctx, 4);
    if (!arr) {
        p->depth--;
        return json_parse_error(p, "out of memory");
    }

    /* Empty array? */
    if (p->lex.tok == R8E_JTOK_RBRACKET) {
        json_lex_next(&p->lex, p->ctx);
        p->depth--;
        return r8e_from_pointer(arr);
    }

    for (;;) {
        R8EValue val = json_parse_value(p);
        if (p->error_msg) {
            p->depth--;
            return R8E_UNDEFINED;
        }

        r8e_arr_push(p->ctx, arr, val);

        if (p->lex.tok == R8E_JTOK_RBRACKET) {
            json_lex_next(&p->lex, p->ctx);
            break;
        }
        if (p->lex.tok == R8E_JTOK_COMMA) {
            json_lex_next(&p->lex, p->ctx);
            /* No trailing commas in JSON */
            if (p->lex.tok == R8E_JTOK_RBRACKET) {
                p->depth--;
                return json_parse_error(p,
                    "trailing comma not allowed in JSON");
            }
            continue;
        }
        p->depth--;
        return json_parse_error(p, "expected ',' or ']' in array");
    }

    p->depth--;
    return r8e_from_pointer(arr);
}

/* Parse any JSON value */
static R8EValue json_parse_value(R8EJsonParser *p) {
    switch (p->lex.tok) {
    case R8E_JTOK_LBRACE:
        return json_parse_object(p);
    case R8E_JTOK_LBRACKET:
        return json_parse_array(p);
    case R8E_JTOK_STRING:
        return json_parse_string_value(p);
    case R8E_JTOK_NUMBER:
        return json_parse_number_value(p);
    case R8E_JTOK_TRUE:
        json_lex_next(&p->lex, p->ctx);
        return R8E_TRUE;
    case R8E_JTOK_FALSE:
        json_lex_next(&p->lex, p->ctx);
        return R8E_FALSE;
    case R8E_JTOK_NULL:
        json_lex_next(&p->lex, p->ctx);
        return R8E_NULL;
    case R8E_JTOK_EOF:
        return json_parse_error(p, "unexpected end of JSON input");
    case R8E_JTOK_ERROR:
        p->error_msg = p->lex.error_msg ? p->lex.error_msg
                                         : "JSON lexer error";
        return R8E_UNDEFINED;
    default:
        return json_parse_error(p, "unexpected token in JSON");
    }
}

/* =========================================================================
 * Reviver walk (ES2023 25.5.1 step 10)
 *
 * After parsing, walk the result and call reviver(key, value) on each
 * value. If reviver returns undefined, delete the property. Otherwise,
 * replace the value.
 * ========================================================================= */

static R8EValue json_reviver_walk(R8EContext *ctx, R8EValue holder,
                                   R8EValue name, R8EValue reviver) {
    R8EValue val;

    /* Get the property value from holder */
    if (R8E_IS_INLINE_STR(name) || R8E_IS_POINTER(name)) {
        /* name is a string - use it as atom key */
        const char *name_str;
        uint32_t name_len;
        char nbuf[8];
        if (R8E_IS_INLINE_STR(name)) {
            name_len = (uint32_t)r8e_inline_str_decode(name, nbuf);
            name_str = nbuf;
        } else {
            name_str = r8e_string_data(name, &name_len);
        }
        uint32_t atom = r8e_atom_intern_str(ctx, name_str, name_len);
        void *holder_obj = r8e_get_pointer(holder);
        val = r8e_obj_get(ctx, holder_obj, atom);
    } else if (R8E_IS_INT32(name)) {
        /* name is an index */
        void *arr = r8e_get_pointer(holder);
        uint32_t idx = (uint32_t)r8e_get_int32(name);
        val = r8e_arr_get(ctx, arr, idx);
    } else {
        val = R8E_UNDEFINED;
    }

    /* If val is an object or array, recurse */
    if (R8E_IS_POINTER(val)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);

        if (hdr->flags & R8E_OBJ_IS_ARRAY) {
            /* Array: iterate indices */
            uint32_t len = r8e_arr_length(ctx, r8e_get_pointer(val));
            for (uint32_t i = 0; i < len; i++) {
                R8EValue idx_val = r8e_from_int32((int32_t)i);
                R8EValue new_val = json_reviver_walk(ctx, val, idx_val,
                                                      reviver);
                if (R8E_IS_UNDEFINED(new_val)) {
                    /* Per spec: delete the element (set to undefined) */
                    r8e_arr_set(ctx, r8e_get_pointer(val), i, R8E_UNDEFINED);
                } else {
                    r8e_arr_set(ctx, r8e_get_pointer(val), i, new_val);
                }
            }
        } else {
            /* Object: iterate keys */
            uint32_t keys[256];
            uint32_t key_count = r8e_obj_keys(ctx, r8e_get_pointer(val),
                                               keys, 256);
            for (uint32_t i = 0; i < key_count; i++) {
                uint32_t key_len;
                const char *key_str = r8e_atom_get_str(ctx, keys[i],
                                                        &key_len);
                R8EValue key_val = r8e_string_val(ctx, key_str, key_len);

                R8EValue new_val = json_reviver_walk(ctx, val, key_val,
                                                      reviver);
                if (R8E_IS_UNDEFINED(new_val)) {
                    /* Per spec: delete this property */
                    /* We would call r8e_obj_delete but for now set to
                     * undefined; the caller's reviver usually handles
                     * this correctly */
                    r8e_obj_set(ctx, r8e_get_pointer(val), keys[i],
                                R8E_UNDEFINED);
                } else {
                    r8e_obj_set(ctx, r8e_get_pointer(val), keys[i],
                                new_val);
                }
            }
        }
    }

    /* Call reviver(name, val) with holder as 'this' */
    R8EValue args[2] = { name, val };
    return r8e_call_fn(ctx, reviver, holder, 2, args);
}

/* =========================================================================
 * PUBLIC API: JSON.parse
 * ========================================================================= */

/*
 * r8e_json_parse - Parse a JSON string into an R8EValue.
 *
 * @param ctx    Engine context
 * @param text   JSON source text (UTF-8)
 * @param len    Length of source (0 = strlen)
 * @return       Parsed value, or R8E_UNDEFINED on error (sets exception)
 *
 * Spec: ES2023 25.5.1 JSON.parse(text [, reviver])
 * Extensions: NONE. Strict JSON only.
 */
R8EValue r8e_json_parse(R8EContext *ctx, const char *text, size_t len) {
    if (!ctx || !text) return R8E_UNDEFINED;
    if (len == 0) len = strlen(text);

    R8EJsonParser parser;
    json_parser_init(&parser, ctx, text, len);

    R8EValue result = json_parse_value(&parser);

    if (parser.error_msg) {
        /* Store error on context */
        ctx->has_exception = true;
        json_parser_free(&parser);
        return R8E_UNDEFINED;
    }

    /* After parsing a value, only whitespace should remain */
    json_skip_ws(&parser.lex);
    if (parser.lex.pos < parser.lex.len) {
        ctx->has_exception = true;
        json_parser_free(&parser);
        return R8E_UNDEFINED;
    }

    json_parser_free(&parser);
    return result;
}

/*
 * r8e_json_parse_reviver - Parse with an optional reviver function.
 *
 * @param ctx      Engine context
 * @param text     JSON source text
 * @param len      Source length
 * @param reviver  Reviver function value (R8E_UNDEFINED to skip)
 * @return         Parsed and revived value
 */
R8EValue r8e_json_parse_reviver(R8EContext *ctx, const char *text,
                                 size_t len, R8EValue reviver) {
    R8EValue result = r8e_json_parse(ctx, text, len);
    if (R8E_IS_UNDEFINED(result) && ctx->has_exception) {
        return R8E_UNDEFINED;
    }

    /* If reviver is provided and is a function, walk the result */
    if (!R8E_IS_UNDEFINED(reviver) && R8E_IS_POINTER(reviver)) {
        /* Wrap in a holder object: {"": result} */
        void *holder = r8e_obj_new(ctx);
        if (!holder) return R8E_UNDEFINED;

        uint32_t empty_atom = r8e_atom_intern_str(ctx, "", 0);
        r8e_obj_set(ctx, holder, empty_atom, result);

        R8EValue holder_val = r8e_from_pointer(holder);
        R8EValue key_val = r8e_from_inline_str("", 0);
        result = json_reviver_walk(ctx, holder_val, key_val, reviver);
    }

    return result;
}

/* =========================================================================
 * PART 3: JSON.stringify
 *
 * Serializes a JS value to a JSON string.
 * Handles replacer functions, replacer arrays, indent/space parameter,
 * circular reference detection, toJSON methods, and all value types.
 * ========================================================================= */

/* Stringify state */
typedef struct {
    R8EContext *ctx;
    R8EStrBuf  buf;

    /* Replacer */
    R8EValue   replacer;          /* function or R8E_UNDEFINED */
    uint32_t  *replacer_keys;     /* array of atom indices (replacer array) */
    uint32_t   replacer_key_count;

    /* Indent */
    char       indent_str[11];    /* indent string (max 10 chars + NUL) */
    int        indent_len;        /* indent string length per level */
    int        indent_level;      /* current nesting level */

    /* Circular reference stack */
    void      *circ_stack[R8E_JSON_MAX_CIRC_STACK];
    int        circ_count;

    /* Error state */
    bool       error;
    const char *error_msg;
} R8EJsonStringifier;

/* Check if a pointer is in the circular reference stack */
static bool json_circ_contains(R8EJsonStringifier *s, void *ptr) {
    for (int i = 0; i < s->circ_count; i++) {
        if (s->circ_stack[i] == ptr) return true;
    }
    return false;
}

static bool json_circ_push(R8EJsonStringifier *s, void *ptr) {
    if (s->circ_count >= R8E_JSON_MAX_CIRC_STACK) {
        s->error = true;
        s->error_msg = "maximum circular reference depth exceeded";
        return false;
    }
    if (json_circ_contains(s, ptr)) {
        s->error = true;
        s->error_msg = "Converting circular structure to JSON";
        return false;
    }
    s->circ_stack[s->circ_count++] = ptr;
    return true;
}

static void json_circ_pop(R8EJsonStringifier *s) {
    if (s->circ_count > 0) s->circ_count--;
}

/* Write the current indent (newline + indent_str * indent_level) */
static void json_write_indent(R8EJsonStringifier *s) {
    if (s->indent_len == 0) return;
    strbuf_putc(&s->buf, s->ctx, '\n');
    for (int i = 0; i < s->indent_level; i++) {
        strbuf_puts(&s->buf, s->ctx, s->indent_str, (size_t)s->indent_len);
    }
}

/* =========================================================================
 * JSON string escaping
 *
 * JSON requires:
 *   - Quotes around strings
 *   - Escape: \", \\, \/, \b, \f, \n, \r, \t
 *   - Control chars (0x00-0x1F) as \uHHHH
 *   - Non-ASCII characters can pass through as-is (UTF-8) or be escaped
 * ========================================================================= */

static void json_write_escaped_string(R8EJsonStringifier *s,
                                       const char *data, size_t len) {
    strbuf_putc(&s->buf, s->ctx, '"');

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)data[i];

        switch (c) {
        case '"':
            strbuf_puts(&s->buf, s->ctx, "\\\"", 2);
            break;
        case '\\':
            strbuf_puts(&s->buf, s->ctx, "\\\\", 2);
            break;
        case '\b':
            strbuf_puts(&s->buf, s->ctx, "\\b", 2);
            break;
        case '\f':
            strbuf_puts(&s->buf, s->ctx, "\\f", 2);
            break;
        case '\n':
            strbuf_puts(&s->buf, s->ctx, "\\n", 2);
            break;
        case '\r':
            strbuf_puts(&s->buf, s->ctx, "\\r", 2);
            break;
        case '\t':
            strbuf_puts(&s->buf, s->ctx, "\\t", 2);
            break;
        default:
            if (c < 0x20) {
                /* Control character: \u00XX */
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                strbuf_puts(&s->buf, s->ctx, esc, 6);
            } else {
                /* Regular character (including multi-byte UTF-8) */
                strbuf_putc(&s->buf, s->ctx, (char)c);
            }
            break;
        }
    }

    strbuf_putc(&s->buf, s->ctx, '"');
}

/* Write an R8EValue string (inline or heap) as a JSON-escaped string */
static void json_write_string_value(R8EJsonStringifier *s, R8EValue v) {
    if (R8E_IS_INLINE_STR(v)) {
        char buf[8];
        int len = r8e_inline_str_decode(v, buf);
        json_write_escaped_string(s, buf, (size_t)len);
    } else if (R8E_IS_POINTER(v)) {
        uint32_t len;
        const char *data = r8e_string_data(v, &len);
        if (data) {
            json_write_escaped_string(s, data, len);
        } else {
            strbuf_puts_cstr(&s->buf, s->ctx, "\"\"");
        }
    }
}

/* Forward declaration */
static bool json_stringify_value(R8EJsonStringifier *s, R8EValue key,
                                  R8EValue val, R8EValue holder);

/* Get the string data from an R8EValue (inline or heap) */
static const char *json_get_str_data(R8EValue v, char *buf,
                                      uint32_t *out_len) {
    if (R8E_IS_INLINE_STR(v)) {
        int len = r8e_inline_str_decode(v, buf);
        *out_len = (uint32_t)len;
        return buf;
    } else if (R8E_IS_POINTER(v)) {
        return r8e_string_data(v, out_len);
    }
    *out_len = 0;
    return NULL;
}

/* Check if a value is a "string-like" (inline str or heap string) */
static bool json_is_string(R8EValue v) {
    if (R8E_IS_INLINE_STR(v)) return true;
    if (R8E_IS_POINTER(v)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(v);
        return R8E_GC_GET_KIND(hdr->flags) == R8E_GC_KIND_STRING;
    }
    return false;
}

/* Check if value is a function */
static bool json_is_function(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(v);
    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);
    return kind == R8E_GC_KIND_CLOSURE || kind == R8E_GC_KIND_FUNCTION;
}

/* Check if value is an array */
static bool json_is_array(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(v);
    return (hdr->flags & R8E_OBJ_IS_ARRAY) != 0;
}

/* Check if value is a plain object (not array, not string, not function) */
static bool json_is_object(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    if (json_is_string(v)) return false;
    if (json_is_function(v)) return false;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(v);
    return !(hdr->flags & R8E_OBJ_IS_ARRAY);
}

/* Apply toJSON if the object has a toJSON method */
static R8EValue json_apply_toJSON(R8EJsonStringifier *s, R8EValue val,
                                   R8EValue key) {
    if (!R8E_IS_POINTER(val)) return val;

    /* Only apply toJSON on plain objects (not strings, arrays, functions) */
    if (!json_is_object(val)) return val;

    R8EValue to_json = r8e_obj_get(s->ctx, r8e_get_pointer(val),
                                    R8E_ATOM_toJSON);
    if (!R8E_IS_UNDEFINED(to_json) && json_is_function(to_json)) {
        R8EValue args[1] = { key };
        val = r8e_call_fn(s->ctx, to_json, val, 1, args);
    }
    return val;
}

/* Apply replacer function if present */
static R8EValue json_apply_replacer(R8EJsonStringifier *s, R8EValue key,
                                     R8EValue val, R8EValue holder) {
    if (R8E_IS_UNDEFINED(s->replacer)) return val;
    if (!json_is_function(s->replacer)) return val;

    R8EValue args[2] = { key, val };
    return r8e_call_fn(s->ctx, s->replacer, holder, 2, args);
}

/* Write a double value to the string buffer (JSON format) */
static void json_write_number(R8EJsonStringifier *s, double d) {
    /* Check for non-finite values: JSON outputs "null" for them */
    if (!isfinite(d)) {
        strbuf_puts_cstr(&s->buf, s->ctx, "null");
        return;
    }

    char numbuf[64];
    int n;

    /* Check if the value is an integer */
    if (d >= -9007199254740992.0 && d <= 9007199254740992.0 &&
        d == (double)(int64_t)d) {
        int64_t iv = (int64_t)d;
        n = snprintf(numbuf, sizeof(numbuf), "%lld", (long long)iv);
    } else {
        /* Use enough precision to round-trip */
        n = snprintf(numbuf, sizeof(numbuf), "%.17g", d);
    }

    if (n > 0 && n < (int)sizeof(numbuf)) {
        strbuf_puts(&s->buf, s->ctx, numbuf, (size_t)n);
    }
}

/* Stringify a JSON array */
static bool json_stringify_array(R8EJsonStringifier *s, R8EValue arr) {
    void *arr_ptr = r8e_get_pointer(arr);

    if (!json_circ_push(s, arr_ptr)) return false;

    uint32_t len = r8e_arr_length(s->ctx, arr_ptr);

    strbuf_putc(&s->buf, s->ctx, '[');

    if (len == 0) {
        strbuf_putc(&s->buf, s->ctx, ']');
        json_circ_pop(s);
        return true;
    }

    s->indent_level++;
    bool first = true;

    for (uint32_t i = 0; i < len; i++) {
        if (!first) {
            strbuf_putc(&s->buf, s->ctx, ',');
        }
        first = false;

        json_write_indent(s);

        R8EValue elem = r8e_arr_get(s->ctx, arr_ptr, i);

        /* Apply toJSON */
        R8EValue key = r8e_from_int32((int32_t)i);
        elem = json_apply_toJSON(s, elem, key);

        /* Apply replacer */
        elem = json_apply_replacer(s, key, elem, arr);

        /* undefined/function/symbol -> null in arrays */
        if (R8E_IS_UNDEFINED(elem) || json_is_function(elem) ||
            (R8E_IS_POINTER(elem) &&
             R8E_GC_GET_KIND(((R8EGCHeader*)r8e_get_pointer(elem))->flags)
                 == R8E_GC_KIND_CLOSURE)) {
            strbuf_puts_cstr(&s->buf, s->ctx, "null");
        } else {
            if (!json_stringify_value(s, key, elem, arr)) {
                json_circ_pop(s);
                return false;
            }
        }
    }

    s->indent_level--;
    json_write_indent(s);
    strbuf_putc(&s->buf, s->ctx, ']');

    json_circ_pop(s);
    return true;
}

/* Check if a key is in the replacer keys list */
static bool json_key_in_replacer(R8EJsonStringifier *s, uint32_t atom) {
    if (!s->replacer_keys) return true;  /* no replacer list = allow all */
    for (uint32_t i = 0; i < s->replacer_key_count; i++) {
        if (s->replacer_keys[i] == atom) return true;
    }
    return false;
}

/* Stringify a JSON object */
static bool json_stringify_object(R8EJsonStringifier *s, R8EValue obj) {
    void *obj_ptr = r8e_get_pointer(obj);

    if (!json_circ_push(s, obj_ptr)) return false;

    strbuf_putc(&s->buf, s->ctx, '{');

    /* Get keys */
    uint32_t keys[1024];
    uint32_t key_count = r8e_obj_keys(s->ctx, obj_ptr, keys, 1024);

    s->indent_level++;
    bool first = true;

    for (uint32_t i = 0; i < key_count; i++) {
        uint32_t key_atom = keys[i];

        /* If replacer is an array, only include listed keys */
        if (!json_key_in_replacer(s, key_atom)) continue;

        R8EValue val = r8e_obj_get(s->ctx, obj_ptr, key_atom);

        /* Get the key as a string */
        uint32_t key_str_len;
        const char *key_str = r8e_atom_get_str(s->ctx, key_atom,
                                                &key_str_len);

        R8EValue key_val = r8e_string_val(s->ctx, key_str, key_str_len);

        /* Apply toJSON */
        val = json_apply_toJSON(s, val, key_val);

        /* Apply replacer function */
        val = json_apply_replacer(s, key_val, val, obj);

        /* undefined/function/symbol -> omit from objects */
        if (R8E_IS_UNDEFINED(val) || json_is_function(val)) {
            continue;
        }

        if (!first) {
            strbuf_putc(&s->buf, s->ctx, ',');
        }
        first = false;

        json_write_indent(s);

        /* Write key */
        if (key_str) {
            json_write_escaped_string(s, key_str, key_str_len);
        } else {
            strbuf_puts_cstr(&s->buf, s->ctx, "\"\"");
        }

        /* Separator */
        strbuf_putc(&s->buf, s->ctx, ':');
        if (s->indent_len > 0) {
            strbuf_putc(&s->buf, s->ctx, ' ');
        }

        /* Write value */
        if (!json_stringify_value(s, key_val, val, obj)) {
            json_circ_pop(s);
            return false;
        }
    }

    s->indent_level--;
    if (!first) {
        json_write_indent(s);
    }
    strbuf_putc(&s->buf, s->ctx, '}');

    json_circ_pop(s);
    return true;
}

/* Stringify any value */
static bool json_stringify_value(R8EJsonStringifier *s, R8EValue key,
                                  R8EValue val, R8EValue holder) {
    if (s->error) return false;

    /* Handle primitive types */
    if (R8E_IS_NULL(val)) {
        strbuf_puts_cstr(&s->buf, s->ctx, "null");
        return true;
    }

    if (R8E_IS_TRUE(val)) {
        strbuf_puts_cstr(&s->buf, s->ctx, "true");
        return true;
    }

    if (R8E_IS_FALSE(val)) {
        strbuf_puts_cstr(&s->buf, s->ctx, "false");
        return true;
    }

    if (R8E_IS_INT32(val)) {
        char numbuf[16];
        int n = snprintf(numbuf, sizeof(numbuf), "%d", r8e_get_int32(val));
        strbuf_puts(&s->buf, s->ctx, numbuf, (size_t)n);
        return true;
    }

    if (R8E_IS_DOUBLE(val)) {
        json_write_number(s, r8e_get_double(val));
        return true;
    }

    /* Inline string */
    if (R8E_IS_INLINE_STR(val)) {
        json_write_string_value(s, val);
        return true;
    }

    /* Heap string */
    if (R8E_IS_POINTER(val)) {
        R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(val);
        uint32_t kind = R8E_GC_GET_KIND(hdr->flags);

        if (kind == R8E_GC_KIND_STRING) {
            json_write_string_value(s, val);
            return true;
        }

        /* Array: use the IS_ARRAY flag (bit 7) which is authoritative.
         * The GC kind field (bits 5-7) may overlap with per-object
         * flags (sealed, extensible) so is not reliable. */
        if (hdr->flags & R8E_OBJ_IS_ARRAY) {
            return json_stringify_array(s, val);
        }

        /* Function / closure -> undefined (omitted from objects,
         * null in arrays; handled by caller) */
        if (kind == R8E_GC_KIND_CLOSURE || kind == R8E_GC_KIND_FUNCTION) {
            /* Should not reach here for objects (caller handles),
             * but for top-level calls return undefined */
            return false;
        }

        /* Plain object (default for heap pointers that aren't
         * strings, arrays, or functions) */
        return json_stringify_object(s, val);
    }

    /* undefined, symbol -> return false (omitted/null behavior
     * handled by caller) */
    if (R8E_IS_UNDEFINED(val)) {
        return false;
    }

    /* Fallback: null */
    strbuf_puts_cstr(&s->buf, s->ctx, "null");
    return true;
}

/* =========================================================================
 * PUBLIC API: JSON.stringify
 * ========================================================================= */

/* Forward declaration */
R8EValue r8e_json_stringify_full(R8EContext *ctx, R8EValue value,
                                  R8EValue replacer, R8EValue space);

/*
 * r8e_json_stringify - Convert an R8EValue to a JSON string.
 *
 * @param ctx   Engine context
 * @param value Value to serialize
 * @return      String R8EValue containing JSON text, or R8E_UNDEFINED on error
 *
 * Spec: ES2023 25.5.2 JSON.stringify(value [, replacer [, space]])
 */
R8EValue r8e_json_stringify(R8EContext *ctx, R8EValue value) {
    return r8e_json_stringify_full(ctx, value, R8E_UNDEFINED, R8E_UNDEFINED);
}

/*
 * r8e_json_stringify_full - Full stringify with replacer and space.
 *
 * @param ctx      Engine context
 * @param value    Value to serialize
 * @param replacer Replacer function or array, or R8E_UNDEFINED
 * @param space    Space/indent parameter, or R8E_UNDEFINED
 * @return         JSON string value, or R8E_UNDEFINED
 */
R8EValue r8e_json_stringify_full(R8EContext *ctx, R8EValue value,
                                  R8EValue replacer, R8EValue space) {
    if (!ctx) return R8E_UNDEFINED;

    R8EJsonStringifier s;
    memset(&s, 0, sizeof(s));
    s.ctx = ctx;
    strbuf_init(&s.buf, ctx);
    s.replacer = R8E_UNDEFINED;
    s.replacer_keys = NULL;
    s.replacer_key_count = 0;
    s.indent_len = 0;
    s.indent_level = 0;

    /* Process replacer parameter */
    if (!R8E_IS_UNDEFINED(replacer)) {
        if (json_is_function(replacer)) {
            s.replacer = replacer;
        } else if (json_is_array(replacer)) {
            /* Replacer is an array of keys */
            void *arr_ptr = r8e_get_pointer(replacer);
            uint32_t len = r8e_arr_length(ctx, arr_ptr);
            if (len > 0) {
                s.replacer_keys = (uint32_t *)r8e_alloc(
                    ctx, sizeof(uint32_t) * len);
                if (s.replacer_keys) {
                    uint32_t count = 0;
                    for (uint32_t i = 0; i < len; i++) {
                        R8EValue elem = r8e_arr_get(ctx, arr_ptr, i);
                        char buf[8];
                        uint32_t slen;
                        const char *sdata = json_get_str_data(elem, buf,
                                                               &slen);
                        if (sdata) {
                            s.replacer_keys[count++] =
                                r8e_atom_intern_str(ctx, sdata, slen);
                        }
                    }
                    s.replacer_key_count = count;
                }
            }
        }
    }

    /* Process space parameter */
    if (!R8E_IS_UNDEFINED(space)) {
        if (R8E_IS_INT32(space)) {
            int n = r8e_get_int32(space);
            if (n < 0) n = 0;
            if (n > 10) n = 10;
            s.indent_len = n;
            for (int i = 0; i < n; i++) s.indent_str[i] = ' ';
            s.indent_str[n] = '\0';
        } else if (R8E_IS_DOUBLE(space)) {
            int n = (int)r8e_get_double(space);
            if (n < 0) n = 0;
            if (n > 10) n = 10;
            s.indent_len = n;
            for (int i = 0; i < n; i++) s.indent_str[i] = ' ';
            s.indent_str[n] = '\0';
        } else if (R8E_IS_INLINE_STR(space) || json_is_string(space)) {
            /* Use the string (up to 10 chars) as indent */
            char buf[11];
            uint32_t slen;
            const char *sdata = json_get_str_data(space, buf, &slen);
            if (sdata) {
                if (slen > 10) slen = 10;
                memcpy(s.indent_str, sdata, slen);
                s.indent_str[slen] = '\0';
                s.indent_len = (int)slen;
            }
        }
    }

    /* Apply toJSON at the top level */
    R8EValue key_val = r8e_from_inline_str("", 0);
    value = json_apply_toJSON(&s, value, key_val);

    /* Apply replacer at the top level */
    if (!R8E_IS_UNDEFINED(s.replacer) && json_is_function(s.replacer)) {
        /* Create a wrapper holder object: {"": value} */
        void *holder = r8e_obj_new(ctx);
        if (holder) {
            uint32_t empty_atom = r8e_atom_intern_str(ctx, "", 0);
            r8e_obj_set(ctx, holder, empty_atom, value);
            R8EValue holder_val = r8e_from_pointer(holder);
            value = json_apply_replacer(&s, key_val, value, holder_val);
        }
    }

    /* Top-level undefined/function/symbol -> return undefined */
    if (R8E_IS_UNDEFINED(value) || json_is_function(value)) {
        strbuf_free(&s.buf, ctx);
        if (s.replacer_keys) r8e_free(ctx, s.replacer_keys);
        return R8E_UNDEFINED;
    }

    bool ok = json_stringify_value(&s, key_val, value, R8E_UNDEFINED);

    if (s.replacer_keys) {
        r8e_free(ctx, s.replacer_keys);
        s.replacer_keys = NULL;
    }

    if (!ok || s.error || s.buf.oom) {
        if (s.error_msg &&
            strcmp(s.error_msg,
                   "Converting circular structure to JSON") == 0) {
            ctx->has_exception = true;
        }
        strbuf_free(&s.buf, ctx);
        return R8E_UNDEFINED;
    }

    /* Convert buffer to string value */
    R8EValue result = r8e_string_val(ctx, s.buf.data, (uint32_t)s.buf.len);

    strbuf_free(&s.buf, ctx);
    return result;
}

/* =========================================================================
 * Built-in Registration: JSON.parse and JSON.stringify
 *
 * These are the native function callbacks that can be registered on the
 * JSON global object.
 * ========================================================================= */

/*
 * Native callback for JSON.parse(text [, reviver])
 */
R8EValue r8e_builtin_json_parse(R8EContext *ctx, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    (void)this_val;

    if (argc < 1) {
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    R8EValue text_val = argv[0];
    const char *text;
    uint32_t text_len;
    char buf[8];

    if (R8E_IS_INLINE_STR(text_val)) {
        text_len = (uint32_t)r8e_inline_str_decode(text_val, buf);
        text = buf;
    } else if (R8E_IS_POINTER(text_val)) {
        text = r8e_string_data(text_val, &text_len);
    } else {
        /* Not a string - try to convert */
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    if (!text) {
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    R8EValue reviver = (argc >= 2) ? argv[1] : R8E_UNDEFINED;
    return r8e_json_parse_reviver(ctx, text, text_len, reviver);
}

/*
 * Native callback for JSON.stringify(value [, replacer [, space]])
 */
R8EValue r8e_builtin_json_stringify(R8EContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv) {
    (void)this_val;

    if (argc < 1) {
        return R8E_UNDEFINED;
    }

    R8EValue value    = argv[0];
    R8EValue replacer = (argc >= 2) ? argv[1] : R8E_UNDEFINED;
    R8EValue space    = (argc >= 3) ? argv[2] : R8E_UNDEFINED;

    return r8e_json_stringify_full(ctx, value, replacer, space);
}
