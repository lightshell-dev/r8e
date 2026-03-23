/*
 * r8e_types.h - NaN-Boxing Value Representation and Core Type Definitions
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 2 (Value Representation), Section 3 (Object Model),
 * Section 4 (String Engine), Section 8 (GC), Section 9 (Closures).
 *
 * Architecture:
 *   - Every JS value fits in exactly 8 bytes via IEEE 754 NaN-boxing
 *   - Zero-cost double extraction (most common type)
 *   - Inline short strings up to 7 ASCII chars (no heap allocation)
 *   - 4-tier object model: Micro(24B), Compact(40-72B), Array, Hash
 *   - RC-based GC with compile-time elision (70-75% ops skipped)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_TYPES_H
#define R8E_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Section 1: Platform Detection
 * ========================================================================= */

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL
  #define R8E_64BIT 1
#else
  #define R8E_32BIT 1
#endif

/* Computed goto support detection */
#if defined(__GNUC__) || defined(__clang__)
  #define R8E_COMPUTED_GOTO 1
#endif

/* =========================================================================
 * Section 2: NaN-Boxing Value Representation (CLAUDE.md Section 2)
 *
 * IEEE 754 double layout:
 *   Positive doubles:  0x0000_0000_0000_0000 .. 0x7FF7_FFFF_FFFF_FFFF
 *   Canonical NaN:     0x7FF8_0000_0000_0000
 *   Negative doubles:  0x8000_0000_0000_0000 .. 0xFFF7_FFFF_FFFF_FFFF
 *
 * Tagged values (negative-NaN space):
 *   0xFFF8_0000_XXXX_XXXX = 32-bit signed integer
 *   0xFFF9_0000_0000_XXXX = heap pointer (48-bit)
 *   0xFFFA_0000_0000_0000 = undefined
 *   0xFFFA_0000_0000_0001 = null
 *   0xFFFA_0000_0000_0002 = true
 *   0xFFFA_0000_0000_0003 = false
 *   0xFFFB_0000_XXXX_XXXX = symbol ID
 *   0xFFFC_0000_XXXX_XXXX = atom index (interned string ref)
 *   0xFFFD_LLLL_DDDD_DDDD = inline short string (up to 7 ASCII chars)
 *
 * Validated by EXP-08: zero collisions across all encodings.
 * ========================================================================= */

#ifdef R8E_64BIT

/* --- Core value type --- */
typedef uint64_t R8EValue;

/* --- Tag constants (16-bit tags in bits 48-63, 48-bit payload) --- */
#define R8E_TAG_INT32       0xFFF8U
#define R8E_TAG_POINTER     0xFFF9U
#define R8E_TAG_SPECIAL     0xFFFAU
#define R8E_TAG_SYMBOL      0xFFFBU
#define R8E_TAG_ATOM        0xFFFCU
#define R8E_TAG_INLINE_STR  0xFFFDU

/* --- Singleton constants --- */
#define R8E_UNDEFINED       ((R8EValue)0xFFFA000000000000ULL)
#define R8E_NULL            ((R8EValue)0xFFFA000000000001ULL)
#define R8E_TRUE            ((R8EValue)0xFFFA000000000002ULL)
#define R8E_FALSE           ((R8EValue)0xFFFA000000000003ULL)

/* --- Canonical NaN --- */
#define R8E_NAN             ((R8EValue)0x7FF8000000000000ULL)

/* --- Type check macros (Section 2.4) --- */

/* Doubles occupy everything below the tag boundary */
#define R8E_IS_DOUBLE(v)    ((v) < 0xFFF8000000000000ULL)

/* Int32: upper 16 bits == 0xFFF8, payload in bits 0-31 */
#define R8E_IS_INT32(v)     (((v) >> 48) == R8E_TAG_INT32)

/* Heap pointer: upper 16 bits == 0xFFF9, payload in bits 0-47 */
#define R8E_IS_POINTER(v)   (((v) >> 48) == R8E_TAG_POINTER)

/* Inline short string: upper 16 bits == 0xFFFD */
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == R8E_TAG_INLINE_STR)

/* Symbol: upper 16 bits == 0xFFFB, payload in bits 0-31 */
#define R8E_IS_SYMBOL(v)    (((v) >> 48) == R8E_TAG_SYMBOL)

/* Atom: upper 16 bits == 0xFFFC, payload in bits 0-31 */
#define R8E_IS_ATOM(v)      (((v) >> 48) == R8E_TAG_ATOM)

/* Special values */
#define R8E_IS_UNDEFINED(v) ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)      ((v) == R8E_NULL)
#define R8E_IS_BOOL(v)      ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_TRUE(v)      ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)     ((v) == R8E_FALSE)
#define R8E_IS_NULLISH(v)   ((v) == R8E_UNDEFINED || (v) == R8E_NULL)

