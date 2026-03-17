/*
 * r8e_number.c - Number operations with int32 fast paths
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 7 (Interpreter) for fast-path design.
 *
 * Architecture:
 *   - All arithmetic operations have an int32 fast path
 *   - Overflow from int32 promotes to double automatically
 *   - Bitwise operations always work on int32 (per ES2023 spec)
 *   - Special value handling: NaN, Infinity, -Infinity, -0
 *   - Number-to-string and string-to-number conversions
 *   - Math object methods (abs, floor, ceil, round, etc.)
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
#include <time.h>

/* -------------------------------------------------------------------------
 * Minimal type definitions (standalone until r8e_types.h arrives)
 * ------------------------------------------------------------------------- */
#ifndef R8E_TYPES_H

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
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == R8E_TAG_INLINE_STR)

static inline double r8e_get_double(R8EValue v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}

static inline R8EValue r8e_from_double(double d) {
    R8EValue v;
    memcpy(&v, &d, 8);
    if (v >= 0xFFF8000000000000ULL && v <= 0xFFFFFFFFFFFFFFFFULL) {
        v = 0x7FF8000000000000ULL; /* canonical NaN */
    }
    return v;
}

static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

static inline R8EValue r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    R8EValue v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len && i < 7; i++) {
        v |= ((uint64_t)(uint8_t)s[i] << (38 - i * 7));
    }
    return v;
}

static inline R8EValue r8e_from_boolean(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}

/* Heap string header */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
} R8EString;

#define R8E_GC_KIND_STRING 0x01

static inline const char *r8e_string_data(const R8EString *s) {
    return (const char *)(s + 1);
}

typedef struct R8EContext R8EContext;

#ifndef R8E_CONTEXT_DEFINED
#define R8E_CONTEXT_DEFINED
struct R8EContext {
    void *arena;
    void *atom_table;
    void *global_object;
    char  error_buf[256];
    int   has_error;
};
#endif

#endif /* R8E_TYPES_H */

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/*
 * Extract a double from any numeric value.
 */
static inline double to_double(R8EValue v) {
    if (R8E_IS_INT32(v)) return (double)r8e_get_int32(v);
    return r8e_get_double(v);
}

/*
 * Try to fit a double into int32 encoding.
 */
static inline bool fits_int32(double d, int32_t *out) {
    if (!isfinite(d)) return false;
    int32_t i = (int32_t)d;
    if ((double)i != d) return false;
    if (i == 0 && signbit(d)) return false; /* -0 stays double */
    *out = i;
    return true;
}

/*
 * Create a number value, preferring int32 when possible.
 */
static inline R8EValue make_number(double d) {
    int32_t i;
    if (fits_int32(d, &i)) return r8e_from_int32(i);
    return r8e_from_double(d);
}

/* =========================================================================
 * BINARY ARITHMETIC with int32 fast paths
 * ========================================================================= */

/*
 * Addition (a + b).
 *
 * NOTE: This handles numeric addition only.
 * String concatenation is handled by the ADD opcode in the interpreter,
 * which checks types before calling this function.
 */
R8EValue r8e_num_add(R8EValue a, R8EValue b) {
    /* Fast path: both int32 */
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        int64_t result = (int64_t)r8e_get_int32(a) + (int64_t)r8e_get_int32(b);
        if (result >= INT32_MIN && result <= INT32_MAX) {
            return r8e_from_int32((int32_t)result);
        }
        /* Overflow: promote to double */
        return r8e_from_double((double)result);
    }

    /* Slow path: convert both to double */
    double da = to_double(a);
    double db = to_double(b);
    return make_number(da + db);
}

/*
 * Subtraction (a - b).
 */
R8EValue r8e_num_sub(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        int64_t result = (int64_t)r8e_get_int32(a) - (int64_t)r8e_get_int32(b);
        if (result >= INT32_MIN && result <= INT32_MAX) {
            return r8e_from_int32((int32_t)result);
        }
        return r8e_from_double((double)result);
    }
    return make_number(to_double(a) - to_double(b));
}

/*
 * Multiplication (a * b).
 *
 * Special case: 0 * -Infinity = NaN (handled by double arithmetic).
 * Special case: int32 * int32 can overflow int32 range.
 */
R8EValue r8e_num_mul(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        int64_t result = (int64_t)r8e_get_int32(a) * (int64_t)r8e_get_int32(b);
        if (result >= INT32_MIN && result <= INT32_MAX) {
            /* Check for -0: 0 * negative = -0 */
            if (result == 0) {
                int32_t ia = r8e_get_int32(a);
                int32_t ib = r8e_get_int32(b);
                if ((ia < 0) != (ib < 0)) {
                    /* Different signs with zero result = -0 */
                    return r8e_from_double(-0.0);
                }
            }
            return r8e_from_int32((int32_t)result);
        }
        return r8e_from_double((double)result);
    }
    return make_number(to_double(a) * to_double(b));
}

