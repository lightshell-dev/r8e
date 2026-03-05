/*
 * r8e_value.c - Value creation, type checks, and ES2023 type conversions
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 2 for NaN-boxing design.
 *
 * NaN-boxing encoding:
 *   Doubles:      0x0000.. to 0x7FF7.. and 0x8000.. to 0xFFF7.. (native IEEE 754)
 *   Int32:        0xFFF8_0000_XXXX_XXXX (32-bit signed integer)
 *   Heap pointer: 0xFFF9_0000_0000_XXXX (48-bit pointer)
 *   Undefined:    0xFFFA_0000_0000_0000
 *   Null:         0xFFFA_0000_0000_0001
 *   True:         0xFFFA_0000_0000_0002
 *   False:        0xFFFA_0000_0000_0003
 *   Symbol:       0xFFFB_0000_XXXX_XXXX
 *   Atom:         0xFFFC_0000_XXXX_XXXX
 *   Inline str:   0xFFFD_LLLL_DDDD_DDDD (up to 7 ASCII chars)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

/* -------------------------------------------------------------------------
 * Minimal type definitions (standalone until r8e_types.h arrives)
 * ------------------------------------------------------------------------- */
#ifndef R8E_TYPES_H

typedef uint64_t R8EValue;

/* Tag constants */
#define R8E_TAG_INT32       0xFFF80000U
#define R8E_TAG_POINTER     0xFFF90000U
#define R8E_TAG_SPECIAL     0xFFFAU
#define R8E_TAG_SYMBOL      0xFFFBU
#define R8E_TAG_ATOM        0xFFFCU
#define R8E_TAG_INLINE_STR  0xFFFDU

/* Singleton values */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* Type check macros */
#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 32) == R8E_TAG_INT32)
#define R8E_IS_POINTER(v)     (((v) >> 32) == R8E_TAG_POINTER)
#define R8E_IS_UNDEFINED(v)   ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)        ((v) == R8E_NULL)
#define R8E_IS_TRUE(v)        ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)       ((v) == R8E_FALSE)
#define R8E_IS_BOOLEAN(v)     ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)      (((v) >> 48) == R8E_TAG_SYMBOL)
#define R8E_IS_ATOM(v)        (((v) >> 48) == R8E_TAG_ATOM)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == R8E_TAG_INLINE_STR)

/* Extraction */
static inline double r8e_get_double(R8EValue v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}

static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint32_t r8e_get_symbol_id(R8EValue v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

static inline uint32_t r8e_get_atom_index(R8EValue v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

/* Encoding */
static inline R8EValue r8e_from_double(double d) {
    R8EValue v;
    memcpy(&v, &d, 8);
    /* Canonicalize NaN to avoid collisions with tag space */
    if (v >= 0xFFF8000000000000ULL && v <= 0xFFFFFFFFFFFFFFFFULL) {
        /* This is a NaN that would collide with our tag space */
        v = 0x7FF8000000000000ULL; /* canonical positive NaN */
    }
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

/* Inline short string encoding: bits [47:45] = length, bits [44:0] = packed 7-bit chars */
static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    R8EValue v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len && i < 7; i++) {
        v |= ((uint64_t)(uint8_t)s[i] << (38 - i * 7));
    }
    return v;
}

static inline int r8e_get_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}

static inline void r8e_get_inline_str(R8EValue v, char *buf, int *len) {
    int l = r8e_get_inline_str_len(v);
    for (int i = 0; i < l; i++) {
        buf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
    }
    buf[l] = '\0';
    if (len) *len = l;
}

/* Heap string header (from Section 4) */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    /* char data[] follows - flexible array member */
} R8EString;

#define R8E_STR_FLAG_ASCII    0x01
#define R8E_STR_FLAG_INTERNED 0x02

static inline const char *r8e_string_data(const R8EString *s) {
    return (const char *)(s + 1);
}

/* GC header flags - object kind bits */
#define R8E_GC_KIND_STRING  0x01
#define R8E_GC_KIND_OBJECT  0x02
#define R8E_GC_KIND_ARRAY   0x03
#define R8E_GC_KIND_FUNC    0x04

/* Minimal object header for type detection */
typedef struct R8EObjectHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjectHeader;