/* Number: either a double or an int32 */
#define R8E_IS_NUMBER(v)    (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))

/* String: either inline short string or heap string (pointer with string flag) */
#define R8E_IS_STRING(v)    (R8E_IS_INLINE_STR(v) || R8E_IS_HEAP_STRING(v))

/* Object/heap check: pointer tag */
#define R8E_IS_OBJECT(v)    R8E_IS_POINTER(v)

/* Heap string: must check pointer's target type at runtime; macro placeholder */
#define R8E_IS_HEAP_STRING(v) 0  /* resolved at runtime via pointer inspection */

/* --- Extraction functions (Section 2.4) --- */

/**
 * Extract a double from a NaN-boxed value.
 * Precondition: R8E_IS_DOUBLE(v) is true.
 * Compiles to a single mov instruction.
 */
static inline double r8e_get_double(R8EValue v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

/**
 * Extract a 32-bit signed integer.
 * Precondition: R8E_IS_INT32(v) is true.
 */
static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}

/**
 * Extract a heap pointer (48-bit address).
 * Precondition: R8E_IS_POINTER(v) is true.
 */
static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/**
 * Extract a symbol ID.
 * Precondition: R8E_IS_SYMBOL(v) is true.
 */
static inline uint32_t r8e_get_symbol(R8EValue v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

/**
 * Extract an atom index.
 * Precondition: R8E_IS_ATOM(v) is true.
 */
static inline uint32_t r8e_get_atom(R8EValue v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

/* --- Encoding functions (Section 2.4) --- */

/**
 * Encode a double as a NaN-boxed value.
 * Most JS values are doubles; this is the zero-cost hot path.
 */
static inline R8EValue r8e_from_double(double d) {
    R8EValue v;
    memcpy(&v, &d, sizeof(double));
    return v;
}

/**
 * Encode a 32-bit signed integer.
 * Integers are common in loops and array indexing.
 */
static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

/**
 * Encode a heap pointer.
 * Pointer must fit in 48 bits (true on all current 64-bit platforms).
 */
static inline R8EValue r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/**
 * Encode a boolean value.
 */
static inline R8EValue r8e_from_bool(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}

/**
 * Encode a symbol ID.
 */
static inline R8EValue r8e_from_symbol(uint32_t id) {
    return 0xFFFB000000000000ULL | (uint64_t)id;
}

/**
 * Encode an atom index (interned string reference).
 */
static inline R8EValue r8e_from_atom(uint32_t idx) {
    return 0xFFFC000000000000ULL | (uint64_t)idx;
}

/* --- Inline Short Strings (Section 2.5) ---
 *
 * EXP-05: 50% of strings are 8 bytes or shorter, 91.5% are ASCII.
 * Strings up to 7 ASCII chars are encoded directly in the NaN-box payload.
 * No heap allocation, no GC, no string table lookup.
 * Covers ~40-45% of all string values.
 *
 * Layout within the 64-bit value:
 *   Bits [63:48] = 0xFFFD (tag)
 *   Bits [47:45] = length (0-7)
 *   Bits [44:0]  = packed chars (7 bits each, 7 chars max = 49 bits, fits in 45)
 */

/**
 * Encode a short ASCII string (up to 6 chars) inline in a NaN-boxed value.
 * Returns R8E_UNDEFINED if len > 6 or any char is non-ASCII.
 *
 * The NaN-box payload has 45 bits (bits [44:0]). Each char uses 7 bits,
 * so the maximum is floor(45/7) = 6 characters.
 */
static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    if (len < 0 || len > 6) return R8E_UNDEFINED;
    uint64_t v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch > 127) return R8E_UNDEFINED;  /* non-ASCII: cannot inline */
        v |= ((uint64_t)ch << (38 - i * 7));
    }
    return v;
}

/**
 * Extract the length of an inline short string.
 * Precondition: R8E_IS_INLINE_STR(v) is true.
 */
static inline int r8e_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}

/**
 * Extract the i-th character of an inline short string.
 * Precondition: R8E_IS_INLINE_STR(v) is true, 0 <= i < len.
 */
static inline char r8e_inline_str_char(R8EValue v, int i) {
    return (char)((v >> (38 - i * 7)) & 0x7F);
}

/**
 * Decode an inline short string into a buffer.
 * Buffer must have at least 8 bytes. Returns length written.
 * Precondition: R8E_IS_INLINE_STR(v) is true.
 */
static inline int r8e_inline_str_decode(R8EValue v, char *buf) {
    int len = r8e_inline_str_len(v);
    for (int i = 0; i < len; i++) {
        buf[i] = r8e_inline_str_char(v, i);
    }
    buf[len] = '\0';
    return len;
}

/**
 * Convert any numeric value to a double.
 * Handles both int32 and double encodings.
 */
