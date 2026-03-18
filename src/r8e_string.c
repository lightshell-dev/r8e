/*
 * r8e_string.c - Three-Tier String Engine (UTF-8 + Inline Shorts)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 4 for design rationale.
 *
 * Three-tier string storage:
 *   Tier A: Inline short string (0-6 ASCII bytes in NaN-box payload, ~40-45%)
 *   Tier B: Heap ASCII string (7+ bytes, pure ASCII, ~47%)
 *   Tier C: Heap Unicode string (non-ASCII, UTF-8, ~8.5%)
 *
 * Key design decisions (from experiments):
 *   - 91.5% of strings are pure ASCII (EXP-05)
 *   - 50.3% are 8 bytes or shorter
 *   - Only 0.002% contain chars above U+FFFF
 *   - FNV-1a hash cached in 16-byte header
 *   - Lazy offset table for random access on non-ASCII strings
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
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

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

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

/* Forward declarations */
typedef struct R8EContext R8EContext;

/*
 * Minimal context stub for allocation. The real R8EContext will have arenas,
 * GC state, atom tables, etc. For now we use malloc/free directly.
 */
struct R8EContext {
    void *arena;  /* placeholder */
};

/* =========================================================================
 * String flags (Section 4.3)
 * ========================================================================= */

#define R8E_STR_IS_ASCII    0x01  /* all bytes are 0x00-0x7F */
#define R8E_STR_IS_INTERNED 0x02  /* stored in atom table */
#define R8E_STR_IS_ROPE     0x04  /* rope node (deferred concat) */
#define R8E_STR_GC_MARK     0x08  /* GC mark bit */
#define R8E_STR_ON_SUSPECT  0x10  /* on GC suspect list */
#define R8E_STR_HAS_OFFSETS 0x20  /* lazy offset table built */
#define R8E_STR_EXTERNAL    0x40  /* data points to external buffer */

/* =========================================================================
 * R8EString - Heap string (Tier B and Tier C) (Section 4.3)
 *
 * 16-byte header followed by UTF-8 data via flexible array member.
 * For ASCII strings, byte_length == char_length.
 * For Unicode strings, char_length counts UTF-16 code units (for JS compat).
 * ========================================================================= */

typedef struct R8EString {
    uint32_t flags;        /* IS_ASCII, IS_INTERNED, IS_ROPE, GC bits */
    uint32_t hash;         /* precomputed FNV-1a */
    uint32_t byte_length;  /* UTF-8 byte count */
    uint32_t char_length;  /* UTF-16 code unit count (== byte_length if ASCII) */
    char     data[];       /* UTF-8 bytes, null-terminated for C compat */
} R8EString;

/* =========================================================================
 * R8EOffsetTable - Lazy offset table for random access on non-ASCII strings
 * (Section 4.4)
 *
 * Built on first bracket-indexing of a non-ASCII string.
 * Maps UTF-16 code unit index -> byte offset in the UTF-8 data.
 * ========================================================================= */

typedef struct R8EOffsetTable {
    uint32_t *offsets;  /* offsets[i] = byte position of i-th UTF-16 code unit */
    uint32_t  length;   /* number of entries (== char_length) */
} R8EOffsetTable;

/*
 * Offset tables are stored in a side table keyed by string pointer,
 * avoiding any bloat to the 16-byte R8EString header.
 * Linear probing hash table, sized for typical workloads.
 */
#define R8E_OFFSET_TABLE_CAP 256

typedef struct {
    R8EString      *key;
    R8EOffsetTable  table;
} R8EOffsetEntry;

static R8EOffsetEntry r8e_offset_tables[R8E_OFFSET_TABLE_CAP];

/* =========================================================================
 * Memory allocation helpers
 *
 * These wrap malloc/free and will be replaced with arena allocation.
 * ========================================================================= */

static inline void *r8e_alloc(R8EContext *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static inline void *r8e_realloc(R8EContext *ctx, void *ptr, size_t size) {
    (void)ctx;
    return realloc(ptr, size);
}

static inline void r8e_free(R8EContext *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

/* =========================================================================
 * Portable memmem
 *
 * Some platforms (MSVC, older macOS without _GNU_SOURCE) lack memmem.
 * We provide our own with a r8e_ prefix to avoid symbol conflicts.
 * ========================================================================= */

static const void *r8e_memmem(const void *haystack, size_t haystacklen,
                               const void *needle, size_t needlelen) {
    if (needlelen == 0) return haystack;
    if (haystacklen < needlelen) return NULL;

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    const uint8_t *end = h + haystacklen - needlelen + 1;
    uint8_t first = n[0];

    for (; h < end; h++) {
        if (*h == first && memcmp(h, n, needlelen) == 0)
            return h;
    }
    return NULL;
}

/* =========================================================================
 * FNV-1a Hash (Section 4.3, 4.5)
 *
 * 32-bit FNV-1a: fast, well-distributed, no lookup table needed.
 * ========================================================================= */

#define R8E_FNV_OFFSET_BASIS 0x811C9DC5u
#define R8E_FNV_PRIME        0x01000193u

static uint32_t r8e_fnv1a(const char *data, uint32_t len) {
    uint32_t hash = R8E_FNV_OFFSET_BASIS;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= R8E_FNV_PRIME;
    }
    return hash;
}

/* =========================================================================
 * UTF-8 Utilities (Section 4, part d)
 * ========================================================================= */

/**
 * Decode one UTF-8 codepoint from `bytes`.
 *
 * @param bytes         pointer to UTF-8 data
 * @param len           remaining bytes available
 * @param codepoint_out receives the decoded codepoint (U+FFFD on error)
 * @return              number of bytes consumed (1-4), or 0 if len == 0
 */
int r8e_utf8_decode(const uint8_t *bytes, uint32_t len, uint32_t *codepoint_out) {
    if (len == 0) {
        *codepoint_out = 0xFFFD;
        return 0;
    }

    uint8_t b0 = bytes[0];

    /* Single byte: 0xxxxxxx */
    if (b0 < 0x80) {
        *codepoint_out = b0;
        return 1;
    }

    /* Two bytes: 110xxxxx 10xxxxxx */
    if ((b0 & 0xE0) == 0xC0) {
        if (len < 2 || (bytes[1] & 0xC0) != 0x80) {
            *codepoint_out = 0xFFFD;
            return 1;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6)
                    | ((uint32_t)(bytes[1] & 0x3F));
        if (cp < 0x80) { /* overlong */
            *codepoint_out = 0xFFFD;
            return 2;
        }
        *codepoint_out = cp;
        return 2;
    }

    /* Three bytes: 1110xxxx 10xxxxxx 10xxxxxx */
    if ((b0 & 0xF0) == 0xE0) {
        if (len < 3 || (bytes[1] & 0xC0) != 0x80
                     || (bytes[2] & 0xC0) != 0x80) {
            *codepoint_out = 0xFFFD;
            return 1;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12)
                    | ((uint32_t)(bytes[1] & 0x3F) << 6)
                    | ((uint32_t)(bytes[2] & 0x3F));
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) { /* overlong or surrogate */
            *codepoint_out = 0xFFFD;
            return 3;
        }
        *codepoint_out = cp;
        return 3;
    }

    /* Four bytes: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if ((b0 & 0xF8) == 0xF0) {
        if (len < 4 || (bytes[1] & 0xC0) != 0x80
                     || (bytes[2] & 0xC0) != 0x80
                     || (bytes[3] & 0xC0) != 0x80) {
            *codepoint_out = 0xFFFD;
            return 1;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18)
                    | ((uint32_t)(bytes[1] & 0x3F) << 12)
                    | ((uint32_t)(bytes[2] & 0x3F) << 6)
                    | ((uint32_t)(bytes[3] & 0x3F));
        if (cp < 0x10000 || cp > 0x10FFFF) { /* overlong or out of range */
            *codepoint_out = 0xFFFD;
            return 4;
        }
        *codepoint_out = cp;
        return 4;
    }

    /* Invalid leading byte */
    *codepoint_out = 0xFFFD;
    return 1;
}