/*
 * Division (a / b).
 *
 * Division always produces double (even 10/2) because:
 * - 1/0 = Infinity
 * - 0/0 = NaN
 * - Integer division can produce non-integers (7/2 = 3.5)
 *
 * Exception: exact integer division that fits int32.
 */
R8EValue r8e_num_div(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        int32_t ia = r8e_get_int32(a);
        int32_t ib = r8e_get_int32(b);

        /* Division by zero */
        if (ib == 0) {
            if (ia == 0) return r8e_from_double(NAN);
            return r8e_from_double(ia > 0 ? INFINITY : -INFINITY);
        }

        /* Check for INT32_MIN / -1 overflow */
        if (ia == INT32_MIN && ib == -1) {
            return r8e_from_double((double)INT32_MIN / -1.0);
        }

        /* Exact integer division? */
        if (ia % ib == 0) {
            int32_t result = ia / ib;
            /* Check for -0 */
            if (result == 0 && ((ia < 0) != (ib < 0))) {
                return r8e_from_double(-0.0);
            }
            return r8e_from_int32(result);
        }

        /* Non-exact: must use double */
        return r8e_from_double((double)ia / (double)ib);
    }

    double da = to_double(a);
    double db = to_double(b);
    return make_number(da / db);
}

/*
 * Modulo (a % b).
 * ES2023: result has the sign of the dividend (like C's fmod).
 */
R8EValue r8e_num_mod(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        int32_t ia = r8e_get_int32(a);
        int32_t ib = r8e_get_int32(b);

        if (ib == 0) return r8e_from_double(NAN);
        if (ia == 0) return r8e_from_int32(0);

        /* Avoid INT32_MIN % -1 (undefined behavior in C) */
        if (ia == INT32_MIN && ib == -1) {
            return r8e_from_int32(0);
        }

        int32_t result = ia % ib;
        if (result == 0 && ia < 0) {
            /* -0 case: e.g., -7 % 7 = -0 */
            return r8e_from_double(-0.0);
        }
        return r8e_from_int32(result);
    }

    double da = to_double(a);
    double db = to_double(b);
    double result = fmod(da, db);
    return make_number(result);
}

/*
 * Exponentiation (a ** b).
 * ES2023: uses Math.pow semantics.
 */
R8EValue r8e_num_pow(R8EValue a, R8EValue b) {
    double da = to_double(a);
    double db = to_double(b);

    /* Special cases per ES2023 */
    if (isnan(db)) return r8e_from_double(NAN);
    if (db == 0.0) return r8e_from_int32(1); /* anything ** 0 = 1, even NaN */
    if (isnan(da)) return r8e_from_double(NAN);

    return make_number(pow(da, db));
}

/* =========================================================================
 * UNARY ARITHMETIC
 * ========================================================================= */

/*
 * Unary negation (-a).
 */
R8EValue r8e_num_neg(R8EValue a) {
    if (R8E_IS_INT32(a)) {
        int32_t i = r8e_get_int32(a);
        if (i == 0) return r8e_from_double(-0.0); /* -0 */
        if (i == INT32_MIN) return r8e_from_double(-(double)INT32_MIN);
        return r8e_from_int32(-i);
    }
    return r8e_from_double(-to_double(a));
}

/*
 * Unary plus (+a). ToNumber conversion.
 * Just returns the numeric value (identity for numbers).
 */
R8EValue r8e_num_pos(R8EValue a) {
    /* If already a number, return as-is */
    if (R8E_IS_INT32(a) || R8E_IS_DOUBLE(a)) return a;
    /* Otherwise, should call r8e_to_number - but that's in r8e_value.c.
     * For pure numeric input, just return the value. */
    return a;
}

/*
 * Increment (++a or a++). Returns the incremented value.
 */
R8EValue r8e_num_inc(R8EValue a) {
    if (R8E_IS_INT32(a)) {
        int32_t i = r8e_get_int32(a);
        if (i < INT32_MAX) return r8e_from_int32(i + 1);
        return r8e_from_double((double)i + 1.0);
    }
    return make_number(to_double(a) + 1.0);
}

/*
 * Decrement (--a or a--). Returns the decremented value.
 */
R8EValue r8e_num_dec(R8EValue a) {
    if (R8E_IS_INT32(a)) {
        int32_t i = r8e_get_int32(a);
        if (i > INT32_MIN) return r8e_from_int32(i - 1);
        return r8e_from_double((double)i - 1.0);
    }
    return make_number(to_double(a) - 1.0);
}

/* =========================================================================
 * BITWISE OPERATIONS
 *
 * ES2023 spec: bitwise ops always convert operands to Int32 first,
 * perform the operation, and return Int32.
 * ========================================================================= */

/*
 * Convert a value to int32 for bitwise operations.
 * This is ES2023 ToInt32 (Section 7.1.6).
 */