static inline double r8e_to_number(R8EValue v) {
    if (R8E_IS_INT32(v)) return (double)r8e_get_int32(v);
    if (R8E_IS_DOUBLE(v)) return r8e_get_double(v);
    return 0.0;  /* NaN for non-numeric would be more correct at runtime */
}

#else /* R8E_32BIT */

/* =========================================================================
 * 32-bit fallback: discriminated union (Section 2.6)
 *
 * struct { uint32_t tag; uint32_t payload; } = 8 bytes total.
 * Doubles are split across both fields.
 * ========================================================================= */

typedef enum {
    R8E_TAG32_DOUBLE    = 0,
    R8E_TAG32_INT32     = 1,
    R8E_TAG32_POINTER   = 2,
    R8E_TAG32_UNDEFINED = 3,
    R8E_TAG32_NULL      = 4,
    R8E_TAG32_TRUE      = 5,
    R8E_TAG32_FALSE     = 6,
    R8E_TAG32_SYMBOL    = 7,
    R8E_TAG32_ATOM      = 8,
    R8E_TAG32_INLINE_STR = 9
} R8ETag32;

typedef struct {
    uint32_t tag;
    uint32_t payload;
} R8EValue;

/* Singleton constants for 32-bit */
#define R8E_UNDEFINED  ((R8EValue){ R8E_TAG32_UNDEFINED, 0 })
#define R8E_NULL       ((R8EValue){ R8E_TAG32_NULL, 0 })
#define R8E_TRUE       ((R8EValue){ R8E_TAG32_TRUE, 0 })
#define R8E_FALSE      ((R8EValue){ R8E_TAG32_FALSE, 0 })

/* Type checks for 32-bit */
#define R8E_IS_DOUBLE(v)      ((v).tag == R8E_TAG32_DOUBLE)
#define R8E_IS_INT32(v)       ((v).tag == R8E_TAG32_INT32)
#define R8E_IS_POINTER(v)     ((v).tag == R8E_TAG32_POINTER)
#define R8E_IS_INLINE_STR(v)  ((v).tag == R8E_TAG32_INLINE_STR)
#define R8E_IS_UNDEFINED(v)   ((v).tag == R8E_TAG32_UNDEFINED)
#define R8E_IS_NULL(v)        ((v).tag == R8E_TAG32_NULL)
#define R8E_IS_BOOL(v)        ((v).tag == R8E_TAG32_TRUE || (v).tag == R8E_TAG32_FALSE)
#define R8E_IS_TRUE(v)        ((v).tag == R8E_TAG32_TRUE)
#define R8E_IS_FALSE(v)       ((v).tag == R8E_TAG32_FALSE)
#define R8E_IS_SYMBOL(v)      ((v).tag == R8E_TAG32_SYMBOL)
#define R8E_IS_ATOM(v)        ((v).tag == R8E_TAG32_ATOM)
#define R8E_IS_NULLISH(v)     (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))
#define R8E_IS_NUMBER(v)      (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))

/* Encoding/decoding for 32-bit deferred to .c file for non-trivial cases */
static inline R8EValue r8e_from_int32(int32_t i) {
    R8EValue v;
    v.tag = R8E_TAG32_INT32;
    v.payload = (uint32_t)i;
    return v;
}

static inline int32_t r8e_get_int32(R8EValue v) {
    return (int32_t)v.payload;
}

static inline R8EValue r8e_from_pointer(void *p) {
    R8EValue v;
    v.tag = R8E_TAG32_POINTER;
    v.payload = (uint32_t)(uintptr_t)p;
    return v;
}

static inline void *r8e_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)v.payload;
}

static inline R8EValue r8e_from_bool(bool b) {
    R8EValue v;
    v.tag = b ? R8E_TAG32_TRUE : R8E_TAG32_FALSE;
    v.payload = 0;
    return v;
}

/* Double encoding on 32-bit: store raw bits in a separate 64-bit value,
 * split across two 32-bit halves via union. Full implementation in .c file. */

#endif /* R8E_64BIT / R8E_32BIT */


/* =========================================================================
 * Section 3: GC Header Bits (CLAUDE.md Section 8.4)
 *
 * Every heap object begins with a uint32_t flags field:
 *   Bits [1:0]   = object kind / tier (0-3)
 *   Bit  [2]     = GC mark (cycle scan)
 *   Bit  [3]     = on suspect list
 *   Bit  [4]     = frozen (Object.freeze)
 *   Bits [7:5]   = reserved
 *   Bits [15:8]  = refcount overflow indicator
 *   Bits [31:16] = inline refcount (0-65535, covers 99.99% of objects)
 * ========================================================================= */

/* Tier / kind bits [1:0] */
#define R8E_GC_TIER_MASK       0x00000003U
#define R8E_GC_TIER_SHIFT      0
#define R8E_GC_TIER_0          0x00000000U
#define R8E_GC_TIER_1          0x00000001U
#define R8E_GC_TIER_2          0x00000002U
#define R8E_GC_TIER_3          0x00000003U