/**
 * Encode one codepoint to UTF-8.
 *
 * @param codepoint  Unicode codepoint (0-0x10FFFF)
 * @param buf        output buffer (must have room for 4 bytes)
 * @return           number of bytes written (1-4)
 */
int r8e_utf8_encode(uint32_t codepoint, uint8_t *buf) {
    if (codepoint < 0x80) {
        buf[0] = (uint8_t)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        buf[0] = (uint8_t)(0xC0 | (codepoint >> 6));
        buf[1] = (uint8_t)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            /* Surrogate: emit U+FFFD */
            buf[0] = 0xEF; buf[1] = 0xBF; buf[2] = 0xBD;
            return 3;
        }
        buf[0] = (uint8_t)(0xE0 | (codepoint >> 12));
        buf[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (uint8_t)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    if (codepoint <= 0x10FFFF) {
        buf[0] = (uint8_t)(0xF0 | (codepoint >> 18));
        buf[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (uint8_t)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    /* Out of range: emit U+FFFD */
    buf[0] = 0xEF; buf[1] = 0xBF; buf[2] = 0xBD;
    return 3;
}

/**
 * Count UTF-16 code units in a UTF-8 byte sequence.
 * Codepoints above U+FFFF count as 2 (surrogate pair in UTF-16).
 */
uint32_t r8e_utf8_strlen(const char *data, uint32_t byte_len) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + byte_len;
    uint32_t count = 0;

    while (p < end) {
        uint32_t cp;
        int n = r8e_utf8_decode(p, (uint32_t)(end - p), &cp);
        if (n == 0) break;
        p += n;
        count++;
    }
    return count;
}

/**
 * Check if a byte sequence is pure ASCII.
 * Processes 8 bytes at a time for throughput on the common case.
 */
static bool r8e_is_ascii(const char *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i = 0;

    /* Process 8 bytes at a time */
    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        memcpy(&chunk, p + i, 8);
        if (chunk & 0x8080808080808080ULL)
            return false;
    }
    /* Remainder */
    for (; i < len; i++) {
        if (p[i] & 0x80)
            return false;
    }
    return true;
}

/**
 * Advance through UTF-8 data by a given number of UTF-16 code units.
 * Returns the byte offset after advancing.
 */
static uint32_t r8e_utf8_advance(const uint8_t *data, uint32_t byte_len,
                                  uint32_t start_byte, uint32_t n_units) {
    const uint8_t *p = data + start_byte;
    const uint8_t *end = data + byte_len;
    uint32_t units = 0;

    while (p < end && units < n_units) {
        uint32_t cp;
        int nb = r8e_utf8_decode(p, (uint32_t)(end - p), &cp);
        if (nb == 0) break;
        p += nb;
        units += (cp > 0xFFFF) ? 2 : 1;
    }
    return (uint32_t)(p - data);
}

/* =========================================================================
 * Inline Short Strings (Tier A) - Section 2.5
 *
 * Encode 0-6 ASCII chars directly into the NaN-box payload.
 *
 * Layout within the 64-bit value:
 *   Bits [63:48] = 0xFFFD (inline string tag)
 *   Bits [47:45] = length (0-6)
 *   Bits [44:0]  = packed characters (7 bits each, up to 6 chars)
 *     char 0 at bits [44:38]
 *     char 1 at bits [37:31]
 *     char 2 at bits [30:24]
 *     char 3 at bits [23:17]
 *     char 4 at bits [16:10]
 *     char 5 at bits [9:3]
 *     bits [2:0] unused
 * ========================================================================= */

#define R8E_INLINE_STR_MAX_LEN 6

static inline bool r8e_can_inline_str(const char *s, uint32_t len) {
    if (len > R8E_INLINE_STR_MAX_LEN) return false;
    for (uint32_t i = 0; i < len; i++) {
        if ((uint8_t)s[i] > 127) return false;
    }
    return true;
}

/**
 * Create an inline short string NaN-boxed value.
 * Caller must ensure len <= R8E_INLINE_STR_MAX_LEN and all chars are ASCII.
 */
R8EValue r8e_inline_str_new(const char *s, uint32_t len) {
    uint64_t v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)len << 45);
    for (uint32_t i = 0; i < len; i++) {
        v |= ((uint64_t)(uint8_t)s[i] << (38 - i * 7));
    }
    return v;
}

/**
 * Extract length from an inline short string.
 */
static inline uint32_t r8e_inline_str_len(R8EValue v) {
    return (uint32_t)((v >> 45) & 0x07);
}

/**
 * Extract characters from an inline short string into a buffer.
 * Buffer must have room for at least R8E_INLINE_STR_MAX_LEN + 1 bytes.
 * Returns the length.
 */
uint32_t r8e_inline_str_extract(R8EValue v, char *buf) {
    uint32_t len = r8e_inline_str_len(v);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
    }
    buf[len] = '\0';
    return len;
}

/**
 * Get the hash of an inline string (computed on the fly since it has no
 * header). Inline strings are short enough that this is fast.
 */
uint32_t r8e_inline_str_hash(R8EValue v) {
    char buf[R8E_INLINE_STR_MAX_LEN + 1];
    uint32_t len = r8e_inline_str_extract(v, buf);
    return r8e_fnv1a(buf, len);
}

/* =========================================================================
 * Heap String Creation (Tier B and Tier C)
 * ========================================================================= */

/**
 * Allocate and initialize a heap string.
 * Auto-detects ASCII. Computes FNV-1a hash.
 *
 * @param ctx   engine context
 * @param data  UTF-8 string data (need not be null-terminated)
 * @param len   byte length
 * @return      newly allocated R8EString, or NULL on OOM
 */
R8EString *r8e_string_new(R8EContext *ctx, const char *data, uint32_t len) {
    R8EString *s = (R8EString *)r8e_alloc(ctx, sizeof(R8EString) + len + 1);
    if (!s) return NULL;

    bool ascii = r8e_is_ascii(data, len);

    s->flags = ascii ? R8E_STR_IS_ASCII : 0;
    s->hash = r8e_fnv1a(data, len);
    s->byte_length = len;
    s->char_length = ascii ? len : r8e_utf8_strlen(data, len);

    memcpy(s->data, data, len);
    s->data[len] = '\0';

    return s;
}

/**
 * Create a heap string known to be pure ASCII. Skips the ASCII detection scan.
 */