static int32_t to_int32(R8EValue v) {
    if (R8E_IS_INT32(v)) return r8e_get_int32(v);

    double d = to_double(v);
    if (!isfinite(d) || d == 0.0) return 0;

    double truncated = trunc(d);
    double mod = fmod(truncated, 4294967296.0);
    if (mod < 0) mod += 4294967296.0;

    if (mod >= 2147483648.0) {
        return (int32_t)(mod - 4294967296.0);
    }
    return (int32_t)mod;
}

/*
 * Convert a value to uint32 for unsigned shift.
 */
static uint32_t to_uint32(R8EValue v) {
    return (uint32_t)to_int32(v);
}

R8EValue r8e_num_bitand(R8EValue a, R8EValue b) {
    return r8e_from_int32(to_int32(a) & to_int32(b));
}

R8EValue r8e_num_bitor(R8EValue a, R8EValue b) {
    return r8e_from_int32(to_int32(a) | to_int32(b));
}

R8EValue r8e_num_bitxor(R8EValue a, R8EValue b) {
    return r8e_from_int32(to_int32(a) ^ to_int32(b));
}

R8EValue r8e_num_bitnot(R8EValue a) {
    return r8e_from_int32(~to_int32(a));
}

/*
 * Left shift (a << b).
 * Shift count is masked to 0-31 (5 bits).
 */
R8EValue r8e_num_shl(R8EValue a, R8EValue b) {
    int32_t lhs = to_int32(a);
    uint32_t shift = to_uint32(b) & 0x1F;
    /* Use unsigned shift to avoid undefined behavior, then cast back */
    return r8e_from_int32((int32_t)((uint32_t)lhs << shift));
}

/*
 * Signed right shift (a >> b).
 * Shift count is masked to 0-31.
 */
R8EValue r8e_num_shr(R8EValue a, R8EValue b) {
    int32_t lhs = to_int32(a);
    uint32_t shift = to_uint32(b) & 0x1F;
    return r8e_from_int32(lhs >> shift);
}

/*
 * Unsigned right shift (a >>> b).
 * Result is always non-negative, so may need double for large values.
 */
R8EValue r8e_num_ushr(R8EValue a, R8EValue b) {
    uint32_t lhs = to_uint32(a);
    uint32_t shift = to_uint32(b) & 0x1F;
    uint32_t result = lhs >> shift;

    /* Result fits in int32 if < 2^31 */
    if (result <= (uint32_t)INT32_MAX) {
        return r8e_from_int32((int32_t)result);
    }
    return r8e_from_double((double)result);
}

/* =========================================================================
 * COMPARISON OPERATIONS
 * ========================================================================= */

/*
 * Abstract relational comparison (< operator).
 * Returns R8E_TRUE, R8E_FALSE, or R8E_UNDEFINED (for NaN comparisons).
 */
R8EValue r8e_num_lt(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        return r8e_from_boolean(r8e_get_int32(a) < r8e_get_int32(b));
    }
    double da = to_double(a);
    double db = to_double(b);
    if (isnan(da) || isnan(db)) return R8E_UNDEFINED;
    return r8e_from_boolean(da < db);
}

R8EValue r8e_num_le(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        return r8e_from_boolean(r8e_get_int32(a) <= r8e_get_int32(b));
    }
    double da = to_double(a);
    double db = to_double(b);
    if (isnan(da) || isnan(db)) return R8E_UNDEFINED;
    return r8e_from_boolean(da <= db);
}

R8EValue r8e_num_gt(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        return r8e_from_boolean(r8e_get_int32(a) > r8e_get_int32(b));
    }
    double da = to_double(a);
    double db = to_double(b);
    if (isnan(da) || isnan(db)) return R8E_UNDEFINED;
    return r8e_from_boolean(da > db);
}

R8EValue r8e_num_ge(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        return r8e_from_boolean(r8e_get_int32(a) >= r8e_get_int32(b));
    }
    double da = to_double(a);
    double db = to_double(b);
    if (isnan(da) || isnan(db)) return R8E_UNDEFINED;
    return r8e_from_boolean(da >= db);
}

/* =========================================================================
 * NUMBER TO STRING CONVERSION
 *
 * ES2023 Number::toString (Section 6.1.6.1.20)
 * ========================================================================= */

/*
 * Convert a number to its string representation.
 * Returns an R8EValue containing the string (inline or heap).
 */