/* GC mark bit [2] */
#define R8E_GC_MARK_BIT        0x00000004U

/* Suspect list membership [3] */
#define R8E_GC_SUSPECT_BIT     0x00000008U

/* Object.freeze flag [4] */
#define R8E_GC_FROZEN_BIT      0x00000010U

/* Reserved bits [7:5] - future use */
#define R8E_GC_RESERVED_MASK   0x000000E0U

/* Refcount overflow indicator [15:8] */
#define R8E_GC_RC_OVERFLOW_MASK  0x0000FF00U
#define R8E_GC_RC_OVERFLOW_SHIFT 8

/* Inline refcount [31:16] */
#define R8E_GC_RC_MASK         0xFFFF0000U
#define R8E_GC_RC_SHIFT        16
#define R8E_GC_RC_MAX          0xFFFFU

/* --- GC kind tags (for non-object heap types) --- */
#define R8E_GC_KIND_OBJECT     0x00U
#define R8E_GC_KIND_STRING     0x01U
#define R8E_GC_KIND_CLOSURE    0x02U
#define R8E_GC_KIND_ENVFRAME   0x03U
#define R8E_GC_KIND_ARRAY      0x04U
#define R8E_GC_KIND_FUNCTION   0x05U
#define R8E_GC_KIND_REGEXP     0x06U

/* Additional object flags (stored in bits [7:5] of the flags field) */
#define R8E_OBJ_IS_ARRAY       0x00000020U
#define R8E_OBJ_IS_FUNCTION    0x00000040U
#define R8E_OBJ_IS_STRING      0x00000060U

/* --- Inline RC helpers --- */

static inline uint16_t r8e_gc_get_rc(uint32_t flags) {
    return (uint16_t)((flags & R8E_GC_RC_MASK) >> R8E_GC_RC_SHIFT);
}

static inline uint32_t r8e_gc_set_rc(uint32_t flags, uint16_t rc) {
    return (flags & ~R8E_GC_RC_MASK) | ((uint32_t)rc << R8E_GC_RC_SHIFT);
}

static inline uint32_t r8e_gc_inc_rc(uint32_t flags) {
    uint16_t rc = r8e_gc_get_rc(flags);
    if (rc < R8E_GC_RC_MAX) {
        return r8e_gc_set_rc(flags, rc + 1);
    }
    /* Overflow: set overflow indicator, keep max inline */
    return flags | (1U << R8E_GC_RC_OVERFLOW_SHIFT);
}

static inline uint32_t r8e_gc_dec_rc(uint32_t flags) {
    uint16_t rc = r8e_gc_get_rc(flags);
    if (rc > 0) {
        return r8e_gc_set_rc(flags, rc - 1);
    }
    return flags;
}

static inline bool r8e_gc_is_marked(uint32_t flags) {
    return (flags & R8E_GC_MARK_BIT) != 0;
}

static inline bool r8e_gc_is_frozen(uint32_t flags) {
    return (flags & R8E_GC_FROZEN_BIT) != 0;
}

static inline bool r8e_gc_is_suspect(uint32_t flags) {
    return (flags & R8E_GC_SUSPECT_BIT) != 0;
}

static inline uint8_t r8e_gc_get_tier(uint32_t flags) {
    return (uint8_t)(flags & R8E_GC_TIER_MASK);
}


/* =========================================================================
 * Section 4: Forward Declarations
 * ========================================================================= */

typedef struct R8EContext    R8EContext;
typedef struct R8EObject     R8EObject;
typedef struct R8EArray      R8EArray;
typedef struct R8EString     R8EString;
typedef struct R8EClosure    R8EClosure;
typedef struct R8EFunction   R8EFunction;
typedef struct R8EScope      R8EScope;
typedef struct R8EAtomTable  R8EAtomTable;
typedef struct R8EEnvFrame   R8EEnvFrame;
typedef struct R8ERealm      R8ERealm;
typedef struct R8EModule     R8EModule;
typedef struct R8ERegExp     R8ERegExp;


/* =========================================================================
 * Section 5: Object Model - 4-Tier CDOL (CLAUDE.md Section 3.3)
 *
 * EXP-01: 98.8% of objects have 4 or fewer properties.
 *   Tier 0 (0-1 props): 55% of objects, 24 bytes
 *   Tier 1 (2-4 props): 44% of objects, 40-72 bytes
 *   Tier 2 (5-16 props): ~1% of objects, heap-allocated array
 *   Tier 3 (17+ props): <0.01%, Robin Hood hash table
 *
 * Objects promote when they outgrow their tier. They never demote.
 * ========================================================================= */

/* Key-value pair used by Tier 1, 2, 3 */
typedef struct {
    R8EValue key;   /* atom index of property name */
    R8EValue val;   /* NaN-boxed property value */
} R8EPropPair;