R8EString *r8e_string_new_ascii(R8EContext *ctx, const char *data, uint32_t len) {
    R8EString *s = (R8EString *)r8e_alloc(ctx, sizeof(R8EString) + len + 1);
    if (!s) return NULL;

    s->flags = R8E_STR_IS_ASCII;
    s->hash = r8e_fnv1a(data, len);
    s->byte_length = len;
    s->char_length = len;

    memcpy(s->data, data, len);
    s->data[len] = '\0';

    return s;
}

/**
 * Create a string from a null-terminated C string.
 */
R8EString *r8e_string_from_cstr(R8EContext *ctx, const char *cstr) {
    return r8e_string_new(ctx, cstr, (uint32_t)strlen(cstr));
}

/**
 * Free a heap string and its associated offset table (if any).
 */
void r8e_string_free(R8EContext *ctx, R8EString *s) {
    if (!s) return;
    if (s->flags & R8E_STR_HAS_OFFSETS) {
        uint32_t slot = (uint32_t)((uintptr_t)s % R8E_OFFSET_TABLE_CAP);
        for (uint32_t i = 0; i < R8E_OFFSET_TABLE_CAP; i++) {
            uint32_t idx = (slot + i) % R8E_OFFSET_TABLE_CAP;
            if (r8e_offset_tables[idx].key == s) {
                r8e_free(ctx, r8e_offset_tables[idx].table.offsets);
                r8e_offset_tables[idx].key = NULL;
                r8e_offset_tables[idx].table.offsets = NULL;
                r8e_offset_tables[idx].table.length = 0;
                break;
            }
        }
    }
    r8e_free(ctx, s);
}

/**
 * Create a NaN-boxed value from a string.
 * Strings that qualify for inline storage (0-6 ASCII chars) are stored
 * entirely in the NaN-box payload with zero heap allocation.
 * Others are heap-allocated and stored as pointer-tagged values.
 */
R8EValue r8e_value_from_string(R8EContext *ctx, const char *data, uint32_t len) {
    if (r8e_can_inline_str(data, len)) {
        return r8e_inline_str_new(data, len);
    }
    R8EString *s = r8e_string_new(ctx, data, len);
    if (!s) return R8E_UNDEFINED;
    return r8e_from_pointer(s);
}

/* =========================================================================
 * String Comparison and Hashing
 * ========================================================================= */

/**
 * Compare two heap strings for equality.
 * Hash comparison provides a fast negative check.
 */
bool r8e_string_equal(const R8EString *a, const R8EString *b) {
    if (a == b) return true;
    if (a->hash != b->hash) return false;
    if (a->byte_length != b->byte_length) return false;
    return memcmp(a->data, b->data, a->byte_length) == 0;
}

/**
 * Compare a heap string with raw bytes.
 */
bool r8e_string_equal_raw(const R8EString *s, const char *data, uint32_t len) {
    if (s->byte_length != len) return false;
    return memcmp(s->data, data, len) == 0;
}

/**
 * Compare two R8EValues that are strings (inline or heap).
 * Both values must be known to be strings.
 */
bool r8e_value_string_equal(R8EValue a, R8EValue b) {
    if (a == b) return true;

    char buf_a[R8E_INLINE_STR_MAX_LEN + 1];
    char buf_b[R8E_INLINE_STR_MAX_LEN + 1];
    const char *data_a, *data_b;
    uint32_t len_a, len_b;

    if (R8E_IS_INLINE_STR(a)) {
        len_a = r8e_inline_str_extract(a, buf_a);
        data_a = buf_a;
    } else {
        R8EString *sa = (R8EString *)r8e_get_pointer(a);
        data_a = sa->data;
        len_a = sa->byte_length;
    }

    if (R8E_IS_INLINE_STR(b)) {
        len_b = r8e_inline_str_extract(b, buf_b);
        data_b = buf_b;
    } else {
        R8EString *sb = (R8EString *)r8e_get_pointer(b);
        data_b = sb->data;
        len_b = sb->byte_length;
    }

    if (len_a != len_b) return false;
    return memcmp(data_a, data_b, len_a) == 0;
}

/* =========================================================================
 * Lazy Offset Table (Section 4.4)
 *
 * Built on first random access (bracket indexing) of a non-ASCII string.
 * Maps UTF-16 code unit index to byte offset. Stored in a side table
 * keyed by string pointer, keeping R8EString at exactly 16 bytes.
 * ========================================================================= */

static R8EOffsetTable *r8e_offset_table_find(R8EString *s) {
    uint32_t slot = (uint32_t)((uintptr_t)s % R8E_OFFSET_TABLE_CAP);
    for (uint32_t i = 0; i < R8E_OFFSET_TABLE_CAP; i++) {
        uint32_t idx = (slot + i) % R8E_OFFSET_TABLE_CAP;
        if (r8e_offset_tables[idx].key == s)
            return &r8e_offset_tables[idx].table;
        if (r8e_offset_tables[idx].key == NULL)
            return NULL;
    }
    return NULL;
}

static R8EOffsetTable *r8e_offset_table_build(R8EContext *ctx, R8EString *s) {
    if (s->flags & R8E_STR_IS_ASCII) return NULL;

    R8EOffsetTable *existing = r8e_offset_table_find(s);
    if (existing) return existing;

    uint32_t *offsets = (uint32_t *)r8e_alloc(ctx,
        sizeof(uint32_t) * s->char_length);
    if (!offsets) return NULL;

    const uint8_t *p = (const uint8_t *)s->data;
    const uint8_t *end = p + s->byte_length;
    uint32_t idx = 0;

    while (p < end && idx < s->char_length) {
        uint32_t cp;
        uint32_t byte_off = (uint32_t)(p - (const uint8_t *)s->data);
        int n = r8e_utf8_decode(p, (uint32_t)(end - p), &cp);
        if (n == 0) break;

        if (cp > 0xFFFF) {
            /* Supplementary plane: two UTF-16 code units map to same byte offset */
            if (idx < s->char_length) offsets[idx++] = byte_off;
            if (idx < s->char_length) offsets[idx++] = byte_off;
        } else {
            offsets[idx++] = byte_off;
        }
        p += n;
    }

    /* Insert into side table */
    uint32_t slot = (uint32_t)((uintptr_t)s % R8E_OFFSET_TABLE_CAP);
    for (uint32_t i = 0; i < R8E_OFFSET_TABLE_CAP; i++) {
        uint32_t sidx = (slot + i) % R8E_OFFSET_TABLE_CAP;
        if (r8e_offset_tables[sidx].key == NULL) {
            r8e_offset_tables[sidx].key = s;
            r8e_offset_tables[sidx].table.offsets = offsets;
            r8e_offset_tables[sidx].table.length = idx;
            s->flags |= R8E_STR_HAS_OFFSETS;
            return &r8e_offset_tables[sidx].table;
        }
    }

    /* Side table full: degrade gracefully */
    r8e_free(ctx, offsets);
    return NULL;
}

/**
 * Get the byte offset for a given UTF-16 code unit index in a string.
 * For ASCII strings: identity mapping (index == byte offset).
 * For Unicode strings: lazily builds the offset table.
 *
 * @return byte offset, or UINT32_MAX if out of range
 */
static uint32_t r8e_string_char_to_byte(R8EContext *ctx, R8EString *s,
                                         uint32_t index) {
    if (index >= s->char_length) return UINT32_MAX;
    if (s->flags & R8E_STR_IS_ASCII) return index;

    R8EOffsetTable *ot = r8e_offset_table_build(ctx, s);
    if (!ot || index >= ot->length) return UINT32_MAX;
    return ot->offsets[index];
}