R8EValue r8e_num_to_string(R8EContext *ctx, R8EValue val) {
    (void)ctx; /* unused for now; will use for heap allocation */

    /* Int32 fast path */
    if (R8E_IS_INT32(val)) {
        int32_t i = r8e_get_int32(val);
        char buf[12]; /* -2147483648 is 11 chars + null */
        int len = snprintf(buf, sizeof(buf), "%d", (int)i);
        if (len > 0 && len <= 7) {
            return r8e_from_inline_str(buf, len);
        }
        /* Longer than 7 chars: need heap string. For now, use inline_str
         * truncation as placeholder until string engine is integrated. */
        /* TODO: allocate heap string via context */
        if (len > 7) {
            /* Return a heap-allocated string. Simplified allocation. */
            size_t total = sizeof(R8EString) + (size_t)len + 1;
            R8EString *s = (R8EString *)malloc(total);
            if (!s) return R8E_UNDEFINED;
            s->flags = 0x01; /* ASCII */
            s->hash = 0;
            s->byte_length = (uint32_t)len;
            s->char_length = (uint32_t)len;
            memcpy((char *)(s + 1), buf, (size_t)len + 1);
            return r8e_from_pointer(s);
        }
        return R8E_UNDEFINED;
    }

    /* Double */
    if (R8E_IS_DOUBLE(val)) {
        double d = r8e_get_double(val);

        /* Special values */
        if (isnan(d)) return r8e_from_inline_str("NaN", 3);

        if (isinf(d)) {
            if (d > 0) {
                /* "Infinity" is 8 chars - one too long for inline */
                size_t total = sizeof(R8EString) + 9;
                R8EString *s = (R8EString *)malloc(total);
                if (!s) return R8E_UNDEFINED;
                s->flags = 0x01;
                s->hash = 0;
                s->byte_length = 8;
                s->char_length = 8;
                memcpy((char *)(s + 1), "Infinity", 9);
                return r8e_from_pointer(s);
            } else {
                size_t total = sizeof(R8EString) + 10;
                R8EString *s = (R8EString *)malloc(total);
                if (!s) return R8E_UNDEFINED;
                s->flags = 0x01;
                s->hash = 0;
                s->byte_length = 9;
                s->char_length = 9;
                memcpy((char *)(s + 1), "-Infinity", 10);
                return r8e_from_pointer(s);
            }
        }

        /* -0 -> "0" */
        if (d == 0.0 && signbit(d)) {
            return r8e_from_inline_str("0", 1);
        }

        /* Check if it is actually an integer that fits int32 */
        int32_t as_int;
        if (d == trunc(d) && d >= INT32_MIN && d <= INT32_MAX) {
            as_int = (int32_t)d;
            if (as_int != 0 || !signbit(d)) {
                char buf[12];
                int len = snprintf(buf, sizeof(buf), "%d", (int)as_int);
                if (len > 0 && len <= 7) {
                    return r8e_from_inline_str(buf, len);
                }
                if (len > 0) {
                    size_t total = sizeof(R8EString) + (size_t)len + 1;
                    R8EString *s = (R8EString *)malloc(total);
                    if (!s) return R8E_UNDEFINED;
                    s->flags = 0x01;
                    s->hash = 0;
                    s->byte_length = (uint32_t)len;
                    s->char_length = (uint32_t)len;
                    memcpy((char *)(s + 1), buf, (size_t)len + 1);
                    return r8e_from_pointer(s);
                }
            }
        }

        /*
         * General double-to-string.
         * ES2023 requires the shortest representation that round-trips.
         * We use %.17g which is always sufficient, then strip trailing zeros.
         */
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%.17g", d);
        if (len <= 0) return R8E_UNDEFINED;

        /* Strip unnecessary trailing zeros after decimal point */
        char *dot = strchr(buf, '.');
        if (dot && !strchr(buf, 'e') && !strchr(buf, 'E')) {
            char *end = buf + len - 1;
            while (end > dot && *end == '0') {
                *end-- = '\0';
                len--;
            }
            if (end == dot) {
                *end = '\0';
                len--;
            }
        }

        if (len <= 7) {
            return r8e_from_inline_str(buf, len);
        }

        /* Heap-allocate */
        size_t total = sizeof(R8EString) + (size_t)len + 1;
        R8EString *s = (R8EString *)malloc(total);
        if (!s) return R8E_UNDEFINED;
        s->flags = 0x01;
        s->hash = 0;
        s->byte_length = (uint32_t)len;
        s->char_length = (uint32_t)len;
        memcpy((char *)(s + 1), buf, (size_t)len + 1);
        return r8e_from_pointer(s);
    }

    /* Not a number - return "NaN" as fallback */
    return r8e_from_inline_str("NaN", 3);
}

/*
 * Convert a number to string with a specific radix (2-36).
 * Used by Number.prototype.toString(radix).
 */