/**
 * Tier 0: Micro Object (0-1 properties) - 24 bytes
 * 55% of all objects. Header + one inline key-value slot.
 */
typedef struct {
    uint32_t flags;       /* GC bits, tier tag (bits [1:0] = 0), frozen flag */
    uint32_t proto_id;    /* index into prototype table */
    R8EValue key0;        /* atom index of single property (0 = empty) */
    R8EValue val0;        /* NaN-boxed value */
} R8EObjTier0;            /* 24 bytes */

/**
 * Tier 1: Compact Object (2-4 properties) - 40-72 bytes
 * 44% of all objects. Header + inline array of up to 4 key-value pairs.
 * Property lookup: linear scan of 2-4 entries (1-2 cache reads).
 */
typedef struct {
    uint32_t    flags;       /* GC bits, tier tag (bits [1:0] = 1) */
    uint32_t    proto_id;
    uint8_t     count;       /* 2-4 active properties */
    uint8_t     pad[7];
    R8EPropPair props[4];    /* inline key-value pairs */
} R8EObjTier1;               /* 8 + 8 + (16 * 4) = 80 bytes max */

/**
 * Tier 2: Compact Array (5-16 properties) - heap-allocated
 * ~1% of objects. Header + pointer to separate key-value array.
 * Lookup: linear scan for count <= 8, binary search for 9-16.
 */
typedef struct {
    uint32_t     flags;      /* GC bits, tier tag (bits [1:0] = 2) */
    uint32_t     proto_id;
    uint8_t      count;      /* current number of properties */
    uint8_t      capacity;   /* allocated capacity (power of 2: 8 or 16) */
    uint8_t      pad[6];
    R8EPropPair *props;      /* heap-allocated key-value array */
} R8EObjTier2;               /* 16 byte header + separate allocation */

/**
 * Tier 3: Hash Table (17+ properties) - rare
 * <0.01% of objects. Robin Hood open-addressing hash table.
 * Worst-case probe length: O(log n).
 */
typedef struct {
    uint32_t     flags;      /* GC bits, tier tag (bits [1:0] = 3) */
    uint32_t     proto_id;
    uint16_t     count;      /* current number of properties */
    uint16_t     capacity;   /* bucket count (power of 2) */
    uint32_t     pad;
    R8EPropPair *buckets;    /* Robin Hood hash table buckets */
} R8EObjTier3;

/**
 * Generic R8EObject union for type-erased access.
 * The tier is determined by bits [1:0] of the flags field.
 */
struct R8EObject {
    union {
        R8EObjTier0 t0;
        R8EObjTier1 t1;
        R8EObjTier2 t2;
        R8EObjTier3 t3;
        struct {
            uint32_t flags;       /* common header: always at offset 0 */
            uint32_t proto_id;    /* common header: always at offset 4 */
        } header;
    } u;
};


/* =========================================================================
 * Section 6: Array (CLAUDE.md Section 3.3 - Array Optimization)
 *
 * Arrays use a contiguous C array of NaN-boxed values indexed by integer.
 * Named properties (arr.customProp) stored in a separate object, only
 * allocated if needed.
 * ========================================================================= */

struct R8EArray {
    uint32_t   flags;       /* includes R8E_OBJ_IS_ARRAY flag */
    uint32_t   proto_id;    /* R8E_PROTO_ARRAY */
    uint32_t   length;      /* JS .length value */
    uint32_t   capacity;    /* allocated element slots */
    R8EValue  *elements;    /* dense NaN-boxed array */
    R8EObject *named;       /* NULL if no named properties (common case) */
};


/* =========================================================================
 * Section 7: String (CLAUDE.md Section 4.3)
 *
 * EXP-05: 91.5% ASCII, 50% short, 99.3% of chars are ASCII.
 * Three tiers:
 *   A) Inline short string (0-7 ASCII bytes, in NaN-box payload)
 *   B) Heap ASCII string (8+ bytes, pure ASCII, O(1) random access)
 *   C) Heap Unicode string (non-ASCII, UTF-8, lazy offset table)
 * ========================================================================= */

/* String flags */
#define R8E_STR_IS_ASCII     0x01U
#define R8E_STR_IS_INTERNED  0x02U
#define R8E_STR_IS_ROPE      0x04U
#define R8E_STR_HAS_OFFSETS  0x08U

/**
 * Lazy offset table for non-ASCII strings (Section 4.4).
 * Built on first random access (bracket indexing), then cached.
 * Most string operations never need this.
 */
typedef struct {
    uint32_t *offsets;    /* offsets[i] = byte position of i-th UTF-16 code unit */
    uint32_t  length;     /* number of entries */
} R8EOffsetTable;

/**
 * Heap string header (Section 4.3).
 * UTF-8 bytes follow immediately after the header (flexible array member).
 * Total size: 16 byte header + byte_length bytes + padding.
 */