/* Prototype IDs */
#define R8E_PROTO_OBJECT   0
#define R8E_PROTO_ARRAY    1
#define R8E_PROTO_STRING   2
#define R8E_PROTO_NUMBER   3
#define R8E_PROTO_BOOLEAN  4
#define R8E_PROTO_SYMBOL   5
#define R8E_PROTO_FUNCTION 6

/* Forward declarations */
typedef struct R8EContext R8EContext;

/* Context stub - will be replaced by real context from r8e_context.h */
#ifndef R8E_CONTEXT_DEFINED
#define R8E_CONTEXT_DEFINED
struct R8EContext {
    void *arena;          /* R8EArena pointer */
    void *atom_table;     /* R8EAtomTable pointer */
    void *global_object;  /* global object */
    char  error_buf[256]; /* last error message */
    int   has_error;
};
#endif

#endif /* R8E_TYPES_H */

/* =========================================================================
 * TYPE CHECKING FUNCTIONS
 * ========================================================================= */

bool r8e_value_is_number(R8EValue v) {
    return R8E_IS_DOUBLE(v) || R8E_IS_INT32(v);
}

bool r8e_value_is_string(R8EValue v) {
    if (R8E_IS_INLINE_STR(v)) return true;
    if (!R8E_IS_POINTER(v)) return false;
    const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(v);
    return h && (h->flags & 0x0F) == R8E_GC_KIND_STRING;
}

bool r8e_value_is_object(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(v);
    if (!h) return false;
    uint8_t kind = h->flags & 0x0F;
    return kind == R8E_GC_KIND_OBJECT || kind == R8E_GC_KIND_ARRAY ||
           kind == R8E_GC_KIND_FUNC;
}

bool r8e_value_is_function(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(v);
    return h && (h->flags & 0x0F) == R8E_GC_KIND_FUNC;
}

bool r8e_value_is_array(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(v);
    return h && (h->flags & 0x0F) == R8E_GC_KIND_ARRAY;
}

bool r8e_value_is_null(R8EValue v) {
    return R8E_IS_NULL(v);
}

bool r8e_value_is_undefined(R8EValue v) {
    return R8E_IS_UNDEFINED(v);
}

bool r8e_value_is_boolean(R8EValue v) {
    return R8E_IS_BOOLEAN(v);
}

bool r8e_value_is_symbol(R8EValue v) {
    return R8E_IS_SYMBOL(v);
}

bool r8e_value_is_nullish(R8EValue v) {
    return R8E_IS_NULL(v) || R8E_IS_UNDEFINED(v);
}

bool r8e_value_is_int32(R8EValue v) {
    return R8E_IS_INT32(v);
}

bool r8e_value_is_double(R8EValue v) {
    return R8E_IS_DOUBLE(v);
}

bool r8e_value_is_inline_string(R8EValue v) {
    return R8E_IS_INLINE_STR(v);
}

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/*
 * Get the numeric value of an R8EValue as a double.
 * Assumes the caller has already verified the value is a number.
 */
static double value_to_double_raw(R8EValue v) {
    if (R8E_IS_INT32(v)) return (double)r8e_get_int32(v);
    return r8e_get_double(v);
}

/*
 * Check if a double can be losslessly represented as int32.
 * Used to keep values in the int32 fast path when possible.
 */
static bool double_is_int32(double d, int32_t *out) {
    if (!isfinite(d)) return false;
    int32_t i = (int32_t)d;
    if ((double)i != d) return false;
    /* -0 must remain a double */
    if (i == 0 && signbit(d)) return false;
    *out = i;
    return true;
}

/*
 * Create a number value, preferring int32 encoding when possible.
 */
static R8EValue number_from_double(double d) {
    int32_t i;
    if (double_is_int32(d, &i)) {
        return r8e_from_int32(i);
    }
    return r8e_from_double(d);
}

/*
 * Get string data and length from any string value (inline or heap).
 * For inline strings, writes into the provided buffer.
 * Returns pointer to string data.
 */
static const char *get_string_data(R8EValue v, char *inline_buf,
                                   uint32_t *out_len) {
    if (R8E_IS_INLINE_STR(v)) {
        int len;
        r8e_get_inline_str(v, inline_buf, &len);
        if (out_len) *out_len = (uint32_t)len;
        return inline_buf;
    }
    if (R8E_IS_POINTER(v)) {
        const R8EString *s = (const R8EString *)r8e_get_pointer(v);
        if (s) {
            if (out_len) *out_len = s->byte_length;
            return r8e_string_data(s);
        }
    }
    if (out_len) *out_len = 0;
    return "";
}