R8EValue r8e_num_to_string_radix(R8EContext *ctx, R8EValue val, int radix) {
    (void)ctx;

    if (radix < 2 || radix > 36) return R8E_UNDEFINED;
    if (radix == 10) return r8e_num_to_string(ctx, val);

    double d = to_double(val);
    if (isnan(d)) return r8e_from_inline_str("NaN", 3);
    if (d == 0.0) return r8e_from_inline_str("0", 1);

    if (isinf(d)) {
        /* Infinity strings are handled by make_heap_string in r8e_value.c.
         * For now, return inline approximation. */
        return r8e_from_inline_str("Inf", 3);
    }

    /* Integer fast path */
    if (d == trunc(d) && fabs(d) < 4294967296.0) {
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        char buf[65]; /* max binary: 32 digits + sign + null */
        int pos = sizeof(buf) - 1;
        buf[pos] = '\0';

        bool negative = d < 0;
        uint64_t n = (uint64_t)(negative ? -d : d);

        if (n == 0) {
            buf[--pos] = '0';
        } else {
            while (n > 0) {
                buf[--pos] = digits[n % (uint64_t)radix];
                n /= (uint64_t)radix;
            }
        }
        if (negative) buf[--pos] = '-';

        int len = (int)(sizeof(buf) - 1 - (size_t)pos);
        if (len <= 7) {
            return r8e_from_inline_str(&buf[pos], len);
        }

        size_t total = sizeof(R8EString) + (size_t)len + 1;
        R8EString *s = (R8EString *)malloc(total);
        if (!s) return R8E_UNDEFINED;
        s->flags = 0x01;
        s->hash = 0;
        s->byte_length = (uint32_t)len;
        s->char_length = (uint32_t)len;
        memcpy((char *)(s + 1), &buf[pos], (size_t)len + 1);
        return r8e_from_pointer(s);
    }

    /* Non-integer with non-decimal radix: use the decimal representation
     * as a fallback. Full implementation would need a proper algorithm. */
    return r8e_num_to_string(ctx, val);
}

/* =========================================================================
 * STRING TO NUMBER CONVERSION
 *
 * Used by parseInt, parseFloat, and numeric string coercion.
 * ========================================================================= */

/*
 * Parse a string as a number (like parseFloat).
 * Handles integer, decimal, hex (0x), octal (0o), binary (0b), and
 * scientific notation.
 *
 * @param str  Input string (not necessarily null-terminated).
 * @param len  Length of input string.
 * @return     Parsed number as R8EValue, or NaN if unparseable.
 */
R8EValue r8e_string_to_number(const char *str, uint32_t len) {
    if (!str || len == 0) return r8e_from_double(NAN);

    /* Skip leading whitespace */
    const char *p = str;
    const char *end = str + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v')) {
        p++;
    }
    if (p == end) return r8e_from_double(NAN);

    /* Handle sign */
    bool negative = false;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        negative = true;
        p++;
    }
    if (p == end) return r8e_from_double(NAN);

    /* Infinity */
    size_t remaining = (size_t)(end - p);
    if (remaining >= 8 && memcmp(p, "Infinity", 8) == 0) {
        return r8e_from_double(negative ? -INFINITY : INFINITY);
    }

    /* Hex: 0x or 0X */
    if (remaining >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (p == end || !isxdigit((unsigned char)*p)) {
            return r8e_from_double(NAN);
        }
        uint64_t result = 0;
        while (p < end && isxdigit((unsigned char)*p)) {
            int digit;
            if (*p >= '0' && *p <= '9') digit = *p - '0';
            else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
            else digit = *p - 'A' + 10;
            result = result * 16 + (uint64_t)digit;
            p++;
        }
        double d = (double)result;
        if (negative) d = -d;
        return make_number(d);
    }

    /* Octal: 0o or 0O */
    if (remaining >= 2 && p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        p += 2;
        if (p == end || *p < '0' || *p > '7') {
            return r8e_from_double(NAN);
        }
        uint64_t result = 0;
        while (p < end && *p >= '0' && *p <= '7') {
            result = result * 8 + (uint64_t)(*p - '0');
            p++;
        }
        double d = (double)result;
        if (negative) d = -d;
        return make_number(d);
    }

    /* Binary: 0b or 0B */
    if (remaining >= 2 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        if (p == end || (*p != '0' && *p != '1')) {
            return r8e_from_double(NAN);
        }
        uint64_t result = 0;
        while (p < end && (*p == '0' || *p == '1')) {
            result = result * 2 + (uint64_t)(*p - '0');
            p++;
        }
        double d = (double)result;
        if (negative) d = -d;
        return make_number(d);
    }

    /* Decimal (possibly with fractional part and/or exponent) */
    if (!((*p >= '0' && *p <= '9') || *p == '.')) {
        return r8e_from_double(NAN);
    }

    /* Use strtod for general decimal parsing */
    /* Need null-terminated string */
    size_t parse_len = (size_t)(end - p);
    char stack_buf[64];
    char *heap_buf = NULL;
    const char *parse_str;

    if (parse_len < sizeof(stack_buf)) {
        memcpy(stack_buf, p, parse_len);
        stack_buf[parse_len] = '\0';
        parse_str = stack_buf;
    } else {
        heap_buf = (char *)malloc(parse_len + 1);
        if (!heap_buf) return r8e_from_double(NAN);
        memcpy(heap_buf, p, parse_len);
        heap_buf[parse_len] = '\0';
        parse_str = heap_buf;
    }

    char *endptr;
    errno = 0;
    double d = strtod(parse_str, &endptr);
    /* For parseFloat behavior: stop at first non-numeric char */
    bool any_parsed = (endptr > parse_str);
    free(heap_buf);

    if (!any_parsed) return r8e_from_double(NAN);
    if (negative) d = -d;
    return make_number(d);
}