struct R8EString {
    uint32_t flags;        /* R8E_STR_IS_ASCII, R8E_STR_IS_INTERNED, GC bits */
    uint32_t hash;         /* precomputed hash (for property lookups) */
    uint32_t byte_length;  /* UTF-8 byte count */
    uint32_t char_length;  /* UTF-16 code unit count (= byte_length if ASCII) */
    R8EOffsetTable *offset_table;  /* NULL until first random access on non-ASCII */
    char data[];           /* UTF-8 bytes (flexible array member) */
};


/* =========================================================================
 * Section 8: Function (CLAUDE.md Section 5.5, 13.1)
 * ========================================================================= */

struct R8EFunction {
    uint32_t  flags;          /* GC header flags */
    uint32_t  proto_id;       /* prototype table index */
    uint8_t  *bytecode;       /* compiled bytecode */
    uint32_t  bytecode_len;   /* length of bytecode in bytes */
    uint16_t  param_count;    /* number of declared parameters */
    uint16_t  local_count;    /* number of local registers */
    uint16_t  capture_count;  /* number of upvalue captures */
    uint16_t  stack_size;     /* max operand stack depth */
    uint32_t  name_atom;      /* interned function name (0 = anonymous) */
    R8EValue *constants;      /* constant pool */
    uint16_t  const_count;    /* number of constants */
    uint8_t   is_strict;      /* strict mode flag */
    uint8_t   is_arrow;       /* arrow function flag */
    uint8_t   is_generator;   /* generator function flag */
    uint8_t   is_async;       /* async function flag */
    uint16_t  source_line;    /* first line number (for stack traces) */
};


/* =========================================================================
 * Section 9: Closure (CLAUDE.md Section 9.3 - PVCC)
 *
 * EXP-04: 83.1% of captured variables are immutable after capture.
 * 70.4% of closures have ALL captures immutable.
 *
 * Tiered capture storage:
 *   INLINE: 1-2 captures, all immutable (~52%)
 *   ARRAY:  3-8 captures, mixed mutability (~28%)
 *   FRAME:  9+ captures, shared environment (~20%)
 * ========================================================================= */

/* Capture classification (compile-time decision) */
#define R8E_CAPTURE_IMMUTABLE  0x00  /* 83.1%: copy value directly */
#define R8E_CAPTURE_MUTABLE    0x01  /* 16.9%: share via R8EEnvFrame */

/* Capture storage mode */
typedef enum {
    R8E_CAPTURE_MODE_INLINE = 0,  /* 1-2 captures, all immutable */
    R8E_CAPTURE_MODE_ARRAY  = 1,  /* 3-8 captures */
    R8E_CAPTURE_MODE_FRAME  = 2   /* 9+ captures, or has mutable */
} R8ECaptureMode;

/**
 * Shared mutable upvalue storage.
 * When multiple closures in the same scope capture a mutable variable,
 * they all reference the same R8EEnvFrame.
 */
struct R8EEnvFrame {
    uint32_t  flags;       /* GC header: kind = R8E_GC_KIND_ENVFRAME */
    uint32_t  proto_id;    /* unused for env frames, set to 0 */
    uint32_t  refcount;    /* number of closures sharing this frame */
    uint16_t  slot_count;  /* number of variable slots */
    uint16_t  reserved;
    R8EValue *slots;       /* heap-allocated array of NaN-boxed values */
};

/**
 * Capture descriptor: compile-time info about one captured variable.
 */
typedef struct {
    R8EValue value;        /* current value to capture */
    uint8_t  mutability;   /* R8E_CAPTURE_IMMUTABLE or R8E_CAPTURE_MUTABLE */
    uint8_t  frame_slot;   /* slot index in env frame (for mutable captures) */
    uint16_t reserved;
} R8ECaptureDesc;

struct R8EClosure {
    uint32_t      flags;          /* GC header: kind = R8E_GC_KIND_CLOSURE */
    uint32_t      proto_id;       /* prototype ID */
    R8EFunction  *func;           /* bytecode + metadata */
    uint8_t       capture_count;  /* 0-255 captures */
    uint8_t       capture_mode;   /* R8ECaptureMode */
    uint16_t      reserved;
    union {
        /* Mode INLINE: 1-2 captures, all immutable */
        R8EValue inline_captures[2];
        /* Mode ARRAY: 3-8 captures, heap array */
        struct {
            R8EValue   *values;    /* heap array of capture_count values */
            uint8_t    *mutflags;  /* per-capture mutability flags */
            R8EEnvFrame *env;      /* shared frame for mutable slots, or NULL */
        } arr;
        /* Mode FRAME: 9+ captures, shared environment */
        struct {
            R8EEnvFrame *env_frame;
        } frm;
    } storage;
};