/* =========================================================================
 * ES2023 ToNumber (Section 7.1.4)
 * ========================================================================= */

R8EValue r8e_to_number(R8EContext *ctx, R8EValue val) {
    /* Fast path: already a number */
    if (R8E_IS_INT32(val) || R8E_IS_DOUBLE(val)) {
        return val;
    }

    /* Undefined -> NaN */
    if (R8E_IS_UNDEFINED(val)) {
        return r8e_from_double(NAN);
    }

    /* Null -> +0 */
    if (R8E_IS_NULL(val)) {
        return r8e_from_int32(0);
    }

    /* Boolean -> 0 or 1 */
    if (val == R8E_TRUE) return r8e_from_int32(1);
    if (val == R8E_FALSE) return r8e_from_int32(0);

    /* String -> parse as number */
    if (R8E_IS_INLINE_STR(val) || r8e_value_is_string(val)) {
        char inline_buf[8];
        uint32_t len;
        const char *str = get_string_data(val, inline_buf, &len);

        /* Empty string -> 0 */
        if (len == 0) return r8e_from_int32(0);

        /* Skip leading/trailing whitespace */
        const char *start = str;
        const char *end = str + len;
        while (start < end && (*start == ' ' || *start == '\t' ||
               *start == '\n' || *start == '\r' || *start == '\f' ||
               *start == '\v')) {
            start++;
        }
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
               end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\f' ||
               end[-1] == '\v')) {
            end--;
        }

        /* After trimming, empty string -> 0 */
        if (start == end) return r8e_from_int32(0);

        /* Check for special string values */
        size_t trimmed_len = (size_t)(end - start);

        if (trimmed_len == 8 && memcmp(start, "Infinity", 8) == 0) {
            return r8e_from_double(INFINITY);
        }
        if (trimmed_len == 9 && memcmp(start, "+Infinity", 9) == 0) {
            return r8e_from_double(INFINITY);
        }
        if (trimmed_len == 9 && memcmp(start, "-Infinity", 9) == 0) {
            return r8e_from_double(-INFINITY);
        }

        /* Hex literal: 0x or 0X */
        if (trimmed_len > 2 && start[0] == '0' &&
            (start[1] == 'x' || start[1] == 'X')) {
            char *endptr;
            errno = 0;
            unsigned long long ull = strtoull(start, &endptr, 16);
            if (endptr == end && errno == 0) {
                return number_from_double((double)ull);
            }
            return r8e_from_double(NAN);
        }

        /* Octal literal: 0o or 0O */
        if (trimmed_len > 2 && start[0] == '0' &&
            (start[1] == 'o' || start[1] == 'O')) {
            char *endptr;
            errno = 0;
            unsigned long long ull = strtoull(start + 2, &endptr, 8);
            if (endptr == end && errno == 0) {
                return number_from_double((double)ull);
            }
            return r8e_from_double(NAN);
        }

        /* Binary literal: 0b or 0B */
        if (trimmed_len > 2 && start[0] == '0' &&
            (start[1] == 'b' || start[1] == 'B')) {
            char *endptr;
            errno = 0;
            unsigned long long ull = strtoull(start + 2, &endptr, 2);
            if (endptr == end && errno == 0) {
                return number_from_double((double)ull);
            }
            return r8e_from_double(NAN);
        }

        /* General numeric string: use strtod */
        /* Need null-terminated copy since strtod requires it */
        char stack_buf[64];
        char *heap_buf = NULL;
        char *parse_str;
        if (trimmed_len < sizeof(stack_buf)) {
            memcpy(stack_buf, start, trimmed_len);
            stack_buf[trimmed_len] = '\0';
            parse_str = stack_buf;
        } else {
            heap_buf = (char *)malloc(trimmed_len + 1);
            if (!heap_buf) return r8e_from_double(NAN);
            memcpy(heap_buf, start, trimmed_len);
            heap_buf[trimmed_len] = '\0';
            parse_str = heap_buf;
        }

        char *endptr;
        errno = 0;
        double d = strtod(parse_str, &endptr);
        bool ok = ((size_t)(endptr - parse_str) == trimmed_len && errno == 0);
        free(heap_buf);

        if (ok) {
            return number_from_double(d);
        }
        return r8e_from_double(NAN);
    }

    /* Symbol -> TypeError (for now, return NaN and set error) */
    if (R8E_IS_SYMBOL(val)) {
        if (ctx) {
            snprintf(ctx->error_buf, sizeof(ctx->error_buf),
                     "TypeError: Cannot convert a Symbol value to a number");
            ctx->has_error = 1;
        }
        return r8e_from_double(NAN);
    }

    /* Object -> ToPrimitive(hint number) then ToNumber
     * For now, just return NaN. Full ToPrimitive needs object model. */
    if (R8E_IS_POINTER(val)) {
        /* TODO: call ToPrimitive(val, "number") when object model is ready */
        return r8e_from_double(NAN);
    }

    return r8e_from_double(NAN);
}