/*
 * parseInt implementation.
 * Parses an integer from a string with the given radix (2-36, 0 = auto).
 */
R8EValue r8e_parse_int(const char *str, uint32_t len, int radix) {
    if (!str || len == 0) return r8e_from_double(NAN);

    const char *p = str;
    const char *end = str + len;

    /* Skip whitespace */
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v')) {
        p++;
    }
    if (p == end) return r8e_from_double(NAN);

    /* Handle sign */
    bool negative = false;
    if (*p == '+') p++;
    else if (*p == '-') { negative = true; p++; }
    if (p == end) return r8e_from_double(NAN);

    /* Auto-detect radix */
    if (radix == 0) {
        if (*p == '0' && (p + 1) < end) {
            if (p[1] == 'x' || p[1] == 'X') { radix = 16; p += 2; }
            else if (p[1] == 'o' || p[1] == 'O') { radix = 8; p += 2; }
            else if (p[1] == 'b' || p[1] == 'B') { radix = 2; p += 2; }
            else radix = 10;
        } else {
            radix = 10;
        }
    } else if (radix == 16) {
        /* Skip 0x prefix if present */
        if (*p == '0' && (p + 1) < end && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }
    }

    if (radix < 2 || radix > 36) return r8e_from_double(NAN);
    if (p == end) return r8e_from_double(NAN);

    /* Parse digits */
    double result = 0;
    bool any_digits = false;

    while (p < end) {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'z') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') digit = *p - 'A' + 10;
        else break;

        if (digit >= radix) break;

        result = result * radix + digit;
        any_digits = true;
        p++;
    }

    if (!any_digits) return r8e_from_double(NAN);
    if (negative) result = -result;
    return make_number(result);
}

/* =========================================================================
 * MATH OBJECT METHODS
 *
 * These correspond to Math.* in JavaScript.
 * All take and return R8EValue (NaN-boxed numbers).
 * ========================================================================= */

R8EValue r8e_math_abs(R8EValue v) {
    if (R8E_IS_INT32(v)) {
        int32_t i = r8e_get_int32(v);
        if (i >= 0) return v;
        if (i == INT32_MIN) return r8e_from_double(2147483648.0);
        return r8e_from_int32(-i);
    }
    return make_number(fabs(to_double(v)));
}

R8EValue r8e_math_floor(R8EValue v) {
    if (R8E_IS_INT32(v)) return v; /* integers are already floored */
    return make_number(floor(to_double(v)));
}

R8EValue r8e_math_ceil(R8EValue v) {
    if (R8E_IS_INT32(v)) return v;
    return make_number(ceil(to_double(v)));
}

R8EValue r8e_math_round(R8EValue v) {
    if (R8E_IS_INT32(v)) return v;
    double d = to_double(v);
    /* ES2023: Math.round uses "round half to positive infinity" */
    /* This differs from C's round() which uses "round half away from zero" */
    if (d >= 0.0) {
        return make_number(floor(d + 0.5));
    } else {
        /* For negative numbers: -0.5 rounds to -0, not to 0 */
        double r = ceil(d - 0.5);
        /* But ceil(-0.5 - 0.5) = ceil(-1.0) = -1.0, which is wrong for -0.5.
         * ES2023: Math.round(-0.5) = -0 */
        if (d > -0.5 && d < 0.0) return r8e_from_double(-0.0);
        return make_number(r);
    }
}

R8EValue r8e_math_trunc(R8EValue v) {
    if (R8E_IS_INT32(v)) return v;
    return make_number(trunc(to_double(v)));
}

R8EValue r8e_math_max(R8EValue a, R8EValue b) {
    double da = to_double(a);
    double db = to_double(b);
    if (isnan(da) || isnan(db)) return r8e_from_double(NAN);
    /* Handle +0 / -0 */
    if (da == 0.0 && db == 0.0) {
        /* +0 > -0 */
        return (signbit(da) && !signbit(db)) ? b : a;
    }
    return make_number(da >= db ? da : db);
}

R8EValue r8e_math_min(R8EValue a, R8EValue b) {
    double da = to_double(a);
    double db = to_double(b);
    if (isnan(da) || isnan(db)) return r8e_from_double(NAN);
    if (da == 0.0 && db == 0.0) {
        return (!signbit(da) && signbit(db)) ? b : a;
    }
    return make_number(da <= db ? da : db);
}

R8EValue r8e_math_sqrt(R8EValue v) {
    double d = to_double(v);
    if (d < 0) return r8e_from_double(NAN);
    return make_number(sqrt(d));
}

R8EValue r8e_math_pow(R8EValue base, R8EValue exp) {
    return r8e_num_pow(base, exp);
}

R8EValue r8e_math_random(void) {
    /* Simple random: not cryptographically secure */
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
    /* Generate a double in [0, 1) */
    double r = (double)rand() / ((double)RAND_MAX + 1.0);
    return r8e_from_double(r);
}