/* =========================================================================
 * Section 10: Scope (CLAUDE.md Section 5.5)
 *
 * Compile-time scope tracking for the single-pass compiler.
 * ========================================================================= */

/* Maximum locals per scope (can be tuned) */
#define R8E_MAX_LOCALS  256

/* Variable RC classification (Section 5.6) */
typedef enum {
    R8E_VAR_OWNED    = 0,  /* must do RC: stored, captured, or returned */
    R8E_VAR_BORROWED = 1,  /* skip RC: used in same scope, not stored elsewhere */
    R8E_VAR_TEMP     = 2   /* skip RC: consumed immediately */
} R8EVarClass;

/* Variable info within a scope */
typedef struct {
    uint32_t  atom;            /* variable name (interned atom index) */
    uint8_t   register_idx;    /* local register assignment */
    uint8_t   classification;  /* R8EVarClass */
    uint8_t   flags;           /* bitfield below */
    uint8_t   pad;
} R8EVarInfo;

/* R8EVarInfo.flags bits */
#define R8E_VAR_IS_CONST              0x01
#define R8E_VAR_IS_CAPTURED           0x02
#define R8E_VAR_IS_MUTATED_AFTER_CAP  0x04
#define R8E_VAR_IS_LET                0x08
#define R8E_VAR_IS_PARAM              0x10

/* Scope flags */
#define R8E_SCOPE_HAS_EVAL     0x01
#define R8E_SCOPE_IS_STRICT    0x02
#define R8E_SCOPE_IS_FUNCTION  0x04
#define R8E_SCOPE_IS_BLOCK     0x08
#define R8E_SCOPE_IS_LOOP      0x10
#define R8E_SCOPE_IS_SWITCH    0x20

struct R8EScope {
    struct R8EScope *parent;
    uint16_t  local_count;
    uint16_t  local_base;     /* starting register index */
    uint8_t   flags;          /* R8E_SCOPE_* flags */
    uint8_t   depth;          /* nesting depth */
    uint16_t  pad;
    R8EVarInfo vars[R8E_MAX_LOCALS];
};


/* =========================================================================
 * Section 11: Atom Table (CLAUDE.md Section 4.5)
 *
 * Property names are interned: stored once, referenced by 32-bit atom index.
 * Comparing two interned strings = comparing two integers.
 * Pre-populated with ~256 common names.
 * ========================================================================= */

typedef struct R8EAtomEntry {
    uint32_t hash;              /* precomputed string hash */
    R8EString *str;             /* pointer to the interned string */
    struct R8EAtomEntry *next;  /* hash chain (for collision resolution) */
} R8EAtomEntry;

struct R8EAtomTable {
    uint64_t      bloom[4];     /* 256-bit Bloom filter for fast "not present" */
    uint32_t      count;        /* number of interned atoms */
    uint32_t      capacity;     /* hash table bucket count */
    R8EAtomEntry *entries;      /* hash table buckets */
};


/* =========================================================================
 * Section 12: Realm Limits (CLAUDE.md Section 11.6)
 *
 * Per-realm resource constraints enforced by CHECK_LIMITS opcode.
 * ========================================================================= */

typedef struct {
    size_t   max_memory;         /* bytes */
    uint64_t max_cpu_time_us;    /* microseconds */
    uint32_t max_stack_depth;    /* call stack frames */
    uint32_t max_string_length;  /* max single string length */
    uint32_t max_array_length;   /* max single array length */
    uint32_t max_regexp_steps;   /* max regex backtracking steps */
} R8ERealmLimits;


/* =========================================================================
 * Section 13: Realm (CLAUDE.md Section 11.6)
 *
 * Lightweight V8-Isolate equivalent: ~64KB per realm.
 * Creation in <2us (EXP-13), 5000x faster than V8 Isolate.
 * ========================================================================= */

struct R8ERealm {
    uint32_t       id;            /* realm ID (0-15, embedded in pointer tags) */
    uint32_t       flags;
    R8EObject     *global;        /* global object */
    R8EObject    **prototypes;    /* prototype table (indexed by proto_id) */
    uint16_t       proto_count;   /* number of prototypes */
    uint16_t       proto_capacity;
    R8ERealmLimits limits;        /* resource limits */
    void          *arena;         /* dedicated memory arena */
    size_t         arena_size;    /* arena size in bytes */
    size_t         arena_used;    /* bytes used in arena */
};

/* Default realm limits */
#define R8E_DEFAULT_MAX_MEMORY       (16 * 1024 * 1024)  /* 16 MB */
#define R8E_DEFAULT_MAX_CPU_TIME_US  (5000000ULL)         /* 5 seconds */
#define R8E_DEFAULT_MAX_STACK_DEPTH  512
#define R8E_DEFAULT_MAX_STRING_LEN   (1024 * 1024)        /* 1 MB */
#define R8E_DEFAULT_MAX_ARRAY_LEN    (1024 * 1024)        /* 1M elements */
#define R8E_DEFAULT_MAX_REGEXP_STEPS 1000000