/* =========================================================================
 * ES2023 ToBoolean (Section 7.1.2)
 * ========================================================================= */

R8EValue r8e_to_boolean(R8EValue val) {
    /* undefined, null -> false */
    if (R8E_IS_UNDEFINED(val) || R8E_IS_NULL(val)) return R8E_FALSE;

    /* Boolean passthrough */
    if (val == R8E_TRUE) return R8E_TRUE;
    if (val == R8E_FALSE) return R8E_FALSE;

    /* Number: +0, -0, NaN -> false; everything else -> true */
    if (R8E_IS_INT32(val)) {
        return r8e_get_int32(val) == 0 ? R8E_FALSE : R8E_TRUE;
    }
    if (R8E_IS_DOUBLE(val)) {
        double d = r8e_get_double(val);
        return (d == 0.0 || isnan(d)) ? R8E_FALSE : R8E_TRUE;
    }

    /* String: empty -> false, non-empty -> true */
    if (R8E_IS_INLINE_STR(val)) {
        return r8e_get_inline_str_len(val) == 0 ? R8E_FALSE : R8E_TRUE;
    }
    if (r8e_value_is_string(val)) {
        const R8EString *s = (const R8EString *)r8e_get_pointer(val);
        return (s && s->byte_length > 0) ? R8E_TRUE : R8E_FALSE;
    }

    /* Symbol -> true */
    if (R8E_IS_SYMBOL(val)) return R8E_TRUE;

    /* Object (including arrays, functions) -> true */
    if (R8E_IS_POINTER(val)) return R8E_TRUE;

    return R8E_FALSE;
}

/* =========================================================================
 * ES2023 ToString (Section 7.1.17)
 * ========================================================================= */

/*
 * Allocate a heap string from a C string.
 * This is a simplified allocator; the real one will use the arena.
 */
static R8EValue make_heap_string(R8EContext *ctx, const char *data,
                                 uint32_t len) {
    /* Try inline string first (0-7 ASCII chars) */
    if (len <= 7) {
        bool all_ascii = true;
        for (uint32_t i = 0; i < len; i++) {
            if ((uint8_t)data[i] > 127) {
                all_ascii = false;
                break;
            }
        }
        if (all_ascii) {
            return r8e_from_inline_str(data, (int)len);
        }
    }

    /* Heap-allocate: header + data + null terminator */
    size_t total = sizeof(R8EString) + len + 1;
    R8EString *s = (R8EString *)malloc(total);
    if (!s) return R8E_UNDEFINED;

    s->flags = R8E_GC_KIND_STRING | R8E_STR_FLAG_ASCII; /* assume ASCII for now */
    s->hash = 0;
    s->byte_length = len;
    s->char_length = len;

    /* Check for non-ASCII */
    for (uint32_t i = 0; i < len; i++) {
        if ((uint8_t)data[i] > 127) {
            s->flags &= ~R8E_STR_FLAG_ASCII;
            /* TODO: compute char_length for UTF-8 */
            break;
        }
    }

    char *dst = (char *)(s + 1);
    memcpy(dst, data, len);
    dst[len] = '\0';

    /* Compute FNV-1a hash */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    s->hash = h;

    return r8e_from_pointer(s);
}

/* Forward declaration */
R8EValue r8e_num_to_string(R8EContext *ctx, R8EValue val);