/**
 * Convert a char index range to byte offsets.
 */
static void r8e_string_char_range_to_bytes(R8EContext *ctx, R8EString *s,
                                            uint32_t start_idx,
                                            uint32_t end_idx,
                                            uint32_t *start_byte,
                                            uint32_t *end_byte) {
    if (s->flags & R8E_STR_IS_ASCII) {
        *start_byte = start_idx;
        *end_byte = end_idx;
        return;
    }

    R8EOffsetTable *ot = r8e_offset_table_find(s);
    if (ot && ot->length > 0) {
        *start_byte = (start_idx < ot->length)
            ? ot->offsets[start_idx] : s->byte_length;
        *end_byte = (end_idx < ot->length)
            ? ot->offsets[end_idx] : s->byte_length;
        return;
    }

    /* Sequential scan fallback */
    *start_byte = r8e_utf8_advance((const uint8_t *)s->data,
                                    s->byte_length, 0, start_idx);
    *end_byte = r8e_utf8_advance((const uint8_t *)s->data,
                                  s->byte_length, *start_byte,
                                  end_idx - start_idx);
}

/* =========================================================================
 * String.prototype Methods (ES2023)
 *
 * These operate on heap R8EString pointers. The interpreter layer handles
 * inline strings by extracting or promoting them before calling these.
 * ========================================================================= */

/* ---- Clamping helpers ---- */

static inline int32_t r8e_clamp_index(int32_t idx, int32_t len) {
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    return idx;
}

static inline int32_t r8e_resolve_relative_index(int32_t idx, int32_t len) {
    if (idx < 0) idx += len;
    return idx;
}

/* -------------------------------------------------------------------------
 * charAt(index) -> R8EString*
 * Returns a single-character string, or empty string if out of range.
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_char_at(R8EContext *ctx, R8EString *s, int32_t index) {
    if (index < 0 || (uint32_t)index >= s->char_length)
        return r8e_string_new_ascii(ctx, "", 0);

    if (s->flags & R8E_STR_IS_ASCII)
        return r8e_string_new_ascii(ctx, &s->data[index], 1);

    uint32_t byte_off = r8e_string_char_to_byte(ctx, s, (uint32_t)index);
    if (byte_off == UINT32_MAX)
        return r8e_string_new_ascii(ctx, "", 0);

    uint32_t cp;
    const uint8_t *p = (const uint8_t *)s->data + byte_off;
    int n = r8e_utf8_decode(p, s->byte_length - byte_off, &cp);

    if (cp > 0xFFFF) {
        /*
         * Supplementary codepoint: charAt returns the individual UTF-16
         * surrogate. Determine which one based on offset table.
         */
        R8EOffsetTable *ot = r8e_offset_table_find(s);
        uint32_t surrogate;
        if (ot && (uint32_t)index + 1 < ot->length &&
            ot->offsets[index] == ot->offsets[(uint32_t)index + 1]) {
            surrogate = 0xD800 + ((cp - 0x10000) >> 10); /* high */
        } else {
            surrogate = 0xDC00 + ((cp - 0x10000) & 0x3FF); /* low */
        }
        uint8_t buf[4];
        int sn = r8e_utf8_encode(surrogate, buf);
        return r8e_string_new(ctx, (const char *)buf, (uint32_t)sn);
    }

    return r8e_string_new(ctx, (const char *)p, (uint32_t)n);
}

/* -------------------------------------------------------------------------
 * charCodeAt(index) -> int32_t (UTF-16 code unit value)
 * Returns -1 if out of range (caller converts to NaN).
 * ------------------------------------------------------------------------- */