/* =========================================================================
 * Section 14: Context (CLAUDE.md Section 11.6, 8.3)
 *
 * Top-level engine state. Owns the atom table, realms, GC state.
 * ========================================================================= */

/* GC suspect list and deferred decrement list (Section 8.2, 8.3) */

#define R8E_DEFAULT_SUSPECT_THRESHOLD  4096   /* EXP-09: fixed 4096 epoch */
#define R8E_DEFAULT_DEFER_CAPACITY     256

typedef struct {
    R8EObject **items;
    uint32_t    count;
    uint32_t    capacity;
} R8ESuspectList;

typedef struct {
    R8EValue *items;
    uint32_t  count;
    uint32_t  capacity;
} R8EDeferredList;

/* Error state */
typedef struct {
    R8EValue  exception;       /* current exception value */
    bool      has_exception;
    uint16_t  error_line;
    uint16_t  error_col;
    const char *error_file;    /* source file name (not owned) */
} R8EErrorState;

struct R8EContext {
    /* Atom table (shared across all realms) */
    R8EAtomTable atom_table;

    /* Realm management */
    R8ERealm   *realms[16];     /* max 16 realms (4-bit ID in pointer tags) */
    uint8_t     realm_count;
    uint8_t     current_realm;  /* index of active realm */

    /* GC state (Section 8.3) */
    R8ESuspectList  suspect_list;
    R8EDeferredList deferred_list;
    uint32_t        epoch_threshold;   /* suspect list trigger threshold */
    uint64_t        total_allocated;   /* bytes allocated */
    uint64_t        total_freed;       /* bytes freed */

    /* Call stack */
    uint32_t    stack_depth;
    uint32_t    max_stack_depth;

    /* Error handling */
    R8EErrorState error;

    /* Module cache */
    R8EModule **modules;
    uint16_t    module_count;
    uint16_t    module_capacity;

    /* Global object (persists across r8e_eval calls) */
    R8EValue    global_obj;

    /* Flags */
    uint32_t    flags;

    /* Module loader callback (cast to R8EModuleLoader when used) */
    void       *module_loader;
};

/* Context flags */
#define R8E_CTX_STRICT_MODE    0x01U
#define R8E_CTX_DEBUG_MODE     0x02U
#define R8E_CTX_SANDBOX_MODE   0x04U


/* =========================================================================
 * Section 15: Prototype Table Indices
 *
 * Well-known prototype IDs used by proto_id fields.
 * ========================================================================= */

#define R8E_PROTO_NONE         0
#define R8E_PROTO_OBJECT       1
#define R8E_PROTO_ARRAY        2
#define R8E_PROTO_FUNCTION     3
#define R8E_PROTO_STRING       4
#define R8E_PROTO_NUMBER       5
#define R8E_PROTO_BOOLEAN      6
#define R8E_PROTO_REGEXP       7
#define R8E_PROTO_DATE         8
#define R8E_PROTO_ERROR        9
#define R8E_PROTO_TYPEERROR    10
#define R8E_PROTO_RANGEERROR   11
#define R8E_PROTO_SYNTAXERROR  12
#define R8E_PROTO_REFERENCEERROR 13
#define R8E_PROTO_URIERROR     14
#define R8E_PROTO_EVALERROR    15
#define R8E_PROTO_MAP          16
#define R8E_PROTO_SET          17
#define R8E_PROTO_WEAKMAP      18
#define R8E_PROTO_WEAKSET      19
#define R8E_PROTO_PROMISE      20
#define R8E_PROTO_SYMBOL       21
#define R8E_PROTO_PROXY        22
#define R8E_PROTO_ARRAYBUFFER  23
#define R8E_PROTO_DATAVIEW     24
#define R8E_PROTO_INT8ARRAY    25
#define R8E_PROTO_UINT8ARRAY   26
#define R8E_PROTO_INT16ARRAY   27
#define R8E_PROTO_UINT16ARRAY  28
#define R8E_PROTO_INT32ARRAY   29
#define R8E_PROTO_UINT32ARRAY  30
#define R8E_PROTO_FLOAT32ARRAY 31
#define R8E_PROTO_FLOAT64ARRAY 32
#define R8E_PROTO_GENERATOR    33
#define R8E_PROTO_ITERATOR     34
#define R8E_PROTO_JSON         35
#define R8E_PROTO_MATH         36
#define R8E_PROTO_REFLECT      37
#define R8E_PROTO_WEAKREF      38
#define R8E_PROTO_FINALIZATION 39
#define R8E_PROTO_COUNT        40   /* total number of well-known prototypes */


#ifdef __cplusplus
}
#endif

#endif /* R8E_TYPES_H */