R8EValue r8e_to_string(R8EContext *ctx, R8EValue val) {
    /* Undefined -> "undefined" (9 chars, too long for inline, use heap) */
    if (R8E_IS_UNDEFINED(val)) {
        return make_heap_string(ctx, "undefined", 9);
    }

    /* Null -> "null" */
    if (R8E_IS_NULL(val)) {
        return r8e_from_inline_str("null", 4);
    }

    /* Boolean -> "true" / "false" */
    if (val == R8E_TRUE) {
        return r8e_from_inline_str("true", 4);
    }
    if (val == R8E_FALSE) {
        return r8e_from_inline_str("false", 5);
    }

    /* Number -> NumberToString */
    if (R8E_IS_INT32(val) || R8E_IS_DOUBLE(val)) {
        return r8e_num_to_string(ctx, val);
    }

    /* String passthrough */
    if (R8E_IS_INLINE_STR(val) || r8e_value_is_string(val)) {
        return val;
    }

    /* Symbol -> TypeError */
    if (R8E_IS_SYMBOL(val)) {
        if (ctx) {
            snprintf(ctx->error_buf, sizeof(ctx->error_buf),
                     "TypeError: Cannot convert a Symbol value to a string");
            ctx->has_error = 1;
        }
        return R8E_UNDEFINED;
    }

    /* Object -> ToPrimitive(hint string) then ToString */
    /* TODO: implement when object model is ready */
    return make_heap_string(ctx, "[object Object]", 15);
}

/* =========================================================================
 * ES2023 ToObject (Section 7.1.18)
 * ========================================================================= */

R8EValue r8e_to_object(R8EContext *ctx, R8EValue val) {
    /* Undefined or Null -> TypeError */
    if (R8E_IS_UNDEFINED(val) || R8E_IS_NULL(val)) {
        if (ctx) {
            snprintf(ctx->error_buf, sizeof(ctx->error_buf),
                     "TypeError: Cannot convert undefined or null to object");
            ctx->has_error = 1;
        }
        return R8E_UNDEFINED;
    }

    /* Object passthrough */
    if (r8e_value_is_object(val)) {
        return val;
    }

    /*
     * Boolean, Number, String, Symbol -> create wrapper object.
     * TODO: implement wrapper objects when object model is ready.
     * For now, return the value as-is.
     */
    return val;
}

/* =========================================================================
 * ES2023 ToInt32 (Section 7.1.6)
 * ========================================================================= */

R8EValue r8e_to_int32(R8EContext *ctx, R8EValue val) {
    /* Fast path: already int32 */
    if (R8E_IS_INT32(val)) return val;

    /* Convert to number first */
    R8EValue num = r8e_to_number(ctx, val);

    double d;
    if (R8E_IS_INT32(num)) {
        return num; /* already int32 */
    }
    d = r8e_get_double(num);

    /* NaN, +0, -0, Infinity, -Infinity -> 0 */
    if (!isfinite(d) || d == 0.0) {
        return r8e_from_int32(0);
    }

    /* ES2023: truncate toward zero, then modulo 2^32, then adjust to signed */
    double truncated = trunc(d);
    double mod = fmod(truncated, 4294967296.0); /* 2^32 */
    if (mod < 0) mod += 4294967296.0;

    int32_t result;
    if (mod >= 2147483648.0) { /* 2^31 */
        result = (int32_t)(mod - 4294967296.0);
    } else {
        result = (int32_t)mod;
    }

    return r8e_from_int32(result);
}

/* =========================================================================
 * ES2023 ToUint32 (Section 7.1.7)
 * ========================================================================= */

R8EValue r8e_to_uint32(R8EContext *ctx, R8EValue val) {
    /* Get int32 first (same modular arithmetic) */
    R8EValue i32 = r8e_to_int32(ctx, val);
    int32_t i = r8e_get_int32(i32);
    /* Reinterpret as uint32 and store as double if negative */
    uint32_t u = (uint32_t)i;
    if (i < 0) {
        return r8e_from_double((double)u);
    }
    return r8e_from_int32(i);
}

/* =========================================================================
 * ES2023 Abstract Equality (==) (Section 7.2.14)
 * ========================================================================= */

/*
 * Helper: get type category for equality comparisons.
 * Returns: 0=undefined, 1=null, 2=boolean, 3=number, 4=string, 5=symbol,
 *          6=object
 */
static int value_type_category(R8EValue v) {
    if (R8E_IS_UNDEFINED(v)) return 0;
    if (R8E_IS_NULL(v)) return 1;
    if (R8E_IS_BOOLEAN(v)) return 2;
    if (R8E_IS_INT32(v) || R8E_IS_DOUBLE(v)) return 3;
    if (R8E_IS_INLINE_STR(v)) return 4;
    if (R8E_IS_SYMBOL(v)) return 5;
    if (R8E_IS_POINTER(v)) {
        const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(v);
        if (h && (h->flags & 0x0F) == R8E_GC_KIND_STRING) return 4;
        return 6;
    }
    return -1;
}