R8EValue r8e_math_log(R8EValue v) {
    double d = to_double(v);
    if (d < 0) return r8e_from_double(NAN);
    if (d == 0) return r8e_from_double(-INFINITY);
    return r8e_from_double(log(d));
}

R8EValue r8e_math_log2(R8EValue v) {
    double d = to_double(v);
    if (d < 0) return r8e_from_double(NAN);
    if (d == 0) return r8e_from_double(-INFINITY);
    return r8e_from_double(log2(d));
}

R8EValue r8e_math_log10(R8EValue v) {
    double d = to_double(v);
    if (d < 0) return r8e_from_double(NAN);
    if (d == 0) return r8e_from_double(-INFINITY);
    return r8e_from_double(log10(d));
}

R8EValue r8e_math_sin(R8EValue v) {
    return r8e_from_double(sin(to_double(v)));
}

R8EValue r8e_math_cos(R8EValue v) {
    return r8e_from_double(cos(to_double(v)));
}

R8EValue r8e_math_tan(R8EValue v) {
    return r8e_from_double(tan(to_double(v)));
}

R8EValue r8e_math_atan(R8EValue v) {
    return r8e_from_double(atan(to_double(v)));
}

R8EValue r8e_math_atan2(R8EValue y, R8EValue x) {
    return r8e_from_double(atan2(to_double(y), to_double(x)));
}

R8EValue r8e_math_asin(R8EValue v) {
    double d = to_double(v);
    if (d < -1.0 || d > 1.0) return r8e_from_double(NAN);
    return r8e_from_double(asin(d));
}

R8EValue r8e_math_acos(R8EValue v) {
    double d = to_double(v);
    if (d < -1.0 || d > 1.0) return r8e_from_double(NAN);
    return r8e_from_double(acos(d));
}

R8EValue r8e_math_exp(R8EValue v) {
    return r8e_from_double(exp(to_double(v)));
}

R8EValue r8e_math_sign(R8EValue v) {
    if (R8E_IS_INT32(v)) {
        int32_t i = r8e_get_int32(v);
        if (i > 0) return r8e_from_int32(1);
        if (i < 0) return r8e_from_int32(-1);
        return r8e_from_int32(0);
    }
    double d = to_double(v);
    if (isnan(d)) return r8e_from_double(NAN);
    if (d > 0) return r8e_from_int32(1);
    if (d < 0) return r8e_from_int32(-1);
    /* d == 0: preserve sign of zero */
    return r8e_from_double(d); /* +0 or -0 */
}

R8EValue r8e_math_cbrt(R8EValue v) {
    return r8e_from_double(cbrt(to_double(v)));
}

R8EValue r8e_math_hypot(R8EValue a, R8EValue b) {
    return r8e_from_double(hypot(to_double(a), to_double(b)));
}

/*
 * Math.clz32: Count Leading Zeros of the 32-bit integer representation.
 */
R8EValue r8e_math_clz32(R8EValue v) {
    uint32_t n = to_uint32(v);
    if (n == 0) return r8e_from_int32(32);

#if defined(__GNUC__) || defined(__clang__)
    return r8e_from_int32(__builtin_clz(n));
#else
    /* Portable implementation */
    int count = 0;
    if ((n & 0xFFFF0000) == 0) { count += 16; n <<= 16; }
    if ((n & 0xFF000000) == 0) { count += 8;  n <<= 8; }
    if ((n & 0xF0000000) == 0) { count += 4;  n <<= 4; }
    if ((n & 0xC0000000) == 0) { count += 2;  n <<= 2; }
    if ((n & 0x80000000) == 0) { count += 1; }
    return r8e_from_int32(count);
#endif
}

/*
 * Math.fround: Round to nearest float32.
 */
R8EValue r8e_math_fround(R8EValue v) {
    float f = (float)to_double(v);
    return r8e_from_double((double)f);
}

/*
 * Math.imul: 32-bit integer multiplication.
 * Returns the lower 32 bits of the product.
 */
R8EValue r8e_math_imul(R8EValue a, R8EValue b) {
    int32_t ia = to_int32(a);
    int32_t ib = to_int32(b);
    /* Multiply as unsigned, then reinterpret as signed.
     * This avoids signed overflow UB. */
    uint32_t result = (uint32_t)ia * (uint32_t)ib;
    return r8e_from_int32((int32_t)result);
}

/*
 * Math.log1p: log(1 + x), accurate for small x.
 */
R8EValue r8e_math_log1p(R8EValue v) {
    double d = to_double(v);
    if (d < -1.0) return r8e_from_double(NAN);
    if (d == -1.0) return r8e_from_double(-INFINITY);
    return r8e_from_double(log1p(d));
}

/*
 * Math.expm1: exp(x) - 1, accurate for small x.
 */
R8EValue r8e_math_expm1(R8EValue v) {
    return r8e_from_double(expm1(to_double(v)));
}