int32_t r8e_string_char_code_at(R8EContext *ctx, R8EString *s, int32_t index) {
    if (index < 0 || (uint32_t)index >= s->char_length) return -1;

    if (s->flags & R8E_STR_IS_ASCII)
        return (uint8_t)s->data[index];

    uint32_t byte_off = r8e_string_char_to_byte(ctx, s, (uint32_t)index);
    if (byte_off == UINT32_MAX) return -1;

    uint32_t cp;
    r8e_utf8_decode((const uint8_t *)s->data + byte_off,
                    s->byte_length - byte_off, &cp);

    if (cp <= 0xFFFF) return (int32_t)cp;

    /* Return appropriate surrogate half */
    R8EOffsetTable *ot = r8e_offset_table_find(s);
    if (ot && (uint32_t)index + 1 < ot->length &&
        ot->offsets[index] == ot->offsets[(uint32_t)index + 1]) {
        return (int32_t)(0xD800 + ((cp - 0x10000) >> 10));
    }
    return (int32_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
}

/* -------------------------------------------------------------------------
 * codePointAt(index) -> int32_t (full Unicode codepoint)
 * Returns -1 if out of range.
 * ------------------------------------------------------------------------- */
int32_t r8e_string_code_point_at(R8EContext *ctx, R8EString *s, int32_t index) {
    if (index < 0 || (uint32_t)index >= s->char_length) return -1;

    if (s->flags & R8E_STR_IS_ASCII)
        return (uint8_t)s->data[index];

    uint32_t byte_off = r8e_string_char_to_byte(ctx, s, (uint32_t)index);
    if (byte_off == UINT32_MAX) return -1;

    uint32_t cp;
    r8e_utf8_decode((const uint8_t *)s->data + byte_off,
                    s->byte_length - byte_off, &cp);
    return (int32_t)cp;
}

/* -------------------------------------------------------------------------
 * indexOf(search, fromIndex) -> int32_t
 * Returns -1 if not found.
 * ------------------------------------------------------------------------- */
int32_t r8e_string_index_of(R8EContext *ctx, R8EString *s,
                             R8EString *search, int32_t from_index) {
    (void)ctx;

    if (search->byte_length == 0)
        return r8e_clamp_index(from_index, (int32_t)s->char_length);

    if (search->byte_length > s->byte_length) return -1;

    int32_t fi = r8e_clamp_index(from_index, (int32_t)s->char_length);

    if (s->flags & R8E_STR_IS_ASCII) {
        const char *haystack = s->data + fi;
        const char *result = (const char *)r8e_memmem(haystack,
            s->byte_length - (uint32_t)fi,
            search->data, search->byte_length);
        if (!result) return -1;
        return (int32_t)(result - s->data);
    }

    /* Non-ASCII: convert char index to byte offset, then search bytes */
    uint32_t byte_start = r8e_utf8_advance((const uint8_t *)s->data,
                                            s->byte_length, 0, (uint32_t)fi);
    const char *result = (const char *)r8e_memmem(s->data + byte_start,
        s->byte_length - byte_start,
        search->data, search->byte_length);
    if (!result) return -1;

    /* Convert byte offset back to char index */
    uint32_t byte_off = (uint32_t)(result - s->data);
    return (int32_t)r8e_utf8_strlen(s->data, byte_off);
}

/* -------------------------------------------------------------------------
 * lastIndexOf(search, fromIndex) -> int32_t
 * Returns -1 if not found.
 * ------------------------------------------------------------------------- */
int32_t r8e_string_last_index_of(R8EContext *ctx, R8EString *s,
                                  R8EString *search, int32_t from_index) {
    (void)ctx;

    if (search->byte_length == 0)
        return r8e_clamp_index(from_index, (int32_t)s->char_length);

    if (search->byte_length > s->byte_length) return -1;
    if (from_index < 0) return -1;
    if ((uint32_t)from_index > s->char_length)
        from_index = (int32_t)s->char_length;

    if (s->flags & R8E_STR_IS_ASCII) {
        uint32_t end_byte = (uint32_t)from_index;
        if (end_byte + search->byte_length > s->byte_length)
            end_byte = s->byte_length - search->byte_length;
        for (int32_t i = (int32_t)end_byte; i >= 0; i--) {
            if (memcmp(s->data + i, search->data, search->byte_length) == 0)
                return i;
        }
        return -1;
    }

    /* Non-ASCII: convert from_index to byte offset, search backward */
    uint32_t end_byte = r8e_utf8_advance((const uint8_t *)s->data,
                                          s->byte_length, 0,
                                          (uint32_t)from_index);
    if (end_byte + search->byte_length > s->byte_length)
        end_byte = s->byte_length - search->byte_length;

    for (int32_t i = (int32_t)end_byte; i >= 0; i--) {
        if (memcmp(s->data + i, search->data, search->byte_length) == 0)
            return (int32_t)r8e_utf8_strlen(s->data, (uint32_t)i);
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * includes(search, fromIndex) -> bool
 * ------------------------------------------------------------------------- */
bool r8e_string_includes(R8EContext *ctx, R8EString *s,
                          R8EString *search, int32_t from_index) {
    return r8e_string_index_of(ctx, s, search, from_index) >= 0;
}

/* -------------------------------------------------------------------------
 * startsWith(search, position) -> bool
 * ------------------------------------------------------------------------- */
bool r8e_string_starts_with(R8EContext *ctx, R8EString *s,
                             R8EString *search, int32_t position) {
    (void)ctx;
    if (position < 0) position = 0;

    if (s->flags & R8E_STR_IS_ASCII) {
        if ((uint32_t)position + search->byte_length > s->byte_length)
            return false;
        return memcmp(s->data + position, search->data,
                      search->byte_length) == 0;
    }

    uint32_t byte_pos = r8e_utf8_advance((const uint8_t *)s->data,
                                          s->byte_length, 0,
                                          (uint32_t)position);
    if (byte_pos + search->byte_length > s->byte_length) return false;
    return memcmp(s->data + byte_pos, search->data,
                  search->byte_length) == 0;
}

/* -------------------------------------------------------------------------
 * endsWith(search, endPosition) -> bool
 * endPosition defaults to char_length (caller passes it).
 * ------------------------------------------------------------------------- */
bool r8e_string_ends_with(R8EContext *ctx, R8EString *s,
                           R8EString *search, int32_t end_position) {
    (void)ctx;
    if (end_position < 0) return false;
    if ((uint32_t)end_position > s->char_length)
        end_position = (int32_t)s->char_length;

    int32_t start = end_position - (int32_t)search->char_length;
    if (start < 0) return false;

    if (s->flags & R8E_STR_IS_ASCII)
        return memcmp(s->data + start, search->data,
                      search->byte_length) == 0;

    uint32_t byte_start = r8e_utf8_advance((const uint8_t *)s->data,
                                            s->byte_length, 0,
                                            (uint32_t)start);
    uint32_t byte_end = r8e_utf8_advance((const uint8_t *)s->data,
                                          s->byte_length, byte_start,
                                          search->char_length);
    uint32_t range = byte_end - byte_start;
    if (range != search->byte_length) return false;
    return memcmp(s->data + byte_start, search->data,
                  search->byte_length) == 0;
}

/* -------------------------------------------------------------------------
 * slice(start, end) -> R8EString*
 * Supports negative indices.
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_slice(R8EContext *ctx, R8EString *s,
                             int32_t start, int32_t end) {
    int32_t len = (int32_t)s->char_length;

    if (start < 0) { start += len; if (start < 0) start = 0; }
    if (end < 0)   { end += len;   if (end < 0) end = 0; }
    if (start > len) start = len;
    if (end > len)   end = len;
    if (start >= end) return r8e_string_new_ascii(ctx, "", 0);

    if (s->flags & R8E_STR_IS_ASCII)
        return r8e_string_new_ascii(ctx, s->data + start,
                                    (uint32_t)(end - start));

    uint32_t byte_start, byte_end;
    r8e_string_char_range_to_bytes(ctx, s, (uint32_t)start, (uint32_t)end,
                                    &byte_start, &byte_end);
    return r8e_string_new(ctx, s->data + byte_start,
                           byte_end - byte_start);
}

/* -------------------------------------------------------------------------
 * substring(start, end) -> R8EString*
 * Clamps to [0, len], swaps if start > end. No negative index support.
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_substring(R8EContext *ctx, R8EString *s,
                                 int32_t start, int32_t end) {
    int32_t len = (int32_t)s->char_length;
    start = r8e_clamp_index(start, len);
    end = r8e_clamp_index(end, len);
    if (start > end) { int32_t t = start; start = end; end = t; }

    if (s->flags & R8E_STR_IS_ASCII)
        return r8e_string_new_ascii(ctx, s->data + start,
                                    (uint32_t)(end - start));

    uint32_t byte_start, byte_end;
    r8e_string_char_range_to_bytes(ctx, s, (uint32_t)start, (uint32_t)end,
                                    &byte_start, &byte_end);
    return r8e_string_new(ctx, s->data + byte_start,
                           byte_end - byte_start);
}

/* -------------------------------------------------------------------------
 * substr(start, length) -> R8EString*   (legacy, Annex B)
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_substr(R8EContext *ctx, R8EString *s,
                              int32_t start, int32_t length) {
    int32_t slen = (int32_t)s->char_length;
    if (start < 0) { start += slen; if (start < 0) start = 0; }
    if (start >= slen || length <= 0)
        return r8e_string_new_ascii(ctx, "", 0);

    int32_t end = start + length;
    if (end > slen) end = slen;
    return r8e_string_slice(ctx, s, start, end);
}

/* -------------------------------------------------------------------------
 * toUpperCase() -> R8EString*
 * Handles ASCII case mapping. Non-ASCII chars pass through unchanged.
 * Full Unicode case mapping would require ICU-level tables (~100KB+).
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_to_upper(R8EContext *ctx, R8EString *s) {
    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + s->byte_length + 1);
    if (!r) return NULL;

    r->flags = s->flags & ~R8E_STR_IS_INTERNED;
    r->byte_length = s->byte_length;
    r->char_length = s->char_length;

    memcpy(r->data, s->data, s->byte_length);
    r->data[s->byte_length] = '\0';

    for (uint32_t i = 0; i < r->byte_length; i++) {
        uint8_t c = (uint8_t)r->data[i];
        if (c >= 'a' && c <= 'z')
            r->data[i] = (char)(c - 32);
    }

    r->hash = r8e_fnv1a(r->data, r->byte_length);
    return r;
}

/* -------------------------------------------------------------------------
 * toLowerCase() -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_to_lower(R8EContext *ctx, R8EString *s) {
    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + s->byte_length + 1);
    if (!r) return NULL;

    r->flags = s->flags & ~R8E_STR_IS_INTERNED;
    r->byte_length = s->byte_length;
    r->char_length = s->char_length;

    memcpy(r->data, s->data, s->byte_length);
    r->data[s->byte_length] = '\0';

    for (uint32_t i = 0; i < r->byte_length; i++) {
        uint8_t c = (uint8_t)r->data[i];
        if (c >= 'A' && c <= 'Z')
            r->data[i] = (char)(c + 32);
    }

    r->hash = r8e_fnv1a(r->data, r->byte_length);
    return r;
}

/* -------------------------------------------------------------------------
 * Whitespace classification (ECMAScript definition)
 * Covers: WhiteSpace (11.2) + LineTerminator (11.3)
 * ------------------------------------------------------------------------- */
static bool r8e_is_whitespace(uint32_t cp) {
    switch (cp) {
    case 0x0009: /* CHARACTER TABULATION */
    case 0x000A: /* LINE FEED */
    case 0x000B: /* LINE TABULATION */
    case 0x000C: /* FORM FEED */
    case 0x000D: /* CARRIAGE RETURN */
    case 0x0020: /* SPACE */
    case 0x00A0: /* NO-BREAK SPACE */
    case 0x1680: /* OGHAM SPACE MARK */
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: /* EN/EM spaces */
    case 0x2004: case 0x2005: case 0x2006: case 0x2007:
    case 0x2008: case 0x2009: case 0x200A:
    case 0x2028: /* LINE SEPARATOR */
    case 0x2029: /* PARAGRAPH SEPARATOR */
    case 0x202F: /* NARROW NO-BREAK SPACE */
    case 0x205F: /* MEDIUM MATHEMATICAL SPACE */
    case 0x3000: /* IDEOGRAPHIC SPACE */
    case 0xFEFF: /* ZERO WIDTH NO-BREAK SPACE (BOM) */
        return true;
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------
 * trim() -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_trim(R8EContext *ctx, R8EString *s) {
    if (s->byte_length == 0)
        return r8e_string_new_ascii(ctx, "", 0);

    if (s->flags & R8E_STR_IS_ASCII) {
        uint32_t start = 0;
        while (start < s->byte_length &&
               r8e_is_whitespace((uint8_t)s->data[start]))
            start++;
        uint32_t end = s->byte_length;
        while (end > start &&
               r8e_is_whitespace((uint8_t)s->data[end - 1]))
            end--;
        return r8e_string_new_ascii(ctx, s->data + start, end - start);
    }

    /* Non-ASCII: decode codepoints to find whitespace boundaries */
    const uint8_t *p = (const uint8_t *)s->data;
    const uint8_t *pend = p + s->byte_length;
    uint32_t start_byte = 0;

    while (p < pend) {
        uint32_t cp;
        int n = r8e_utf8_decode(p, (uint32_t)(pend - p), &cp);
        if (n == 0 || !r8e_is_whitespace(cp)) break;
        p += n;
        start_byte = (uint32_t)(p - (const uint8_t *)s->data);
    }

    uint32_t end_byte = s->byte_length;
    while (end_byte > start_byte) {
        /* Walk backward to find start of previous character */
        uint32_t prev = end_byte - 1;
        while (prev > start_byte && ((uint8_t)s->data[prev] & 0xC0) == 0x80)
            prev--;
        uint32_t cp;
        r8e_utf8_decode((const uint8_t *)s->data + prev,
                        end_byte - prev, &cp);
        if (!r8e_is_whitespace(cp)) break;
        end_byte = prev;
    }

    return r8e_string_new(ctx, s->data + start_byte,
                           end_byte - start_byte);
}

/* -------------------------------------------------------------------------
 * trimStart() -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_trim_start(R8EContext *ctx, R8EString *s) {
    if (s->byte_length == 0)
        return r8e_string_new_ascii(ctx, "", 0);

    if (s->flags & R8E_STR_IS_ASCII) {
        uint32_t start = 0;
        while (start < s->byte_length &&
               r8e_is_whitespace((uint8_t)s->data[start]))
            start++;
        return r8e_string_new_ascii(ctx, s->data + start,
                                    s->byte_length - start);
    }

    const uint8_t *p = (const uint8_t *)s->data;
    const uint8_t *pend = p + s->byte_length;
    while (p < pend) {
        uint32_t cp;
        int n = r8e_utf8_decode(p, (uint32_t)(pend - p), &cp);
        if (n == 0 || !r8e_is_whitespace(cp)) break;
        p += n;
    }
    uint32_t start_byte = (uint32_t)(p - (const uint8_t *)s->data);
    return r8e_string_new(ctx, s->data + start_byte,
                           s->byte_length - start_byte);
}

/* -------------------------------------------------------------------------
 * trimEnd() -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_trim_end(R8EContext *ctx, R8EString *s) {
    if (s->byte_length == 0)
        return r8e_string_new_ascii(ctx, "", 0);

    if (s->flags & R8E_STR_IS_ASCII) {
        uint32_t end = s->byte_length;
        while (end > 0 && r8e_is_whitespace((uint8_t)s->data[end - 1]))
            end--;
        return r8e_string_new_ascii(ctx, s->data, end);
    }

    uint32_t end_byte = s->byte_length;
    while (end_byte > 0) {
        uint32_t prev = end_byte - 1;
        while (prev > 0 && ((uint8_t)s->data[prev] & 0xC0) == 0x80)
            prev--;
        uint32_t cp;
        r8e_utf8_decode((const uint8_t *)s->data + prev,
                        end_byte - prev, &cp);
        if (!r8e_is_whitespace(cp)) break;
        end_byte = prev;
    }

    return r8e_string_new(ctx, s->data, end_byte);
}

/* -------------------------------------------------------------------------
 * concat(a, b) -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_concat(R8EContext *ctx, R8EString *a, R8EString *b) {
    uint32_t total_bytes = a->byte_length + b->byte_length;
    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + total_bytes + 1);
    if (!r) return NULL;

    bool ascii = (a->flags & R8E_STR_IS_ASCII) &&
                 (b->flags & R8E_STR_IS_ASCII);
    r->flags = ascii ? R8E_STR_IS_ASCII : 0;
    r->byte_length = total_bytes;
    r->char_length = a->char_length + b->char_length;

    memcpy(r->data, a->data, a->byte_length);
    memcpy(r->data + a->byte_length, b->data, b->byte_length);
    r->data[total_bytes] = '\0';

    r->hash = r8e_fnv1a(r->data, total_bytes);
    return r;
}

/* -------------------------------------------------------------------------
 * repeat(count) -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_repeat(R8EContext *ctx, R8EString *s, int32_t count) {
    if (count <= 0 || s->byte_length == 0)
        return r8e_string_new_ascii(ctx, "", 0);

    uint32_t total_bytes = s->byte_length * (uint32_t)count;
    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + total_bytes + 1);
    if (!r) return NULL;

    r->flags = s->flags & R8E_STR_IS_ASCII;
    r->byte_length = total_bytes;
    r->char_length = s->char_length * (uint32_t)count;

    for (int32_t i = 0; i < count; i++)
        memcpy(r->data + (uint32_t)i * s->byte_length,
               s->data, s->byte_length);

    r->data[total_bytes] = '\0';
    r->hash = r8e_fnv1a(r->data, total_bytes);
    return r;
}

/* -------------------------------------------------------------------------
 * padStart(targetLength, padString) -> R8EString*
 * Default pad string is " " (space).
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_pad_start(R8EContext *ctx, R8EString *s,
                                 int32_t target_length, R8EString *pad_str) {
    if ((int32_t)s->char_length >= target_length)
        return r8e_string_new(ctx, s->data, s->byte_length);

    int32_t pad_needed = target_length - (int32_t)s->char_length;
    const char *pad_data = pad_str ? pad_str->data : " ";
    uint32_t pad_byte_len = pad_str ? pad_str->byte_length : 1;
    uint32_t pad_char_len = pad_str ? pad_str->char_length : 1;

    if (pad_char_len == 0)
        return r8e_string_new(ctx, s->data, s->byte_length);

    uint32_t full_repeats = (uint32_t)pad_needed / pad_char_len;
    uint32_t remainder_chars = (uint32_t)pad_needed % pad_char_len;

    uint32_t remainder_bytes;
    if (pad_str && !(pad_str->flags & R8E_STR_IS_ASCII))
        remainder_bytes = r8e_utf8_advance((const uint8_t *)pad_data,
                                            pad_byte_len, 0, remainder_chars);
    else
        remainder_bytes = remainder_chars;

    uint32_t total_bytes = full_repeats * pad_byte_len
                         + remainder_bytes + s->byte_length;
    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + total_bytes + 1);
    if (!r) return NULL;

    bool ascii = (s->flags & R8E_STR_IS_ASCII) &&
                 (!pad_str || (pad_str->flags & R8E_STR_IS_ASCII));

    uint32_t pos = 0;
    for (uint32_t i = 0; i < full_repeats; i++) {
        memcpy(r->data + pos, pad_data, pad_byte_len);
        pos += pad_byte_len;
    }
    if (remainder_bytes > 0) {
        memcpy(r->data + pos, pad_data, remainder_bytes);
        pos += remainder_bytes;
    }
    memcpy(r->data + pos, s->data, s->byte_length);
    pos += s->byte_length;
    r->data[pos] = '\0';

    r->flags = ascii ? R8E_STR_IS_ASCII : 0;
    r->byte_length = total_bytes;
    r->char_length = (uint32_t)target_length;
    r->hash = r8e_fnv1a(r->data, total_bytes);
    return r;
}

/* -------------------------------------------------------------------------
 * padEnd(targetLength, padString) -> R8EString*
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_pad_end(R8EContext *ctx, R8EString *s,
                               int32_t target_length, R8EString *pad_str) {
    if ((int32_t)s->char_length >= target_length)
        return r8e_string_new(ctx, s->data, s->byte_length);

    int32_t pad_needed = target_length - (int32_t)s->char_length;
    const char *pad_data = pad_str ? pad_str->data : " ";
    uint32_t pad_byte_len = pad_str ? pad_str->byte_length : 1;
    uint32_t pad_char_len = pad_str ? pad_str->char_length : 1;

    if (pad_char_len == 0)
        return r8e_string_new(ctx, s->data, s->byte_length);

    uint32_t full_repeats = (uint32_t)pad_needed / pad_char_len;
    uint32_t remainder_chars = (uint32_t)pad_needed % pad_char_len;

    uint32_t remainder_bytes;
    if (pad_str && !(pad_str->flags & R8E_STR_IS_ASCII))
        remainder_bytes = r8e_utf8_advance((const uint8_t *)pad_data,
                                            pad_byte_len, 0, remainder_chars);
    else
        remainder_bytes = remainder_chars;

    uint32_t total_bytes = s->byte_length
                         + full_repeats * pad_byte_len + remainder_bytes;
    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + total_bytes + 1);
    if (!r) return NULL;

    bool ascii = (s->flags & R8E_STR_IS_ASCII) &&
                 (!pad_str || (pad_str->flags & R8E_STR_IS_ASCII));

    uint32_t pos = 0;
    memcpy(r->data, s->data, s->byte_length);
    pos += s->byte_length;
    for (uint32_t i = 0; i < full_repeats; i++) {
        memcpy(r->data + pos, pad_data, pad_byte_len);
        pos += pad_byte_len;
    }
    if (remainder_bytes > 0) {
        memcpy(r->data + pos, pad_data, remainder_bytes);
        pos += remainder_bytes;
    }
    r->data[pos] = '\0';

    r->flags = ascii ? R8E_STR_IS_ASCII : 0;
    r->byte_length = total_bytes;
    r->char_length = (uint32_t)target_length;
    r->hash = r8e_fnv1a(r->data, total_bytes);
    return r;
}

/* -------------------------------------------------------------------------
 * split(separator, limit) -> R8EString** (array of heap strings)
 *
 * Writes the result count to *out_count. Caller is responsible for freeing
 * the returned array AND each string in it.
 *
 * If separator is NULL: returns the whole string as a 1-element array.
 * If separator is empty: splits into individual characters.
 * ------------------------------------------------------------------------- */
R8EString **r8e_string_split(R8EContext *ctx, R8EString *s,
                              R8EString *separator, int32_t limit,
                              uint32_t *out_count) {
    *out_count = 0;

    if (!separator) {
        R8EString **arr = (R8EString **)r8e_alloc(ctx, sizeof(R8EString *));
        if (!arr) return NULL;
        arr[0] = r8e_string_new(ctx, s->data, s->byte_length);
        if (!arr[0]) { r8e_free(ctx, arr); return NULL; }
        *out_count = 1;
        return arr;
    }

    if (limit == 0) return NULL;
    if (limit < 0) limit = INT32_MAX;

    uint32_t cap = 16;
    R8EString **arr = (R8EString **)r8e_alloc(ctx, sizeof(R8EString *) * cap);
    if (!arr) return NULL;
    uint32_t count = 0;

    /* Empty separator: split into individual characters */
    if (separator->byte_length == 0) {
        if (s->flags & R8E_STR_IS_ASCII) {
            for (uint32_t i = 0; i < s->byte_length &&
                 (int32_t)count < limit; i++) {
                if (count >= cap) {
                    cap *= 2;
                    R8EString **na = (R8EString **)r8e_realloc(ctx, arr,
                        sizeof(R8EString *) * cap);
                    if (!na) goto oom;
                    arr = na;
                }
                arr[count] = r8e_string_new_ascii(ctx, &s->data[i], 1);
                if (!arr[count]) goto oom;
                count++;
            }
        } else {
            const uint8_t *p = (const uint8_t *)s->data;
            const uint8_t *end = p + s->byte_length;
            while (p < end && (int32_t)count < limit) {
                uint32_t cp;
                int n = r8e_utf8_decode(p, (uint32_t)(end - p), &cp);
                if (n == 0) break;
                if (count >= cap) {
                    cap *= 2;
                    R8EString **na = (R8EString **)r8e_realloc(ctx, arr,
                        sizeof(R8EString *) * cap);
                    if (!na) goto oom;
                    arr = na;
                }
                arr[count] = r8e_string_new(ctx, (const char *)p,
                                             (uint32_t)n);
                if (!arr[count]) goto oom;
                count++;
                p += n;
            }
        }
        *out_count = count;
        return arr;
    }

    /* Normal split: find separator occurrences */
    uint32_t pos = 0;
    while (pos <= s->byte_length && (int32_t)count < limit) {
        const char *found = NULL;
        if (pos + separator->byte_length <= s->byte_length) {
            found = (const char *)r8e_memmem(s->data + pos,
                s->byte_length - pos,
                separator->data, separator->byte_length);
        }

        uint32_t seg_end = found
            ? (uint32_t)(found - s->data) : s->byte_length;

        if (count >= cap) {
            cap *= 2;
            R8EString **na = (R8EString **)r8e_realloc(ctx, arr,
                sizeof(R8EString *) * cap);
            if (!na) goto oom;
            arr = na;
        }

        arr[count] = r8e_string_new(ctx, s->data + pos, seg_end - pos);
        if (!arr[count]) goto oom;
        count++;

        if (!found) break;
        pos = seg_end + separator->byte_length;

        /* Separator at very end: add trailing empty element */
        if (pos == s->byte_length && (int32_t)count < limit) {
            if (count >= cap) {
                cap *= 2;
                R8EString **na = (R8EString **)r8e_realloc(ctx, arr,
                    sizeof(R8EString *) * cap);
                if (!na) goto oom;
                arr = na;
            }
            arr[count] = r8e_string_new_ascii(ctx, "", 0);
            if (!arr[count]) goto oom;
            count++;
            break;
        }
    }

    *out_count = count;
    return arr;

oom:
    for (uint32_t i = 0; i < count; i++)
        r8e_string_free(ctx, arr[i]);
    r8e_free(ctx, arr);
    *out_count = 0;
    return NULL;
}

/* -------------------------------------------------------------------------
 * replace(search, replacement) -> R8EString*
 * Replaces the FIRST occurrence. No regex support at this layer.
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_replace(R8EContext *ctx, R8EString *s,
                               R8EString *search, R8EString *replacement) {
    if (search->byte_length == 0)
        return r8e_string_concat(ctx, replacement, s);

    const char *found = (const char *)r8e_memmem(s->data, s->byte_length,
        search->data, search->byte_length);
    if (!found)
        return r8e_string_new(ctx, s->data, s->byte_length);

    uint32_t prefix_len = (uint32_t)(found - s->data);
    uint32_t suffix_start = prefix_len + search->byte_length;
    uint32_t suffix_len = s->byte_length - suffix_start;
    uint32_t total = prefix_len + replacement->byte_length + suffix_len;

    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + total + 1);
    if (!r) return NULL;

    memcpy(r->data, s->data, prefix_len);
    memcpy(r->data + prefix_len, replacement->data,
           replacement->byte_length);
    memcpy(r->data + prefix_len + replacement->byte_length,
           s->data + suffix_start, suffix_len);
    r->data[total] = '\0';

    bool ascii = r8e_is_ascii(r->data, total);
    r->flags = ascii ? R8E_STR_IS_ASCII : 0;
    r->byte_length = total;
    r->char_length = ascii ? total : r8e_utf8_strlen(r->data, total);
    r->hash = r8e_fnv1a(r->data, total);
    return r;
}

/* -------------------------------------------------------------------------
 * replaceAll(search, replacement) -> R8EString*
 * Replaces ALL non-overlapping occurrences.
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_replace_all(R8EContext *ctx, R8EString *s,
                                   R8EString *search,
                                   R8EString *replacement) {
    if (search->byte_length == 0) {
        /*
         * ES2021: replaceAll with empty search inserts replacement between
         * every character and at the start/end.
         */
        uint32_t est_len = replacement->byte_length * (s->char_length + 1)
                         + s->byte_length;
        R8EString *r = (R8EString *)r8e_alloc(ctx,
            sizeof(R8EString) + est_len + 1);
        if (!r) return NULL;

        uint32_t pos = 0;
        const uint8_t *p = (const uint8_t *)s->data;
        const uint8_t *end = p + s->byte_length;

        memcpy(r->data + pos, replacement->data, replacement->byte_length);
        pos += replacement->byte_length;

        while (p < end) {
            uint32_t cp;
            int n = r8e_utf8_decode(p, (uint32_t)(end - p), &cp);
            if (n == 0) break;
            memcpy(r->data + pos, p, (uint32_t)n);
            pos += (uint32_t)n;
            p += n;
            memcpy(r->data + pos, replacement->data,
                   replacement->byte_length);
            pos += replacement->byte_length;
        }
        r->data[pos] = '\0';

        bool ascii = r8e_is_ascii(r->data, pos);
        r->flags = ascii ? R8E_STR_IS_ASCII : 0;
        r->byte_length = pos;
        r->char_length = ascii ? pos : r8e_utf8_strlen(r->data, pos);
        r->hash = r8e_fnv1a(r->data, pos);
        return r;
    }

    /* Count occurrences */
    uint32_t occ_count = 0;
    {
        const char *p = s->data;
        uint32_t remaining = s->byte_length;
        while (remaining >= search->byte_length) {
            const char *found = (const char *)r8e_memmem(p, remaining,
                search->data, search->byte_length);
            if (!found) break;
            occ_count++;
            uint32_t skip = (uint32_t)(found - p) + search->byte_length;
            p = found + search->byte_length;
            remaining -= skip;
        }
    }

    if (occ_count == 0)
        return r8e_string_new(ctx, s->data, s->byte_length);

    uint32_t total = s->byte_length
        - occ_count * search->byte_length
        + occ_count * replacement->byte_length;

    R8EString *r = (R8EString *)r8e_alloc(ctx,
        sizeof(R8EString) + total + 1);
    if (!r) return NULL;

    uint32_t wpos = 0;
    const char *rp = s->data;
    uint32_t remaining = s->byte_length;

    while (remaining >= search->byte_length) {
        const char *found = (const char *)r8e_memmem(rp, remaining,
            search->data, search->byte_length);
        if (!found) break;

        uint32_t prefix = (uint32_t)(found - rp);
        memcpy(r->data + wpos, rp, prefix);
        wpos += prefix;
        memcpy(r->data + wpos, replacement->data,
               replacement->byte_length);
        wpos += replacement->byte_length;
        rp = found + search->byte_length;
        remaining = s->byte_length - (uint32_t)(rp - s->data);
    }
    /* Copy remainder */
    memcpy(r->data + wpos, rp, remaining);
    wpos += remaining;
    r->data[wpos] = '\0';

    bool ascii = r8e_is_ascii(r->data, wpos);
    r->flags = ascii ? R8E_STR_IS_ASCII : 0;
    r->byte_length = wpos;
    r->char_length = ascii ? wpos : r8e_utf8_strlen(r->data, wpos);
    r->hash = r8e_fnv1a(r->data, wpos);
    return r;
}

/* -------------------------------------------------------------------------
 * at(index) -> R8EString*   (ES2022)
 * Like charAt but supports negative indices (counting from end).
 * Returns NULL for out-of-range (representing undefined in JS).
 * ------------------------------------------------------------------------- */
R8EString *r8e_string_at(R8EContext *ctx, R8EString *s, int32_t index) {
    int32_t resolved = r8e_resolve_relative_index(index,
                                                   (int32_t)s->char_length);
    if (resolved < 0 || (uint32_t)resolved >= s->char_length)
        return NULL;
    return r8e_string_char_at(ctx, s, resolved);
}