/*
 * Compare two string values for equality.
 */
static bool strings_equal(R8EValue a, R8EValue b) {
    /* Both inline? Direct bit comparison. */
    if (R8E_IS_INLINE_STR(a) && R8E_IS_INLINE_STR(b)) {
        return a == b;
    }

    /* Extract both strings */
    char buf_a[8], buf_b[8];
    uint32_t len_a, len_b;
    const char *sa = get_string_data(a, buf_a, &len_a);
    const char *sb = get_string_data(b, buf_b, &len_b);

    if (len_a != len_b) return false;
    return memcmp(sa, sb, len_a) == 0;
}

/*
 * Get the numeric value of a string for comparison purposes.
 */
static double string_to_double(R8EValue v) {
    char inline_buf[8];
    uint32_t len;
    const char *s = get_string_data(v, inline_buf, &len);

    if (len == 0) return 0.0;

    /* Skip whitespace */
    const char *start = s;
    const char *end = s + len;
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (start == end) return 0.0;

    size_t trimmed_len = (size_t)(end - start);
    char buf[64];
    if (trimmed_len >= sizeof(buf)) return NAN;
    memcpy(buf, start, trimmed_len);
    buf[trimmed_len] = '\0';

    char *endptr;
    double d = strtod(buf, &endptr);
    if ((size_t)(endptr - buf) != trimmed_len) return NAN;
    return d;
}

/* Forward declaration */
R8EValue r8e_strict_eq(R8EValue a, R8EValue b);

R8EValue r8e_abstract_eq(R8EContext *ctx, R8EValue a, R8EValue b) {
    /* Retry limit to prevent infinite recursion from bad coercions */
    int retries = 10;

retry:
    if (retries-- <= 0) return R8E_FALSE;

    int ta = value_type_category(a);
    int tb = value_type_category(b);

    /* Same type: use strict equality */
    if (ta == tb) {
        return r8e_strict_eq(a, b);
    }

    /* null == undefined -> true */
    if ((ta == 0 && tb == 1) || (ta == 1 && tb == 0)) {
        return R8E_TRUE;
    }

    /* Number == String -> Number == ToNumber(String) */
    if (ta == 3 && tb == 4) {
        b = r8e_to_number(ctx, b);
        goto retry;
    }
    if (ta == 4 && tb == 3) {
        a = r8e_to_number(ctx, a);
        goto retry;
    }

    /* Boolean == any -> ToNumber(Boolean) == any */
    if (ta == 2) {
        a = r8e_to_number(ctx, a);
        goto retry;
    }
    if (tb == 2) {
        b = r8e_to_number(ctx, b);
        goto retry;
    }

    /* (String|Number|Symbol) == Object -> ToPrimitive(Object) */
    if ((ta == 3 || ta == 4 || ta == 5) && tb == 6) {
        /* TODO: call ToPrimitive when object model is ready */
        return R8E_FALSE;
    }
    if (ta == 6 && (tb == 3 || tb == 4 || tb == 5)) {
        /* TODO: call ToPrimitive when object model is ready */
        return R8E_FALSE;
    }

    return R8E_FALSE;
}

/* =========================================================================
 * ES2023 Strict Equality (===) (Section 7.2.16)
 * ========================================================================= */

R8EValue r8e_strict_eq(R8EValue a, R8EValue b) {
    int ta = value_type_category(a);
    int tb = value_type_category(b);

    /* Different types -> false */
    if (ta != tb) return R8E_FALSE;

    switch (ta) {
    case 0: /* undefined === undefined */
    case 1: /* null === null */
        return R8E_TRUE;

    case 2: /* boolean === boolean */
        return (a == b) ? R8E_TRUE : R8E_FALSE;

    case 3: { /* number === number */
        double da, db;
        da = value_to_double_raw(a);
        db = value_to_double_raw(b);
        /* NaN !== NaN */
        if (isnan(da) || isnan(db)) return R8E_FALSE;
        /* +0 === -0 */
        if (da == 0.0 && db == 0.0) return R8E_TRUE;
        return (da == db) ? R8E_TRUE : R8E_FALSE;
    }

    case 4: /* string === string */
        return strings_equal(a, b) ? R8E_TRUE : R8E_FALSE;

    case 5: /* symbol === symbol (identity comparison) */
        return (a == b) ? R8E_TRUE : R8E_FALSE;

    case 6: /* object === object (reference equality) */
        return (a == b) ? R8E_TRUE : R8E_FALSE;

    default:
        return R8E_FALSE;
    }
}