/*
 * Math.sinh, Math.cosh, Math.tanh
 */
R8EValue r8e_math_sinh(R8EValue v) {
    return r8e_from_double(sinh(to_double(v)));
}

R8EValue r8e_math_cosh(R8EValue v) {
    return r8e_from_double(cosh(to_double(v)));
}

R8EValue r8e_math_tanh(R8EValue v) {
    return r8e_from_double(tanh(to_double(v)));
}

/*
 * Math.asinh, Math.acosh, Math.atanh
 */
R8EValue r8e_math_asinh(R8EValue v) {
    return r8e_from_double(asinh(to_double(v)));
}

R8EValue r8e_math_acosh(R8EValue v) {
    double d = to_double(v);
    if (d < 1.0) return r8e_from_double(NAN);
    return r8e_from_double(acosh(d));
}

R8EValue r8e_math_atanh(R8EValue v) {
    double d = to_double(v);
    if (d < -1.0 || d > 1.0) return r8e_from_double(NAN);
    if (d == -1.0) return r8e_from_double(-INFINITY);
    if (d == 1.0) return r8e_from_double(INFINITY);
    return r8e_from_double(atanh(d));
}

/* =========================================================================
 * MATH CONSTANTS (as R8EValue)
 * ========================================================================= */

R8EValue r8e_math_e(void) {
    return r8e_from_double(2.718281828459045);
}

R8EValue r8e_math_pi(void) {
    return r8e_from_double(3.141592653589793);
}

R8EValue r8e_math_ln2(void) {
    return r8e_from_double(0.6931471805599453);
}

R8EValue r8e_math_ln10(void) {
    return r8e_from_double(2.302585092994046);
}

R8EValue r8e_math_log2e(void) {
    return r8e_from_double(1.4426950408889634);
}

R8EValue r8e_math_log10e(void) {
    return r8e_from_double(0.4342944819032518);
}

R8EValue r8e_math_sqrt2(void) {
    return r8e_from_double(1.4142135623730951);
}

R8EValue r8e_math_sqrt1_2(void) {
    return r8e_from_double(0.7071067811865476);
}

/* =========================================================================
 * SPECIAL VALUE HELPERS
 * ========================================================================= */

R8EValue r8e_nan(void) {
    return r8e_from_double(NAN);
}

R8EValue r8e_infinity(void) {
    return r8e_from_double(INFINITY);
}

R8EValue r8e_neg_infinity(void) {
    return r8e_from_double(-INFINITY);
}

R8EValue r8e_neg_zero(void) {
    return r8e_from_double(-0.0);
}

bool r8e_is_nan(R8EValue v) {
    if (R8E_IS_INT32(v)) return false;
    if (!R8E_IS_DOUBLE(v)) return false;
    return isnan(r8e_get_double(v));
}

bool r8e_is_finite(R8EValue v) {
    if (R8E_IS_INT32(v)) return true;
    if (!R8E_IS_DOUBLE(v)) return false;
    return isfinite(r8e_get_double(v));
}

bool r8e_is_neg_zero(R8EValue v) {
    if (R8E_IS_INT32(v)) return false;
    if (!R8E_IS_DOUBLE(v)) return false;
    double d = r8e_get_double(v);
    return d == 0.0 && signbit(d);
}

/*
 * Number.isInteger(v): returns true if v is a finite integer.
 */
bool r8e_is_integer(R8EValue v) {
    if (R8E_IS_INT32(v)) return true;
    if (!R8E_IS_DOUBLE(v)) return false;
    double d = r8e_get_double(v);
    if (!isfinite(d)) return false;
    return d == trunc(d);
}

/*
 * Number.isSafeInteger(v): true if v is an integer in
 * [-(2^53 - 1), 2^53 - 1].
 */
bool r8e_is_safe_integer(R8EValue v) {
    if (R8E_IS_INT32(v)) return true;
    if (!R8E_IS_DOUBLE(v)) return false;
    double d = r8e_get_double(v);
    if (!isfinite(d)) return false;
    if (d != trunc(d)) return false;
    return fabs(d) <= 9007199254740991.0; /* 2^53 - 1 */
}

/* =========================================================================
 * NUMBER CONSTANTS as R8EValue
 * ========================================================================= */

R8EValue r8e_number_max_value(void) {
    return r8e_from_double(DBL_MAX);
}

R8EValue r8e_number_min_value(void) {
    return r8e_from_double(DBL_MIN);
}

R8EValue r8e_number_max_safe_integer(void) {
    return r8e_from_double(9007199254740991.0);
}

R8EValue r8e_number_min_safe_integer(void) {
    return r8e_from_double(-9007199254740991.0);
}

R8EValue r8e_number_epsilon(void) {
    return r8e_from_double(DBL_EPSILON);
}

R8EValue r8e_number_positive_infinity(void) {
    return r8e_from_double(INFINITY);
}

R8EValue r8e_number_negative_infinity(void) {
    return r8e_from_double(-INFINITY);
}