/* =========================================================================
 * typeof operator
 * ========================================================================= */

R8EValue r8e_typeof(R8EValue val) {
    if (R8E_IS_UNDEFINED(val)) {
        return make_heap_string(NULL, "undefined", 9);
    }
    if (R8E_IS_NULL(val)) {
        return r8e_from_inline_str("object", 6); /* typeof null === "object" */
    }
    if (R8E_IS_BOOLEAN(val)) {
        return r8e_from_inline_str("boolean", 7);
    }
    if (R8E_IS_INT32(val) || R8E_IS_DOUBLE(val)) {
        return r8e_from_inline_str("number", 6);
    }
    if (R8E_IS_INLINE_STR(val)) {
        return r8e_from_inline_str("string", 6);
    }
    if (R8E_IS_SYMBOL(val)) {
        return r8e_from_inline_str("symbol", 6);
    }
    if (R8E_IS_POINTER(val)) {
        const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(val);
        if (h) {
            if ((h->flags & 0x0F) == R8E_GC_KIND_STRING)
                return r8e_from_inline_str("string", 6);
            if ((h->flags & 0x0F) == R8E_GC_KIND_FUNC)
                return make_heap_string(NULL, "function", 8);
        }
        return r8e_from_inline_str("object", 6);
    }
    return make_heap_string(NULL, "undefined", 9);
}

/*
 * NOTE on typeof return values:
 * "undefined" (9 chars) and "function" (8 chars) exceed inline string
 * capacity (7 chars). They are allocated as heap strings. In the full
 * engine these will be pre-interned atoms looked up by ID for zero
 * allocation. The r8e_typeof_cstring function below returns the C string.
 */

/* =========================================================================
 * typeof as C string (for internal use and debugging)
 * ========================================================================= */

const char *r8e_typeof_cstring(R8EValue val) {
    if (R8E_IS_UNDEFINED(val)) return "undefined";
    if (R8E_IS_NULL(val)) return "object";
    if (R8E_IS_BOOLEAN(val)) return "boolean";
    if (R8E_IS_INT32(val) || R8E_IS_DOUBLE(val)) return "number";
    if (R8E_IS_INLINE_STR(val)) return "string";
    if (R8E_IS_SYMBOL(val)) return "symbol";
    if (R8E_IS_POINTER(val)) {
        const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(val);
        if (h) {
            if ((h->flags & 0x0F) == R8E_GC_KIND_STRING) return "string";
            if ((h->flags & 0x0F) == R8E_GC_KIND_FUNC) return "function";
        }
        return "object";
    }
    return "undefined";
}

/* =========================================================================
 * Debug printing: r8e_value_to_cstring
 * ========================================================================= */

/*
 * Returns a human-readable C string representation of a value.
 * The buffer is ctx->error_buf (reused). NOT thread-safe.
 * For longer-lived strings, caller must copy.
 */
const char *r8e_value_to_cstring(R8EContext *ctx, R8EValue val) {
    static char static_buf[256];
    char *buf = ctx ? ctx->error_buf : static_buf;
    size_t bufsz = ctx ? sizeof(ctx->error_buf) : sizeof(static_buf);

    if (R8E_IS_UNDEFINED(val)) {
        snprintf(buf, bufsz, "undefined");
        return buf;
    }
    if (R8E_IS_NULL(val)) {
        snprintf(buf, bufsz, "null");
        return buf;
    }
    if (val == R8E_TRUE) {
        snprintf(buf, bufsz, "true");
        return buf;
    }
    if (val == R8E_FALSE) {
        snprintf(buf, bufsz, "false");
        return buf;
    }
    if (R8E_IS_INT32(val)) {
        snprintf(buf, bufsz, "%d", r8e_get_int32(val));
        return buf;
    }
    if (R8E_IS_DOUBLE(val)) {
        double d = r8e_get_double(val);
        if (isnan(d)) {
            snprintf(buf, bufsz, "NaN");
        } else if (isinf(d)) {
            snprintf(buf, bufsz, "%sInfinity", d < 0 ? "-" : "");
        } else if (d == 0.0 && signbit(d)) {
            snprintf(buf, bufsz, "-0");
        } else {
            /* Use %g to minimize trailing zeros */
            snprintf(buf, bufsz, "%.17g", d);
            /* Remove unnecessary trailing zeros after decimal */
            char *dot = strchr(buf, '.');
            if (dot && !strchr(buf, 'e') && !strchr(buf, 'E')) {
                char *end = buf + strlen(buf) - 1;
                while (end > dot && *end == '0') {
                    *end-- = '\0';
                }
                if (end == dot) *end = '\0'; /* remove lone decimal point */
            }
        }
        return buf;
    }
    if (R8E_IS_INLINE_STR(val)) {
        char strbuf[8];
        int len;
        r8e_get_inline_str(val, strbuf, &len);
        snprintf(buf, bufsz, "%.*s", len, strbuf);
        return buf;
    }
    if (R8E_IS_SYMBOL(val)) {
        snprintf(buf, bufsz, "Symbol(%u)", r8e_get_symbol_id(val));
        return buf;
    }
    if (R8E_IS_ATOM(val)) {
        snprintf(buf, bufsz, "Atom(%u)", r8e_get_atom_index(val));
        return buf;
    }
    if (R8E_IS_POINTER(val)) {
        const R8EObjectHeader *h = (const R8EObjectHeader *)r8e_get_pointer(val);
        if (h) {
            uint8_t kind = h->flags & 0x0F;
            if (kind == R8E_GC_KIND_STRING) {
                const R8EString *s = (const R8EString *)h;
                const char *data = r8e_string_data(s);
                uint32_t len = s->byte_length;
                if (len > bufsz - 1) len = (uint32_t)(bufsz - 1);
                memcpy(buf, data, len);
                buf[len] = '\0';
                return buf;
            }
            switch (kind) {
            case R8E_GC_KIND_OBJECT:
                snprintf(buf, bufsz, "[object Object]");
                break;
            case R8E_GC_KIND_ARRAY:
                snprintf(buf, bufsz, "[object Array]");
                break;
            case R8E_GC_KIND_FUNC:
                snprintf(buf, bufsz, "[object Function]");
                break;
            default:
                snprintf(buf, bufsz, "[object Unknown(%d)]", kind);
                break;
            }
            return buf;
        }
        snprintf(buf, bufsz, "[null pointer]");
        return buf;
    }

    snprintf(buf, bufsz, "[unknown value: 0x%016llx]",
             (unsigned long long)val);
    return buf;
}

/* =========================================================================
 * Utility: SameValue and SameValueZero
 * ========================================================================= */

/*
 * ES2023 SameValue (Section 7.2.11)
 * Like ===, but NaN === NaN is true, and +0 !== -0.
 * Used by Object.is().
 */
R8EValue r8e_same_value(R8EValue a, R8EValue b) {
    int ta = value_type_category(a);
    int tb = value_type_category(b);

    if (ta != tb) return R8E_FALSE;

    if (ta == 3) {
        double da = value_to_double_raw(a);
        double db = value_to_double_raw(b);
        /* NaN is the same as NaN */
        if (isnan(da) && isnan(db)) return R8E_TRUE;
        /* +0 is NOT the same as -0 */
        if (da == 0.0 && db == 0.0) {
            return (signbit(da) == signbit(db)) ? R8E_TRUE : R8E_FALSE;
        }
        return (da == db) ? R8E_TRUE : R8E_FALSE;
    }

    /* For all other types, SameValue == === */
    return r8e_strict_eq(a, b);
}

/*
 * ES2023 SameValueZero (Section 7.2.12)
 * Like SameValue, but +0 === -0 is true.
 * Used by Map, Set, Array.includes().
 */
R8EValue r8e_same_value_zero(R8EValue a, R8EValue b) {
    int ta = value_type_category(a);
    int tb = value_type_category(b);

    if (ta != tb) return R8E_FALSE;

    if (ta == 3) {
        double da = value_to_double_raw(a);
        double db = value_to_double_raw(b);
        /* NaN is the same as NaN */
        if (isnan(da) && isnan(db)) return R8E_TRUE;
        /* +0 IS the same as -0 (unlike SameValue) */
        return (da == db) ? R8E_TRUE : R8E_FALSE;
    }

    return r8e_strict_eq(a, b);
}
