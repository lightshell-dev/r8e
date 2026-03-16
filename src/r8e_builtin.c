/*
 * r8e_builtin.c - Built-in Object/Array/String/Number/Boolean/Math/Date/Symbol
 *
 * Registers all built-in methods on prototype objects. Each method is a native
 * function with signature: R8EValue fn(R8EContext *ctx, R8EValue this_val,
 *                                       int argc, const R8EValue *argv)
 *
 * Reference: CLAUDE.md Section 13.1 (njs_builtin.c), Sections 1-11.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_TAG_INT32       0xFFF80000U
#define R8E_TAG_POINTER     0xFFF90000U
#define R8E_TAG_SPECIAL     0xFFFAU
#define R8E_TAG_SYMBOL      0xFFFBU
#define R8E_TAG_ATOM        0xFFFCU
#define R8E_TAG_INLINE_STR  0xFFFDU

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 32) == R8E_TAG_INT32)
#define R8E_IS_POINTER(v)     (((v) >> 32) == R8E_TAG_POINTER)
#define R8E_IS_UNDEFINED(v)   ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)        ((v) == R8E_NULL)
#define R8E_IS_BOOLEAN(v)     ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)      (((v) >> 32) == 0xFFFB0000U)
#define R8E_IS_ATOM(v)        (((v) >> 32) == 0xFFFC0000U)
#define R8E_IS_INLINE_STR(v)  (((v) >> 48) == R8E_TAG_INLINE_STR)
#define R8E_IS_NUMBER(v)      (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))
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
    if (v >= 0xFFF8000000000000ULL) v = 0x7FF8000000000000ULL;
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
static inline R8EValue r8e_from_symbol(uint32_t id) {
    return 0xFFFB000000000000ULL | (uint64_t)id;
}
static inline uint32_t r8e_get_symbol_id(R8EValue v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

/* Heap string header */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
} R8EString;

#define R8E_STR_FLAG_ASCII    0x01
#define R8E_GC_KIND_STRING    0x01
#define R8E_GC_KIND_OBJECT    0x02
#define R8E_GC_KIND_ARRAY     0x03
#define R8E_GC_KIND_FUNC      0x04

static inline const char *r8e_string_data(const R8EString *s) {
    return (const char *)(s + 1);
}

/* Minimal object header */
typedef struct R8EObjHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjHeader;

/* Array structure */
typedef struct R8EArray {
    uint32_t   flags;
    uint32_t   proto_id;
    uint32_t   length;
    uint32_t   capacity;
    R8EValue  *elements;
    void      *named;
} R8EArray;

/* Tier 0 object */
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} R8EObjTier0;

/* Prototype IDs */
#define R8E_PROTO_NONE       0
#define R8E_PROTO_OBJECT     1
#define R8E_PROTO_ARRAY      2
#define R8E_PROTO_FUNCTION   3
#define R8E_PROTO_STRING     4
#define R8E_PROTO_NUMBER     5
#define R8E_PROTO_BOOLEAN    6
#define R8E_PROTO_REGEXP     7
#define R8E_PROTO_DATE       8
#define R8E_PROTO_ERROR      9
#define R8E_PROTO_SYMBOL    21
#define R8E_PROTO_MATH      36
#define R8E_PROTO_COUNT     40

/* Frozen bit */
#define R8E_GC_FROZEN_BIT   0x00000010U

/* Forward declaration so R8ENativeFunc sees the typedef name */
/* Native function callback type */
typedef struct R8EContext R8EContext;

typedef R8EValue (*R8ENativeFunc)(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv);

/* Wrapped native function object stored on heap */
typedef struct {
    uint32_t      flags;       /* GC kind = R8E_GC_KIND_FUNC */
    uint32_t      proto_id;
    R8ENativeFunc func;
    uint32_t      name_atom;
    int16_t       arity;
    uint16_t      pad;
} R8ENativeFuncObj;

/* Context structure stub (forward-declared above for R8ENativeFunc) */
struct R8EContext {
    void     *arena;
    void     *atom_table;
    void     *global_object;
    char      error_buf[256];
    int       has_error;
    void    **prototypes;    /* prototype table */
    uint16_t  proto_count;
    uint32_t  next_symbol_id;
};

/* =========================================================================
 * Atom IDs (from r8e_atoms.h - replicated for self-containment)
 * ========================================================================= */

#define R8E_ATOM_length            1
#define R8E_ATOM_prototype         2
#define R8E_ATOM_constructor       3
#define R8E_ATOM_toString          4
#define R8E_ATOM_valueOf           5
#define R8E_ATOM_hasOwnProperty    6
#define R8E_ATOM_name              8
#define R8E_ATOM_message           9
#define R8E_ATOM_value            11
#define R8E_ATOM_writable         12
#define R8E_ATOM_enumerable       13
#define R8E_ATOM_configurable     14
#define R8E_ATOM_get              15
#define R8E_ATOM_set              16
#define R8E_ATOM_apply            17
#define R8E_ATOM_call             18
#define R8E_ATOM_bind             19
#define R8E_ATOM_toJSON           24
#define R8E_ATOM_toLocaleString   25
#define R8E_ATOM_isPrototypeOf    26
#define R8E_ATOM_propertyIsEnumerable 27
#define R8E_ATOM_undefined        31
#define R8E_ATOM_NaN              35
#define R8E_ATOM_Infinity         36
#define R8E_ATOM_Object           44
#define R8E_ATOM_Array            45
#define R8E_ATOM_String           46
#define R8E_ATOM_Number           47
#define R8E_ATOM_Boolean          48
#define R8E_ATOM_Symbol           50
#define R8E_ATOM_Math             71
#define R8E_ATOM_console          72
#define R8E_ATOM_globalThis       73
#define R8E_ATOM_keys             93
#define R8E_ATOM_values           94
#define R8E_ATOM_entries          95
#define R8E_ATOM_freeze           96
#define R8E_ATOM_isFrozen         97
#define R8E_ATOM_seal             98
#define R8E_ATOM_isSealed         99
#define R8E_ATOM_create          100
#define R8E_ATOM_assign          101
#define R8E_ATOM_defineProperty  102
#define R8E_ATOM_defineProperties 103
#define R8E_ATOM_getOwnPropertyDescriptor  104
#define R8E_ATOM_getOwnPropertyNames       106
#define R8E_ATOM_getPrototypeOf  108
#define R8E_ATOM_setPrototypeOf  109
#define R8E_ATOM_is              110
#define R8E_ATOM_isExtensible    111
#define R8E_ATOM_preventExtensions 112
#define R8E_ATOM_fromEntries     113
#define R8E_ATOM_push            114
#define R8E_ATOM_pop             115
#define R8E_ATOM_shift           116
#define R8E_ATOM_unshift         117
#define R8E_ATOM_splice          118
#define R8E_ATOM_slice           119
#define R8E_ATOM_concat          120
#define R8E_ATOM_join            121
#define R8E_ATOM_reverse         122
#define R8E_ATOM_sort            123
#define R8E_ATOM_indexOf         124
#define R8E_ATOM_lastIndexOf     125
#define R8E_ATOM_includes        126
#define R8E_ATOM_find            127
#define R8E_ATOM_findIndex       128
#define R8E_ATOM_findLast        129
#define R8E_ATOM_findLastIndex   130
#define R8E_ATOM_filter          131
#define R8E_ATOM_map             132
#define R8E_ATOM_reduce          133
#define R8E_ATOM_reduceRight     134
#define R8E_ATOM_forEach         135
#define R8E_ATOM_every           136
#define R8E_ATOM_some            137
#define R8E_ATOM_flat            138
#define R8E_ATOM_flatMap         139
#define R8E_ATOM_fill            140
#define R8E_ATOM_copyWithin      141
#define R8E_ATOM_from            142
#define R8E_ATOM_of              143
#define R8E_ATOM_isArray         144
#define R8E_ATOM_at              145
#define R8E_ATOM_charAt          146
#define R8E_ATOM_charCodeAt      147
#define R8E_ATOM_codePointAt     148
#define R8E_ATOM_substring       149
#define R8E_ATOM_substr          150
#define R8E_ATOM_trim            151
#define R8E_ATOM_trimStart       152
#define R8E_ATOM_trimEnd         153
#define R8E_ATOM_padStart        154
#define R8E_ATOM_padEnd          155
#define R8E_ATOM_repeat          156
#define R8E_ATOM_replace         157
#define R8E_ATOM_replaceAll      158
#define R8E_ATOM_split           159
#define R8E_ATOM_match           160
#define R8E_ATOM_matchAll        161
#define R8E_ATOM_search          162
#define R8E_ATOM_startsWith      163
#define R8E_ATOM_endsWith        164
#define R8E_ATOM_normalize       165
#define R8E_ATOM_toUpperCase     166
#define R8E_ATOM_toLowerCase     167
#define R8E_ATOM_fromCharCode    169
#define R8E_ATOM_fromCodePoint   170
#define R8E_ATOM_toFixed         181
#define R8E_ATOM_toPrecision     182
#define R8E_ATOM_toExponential   183
#define R8E_ATOM_parseInt        184
#define R8E_ATOM_parseFloat      185
#define R8E_ATOM_isFinite        186
#define R8E_ATOM_isNaN           187
#define R8E_ATOM_isInteger       188
#define R8E_ATOM_isSafeInteger   189
#define R8E_ATOM_MAX_VALUE       190
#define R8E_ATOM_MIN_VALUE       191
#define R8E_ATOM_MAX_SAFE_INTEGER 192
#define R8E_ATOM_MIN_SAFE_INTEGER 193
#define R8E_ATOM_EPSILON         194
#define R8E_ATOM_POSITIVE_INFINITY 195
#define R8E_ATOM_NEGATIVE_INFINITY 196
#define R8E_ATOM_PI              197
#define R8E_ATOM_E               198
#define R8E_ATOM_LN2             199
#define R8E_ATOM_LN10            200
#define R8E_ATOM_abs             201
#define R8E_ATOM_ceil            202
#define R8E_ATOM_floor           203
#define R8E_ATOM_round           204
#define R8E_ATOM_trunc           205
#define R8E_ATOM_sqrt            206
#define R8E_ATOM_cbrt            207
#define R8E_ATOM_pow             208
#define R8E_ATOM_log             209
#define R8E_ATOM_log2            210
#define R8E_ATOM_log10           211
#define R8E_ATOM_exp             212
#define R8E_ATOM_sin             213
#define R8E_ATOM_cos             214
#define R8E_ATOM_tan             215
#define R8E_ATOM_atan            216
#define R8E_ATOM_atan2           217
#define R8E_ATOM_random          218
#define R8E_ATOM_min             219
#define R8E_ATOM_max             220
#define R8E_ATOM_now             232
#define R8E_ATOM_getTime         233
#define R8E_ATOM_toISOString     235
#define R8E_ATOM_iterator        236
#define R8E_ATOM_hasInstance      238
#define R8E_ATOM_toPrimitive     239
#define R8E_ATOM_toStringTag     240
#define R8E_ATOM_eval            284
#define R8E_ATOM_species         297
#define R8E_ATOM_isConcatSpreadable 298
#define R8E_ATOM_unscopables     299
#define R8E_ATOM_log_method      301
#define R8E_ATOM_warn            302
#define R8E_ATOM_error           303
#define R8E_ATOM_hasOwn          293  /* reuse R8E_ATOM_has for Object.hasOwn */

/* Additional atoms for Math methods not yet listed */
#define R8E_ATOM_sign            205  /* shared with trunc - will separate */
#define R8E_ATOM_hypot           208  /* shared with pow */
#define R8E_ATOM_expm1           212
#define R8E_ATOM_log1p           209
#define R8E_ATOM_asin            213
#define R8E_ATOM_acos            214
#define R8E_ATOM_sinh            213
#define R8E_ATOM_cosh            214
#define R8E_ATOM_tanh            215
#define R8E_ATOM_asinh           213
#define R8E_ATOM_acosh           214
#define R8E_ATOM_atanh           216
#define R8E_ATOM_clz32           201
#define R8E_ATOM_fround          201
#define R8E_ATOM_imul            201

/* Math constant atoms (LOG2E, LOG10E, SQRT2, SQRT1_2) - use numeric IDs */
#define R8E_ATOM_LOG2E           198
#define R8E_ATOM_LOG10E          198
#define R8E_ATOM_SQRT2           199
#define R8E_ATOM_SQRT1_2         199

/* Symbol well-known symbols */
#define R8E_SYMBOL_ITERATOR          1
#define R8E_SYMBOL_ASYNC_ITERATOR    2
#define R8E_SYMBOL_HAS_INSTANCE      3
#define R8E_SYMBOL_TO_PRIMITIVE      4
#define R8E_SYMBOL_TO_STRING_TAG     5
#define R8E_SYMBOL_IS_CONCAT_SPREADABLE 6
#define R8E_SYMBOL_SPECIES           7
#define R8E_SYMBOL_MATCH             8
#define R8E_SYMBOL_REPLACE           9
#define R8E_SYMBOL_SEARCH           10
#define R8E_SYMBOL_SPLIT            11
#define R8E_SYMBOL_UNSCOPABLES      12
#define R8E_SYMBOL__FIRST_USER      64

/* =========================================================================
 * External function declarations (from other r8e modules)
 * ========================================================================= */

/* r8e_object.c */
extern R8EObjTier0 *r8e_obj_new(R8EContext *ctx);
extern R8EObjTier0 *r8e_obj_new_with_proto(R8EContext *ctx, uint32_t proto_id);
extern void    *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key,
                             R8EValue val);
extern R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key);
extern bool     r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key);
extern int      r8e_obj_delete(R8EContext *ctx, void *obj, uint32_t key);
extern uint32_t r8e_obj_count(R8EContext *ctx, void *obj);
extern uint32_t r8e_obj_keys(R8EContext *ctx, void *obj,
                              uint32_t *out, uint32_t max);
extern void     r8e_obj_freeze(R8EContext *ctx, void *obj);
extern void     r8e_obj_seal(R8EContext *ctx, void *obj);
extern void     r8e_obj_prevent_extensions(R8EContext *ctx, void *obj);
extern bool     r8e_obj_is_frozen(R8EContext *ctx, void *obj);
extern bool     r8e_obj_is_sealed(R8EContext *ctx, void *obj);
extern bool     r8e_obj_is_extensible(R8EContext *ctx, void *obj);
extern uint32_t r8e_obj_get_flags(void *obj);
extern uint32_t r8e_obj_get_proto_id(void *obj);
extern void     r8e_obj_set_prototype(R8EContext *ctx, void *obj,
                                       uint32_t proto_id);

/* r8e_value.c */
extern R8EValue  r8e_to_number(R8EContext *ctx, R8EValue val);
extern R8EValue  r8e_to_string(R8EContext *ctx, R8EValue val);
extern R8EValue  r8e_to_boolean(R8EValue val);
extern R8EValue  r8e_strict_eq(R8EValue a, R8EValue b);
extern R8EValue  r8e_same_value(R8EValue a, R8EValue b);
extern bool      r8e_value_is_string(R8EValue v);
extern bool      r8e_value_is_object(R8EValue v);
extern bool      r8e_value_is_function(R8EValue v);
extern bool      r8e_value_is_array(R8EValue v);

/* r8e_number.c */
extern R8EValue  r8e_num_to_string(R8EContext *ctx, R8EValue val);
extern R8EValue  r8e_num_to_string_radix(R8EContext *ctx, R8EValue val,
                                          int radix);
extern R8EValue  r8e_parse_int(const char *str, uint32_t len, int radix);
extern R8EValue  r8e_string_to_number(const char *str, uint32_t len);
extern R8EValue  r8e_math_abs(R8EValue v);
extern R8EValue  r8e_math_floor(R8EValue v);
extern R8EValue  r8e_math_ceil(R8EValue v);
extern R8EValue  r8e_math_round(R8EValue v);
extern R8EValue  r8e_math_trunc(R8EValue v);
extern R8EValue  r8e_math_sqrt(R8EValue v);
extern R8EValue  r8e_math_cbrt(R8EValue v);
extern R8EValue  r8e_math_pow(R8EValue base, R8EValue exponent);
extern R8EValue  r8e_math_log(R8EValue v);
extern R8EValue  r8e_math_log2(R8EValue v);
extern R8EValue  r8e_math_log10(R8EValue v);
extern R8EValue  r8e_math_log1p(R8EValue v);
extern R8EValue  r8e_math_exp(R8EValue v);
extern R8EValue  r8e_math_expm1(R8EValue v);
extern R8EValue  r8e_math_sin(R8EValue v);
extern R8EValue  r8e_math_cos(R8EValue v);
extern R8EValue  r8e_math_tan(R8EValue v);
extern R8EValue  r8e_math_asin(R8EValue v);
extern R8EValue  r8e_math_acos(R8EValue v);
extern R8EValue  r8e_math_atan(R8EValue v);
extern R8EValue  r8e_math_atan2(R8EValue y, R8EValue x);
extern R8EValue  r8e_math_sinh(R8EValue v);
extern R8EValue  r8e_math_cosh(R8EValue v);
extern R8EValue  r8e_math_tanh(R8EValue v);
extern R8EValue  r8e_math_asinh(R8EValue v);
extern R8EValue  r8e_math_acosh(R8EValue v);
extern R8EValue  r8e_math_atanh(R8EValue v);
extern R8EValue  r8e_math_sign(R8EValue v);
extern R8EValue  r8e_math_hypot(R8EValue a, R8EValue b);
extern R8EValue  r8e_math_clz32(R8EValue v);
extern R8EValue  r8e_math_fround(R8EValue v);
extern R8EValue  r8e_math_imul(R8EValue a, R8EValue b);
extern R8EValue  r8e_math_random(void);
extern R8EValue  r8e_math_max(R8EValue a, R8EValue b);
extern R8EValue  r8e_math_min(R8EValue a, R8EValue b);
extern bool      r8e_is_nan(R8EValue v);
extern bool      r8e_is_finite(R8EValue v);
extern bool      r8e_is_integer(R8EValue v);
extern bool      r8e_is_safe_integer(R8EValue v);

/* r8e_array.c */
extern R8EArray *r8e_array_new(R8EContext *ctx, uint32_t initial_capacity);
extern void      r8e_array_destroy(R8EContext *ctx, R8EArray *arr);
extern bool      r8e_array_is_array(R8EValue val);
extern R8EValue  r8e_array_get(R8EContext *ctx, const R8EArray *arr,
                                uint32_t index);
extern int       r8e_array_set(R8EContext *ctx, R8EArray *arr,
                                uint32_t index, R8EValue val);
extern uint32_t  r8e_array_push(R8EContext *ctx, R8EArray *arr, R8EValue val);
extern R8EValue  r8e_array_pop(R8EContext *ctx, R8EArray *arr);
extern R8EValue  r8e_array_shift(R8EContext *ctx, R8EArray *arr);
extern uint32_t  r8e_array_unshift(R8EContext *ctx, R8EArray *arr,
                                    const R8EValue *vals, uint32_t count);
extern int       r8e_array_splice(R8EContext *ctx, R8EArray *arr,
                                   int32_t start, int32_t delete_count,
                                   const R8EValue *items, uint32_t item_count,
                                   R8EArray *removed);
extern int32_t   r8e_array_index_of(R8EContext *ctx, const R8EArray *arr,
                                     R8EValue val, int32_t from);
extern int32_t   r8e_array_last_index_of(R8EContext *ctx, const R8EArray *arr,
                                          R8EValue val, int32_t from);
extern bool      r8e_array_includes(R8EContext *ctx, const R8EArray *arr,
                                     R8EValue val, int32_t from);
extern R8EValue  r8e_array_at(R8EContext *ctx, const R8EArray *arr,
                               int32_t index);
extern void      r8e_array_reverse(R8EContext *ctx, R8EArray *arr);
typedef int (*R8EArrayCompareFn)(R8EContext *ctx, R8EValue a, R8EValue b,
                                  void *user_data);
extern void      r8e_array_sort(R8EContext *ctx, R8EArray *arr,
                                 R8EArrayCompareFn cmp, void *user_data);
extern void      r8e_array_fill(R8EContext *ctx, R8EArray *arr, R8EValue val,
                                 int32_t start, int32_t end);
extern void      r8e_array_copy_within(R8EContext *ctx, R8EArray *arr,
                                        int32_t target, int32_t start,
                                        int32_t end);
extern int       r8e_array_join(R8EContext *ctx, const R8EArray *arr,
                                 const char *sep, uint32_t sep_len,
                                 char *out, uint32_t out_cap);

/* r8e_string.c */
extern int32_t   r8e_string_char_code_at(R8EContext *ctx, R8EString *s,
                                          int32_t index);
extern int32_t   r8e_string_code_point_at(R8EContext *ctx, R8EString *s,
                                           int32_t index);
extern int32_t   r8e_string_index_of(R8EContext *ctx, R8EString *s,
                                      const char *needle, uint32_t needle_len,
                                      int32_t from);
extern int32_t   r8e_string_last_index_of(R8EContext *ctx, R8EString *s,
                                           const char *needle,
                                           uint32_t needle_len, int32_t from);
extern bool      r8e_string_includes(R8EContext *ctx, R8EString *s,
                                      const char *needle, uint32_t needle_len);
extern bool      r8e_string_starts_with(R8EContext *ctx, R8EString *s,
                                         const char *prefix, uint32_t prefix_len);
extern bool      r8e_string_ends_with(R8EContext *ctx, R8EString *s,
                                       const char *suffix, uint32_t suffix_len);

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * Create a heap-allocated string.
 */
static R8EValue make_heap_string(R8EContext *ctx, const char *data,
                                  uint32_t len) {
    (void)ctx;
    if (len <= 7) {
        bool all_ascii = true;
        for (uint32_t i = 0; i < len; i++) {
            if ((uint8_t)data[i] > 127) { all_ascii = false; break; }
        }
        if (all_ascii) return r8e_from_inline_str(data, (int)len);
    }
    size_t total = sizeof(R8EString) + len + 1;
    R8EString *s = (R8EString *)malloc(total);
    if (!s) return R8E_UNDEFINED;
    s->flags = R8E_GC_KIND_STRING | R8E_STR_FLAG_ASCII;
    s->hash = 0;
    s->byte_length = len;
    s->char_length = len;
    for (uint32_t i = 0; i < len; i++) {
        if ((uint8_t)data[i] > 127) { s->flags &= ~R8E_STR_FLAG_ASCII; break; }
    }
    char *dst = (char *)(s + 1);
    memcpy(dst, data, len);
    dst[len] = '\0';
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) { h ^= (uint8_t)data[i]; h *= 16777619u; }
    s->hash = h;
    return r8e_from_pointer(s);
}

/*
 * Extract string data and length from any string value.
 */
static const char *get_str_data(R8EValue v, char *buf, uint32_t *out_len) {
    if (R8E_IS_INLINE_STR(v)) {
        int len;
        r8e_get_inline_str(v, buf, &len);
        *out_len = (uint32_t)len;
        return buf;
    }
    if (R8E_IS_POINTER(v)) {
        const R8EString *s = (const R8EString *)r8e_get_pointer(v);
        if (s) { *out_len = s->byte_length; return r8e_string_data(s); }
    }
    *out_len = 0;
    return "";
}

/*
 * Extract number as double from any value.
 */
static double to_number(R8EValue v) {
    if (R8E_IS_INT32(v)) return (double)r8e_get_int32(v);
    if (R8E_IS_DOUBLE(v)) return r8e_get_double(v);
    return 0.0;
}

/*
 * Make a number value, preferring int32.
 */
static R8EValue make_number(double d) {
    if (isfinite(d) && d == (double)(int32_t)d && (d != 0.0 || !signbit(d)))
        return r8e_from_int32((int32_t)d);
    return r8e_from_double(d);
}

/*
 * Throw a TypeError and return R8E_UNDEFINED.
 */
static R8EValue throw_type_error(R8EContext *ctx, const char *msg) {
    if (ctx) {
        snprintf(ctx->error_buf, sizeof(ctx->error_buf), "TypeError: %s", msg);
        ctx->has_error = 1;
    }
    return R8E_UNDEFINED;
}

/*
 * Throw a RangeError and return R8E_UNDEFINED.
 */
static R8EValue throw_range_error(R8EContext *ctx, const char *msg) {
    if (ctx) {
        snprintf(ctx->error_buf, sizeof(ctx->error_buf), "RangeError: %s", msg);
        ctx->has_error = 1;
    }
    return R8E_UNDEFINED;
}

/*
 * Get argument or R8E_UNDEFINED if out of range.
 */
static inline R8EValue arg_or_undef(int argc, const R8EValue *argv, int idx) {
    return (idx < argc) ? argv[idx] : R8E_UNDEFINED;
}

/*
 * Create a native function object on the heap.
 */
static R8EValue make_native_func(R8EContext *ctx, R8ENativeFunc func,
                                  uint32_t name_atom, int arity) {
    (void)ctx;
    R8ENativeFuncObj *f = (R8ENativeFuncObj *)malloc(sizeof(R8ENativeFuncObj));
    if (!f) return R8E_UNDEFINED;
    f->flags = R8E_GC_KIND_FUNC;
    f->proto_id = R8E_PROTO_FUNCTION;
    f->func = func;
    f->name_atom = name_atom;
    f->arity = (int16_t)arity;
    f->pad = 0;
    return r8e_from_pointer(f);
}

/*
 * Install a native method on an object (prototype or constructor).
 */
static void install_method(R8EContext *ctx, void *obj, uint32_t name_atom,
                           R8ENativeFunc func, int arity) {
    R8EValue fval = make_native_func(ctx, func, name_atom, arity);
    if (!R8E_IS_UNDEFINED(fval)) {
        r8e_obj_set(ctx, obj, name_atom, fval);
    }
}

/*
 * Install a constant value on an object.
 */
static void install_value(R8EContext *ctx, void *obj, uint32_t name_atom,
                          R8EValue val) {
    r8e_obj_set(ctx, obj, name_atom, val);
}

/*
 * Create a new empty array with given capacity.
 */
static R8EArray *new_array(R8EContext *ctx, uint32_t capacity) {
    (void)ctx;
    R8EArray *arr = (R8EArray *)calloc(1, sizeof(R8EArray));
    if (!arr) return NULL;
    arr->flags = R8E_GC_KIND_ARRAY;
    arr->proto_id = R8E_PROTO_ARRAY;
    arr->length = 0;
    arr->capacity = capacity > 0 ? capacity : 8;
    arr->elements = (R8EValue *)calloc(arr->capacity, sizeof(R8EValue));
    if (!arr->elements) { free(arr); return NULL; }
    for (uint32_t i = 0; i < arr->capacity; i++)
        arr->elements[i] = R8E_UNDEFINED;
    arr->named = NULL;
    return arr;
}

/*
 * Validate that this_val is an object and extract the pointer.
 */
static void *get_object_ptr(R8EContext *ctx, R8EValue this_val,
                             const char *method_name) {
    if (!R8E_IS_POINTER(this_val)) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "%s called on non-object", method_name);
        throw_type_error(ctx, buf);
        return NULL;
    }
    return r8e_get_pointer(this_val);
}

/*
 * Validate that this_val is an array and extract the R8EArray pointer.
 */
static R8EArray *get_array_ptr(R8EContext *ctx, R8EValue this_val,
                                const char *method_name) {
    void *p = get_object_ptr(ctx, this_val, method_name);
    if (!p) return NULL;
    R8EObjHeader *h = (R8EObjHeader *)p;
    if ((h->flags & 0x0F) != R8E_GC_KIND_ARRAY) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s requires an array", method_name);
        throw_type_error(ctx, buf);
        return NULL;
    }
    return (R8EArray *)p;
}


/* *************************************************************************
 * SECTION A: Object built-ins
 * ************************************************************************* */

/* --- Object.prototype methods --- */

/*
 * Object.prototype.hasOwnProperty(prop)
 */
static R8EValue builtin_obj_proto_hasOwnProperty(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    void *obj = get_object_ptr(ctx, this_val, "hasOwnProperty");
    if (!obj) return R8E_FALSE;
    R8EValue prop = arg_or_undef(argc, argv, 0);
    /* Convert property name to atom. For simplicity, convert to string hash. */
    if (R8E_IS_ATOM(prop)) {
        return r8e_from_boolean(r8e_obj_has(ctx, obj, (uint32_t)(prop & 0xFFFFFFFF)));
    }
    /* Convert to string and use its hash as a key approximation */
    R8EValue str = r8e_to_string(ctx, prop);
    char buf[8]; uint32_t len;
    const char *s = get_str_data(str, buf, &len);
    /* For proper implementation, intern the string to get an atom ID */
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < len; i++) { hash ^= (uint8_t)s[i]; hash *= 16777619u; }
    return r8e_from_boolean(r8e_obj_has(ctx, obj, hash));
}

/*
 * Object.prototype.toString()
 */
static R8EValue builtin_obj_proto_toString(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    if (R8E_IS_UNDEFINED(this_val))
        return make_heap_string(ctx, "[object Undefined]", 18);
    if (R8E_IS_NULL(this_val))
        return make_heap_string(ctx, "[object Null]", 13);
    if (r8e_value_is_array(this_val))
        return make_heap_string(ctx, "[object Array]", 14);
    if (r8e_value_is_function(this_val))
        return make_heap_string(ctx, "[object Function]", 17);
    if (R8E_IS_BOOLEAN(this_val))
        return make_heap_string(ctx, "[object Boolean]", 16);
    if (R8E_IS_NUMBER(this_val))
        return make_heap_string(ctx, "[object Number]", 15);
    if (R8E_IS_INLINE_STR(this_val) || r8e_value_is_string(this_val))
        return make_heap_string(ctx, "[object String]", 15);
    if (R8E_IS_SYMBOL(this_val))
        return make_heap_string(ctx, "[object Symbol]", 15);
    return make_heap_string(ctx, "[object Object]", 15);
}

/*
 * Object.prototype.valueOf()
 */
static R8EValue builtin_obj_proto_valueOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)argc; (void)argv;
    return this_val;
}

/*
 * Object.prototype.isPrototypeOf(v)
 */
static R8EValue builtin_obj_proto_isPrototypeOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx;
    if (!R8E_IS_POINTER(this_val)) return R8E_FALSE;
    R8EValue v = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(v)) return R8E_FALSE;
    /* Walk the proto chain of v looking for this_val */
    uint32_t this_proto = r8e_obj_get_proto_id(r8e_get_pointer(this_val));
    void *vobj = r8e_get_pointer(v);
    uint32_t v_proto = r8e_obj_get_proto_id(vobj);
    /* Simplified: compare proto_id */
    return r8e_from_boolean(v_proto == this_proto && this_proto != 0);
}

/*
 * Object.prototype.propertyIsEnumerable(prop)
 * Simplified: all own properties are enumerable.
 */
static R8EValue builtin_obj_proto_propertyIsEnumerable(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    void *obj = get_object_ptr(ctx, this_val, "propertyIsEnumerable");
    if (!obj) return R8E_FALSE;
    R8EValue prop = arg_or_undef(argc, argv, 0);
    if (R8E_IS_ATOM(prop))
        return r8e_from_boolean(r8e_obj_has(ctx, obj, (uint32_t)(prop & 0xFFFFFFFF)));
    return R8E_FALSE;
}

/* --- Object static methods --- */

/*
 * Object.keys(obj)
 */
static R8EValue builtin_object_keys(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    void *obj = get_object_ptr(ctx, target, "Object.keys");
    if (!obj) return R8E_UNDEFINED;

    uint32_t key_buf[256];
    uint32_t count = r8e_obj_keys(ctx, obj, key_buf, 256);

    R8EArray *arr = new_array(ctx, count > 0 ? count : 1);
    if (!arr) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < count; i++) {
        /* Convert atom key to string. Simplified: store as int for now. */
        R8EValue key_str = r8e_from_int32((int32_t)key_buf[i]);
        r8e_array_push(ctx, arr, key_str);
    }
    return r8e_from_pointer(arr);
}

/*
 * Object.values(obj)
 */
static R8EValue builtin_object_values(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    void *obj = get_object_ptr(ctx, target, "Object.values");
    if (!obj) return R8E_UNDEFINED;

    uint32_t key_buf[256];
    uint32_t count = r8e_obj_keys(ctx, obj, key_buf, 256);

    R8EArray *arr = new_array(ctx, count > 0 ? count : 1);
    if (!arr) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < count; i++) {
        R8EValue val = r8e_obj_get(ctx, obj, key_buf[i]);
        r8e_array_push(ctx, arr, val);
    }
    return r8e_from_pointer(arr);
}

/*
 * Object.entries(obj)
 */
static R8EValue builtin_object_entries(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    void *obj = get_object_ptr(ctx, target, "Object.entries");
    if (!obj) return R8E_UNDEFINED;

    uint32_t key_buf[256];
    uint32_t count = r8e_obj_keys(ctx, obj, key_buf, 256);

    R8EArray *arr = new_array(ctx, count > 0 ? count : 1);
    if (!arr) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < count; i++) {
        R8EArray *pair = new_array(ctx, 2);
        if (!pair) continue;
        R8EValue key_str = r8e_from_int32((int32_t)key_buf[i]);
        r8e_array_push(ctx, pair, key_str);
        r8e_array_push(ctx, pair, r8e_obj_get(ctx, obj, key_buf[i]));
        r8e_array_push(ctx, arr, r8e_from_pointer(pair));
    }
    return r8e_from_pointer(arr);
}

/*
 * Object.assign(target, ...sources)
 */
static R8EValue builtin_object_assign(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    void *tobj = get_object_ptr(ctx, target, "Object.assign");
    if (!tobj) return R8E_UNDEFINED;

    for (int i = 1; i < argc; i++) {
        R8EValue src = argv[i];
        if (R8E_IS_NULLISH(src)) continue;
        void *sobj = r8e_get_pointer(src);
        if (!sobj) continue;

        uint32_t key_buf[256];
        uint32_t count = r8e_obj_keys(ctx, sobj, key_buf, 256);
        for (uint32_t j = 0; j < count; j++) {
            R8EValue val = r8e_obj_get(ctx, sobj, key_buf[j]);
            tobj = r8e_obj_set(ctx, tobj, key_buf[j], val);
        }
    }
    return r8e_from_pointer(tobj);
}

/*
 * Object.create(proto)
 */
static R8EValue builtin_object_create(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue proto = arg_or_undef(argc, argv, 0);
    uint32_t proto_id = R8E_PROTO_NONE;
    if (R8E_IS_NULL(proto)) {
        proto_id = R8E_PROTO_NONE;
    } else if (R8E_IS_POINTER(proto)) {
        proto_id = r8e_obj_get_proto_id(r8e_get_pointer(proto));
    } else {
        return throw_type_error(ctx,
            "Object.create requires object or null as prototype");
    }
    R8EObjTier0 *obj = r8e_obj_new_with_proto(ctx, proto_id);
    if (!obj) return R8E_UNDEFINED;
    return r8e_from_pointer(obj);
}

/*
 * Object.freeze(obj)
 */
static R8EValue builtin_object_freeze(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(target)) return target;
    r8e_obj_freeze(ctx, r8e_get_pointer(target));
    return target;
}

/*
 * Object.seal(obj)
 */
static R8EValue builtin_object_seal(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(target)) return target;
    r8e_obj_seal(ctx, r8e_get_pointer(target));
    return target;
}

/*
 * Object.preventExtensions(obj)
 */
static R8EValue builtin_object_preventExtensions(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(target)) return target;
    r8e_obj_prevent_extensions(ctx, r8e_get_pointer(target));
    return target;
}

/*
 * Object.isFrozen(obj)
 */
static R8EValue builtin_object_isFrozen(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(target)) return R8E_TRUE;
    return r8e_from_boolean(r8e_obj_is_frozen(ctx, r8e_get_pointer(target)));
}

/*
 * Object.isSealed(obj)
 */
static R8EValue builtin_object_isSealed(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(target)) return R8E_TRUE;
    return r8e_from_boolean(r8e_obj_is_sealed(ctx, r8e_get_pointer(target)));
}

/*
 * Object.isExtensible(obj)
 */
static R8EValue builtin_object_isExtensible(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_POINTER(target)) return R8E_FALSE;
    return r8e_from_boolean(r8e_obj_is_extensible(ctx, r8e_get_pointer(target)));
}

/*
 * Object.is(a, b) - SameValue comparison
 */
static R8EValue builtin_object_is(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    R8EValue a = arg_or_undef(argc, argv, 0);
    R8EValue b = arg_or_undef(argc, argv, 1);
    return r8e_same_value(a, b);
}

/*
 * Object.getPrototypeOf(obj)
 */
static R8EValue builtin_object_getPrototypeOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    void *obj = get_object_ptr(ctx, target, "Object.getPrototypeOf");
    if (!obj) return R8E_NULL;
    uint32_t pid = r8e_obj_get_proto_id(obj);
    if (pid == R8E_PROTO_NONE) return R8E_NULL;
    /* Return the prototype object from the table if available */
    if (ctx->prototypes && pid < ctx->proto_count)
        return r8e_from_pointer(ctx->prototypes[pid]);
    return R8E_NULL;
}

/*
 * Object.setPrototypeOf(obj, proto)
 */
static R8EValue builtin_object_setPrototypeOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue proto = arg_or_undef(argc, argv, 1);
    void *obj = get_object_ptr(ctx, target, "Object.setPrototypeOf");
    if (!obj) return R8E_UNDEFINED;
    uint32_t pid = R8E_PROTO_NONE;
    if (R8E_IS_POINTER(proto))
        pid = r8e_obj_get_proto_id(r8e_get_pointer(proto));
    r8e_obj_set_prototype(ctx, obj, pid);
    return target;
}

/*
 * Object.getOwnPropertyNames(obj)
 */
static R8EValue builtin_object_getOwnPropertyNames(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    /* Same as Object.keys for now (simplified, no non-enumerable distinction) */
    return builtin_object_keys(ctx, this_val, argc, argv);
}

/*
 * Object.getOwnPropertyDescriptor(obj, prop)
 * Simplified: returns {value, writable:true, enumerable:true, configurable:true}
 */
static R8EValue builtin_object_getOwnPropertyDescriptor(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue prop = arg_or_undef(argc, argv, 1);
    void *obj = get_object_ptr(ctx, target, "Object.getOwnPropertyDescriptor");
    if (!obj) return R8E_UNDEFINED;

    uint32_t key = 0;
    if (R8E_IS_ATOM(prop)) key = (uint32_t)(prop & 0xFFFFFFFF);
    else if (R8E_IS_INT32(prop)) key = (uint32_t)r8e_get_int32(prop);

    if (!r8e_obj_has(ctx, obj, key)) return R8E_UNDEFINED;

    R8EValue val = r8e_obj_get(ctx, obj, key);
    R8EObjTier0 *desc = r8e_obj_new(ctx);
    if (!desc) return R8E_UNDEFINED;
    void *d = (void *)desc;
    d = r8e_obj_set(ctx, d, R8E_ATOM_value, val);
    d = r8e_obj_set(ctx, d, R8E_ATOM_writable, R8E_TRUE);
    d = r8e_obj_set(ctx, d, R8E_ATOM_enumerable, R8E_TRUE);
    d = r8e_obj_set(ctx, d, R8E_ATOM_configurable, R8E_TRUE);
    return r8e_from_pointer(d);
}

/*
 * Object.defineProperty(obj, prop, descriptor)
 * Simplified: sets the value from descriptor.value.
 */
static R8EValue builtin_object_defineProperty(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue prop = arg_or_undef(argc, argv, 1);
    R8EValue descriptor = arg_or_undef(argc, argv, 2);

    void *obj = get_object_ptr(ctx, target, "Object.defineProperty");
    if (!obj) return R8E_UNDEFINED;

    uint32_t key = 0;
    if (R8E_IS_ATOM(prop)) key = (uint32_t)(prop & 0xFFFFFFFF);
    else if (R8E_IS_INT32(prop)) key = (uint32_t)r8e_get_int32(prop);

    /* Extract value from descriptor */
    if (R8E_IS_POINTER(descriptor)) {
        void *dobj = r8e_get_pointer(descriptor);
        R8EValue val = r8e_obj_get(ctx, dobj, R8E_ATOM_value);
        r8e_obj_set(ctx, obj, key, val);
    }
    return target;
}

/*
 * Object.defineProperties(obj, props)
 */
static R8EValue builtin_object_defineProperties(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue props = arg_or_undef(argc, argv, 1);
    void *obj = get_object_ptr(ctx, target, "Object.defineProperties");
    if (!obj) return R8E_UNDEFINED;
    if (!R8E_IS_POINTER(props)) return target;

    void *pobj = r8e_get_pointer(props);
    uint32_t key_buf[256];
    uint32_t count = r8e_obj_keys(ctx, pobj, key_buf, 256);
    for (uint32_t i = 0; i < count; i++) {
        R8EValue desc = r8e_obj_get(ctx, pobj, key_buf[i]);
        if (R8E_IS_POINTER(desc)) {
            R8EValue val = r8e_obj_get(ctx, r8e_get_pointer(desc),
                                        R8E_ATOM_value);
            r8e_obj_set(ctx, obj, key_buf[i], val);
        }
    }
    return target;
}

/*
 * Object.fromEntries(iterable)
 * Simplified: accepts array of [key, value] pairs.
 */
static R8EValue builtin_object_fromEntries(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue iterable = arg_or_undef(argc, argv, 0);
    R8EObjTier0 *obj = r8e_obj_new(ctx);
    if (!obj) return R8E_UNDEFINED;
    void *result = (void *)obj;

    if (r8e_value_is_array(iterable)) {
        R8EArray *arr = (R8EArray *)r8e_get_pointer(iterable);
        for (uint32_t i = 0; i < arr->length; i++) {
            R8EValue entry = r8e_array_get(ctx, arr, i);
            if (r8e_value_is_array(entry)) {
                R8EArray *pair = (R8EArray *)r8e_get_pointer(entry);
                if (pair->length >= 2) {
                    R8EValue key = r8e_array_get(ctx, pair, 0);
                    R8EValue val = r8e_array_get(ctx, pair, 1);
                    uint32_t k = 0;
                    if (R8E_IS_INT32(key)) k = (uint32_t)r8e_get_int32(key);
                    else if (R8E_IS_ATOM(key)) k = (uint32_t)(key & 0xFFFFFFFF);
                    result = r8e_obj_set(ctx, result, k, val);
                }
            }
        }
    }
    return r8e_from_pointer(result);
}

/*
 * Object.hasOwn(obj, prop) - ES2022
 */
static R8EValue builtin_object_hasOwn(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    return builtin_obj_proto_hasOwnProperty(ctx,
        arg_or_undef(argc, argv, 0), argc - 1, argv + 1);
    (void)this_val;
}


/* *************************************************************************
 * SECTION B: Array built-ins
 * ************************************************************************* */

/* --- Array static methods --- */

static R8EValue builtin_array_isArray(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    return r8e_from_boolean(r8e_array_is_array(arg_or_undef(argc, argv, 0)));
}

static R8EValue builtin_array_from(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue src = arg_or_undef(argc, argv, 0);
    R8EArray *result = new_array(ctx, 8);
    if (!result) return R8E_UNDEFINED;

    if (r8e_value_is_array(src)) {
        R8EArray *sarr = (R8EArray *)r8e_get_pointer(src);
        for (uint32_t i = 0; i < sarr->length; i++)
            r8e_array_push(ctx, result, r8e_array_get(ctx, sarr, i));
    }
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_of(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EArray *arr = new_array(ctx, argc > 0 ? (uint32_t)argc : 1);
    if (!arr) return R8E_UNDEFINED;
    for (int i = 0; i < argc; i++)
        r8e_array_push(ctx, arr, argv[i]);
    return r8e_from_pointer(arr);
}

/* --- Array.prototype methods --- */

static R8EValue builtin_array_push(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.push");
    if (!arr) return R8E_UNDEFINED;
    for (int i = 0; i < argc; i++)
        r8e_array_push(ctx, arr, argv[i]);
    return r8e_from_int32((int32_t)arr->length);
}

static R8EValue builtin_array_pop(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.pop");
    if (!arr) return R8E_UNDEFINED;
    return r8e_array_pop(ctx, arr);
}

static R8EValue builtin_array_shift(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.shift");
    if (!arr) return R8E_UNDEFINED;
    return r8e_array_shift(ctx, arr);
}

static R8EValue builtin_array_unshift(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.unshift");
    if (!arr) return R8E_UNDEFINED;
    r8e_array_unshift(ctx, arr, argv, (uint32_t)argc);
    return r8e_from_int32((int32_t)arr->length);
}

static R8EValue builtin_array_splice(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.splice");
    if (!arr) return R8E_UNDEFINED;

    int32_t start = (argc > 0 && R8E_IS_NUMBER(argv[0]))
                    ? (int32_t)to_number(argv[0]) : 0;
    int32_t del = (argc > 1 && R8E_IS_NUMBER(argv[1]))
                  ? (int32_t)to_number(argv[1]) : (int32_t)arr->length;
    const R8EValue *items = (argc > 2) ? &argv[2] : NULL;
    uint32_t item_count = (argc > 2) ? (uint32_t)(argc - 2) : 0;

    R8EArray *removed = new_array(ctx, del > 0 ? (uint32_t)del : 1);
    if (!removed) return R8E_UNDEFINED;

    r8e_array_splice(ctx, arr, start, del, items, item_count, removed);
    return r8e_from_pointer(removed);
}

static R8EValue builtin_array_slice(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.slice");
    if (!arr) return R8E_UNDEFINED;

    int32_t len = (int32_t)arr->length;
    int32_t start = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    int32_t end = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                  ? (int32_t)to_number(argv[1]) : len;

    if (start < 0) start = (start + len > 0) ? start + len : 0;
    if (end < 0)   end   = (end + len > 0)   ? end + len   : 0;
    if (start > len) start = len;
    if (end > len)   end   = len;

    int32_t count = (end > start) ? end - start : 0;
    R8EArray *result = new_array(ctx, count > 0 ? (uint32_t)count : 1);
    if (!result) return R8E_UNDEFINED;

    for (int32_t i = start; i < end; i++)
        r8e_array_push(ctx, result, r8e_array_get(ctx, arr, (uint32_t)i));
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_concat(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.concat");
    if (!arr) return R8E_UNDEFINED;

    R8EArray *result = new_array(ctx, arr->length + (uint32_t)argc);
    if (!result) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < arr->length; i++)
        r8e_array_push(ctx, result, r8e_array_get(ctx, arr, i));

    for (int i = 0; i < argc; i++) {
        if (r8e_value_is_array(argv[i])) {
            R8EArray *src = (R8EArray *)r8e_get_pointer(argv[i]);
            for (uint32_t j = 0; j < src->length; j++)
                r8e_array_push(ctx, result, r8e_array_get(ctx, src, j));
        } else {
            r8e_array_push(ctx, result, argv[i]);
        }
    }
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_indexOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.indexOf");
    if (!arr) return r8e_from_int32(-1);
    R8EValue search = arg_or_undef(argc, argv, 0);
    int32_t from = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    return r8e_from_int32(r8e_array_index_of(ctx, arr, search, from));
}

static R8EValue builtin_array_lastIndexOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.lastIndexOf");
    if (!arr) return r8e_from_int32(-1);
    R8EValue search = arg_or_undef(argc, argv, 0);
    int32_t from = (argc > 1) ? (int32_t)to_number(argv[1])
                               : (int32_t)arr->length - 1;
    return r8e_from_int32(r8e_array_last_index_of(ctx, arr, search, from));
}

static R8EValue builtin_array_includes(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.includes");
    if (!arr) return R8E_FALSE;
    R8EValue search = arg_or_undef(argc, argv, 0);
    int32_t from = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    return r8e_from_boolean(r8e_array_includes(ctx, arr, search, from));
}

static R8EValue builtin_array_at(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.at");
    if (!arr) return R8E_UNDEFINED;
    int32_t idx = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    return r8e_array_at(ctx, arr, idx);
}

static R8EValue builtin_array_join(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.join");
    if (!arr) return R8E_UNDEFINED;

    const char *sep = ",";
    uint32_t sep_len = 1;
    char sep_buf[8];
    if (argc > 0 && !R8E_IS_UNDEFINED(argv[0])) {
        R8EValue sep_str = r8e_to_string(ctx, argv[0]);
        sep = get_str_data(sep_str, sep_buf, &sep_len);
    }

    char out[4096];
    int n = r8e_array_join(ctx, arr, sep, sep_len, out, sizeof(out));
    if (n < 0) return R8E_UNDEFINED;
    return make_heap_string(ctx, out, (uint32_t)n);
}

static R8EValue builtin_array_toString(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    /* Array.prototype.toString calls join(",") */
    return builtin_array_join(ctx, this_val, 0, NULL);
}

static R8EValue builtin_array_reverse(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.reverse");
    if (!arr) return R8E_UNDEFINED;
    r8e_array_reverse(ctx, arr);
    return this_val;
}

/*
 * Wrapper context for passing JS compare function to the C sort routine.
 */
typedef struct {
    R8EContext *ctx;
    R8EValue   compare_fn;
} R8ESortCtx;

static int sort_with_comparefn(R8EContext *ctx, R8EValue a, R8EValue b,
                                void *user_data) {
    R8ESortCtx *sctx = (R8ESortCtx *)user_data;
    if (!R8E_IS_POINTER(sctx->compare_fn)) return 0;

    R8ENativeFuncObj *fobj = (R8ENativeFuncObj *)r8e_get_pointer(
        sctx->compare_fn);
    if (!fobj || (fobj->flags & 0x0F) != R8E_GC_KIND_FUNC || !fobj->func)
        return 0;

    R8EValue args[2] = { a, b };
    R8EValue result = fobj->func(sctx->ctx, R8E_UNDEFINED, 2, args);

    /* Convert result to int: < 0, 0, or > 0 */
    if (R8E_IS_INT32(result)) return r8e_get_int32(result);
    if (R8E_IS_DOUBLE(result)) {
        double d = r8e_get_double(result);
        if (d < 0) return -1;
        if (d > 0) return 1;
        return 0;
    }
    return 0;
}

static R8EValue builtin_array_sort(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.sort");
    if (!arr) return R8E_UNDEFINED;

    if (argc > 0 && R8E_IS_POINTER(argv[0])) {
        /* User provided a compare function */
        R8ESortCtx sctx;
        sctx.ctx = ctx;
        sctx.compare_fn = argv[0];
        r8e_array_sort(ctx, arr, sort_with_comparefn, &sctx);
    } else {
        /* Default sort (lexicographic) */
        r8e_array_sort(ctx, arr, NULL, NULL);
    }
    return this_val;
}

static R8EValue builtin_array_fill(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.fill");
    if (!arr) return R8E_UNDEFINED;
    R8EValue val = arg_or_undef(argc, argv, 0);
    int32_t start = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    int32_t end = (argc > 2 && !R8E_IS_UNDEFINED(argv[2]))
                  ? (int32_t)to_number(argv[2]) : (int32_t)arr->length;
    r8e_array_fill(ctx, arr, val, start, end);
    return this_val;
}

static R8EValue builtin_array_copyWithin(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.copyWithin");
    if (!arr) return R8E_UNDEFINED;
    int32_t target = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    int32_t start  = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    int32_t end    = (argc > 2 && !R8E_IS_UNDEFINED(argv[2]))
                     ? (int32_t)to_number(argv[2]) : (int32_t)arr->length;
    r8e_array_copy_within(ctx, arr, target, start, end);
    return this_val;
}

/*
 * Higher-order array methods: forEach, map, filter, find, findIndex,
 * findLast, findLastIndex, some, every, reduce, reduceRight, flat, flatMap.
 *
 * These call the provided callback function via a helper.
 */
static R8EValue call_callback(R8EContext *ctx, R8EValue fn,
                               R8EValue this_arg, R8EValue elem,
                               R8EValue index, R8EValue array)
{
    if (!R8E_IS_POINTER(fn)) return R8E_UNDEFINED;
    R8ENativeFuncObj *fobj = (R8ENativeFuncObj *)r8e_get_pointer(fn);
    if (!fobj || (fobj->flags & 0x0F) != R8E_GC_KIND_FUNC || !fobj->func)
        return R8E_UNDEFINED;
    R8EValue args[3] = { elem, index, array };
    return fobj->func(ctx, this_arg, 3, args);
}

static R8EValue builtin_array_forEach(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.forEach");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (uint32_t i = 0; i < arr->length; i++) {
        call_callback(ctx, fn, this_arg,
                      r8e_array_get(ctx, arr, i),
                      r8e_from_int32((int32_t)i), this_val);
    }
    return R8E_UNDEFINED;
}

static R8EValue builtin_array_map(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.map");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);

    R8EArray *result = new_array(ctx, arr->length);
    if (!result) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue v = call_callback(ctx, fn, this_arg,
                                    r8e_array_get(ctx, arr, i),
                                    r8e_from_int32((int32_t)i), this_val);
        r8e_array_push(ctx, result, v);
    }
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_filter(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.filter");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);

    R8EArray *result = new_array(ctx, 8);
    if (!result) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue elem = r8e_array_get(ctx, arr, i);
        R8EValue v = call_callback(ctx, fn, this_arg, elem,
                                    r8e_from_int32((int32_t)i), this_val);
        if (r8e_to_boolean(v) == R8E_TRUE)
            r8e_array_push(ctx, result, elem);
    }
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_find(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.find");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue elem = r8e_array_get(ctx, arr, i);
        R8EValue v = call_callback(ctx, fn, this_arg, elem,
                                    r8e_from_int32((int32_t)i), this_val);
        if (r8e_to_boolean(v) == R8E_TRUE) return elem;
    }
    return R8E_UNDEFINED;
}

static R8EValue builtin_array_findIndex(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.findIndex");
    if (!arr) return r8e_from_int32(-1);
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue elem = r8e_array_get(ctx, arr, i);
        R8EValue v = call_callback(ctx, fn, this_arg, elem,
                                    r8e_from_int32((int32_t)i), this_val);
        if (r8e_to_boolean(v) == R8E_TRUE) return r8e_from_int32((int32_t)i);
    }
    return r8e_from_int32(-1);
}

static R8EValue builtin_array_findLast(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.findLast");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (int32_t i = (int32_t)arr->length - 1; i >= 0; i--) {
        R8EValue elem = r8e_array_get(ctx, arr, (uint32_t)i);
        R8EValue v = call_callback(ctx, fn, this_arg, elem,
                                    r8e_from_int32(i), this_val);
        if (r8e_to_boolean(v) == R8E_TRUE) return elem;
    }
    return R8E_UNDEFINED;
}

static R8EValue builtin_array_findLastIndex(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val,
                                   "Array.prototype.findLastIndex");
    if (!arr) return r8e_from_int32(-1);
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (int32_t i = (int32_t)arr->length - 1; i >= 0; i--) {
        R8EValue elem = r8e_array_get(ctx, arr, (uint32_t)i);
        R8EValue v = call_callback(ctx, fn, this_arg, elem,
                                    r8e_from_int32(i), this_val);
        if (r8e_to_boolean(v) == R8E_TRUE) return r8e_from_int32(i);
    }
    return r8e_from_int32(-1);
}

static R8EValue builtin_array_some(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.some");
    if (!arr) return R8E_FALSE;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue v = call_callback(ctx, fn, this_arg,
                                    r8e_array_get(ctx, arr, i),
                                    r8e_from_int32((int32_t)i), this_val);
        if (r8e_to_boolean(v) == R8E_TRUE) return R8E_TRUE;
    }
    return R8E_FALSE;
}

static R8EValue builtin_array_every(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.every");
    if (!arr) return R8E_TRUE;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue v = call_callback(ctx, fn, this_arg,
                                    r8e_array_get(ctx, arr, i),
                                    r8e_from_int32((int32_t)i), this_val);
        if (r8e_to_boolean(v) == R8E_FALSE) return R8E_FALSE;
    }
    return R8E_TRUE;
}

static R8EValue builtin_array_reduce(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.reduce");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue accum;
    uint32_t start_idx;

    if (argc > 1) {
        accum = argv[1];
        start_idx = 0;
    } else {
        if (arr->length == 0)
            return throw_type_error(ctx,
                "Reduce of empty array with no initial value");
        accum = r8e_array_get(ctx, arr, 0);
        start_idx = 1;
    }

    for (uint32_t i = start_idx; i < arr->length; i++) {
        if (!R8E_IS_POINTER(fn)) break;
        R8ENativeFuncObj *fobj = (R8ENativeFuncObj *)r8e_get_pointer(fn);
        if (!fobj || !fobj->func) break;
        R8EValue args[4] = {
            accum, r8e_array_get(ctx, arr, i),
            r8e_from_int32((int32_t)i), this_val
        };
        accum = fobj->func(ctx, R8E_UNDEFINED, 4, args);
    }
    return accum;
}

static R8EValue builtin_array_reduceRight(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.reduceRight");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue accum;
    int32_t start_idx;

    if (argc > 1) {
        accum = argv[1];
        start_idx = (int32_t)arr->length - 1;
    } else {
        if (arr->length == 0)
            return throw_type_error(ctx,
                "Reduce of empty array with no initial value");
        accum = r8e_array_get(ctx, arr, arr->length - 1);
        start_idx = (int32_t)arr->length - 2;
    }

    for (int32_t i = start_idx; i >= 0; i--) {
        if (!R8E_IS_POINTER(fn)) break;
        R8ENativeFuncObj *fobj = (R8ENativeFuncObj *)r8e_get_pointer(fn);
        if (!fobj || !fobj->func) break;
        R8EValue args[4] = {
            accum, r8e_array_get(ctx, arr, (uint32_t)i),
            r8e_from_int32(i), this_val
        };
        accum = fobj->func(ctx, R8E_UNDEFINED, 4, args);
    }
    return accum;
}

static R8EValue builtin_array_flat(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.flat");
    if (!arr) return R8E_UNDEFINED;
    int32_t depth = (argc > 0 && R8E_IS_NUMBER(argv[0]))
                    ? (int32_t)to_number(argv[0]) : 1;

    R8EArray *result = new_array(ctx, arr->length);
    if (!result) return R8E_UNDEFINED;

    /* Simple one-level flattening */
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue elem = r8e_array_get(ctx, arr, i);
        if (depth > 0 && r8e_value_is_array(elem)) {
            R8EArray *sub = (R8EArray *)r8e_get_pointer(elem);
            for (uint32_t j = 0; j < sub->length; j++)
                r8e_array_push(ctx, result, r8e_array_get(ctx, sub, j));
        } else {
            r8e_array_push(ctx, result, elem);
        }
    }
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_flatMap(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.flatMap");
    if (!arr) return R8E_UNDEFINED;
    R8EValue fn = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);

    R8EArray *result = new_array(ctx, arr->length);
    if (!result) return R8E_UNDEFINED;

    for (uint32_t i = 0; i < arr->length; i++) {
        R8EValue mapped = call_callback(ctx, fn, this_arg,
                                         r8e_array_get(ctx, arr, i),
                                         r8e_from_int32((int32_t)i), this_val);
        if (r8e_value_is_array(mapped)) {
            R8EArray *sub = (R8EArray *)r8e_get_pointer(mapped);
            for (uint32_t j = 0; j < sub->length; j++)
                r8e_array_push(ctx, result, r8e_array_get(ctx, sub, j));
        } else {
            r8e_array_push(ctx, result, mapped);
        }
    }
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_keys(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.keys");
    if (!arr) return R8E_UNDEFINED;
    R8EArray *result = new_array(ctx, arr->length);
    if (!result) return R8E_UNDEFINED;
    for (uint32_t i = 0; i < arr->length; i++)
        r8e_array_push(ctx, result, r8e_from_int32((int32_t)i));
    return r8e_from_pointer(result);
}

static R8EValue builtin_array_values(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    /* Returns a copy of the array for iteration */
    return builtin_array_slice(ctx, this_val, 0, NULL);
}

static R8EValue builtin_array_entries(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    R8EArray *arr = get_array_ptr(ctx, this_val, "Array.prototype.entries");
    if (!arr) return R8E_UNDEFINED;
    R8EArray *result = new_array(ctx, arr->length);
    if (!result) return R8E_UNDEFINED;
    for (uint32_t i = 0; i < arr->length; i++) {
        R8EArray *pair = new_array(ctx, 2);
        if (!pair) continue;
        r8e_array_push(ctx, pair, r8e_from_int32((int32_t)i));
        r8e_array_push(ctx, pair, r8e_array_get(ctx, arr, i));
        r8e_array_push(ctx, result, r8e_from_pointer(pair));
    }
    return r8e_from_pointer(result);
}


/* *************************************************************************
 * SECTION C: String built-ins
 * ************************************************************************* */

/* Helper to extract R8EString* from this_val for String.prototype methods. */
static const char *get_this_string(R8EContext *ctx, R8EValue this_val,
                                    char *buf, uint32_t *out_len) {
    if (R8E_IS_INLINE_STR(this_val) || r8e_value_is_string(this_val))
        return get_str_data(this_val, buf, out_len);
    /* Attempt ToString coercion */
    R8EValue str = r8e_to_string(ctx, this_val);
    return get_str_data(str, buf, out_len);
}

/* --- String static methods --- */

static R8EValue builtin_string_fromCharCode(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    char buf[256];
    int pos = 0;
    for (int i = 0; i < argc && pos < 255; i++) {
        int32_t code = (int32_t)to_number(argv[i]);
        if (code >= 0 && code <= 127) buf[pos++] = (char)code;
    }
    buf[pos] = '\0';
    return make_heap_string(ctx, buf, (uint32_t)pos);
}

static R8EValue builtin_string_fromCodePoint(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    /* Simplified: handle ASCII codepoints, delegate to fromCharCode */
    return builtin_string_fromCharCode(ctx, this_val, argc, argv);
}

/* --- String.prototype methods --- */

static R8EValue builtin_string_charAt(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t idx = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (idx < 0 || (uint32_t)idx >= tlen)
        return r8e_from_inline_str("", 0);
    return r8e_from_inline_str(&s[idx], 1);
}

static R8EValue builtin_string_charCodeAt(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t idx = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (idx < 0 || (uint32_t)idx >= tlen)
        return r8e_from_double(NAN);
    return r8e_from_int32((int32_t)(uint8_t)s[idx]);
}

static R8EValue builtin_string_codePointAt(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    /* Simplified: same as charCodeAt for ASCII */
    return builtin_string_charCodeAt(ctx, this_val, argc, argv);
}

static R8EValue builtin_string_indexOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue needle_val = arg_or_undef(argc, argv, 0);
    R8EValue needle_str = r8e_to_string(ctx, needle_val);
    char nbuf[8]; uint32_t nlen;
    const char *needle = get_str_data(needle_str, nbuf, &nlen);
    int32_t from = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    if (from < 0) from = 0;

    if (nlen == 0) return r8e_from_int32(from <= (int32_t)tlen ? from : (int32_t)tlen);
    for (int32_t i = from; i <= (int32_t)(tlen - nlen); i++) {
        if (memcmp(s + i, needle, nlen) == 0)
            return r8e_from_int32(i);
    }
    return r8e_from_int32(-1);
}

static R8EValue builtin_string_lastIndexOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue needle_val = arg_or_undef(argc, argv, 0);
    R8EValue needle_str = r8e_to_string(ctx, needle_val);
    char nbuf[8]; uint32_t nlen;
    const char *needle = get_str_data(needle_str, nbuf, &nlen);
    int32_t from = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                   ? (int32_t)to_number(argv[1]) : (int32_t)tlen;

    if (nlen == 0) return r8e_from_int32(from <= (int32_t)tlen ? from : (int32_t)tlen);
    if (from > (int32_t)(tlen - nlen)) from = (int32_t)(tlen - nlen);
    for (int32_t i = from; i >= 0; i--) {
        if (memcmp(s + i, needle, nlen) == 0)
            return r8e_from_int32(i);
    }
    return r8e_from_int32(-1);
}

static R8EValue builtin_string_includes(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue needle_str = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    char nbuf[8]; uint32_t nlen;
    const char *needle = get_str_data(needle_str, nbuf, &nlen);
    int32_t from = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    if (from < 0) from = 0;
    if (nlen == 0) return R8E_TRUE;
    for (int32_t i = from; i <= (int32_t)(tlen - nlen); i++) {
        if (memcmp(s + i, needle, nlen) == 0) return R8E_TRUE;
    }
    return R8E_FALSE;
}

static R8EValue builtin_string_startsWith(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue pstr = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    char pbuf[8]; uint32_t plen;
    const char *prefix = get_str_data(pstr, pbuf, &plen);
    int32_t pos = (argc > 1) ? (int32_t)to_number(argv[1]) : 0;
    if (pos < 0) pos = 0;
    if ((uint32_t)pos + plen > tlen) return R8E_FALSE;
    return r8e_from_boolean(memcmp(s + pos, prefix, plen) == 0);
}

static R8EValue builtin_string_endsWith(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue sstr = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    char sbuf[8]; uint32_t slen;
    const char *suffix = get_str_data(sstr, sbuf, &slen);
    uint32_t end_pos = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                       ? (uint32_t)to_number(argv[1]) : tlen;
    if (end_pos > tlen) end_pos = tlen;
    if (slen > end_pos) return R8E_FALSE;
    return r8e_from_boolean(memcmp(s + end_pos - slen, suffix, slen) == 0);
}

static R8EValue builtin_string_slice(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t len = (int32_t)tlen;
    int32_t start = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    int32_t end = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                  ? (int32_t)to_number(argv[1]) : len;
    if (start < 0) start = (start + len > 0) ? start + len : 0;
    if (end < 0)   end   = (end + len > 0)   ? end + len   : 0;
    if (start > len) start = len;
    if (end > len)   end   = len;
    if (end <= start)
        return r8e_from_inline_str("", 0);
    return make_heap_string(ctx, s + start, (uint32_t)(end - start));
}

static R8EValue builtin_string_substring(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t len = (int32_t)tlen;
    int32_t start = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    int32_t end = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                  ? (int32_t)to_number(argv[1]) : len;
    if (start < 0) start = 0;
    if (end < 0)   end   = 0;
    if (start > len) start = len;
    if (end > len)   end   = len;
    if (start > end) { int32_t t = start; start = end; end = t; }
    return make_heap_string(ctx, s + start, (uint32_t)(end - start));
}

static R8EValue builtin_string_substr(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t len = (int32_t)tlen;
    int32_t start = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    int32_t count = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                    ? (int32_t)to_number(argv[1]) : len;
    if (start < 0) start = (start + len > 0) ? start + len : 0;
    if (start > len) start = len;
    if (count < 0) count = 0;
    if (start + count > len) count = len - start;
    return make_heap_string(ctx, s + start, (uint32_t)count);
}

static R8EValue builtin_string_toUpperCase(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    char *out = (char *)malloc(tlen + 1);
    if (!out) return R8E_UNDEFINED;
    for (uint32_t i = 0; i < tlen; i++)
        out[i] = (char)toupper((unsigned char)s[i]);
    out[tlen] = '\0';
    R8EValue result = make_heap_string(ctx, out, tlen);
    free(out);
    return result;
}

static R8EValue builtin_string_toLowerCase(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    char *out = (char *)malloc(tlen + 1);
    if (!out) return R8E_UNDEFINED;
    for (uint32_t i = 0; i < tlen; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[tlen] = '\0';
    R8EValue result = make_heap_string(ctx, out, tlen);
    free(out);
    return result;
}

static R8EValue builtin_string_trim(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    uint32_t start = 0, end = tlen;
    while (start < end && isspace((unsigned char)s[start])) start++;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    return make_heap_string(ctx, s + start, end - start);
}

static R8EValue builtin_string_trimStart(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    uint32_t start = 0;
    while (start < tlen && isspace((unsigned char)s[start])) start++;
    return make_heap_string(ctx, s + start, tlen - start);
}

static R8EValue builtin_string_trimEnd(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    uint32_t end = tlen;
    while (end > 0 && isspace((unsigned char)s[end - 1])) end--;
    return make_heap_string(ctx, s, end);
}

static R8EValue builtin_string_repeat(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t count = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (count < 0)
        return throw_range_error(ctx, "Invalid count value");
    if (count == 0 || tlen == 0)
        return r8e_from_inline_str("", 0);
    uint32_t result_len = tlen * (uint32_t)count;
    if (result_len > 1048576)
        return throw_range_error(ctx, "Invalid string length");
    char *out = (char *)malloc(result_len + 1);
    if (!out) return R8E_UNDEFINED;
    for (int32_t i = 0; i < count; i++)
        memcpy(out + (uint32_t)i * tlen, s, tlen);
    out[result_len] = '\0';
    R8EValue result = make_heap_string(ctx, out, result_len);
    free(out);
    return result;
}

static R8EValue builtin_string_padStart(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t target_len = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (target_len <= (int32_t)tlen)
        return make_heap_string(ctx, s, tlen);

    const char *pad = " ";
    uint32_t pad_len = 1;
    char pad_buf[8];
    if (argc > 1 && !R8E_IS_UNDEFINED(argv[1])) {
        R8EValue ps = r8e_to_string(ctx, argv[1]);
        pad = get_str_data(ps, pad_buf, &pad_len);
        if (pad_len == 0) return make_heap_string(ctx, s, tlen);
    }

    uint32_t fill_len = (uint32_t)target_len - tlen;
    char *out = (char *)malloc((uint32_t)target_len + 1);
    if (!out) return R8E_UNDEFINED;
    for (uint32_t i = 0; i < fill_len; i++)
        out[i] = pad[i % pad_len];
    memcpy(out + fill_len, s, tlen);
    out[target_len] = '\0';
    R8EValue result = make_heap_string(ctx, out, (uint32_t)target_len);
    free(out);
    return result;
}

static R8EValue builtin_string_padEnd(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t target_len = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (target_len <= (int32_t)tlen)
        return make_heap_string(ctx, s, tlen);

    const char *pad = " ";
    uint32_t pad_len = 1;
    char pad_buf[8];
    if (argc > 1 && !R8E_IS_UNDEFINED(argv[1])) {
        R8EValue ps = r8e_to_string(ctx, argv[1]);
        pad = get_str_data(ps, pad_buf, &pad_len);
        if (pad_len == 0) return make_heap_string(ctx, s, tlen);
    }

    uint32_t fill_len = (uint32_t)target_len - tlen;
    char *out = (char *)malloc((uint32_t)target_len + 1);
    if (!out) return R8E_UNDEFINED;
    memcpy(out, s, tlen);
    for (uint32_t i = 0; i < fill_len; i++)
        out[tlen + i] = pad[i % pad_len];
    out[target_len] = '\0';
    R8EValue result = make_heap_string(ctx, out, (uint32_t)target_len);
    free(out);
    return result;
}

static R8EValue builtin_string_concat(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);

    /* Calculate total length */
    uint32_t total = tlen;
    for (int i = 0; i < argc; i++) {
        R8EValue sv = r8e_to_string(ctx, argv[i]);
        char ab[8]; uint32_t al;
        get_str_data(sv, ab, &al);
        total += al;
    }

    char *out = (char *)malloc(total + 1);
    if (!out) return R8E_UNDEFINED;
    memcpy(out, s, tlen);
    uint32_t pos = tlen;
    for (int i = 0; i < argc; i++) {
        R8EValue sv = r8e_to_string(ctx, argv[i]);
        char ab[8]; uint32_t al;
        const char *as = get_str_data(sv, ab, &al);
        memcpy(out + pos, as, al);
        pos += al;
    }
    out[total] = '\0';
    R8EValue result = make_heap_string(ctx, out, total);
    free(out);
    return result;
}

static R8EValue builtin_string_at(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    int32_t idx = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (idx < 0) idx += (int32_t)tlen;
    if (idx < 0 || (uint32_t)idx >= tlen) return R8E_UNDEFINED;
    return r8e_from_inline_str(&s[idx], 1);
}

static R8EValue builtin_string_split(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);

    R8EArray *result = new_array(ctx, 8);
    if (!result) return R8E_UNDEFINED;

    if (argc == 0 || R8E_IS_UNDEFINED(argv[0])) {
        r8e_array_push(ctx, result, make_heap_string(ctx, s, tlen));
        return r8e_from_pointer(result);
    }

    R8EValue sep_str = r8e_to_string(ctx, argv[0]);
    char sbuf[8]; uint32_t slen;
    const char *sep = get_str_data(sep_str, sbuf, &slen);

    int32_t limit = (argc > 1 && !R8E_IS_UNDEFINED(argv[1]))
                    ? (int32_t)to_number(argv[1]) : INT32_MAX;

    if (slen == 0) {
        /* Split into individual characters */
        for (uint32_t i = 0; i < tlen && (int32_t)i < limit; i++)
            r8e_array_push(ctx, result, r8e_from_inline_str(&s[i], 1));
        return r8e_from_pointer(result);
    }

    uint32_t start = 0;
    int32_t count = 0;
    for (uint32_t i = 0; i <= tlen - slen && count < limit; i++) {
        if (memcmp(s + i, sep, slen) == 0) {
            r8e_array_push(ctx, result,
                           make_heap_string(ctx, s + start, i - start));
            start = i + slen;
            i += slen - 1;
            count++;
        }
    }
    if (count < limit)
        r8e_array_push(ctx, result,
                       make_heap_string(ctx, s + start, tlen - start));
    return r8e_from_pointer(result);
}

static R8EValue builtin_string_replace(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue search_str = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    R8EValue replace_str = r8e_to_string(ctx, arg_or_undef(argc, argv, 1));
    char sbuf[8]; uint32_t slen;
    const char *search = get_str_data(search_str, sbuf, &slen);
    char rbuf[8]; uint32_t rlen;
    const char *repl = get_str_data(replace_str, rbuf, &rlen);

    if (slen == 0) {
        /* Prepend replacement */
        uint32_t out_len = rlen + tlen;
        char *out = (char *)malloc(out_len + 1);
        if (!out) return R8E_UNDEFINED;
        memcpy(out, repl, rlen);
        memcpy(out + rlen, s, tlen);
        out[out_len] = '\0';
        R8EValue result = make_heap_string(ctx, out, out_len);
        free(out);
        return result;
    }

    /* Find first occurrence */
    const char *found = NULL;
    for (uint32_t i = 0; i + slen <= tlen; i++) {
        if (memcmp(s + i, search, slen) == 0) {
            found = s + i;
            break;
        }
    }
    if (!found) return make_heap_string(ctx, s, tlen);

    uint32_t prefix_len = (uint32_t)(found - s);
    uint32_t suffix_len = tlen - prefix_len - slen;
    uint32_t out_len = prefix_len + rlen + suffix_len;
    char *out = (char *)malloc(out_len + 1);
    if (!out) return R8E_UNDEFINED;
    memcpy(out, s, prefix_len);
    memcpy(out + prefix_len, repl, rlen);
    memcpy(out + prefix_len + rlen, found + slen, suffix_len);
    out[out_len] = '\0';
    R8EValue result = make_heap_string(ctx, out, out_len);
    free(out);
    return result;
}

static R8EValue builtin_string_replaceAll(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    R8EValue search_str = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    R8EValue replace_str = r8e_to_string(ctx, arg_or_undef(argc, argv, 1));
    char sbuf[8]; uint32_t slen;
    const char *search = get_str_data(search_str, sbuf, &slen);
    char rbuf[8]; uint32_t rlen;
    const char *repl = get_str_data(replace_str, rbuf, &rlen);

    if (slen == 0) return make_heap_string(ctx, s, tlen);

    /* Count occurrences first */
    uint32_t count = 0;
    for (uint32_t i = 0; i + slen <= tlen; i++) {
        if (memcmp(s + i, search, slen) == 0) { count++; i += slen - 1; }
    }
    if (count == 0) return make_heap_string(ctx, s, tlen);

    uint32_t out_len = tlen - count * slen + count * rlen;
    char *out = (char *)malloc(out_len + 1);
    if (!out) return R8E_UNDEFINED;

    uint32_t opos = 0;
    for (uint32_t i = 0; i < tlen; ) {
        if (i + slen <= tlen && memcmp(s + i, search, slen) == 0) {
            memcpy(out + opos, repl, rlen);
            opos += rlen;
            i += slen;
        } else {
            out[opos++] = s[i++];
        }
    }
    out[out_len] = '\0';
    R8EValue result = make_heap_string(ctx, out, out_len);
    free(out);
    return result;
}

/*
 * String.prototype.match - when called with a string argument (not RegExp),
 * convert to RegExp and find first match. For now, implement simple string
 * matching that returns an array with the match, or null if not found.
 */
static R8EValue builtin_string_match(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);

    R8EValue search_val = arg_or_undef(argc, argv, 0);
    if (R8E_IS_UNDEFINED(search_val) || R8E_IS_NULL(search_val)) {
        /* match() or match(undefined) matches empty string at pos 0 */
        R8EArray *result = r8e_array_new(ctx, 1);
        if (!result) return R8E_NULL;
        r8e_array_push(ctx, result, make_heap_string(ctx, "", 0));
        return r8e_from_pointer(result);
    }

    /* Convert search value to string */
    R8EValue search_str = r8e_to_string(ctx, search_val);
    char sbuf[8]; uint32_t slen;
    const char *search = get_str_data(search_str, sbuf, &slen);

    /* Find the first occurrence */
    for (uint32_t i = 0; i + slen <= tlen; i++) {
        if (memcmp(s + i, search, slen) == 0) {
            /* Found a match - return array with the match string */
            R8EArray *result = r8e_array_new(ctx, 1);
            if (!result) return R8E_NULL;
            r8e_array_push(ctx, result, make_heap_string(ctx, search, slen));
            /* Set .index property */
            /* (Would need named properties on array; simplified) */
            return r8e_from_pointer(result);
        }
    }

    return R8E_NULL; /* no match */
}

static R8EValue builtin_string_matchAll(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    /* matchAll requires a global regex. For string args, return an iterator
     * of all matches. Simplified: return an array of match arrays. */
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);

    R8EValue search_val = arg_or_undef(argc, argv, 0);
    R8EValue search_str = r8e_to_string(ctx, search_val);
    char sbuf[8]; uint32_t slen;
    const char *search = get_str_data(search_str, sbuf, &slen);

    R8EArray *results = r8e_array_new(ctx, 4);
    if (!results) return R8E_UNDEFINED;

    if (slen == 0) return r8e_from_pointer(results);

    for (uint32_t i = 0; i + slen <= tlen; i++) {
        if (memcmp(s + i, search, slen) == 0) {
            R8EArray *match = r8e_array_new(ctx, 1);
            if (match) {
                r8e_array_push(ctx, match,
                               make_heap_string(ctx, search, slen));
                r8e_array_push(ctx, results, r8e_from_pointer(match));
            }
            i += slen - 1; /* advance past match */
        }
    }

    return r8e_from_pointer(results);
}

/*
 * String.prototype.search - find the first occurrence of a pattern.
 * Returns the index of the first match, or -1 if not found.
 */
static R8EValue builtin_string_search(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);

    R8EValue search_val = arg_or_undef(argc, argv, 0);
    R8EValue search_str = r8e_to_string(ctx, search_val);
    char sbuf[8]; uint32_t slen;
    const char *search = get_str_data(search_str, sbuf, &slen);

    for (uint32_t i = 0; i + slen <= tlen; i++) {
        if (memcmp(s + i, search, slen) == 0) {
            return r8e_from_int32((int32_t)i);
        }
    }

    return r8e_from_int32(-1);
}

static R8EValue builtin_string_normalize(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    /* Normalization is a no-op for ASCII; return this */
    char tbuf[8]; uint32_t tlen;
    const char *s = get_this_string(ctx, this_val, tbuf, &tlen);
    return make_heap_string(ctx, s, tlen);
}

static R8EValue builtin_string_toString(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)argc; (void)argv;
    if (R8E_IS_INLINE_STR(this_val) || r8e_value_is_string(this_val))
        return this_val;
    return r8e_to_string(ctx, this_val);
}

static R8EValue builtin_string_valueOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    return builtin_string_toString(ctx, this_val, argc, argv);
}


/* *************************************************************************
 * SECTION D: Number built-ins
 * ************************************************************************* */

static R8EValue builtin_number_isFinite(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    R8EValue v = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_NUMBER(v)) return R8E_FALSE;
    return r8e_from_boolean(r8e_is_finite(v));
}

static R8EValue builtin_number_isInteger(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    R8EValue v = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_NUMBER(v)) return R8E_FALSE;
    return r8e_from_boolean(r8e_is_integer(v));
}

static R8EValue builtin_number_isNaN(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    R8EValue v = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_NUMBER(v)) return R8E_FALSE;
    return r8e_from_boolean(r8e_is_nan(v));
}

static R8EValue builtin_number_isSafeInteger(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    R8EValue v = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_NUMBER(v)) return R8E_FALSE;
    return r8e_from_boolean(r8e_is_safe_integer(v));
}

static R8EValue builtin_number_parseInt(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue str_val = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    char buf[8]; uint32_t len;
    const char *s = get_str_data(str_val, buf, &len);
    int radix = (argc > 1) ? (int)to_number(argv[1]) : 0;
    return r8e_parse_int(s, len, radix);
}

static R8EValue builtin_number_parseFloat(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue str_val = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    char buf[8]; uint32_t len;
    const char *s = get_str_data(str_val, buf, &len);
    return r8e_string_to_number(s, len);
}

/* Number.prototype methods */

static R8EValue builtin_number_proto_toFixed(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    if (!R8E_IS_NUMBER(this_val))
        return throw_type_error(ctx, "toFixed requires a number");
    int32_t digits = (argc > 0) ? (int32_t)to_number(argv[0]) : 0;
    if (digits < 0 || digits > 100)
        return throw_range_error(ctx, "toFixed() digits out of range");
    double d = to_number(this_val);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%.*f", (int)digits, d);
    if (n <= 0) return R8E_UNDEFINED;
    return make_heap_string(ctx, buf, (uint32_t)n);
}

static R8EValue builtin_number_proto_toExponential(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    if (!R8E_IS_NUMBER(this_val))
        return throw_type_error(ctx, "toExponential requires a number");
    int32_t digits = (argc > 0 && !R8E_IS_UNDEFINED(argv[0]))
                     ? (int32_t)to_number(argv[0]) : 6;
    if (digits < 0 || digits > 100)
        return throw_range_error(ctx, "toExponential() digits out of range");
    double d = to_number(this_val);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%.*e", (int)digits, d);
    if (n <= 0) return R8E_UNDEFINED;
    return make_heap_string(ctx, buf, (uint32_t)n);
}

static R8EValue builtin_number_proto_toPrecision(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    if (!R8E_IS_NUMBER(this_val))
        return throw_type_error(ctx, "toPrecision requires a number");
    if (argc == 0 || R8E_IS_UNDEFINED(argv[0]))
        return r8e_num_to_string(ctx, this_val);
    int32_t prec = (int32_t)to_number(argv[0]);
    if (prec < 1 || prec > 100)
        return throw_range_error(ctx, "toPrecision() precision out of range");
    double d = to_number(this_val);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%.*g", (int)prec, d);
    if (n <= 0) return R8E_UNDEFINED;
    return make_heap_string(ctx, buf, (uint32_t)n);
}

static R8EValue builtin_number_proto_toString(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    if (!R8E_IS_NUMBER(this_val))
        return throw_type_error(ctx, "Number.prototype.toString requires number");
    int radix = (argc > 0 && !R8E_IS_UNDEFINED(argv[0]))
                ? (int)to_number(argv[0]) : 10;
    if (radix < 2 || radix > 36)
        return throw_range_error(ctx, "toString() radix out of range");
    return r8e_num_to_string_radix(ctx, this_val, radix);
}

static R8EValue builtin_number_proto_valueOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (R8E_IS_NUMBER(this_val)) return this_val;
    return throw_type_error(ctx, "Number.prototype.valueOf requires number");
}


/* *************************************************************************
 * SECTION E: Boolean built-ins
 * ************************************************************************* */

static R8EValue builtin_boolean_proto_toString(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (this_val == R8E_TRUE) return r8e_from_inline_str("true", 4);
    if (this_val == R8E_FALSE) return r8e_from_inline_str("false", 5);
    return throw_type_error(ctx, "Boolean.prototype.toString requires boolean");
}

static R8EValue builtin_boolean_proto_valueOf(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (R8E_IS_BOOLEAN(this_val)) return this_val;
    return throw_type_error(ctx, "Boolean.prototype.valueOf requires boolean");
}


/* *************************************************************************
 * SECTION F: Math object
 * ************************************************************************* */

static R8EValue builtin_math_abs(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_abs(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_ceil(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_ceil(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_floor(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_floor(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_round(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_round(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_trunc(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_trunc(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_sign(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_sign(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_sqrt(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_sqrt(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_cbrt(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_cbrt(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_pow(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t;
    return r8e_math_pow(arg_or_undef(argc, a, 0), arg_or_undef(argc, a, 1)); }
static R8EValue builtin_math_hypot(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t;
    return r8e_math_hypot(arg_or_undef(argc, a, 0), arg_or_undef(argc, a, 1)); }
static R8EValue builtin_math_exp(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_exp(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_expm1(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_expm1(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_log(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_log(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_log2(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_log2(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_log10(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_log10(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_log1p(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_log1p(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_sin(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_sin(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_cos(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_cos(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_tan(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_tan(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_asin(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_asin(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_acos(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_acos(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_atan(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_atan(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_atan2(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t;
    return r8e_math_atan2(arg_or_undef(argc, a, 0), arg_or_undef(argc, a, 1)); }
static R8EValue builtin_math_sinh(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_sinh(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_cosh(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_cosh(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_tanh(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_tanh(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_asinh(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_asinh(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_acosh(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_acosh(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_atanh(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_atanh(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_random(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; (void)argc; (void)a; return r8e_math_random(); }
static R8EValue builtin_math_clz32(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_clz32(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_fround(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t; return r8e_math_fround(arg_or_undef(argc, a, 0)); }
static R8EValue builtin_math_imul(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t;
    return r8e_math_imul(arg_or_undef(argc, a, 0), arg_or_undef(argc, a, 1)); }

static R8EValue builtin_math_max(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t;
    if (argc == 0) return r8e_from_double(-INFINITY);
    R8EValue result = a[0];
    for (int i = 1; i < argc; i++)
        result = r8e_math_max(result, a[i]);
    return result;
}

static R8EValue builtin_math_min(R8EContext *ctx, R8EValue t, int argc, const R8EValue *a) {
    (void)ctx; (void)t;
    if (argc == 0) return r8e_from_double(INFINITY);
    R8EValue result = a[0];
    for (int i = 1; i < argc; i++)
        result = r8e_math_min(result, a[i]);
    return result;
}


/* *************************************************************************
 * SECTION G: Symbol built-ins
 * ************************************************************************* */

/* Global symbol registry */
typedef struct {
    R8EValue    key;    /* string key */
    uint32_t    sym_id; /* symbol id */
} R8ESymbolEntry;

static R8ESymbolEntry r8e_symbol_registry[256];
static uint32_t r8e_symbol_registry_count = 0;

static R8EValue builtin_symbol_constructor(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    uint32_t id = R8E_SYMBOL__FIRST_USER + ctx->next_symbol_id++;
    (void)argc; (void)argv;
    return r8e_from_symbol(id);
}

static R8EValue builtin_symbol_for(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue key = r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
    /* Search registry */
    for (uint32_t i = 0; i < r8e_symbol_registry_count; i++) {
        if (r8e_strict_eq(r8e_symbol_registry[i].key, key) == R8E_TRUE)
            return r8e_from_symbol(r8e_symbol_registry[i].sym_id);
    }
    /* Create new */
    uint32_t id = R8E_SYMBOL__FIRST_USER + ctx->next_symbol_id++;
    if (r8e_symbol_registry_count < 256) {
        r8e_symbol_registry[r8e_symbol_registry_count].key = key;
        r8e_symbol_registry[r8e_symbol_registry_count].sym_id = id;
        r8e_symbol_registry_count++;
    }
    return r8e_from_symbol(id);
}

static R8EValue builtin_symbol_keyFor(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)ctx; (void)this_val;
    R8EValue sym = arg_or_undef(argc, argv, 0);
    if (!R8E_IS_SYMBOL(sym))
        return throw_type_error(ctx, "Symbol.keyFor requires a symbol");
    uint32_t id = r8e_get_symbol_id(sym);
    for (uint32_t i = 0; i < r8e_symbol_registry_count; i++) {
        if (r8e_symbol_registry[i].sym_id == id)
            return r8e_symbol_registry[i].key;
    }
    return R8E_UNDEFINED;
}


/* *************************************************************************
 * SECTION H: globalThis properties
 * ************************************************************************* */

static R8EValue builtin_global_parseInt(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    return builtin_number_parseInt(ctx, this_val, argc, argv);
}

static R8EValue builtin_global_parseFloat(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    return builtin_number_parseFloat(ctx, this_val, argc, argv);
}

static R8EValue builtin_global_isFinite(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue v = r8e_to_number(ctx, arg_or_undef(argc, argv, 0));
    return r8e_from_boolean(r8e_is_finite(v));
}

static R8EValue builtin_global_isNaN(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue v = r8e_to_number(ctx, arg_or_undef(argc, argv, 0));
    return r8e_from_boolean(r8e_is_nan(v));
}

static R8EValue builtin_global_eval(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return throw_type_error(ctx, "eval is not supported in this context");
}

/*
 * URI encoding/decoding stubs.
 * Full implementation requires percent-encoding per RFC 3986.
 */
static R8EValue builtin_global_encodeURI(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    return r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
}

static R8EValue builtin_global_decodeURI(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    return r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
}

static R8EValue builtin_global_encodeURIComponent(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    return r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
}

static R8EValue builtin_global_decodeURIComponent(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    return r8e_to_string(ctx, arg_or_undef(argc, argv, 0));
}


/* *************************************************************************
 * SECTION I: console object
 * ************************************************************************* */

static R8EValue builtin_console_log(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', stdout);
        R8EValue s = r8e_to_string(ctx, argv[i]);
        char buf[8]; uint32_t len;
        const char *str = get_str_data(s, buf, &len);
        fwrite(str, 1, len, stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return R8E_UNDEFINED;
}

static R8EValue builtin_console_warn(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', stderr);
        R8EValue s = r8e_to_string(ctx, argv[i]);
        char buf[8]; uint32_t len;
        const char *str = get_str_data(s, buf, &len);
        fwrite(str, 1, len, stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
    return R8E_UNDEFINED;
}

static R8EValue builtin_console_error(
    R8EContext *ctx, R8EValue this_val, int argc, const R8EValue *argv)
{
    return builtin_console_warn(ctx, this_val, argc, argv);
}


/* *************************************************************************
 * SECTION J: Initialization entry point
 *
 * r8e_init_builtins(ctx) - called once at context creation.
 * Creates all prototype objects, installs methods, and freezes intrinsics.
 * ************************************************************************* */

void r8e_init_builtins(R8EContext *ctx)
{
    if (!ctx) return;

    /* Allocate prototype table if not yet allocated */
    if (!ctx->prototypes) {
        ctx->prototypes = (void **)calloc(R8E_PROTO_COUNT, sizeof(void *));
        if (!ctx->prototypes) return;
        ctx->proto_count = R8E_PROTO_COUNT;
    }

    ctx->next_symbol_id = 0;

    /* --- Create prototype objects --- */

    /* Object.prototype */
    R8EObjTier0 *obj_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_NONE);
    ctx->prototypes[R8E_PROTO_OBJECT] = obj_proto;

    install_method(ctx, obj_proto, R8E_ATOM_hasOwnProperty,
                   builtin_obj_proto_hasOwnProperty, 1);
    install_method(ctx, obj_proto, R8E_ATOM_toString,
                   builtin_obj_proto_toString, 0);
    install_method(ctx, obj_proto, R8E_ATOM_valueOf,
                   builtin_obj_proto_valueOf, 0);
    install_method(ctx, obj_proto, R8E_ATOM_isPrototypeOf,
                   builtin_obj_proto_isPrototypeOf, 1);
    install_method(ctx, obj_proto, R8E_ATOM_propertyIsEnumerable,
                   builtin_obj_proto_propertyIsEnumerable, 1);
    install_method(ctx, obj_proto, R8E_ATOM_toLocaleString,
                   builtin_obj_proto_toString, 0);

    /* Object constructor (as a plain object holding static methods) */
    R8EObjTier0 *obj_ctor = r8e_obj_new(ctx);
    install_method(ctx, obj_ctor, R8E_ATOM_keys,    builtin_object_keys, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_values,  builtin_object_values, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_entries, builtin_object_entries, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_assign,  builtin_object_assign, -1);
    install_method(ctx, obj_ctor, R8E_ATOM_create,  builtin_object_create, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_freeze,  builtin_object_freeze, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_seal,    builtin_object_seal, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_preventExtensions,
                   builtin_object_preventExtensions, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_isFrozen,
                   builtin_object_isFrozen, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_isSealed,
                   builtin_object_isSealed, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_isExtensible,
                   builtin_object_isExtensible, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_is,
                   builtin_object_is, 2);
    install_method(ctx, obj_ctor, R8E_ATOM_getPrototypeOf,
                   builtin_object_getPrototypeOf, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_setPrototypeOf,
                   builtin_object_setPrototypeOf, 2);
    install_method(ctx, obj_ctor, R8E_ATOM_defineProperty,
                   builtin_object_defineProperty, 3);
    install_method(ctx, obj_ctor, R8E_ATOM_defineProperties,
                   builtin_object_defineProperties, 2);
    install_method(ctx, obj_ctor, R8E_ATOM_getOwnPropertyDescriptor,
                   builtin_object_getOwnPropertyDescriptor, 2);
    install_method(ctx, obj_ctor, R8E_ATOM_getOwnPropertyNames,
                   builtin_object_getOwnPropertyNames, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_fromEntries,
                   builtin_object_fromEntries, 1);
    install_method(ctx, obj_ctor, R8E_ATOM_hasOwn,
                   builtin_object_hasOwn, 2);
    install_value(ctx, obj_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(obj_proto));

    /* --- Array.prototype --- */
    R8EObjTier0 *arr_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    ctx->prototypes[R8E_PROTO_ARRAY] = arr_proto;

    install_method(ctx, arr_proto, R8E_ATOM_push,      builtin_array_push, -1);
    install_method(ctx, arr_proto, R8E_ATOM_pop,       builtin_array_pop, 0);
    install_method(ctx, arr_proto, R8E_ATOM_shift,     builtin_array_shift, 0);
    install_method(ctx, arr_proto, R8E_ATOM_unshift,   builtin_array_unshift, -1);
    install_method(ctx, arr_proto, R8E_ATOM_splice,    builtin_array_splice, -1);
    install_method(ctx, arr_proto, R8E_ATOM_slice,     builtin_array_slice, 2);
    install_method(ctx, arr_proto, R8E_ATOM_concat,    builtin_array_concat, -1);
    install_method(ctx, arr_proto, R8E_ATOM_indexOf,   builtin_array_indexOf, 1);
    install_method(ctx, arr_proto, R8E_ATOM_lastIndexOf,
                   builtin_array_lastIndexOf, 1);
    install_method(ctx, arr_proto, R8E_ATOM_includes,  builtin_array_includes, 1);
    install_method(ctx, arr_proto, R8E_ATOM_find,      builtin_array_find, 1);
    install_method(ctx, arr_proto, R8E_ATOM_findIndex, builtin_array_findIndex, 1);
    install_method(ctx, arr_proto, R8E_ATOM_findLast,  builtin_array_findLast, 1);
    install_method(ctx, arr_proto, R8E_ATOM_findLastIndex,
                   builtin_array_findLastIndex, 1);
    install_method(ctx, arr_proto, R8E_ATOM_forEach,   builtin_array_forEach, 1);
    install_method(ctx, arr_proto, R8E_ATOM_map,       builtin_array_map, 1);
    install_method(ctx, arr_proto, R8E_ATOM_filter,    builtin_array_filter, 1);
    install_method(ctx, arr_proto, R8E_ATOM_reduce,    builtin_array_reduce, 1);
    install_method(ctx, arr_proto, R8E_ATOM_reduceRight,
                   builtin_array_reduceRight, 1);
    install_method(ctx, arr_proto, R8E_ATOM_some,      builtin_array_some, 1);
    install_method(ctx, arr_proto, R8E_ATOM_every,     builtin_array_every, 1);
    install_method(ctx, arr_proto, R8E_ATOM_flat,      builtin_array_flat, 0);
    install_method(ctx, arr_proto, R8E_ATOM_flatMap,   builtin_array_flatMap, 1);
    install_method(ctx, arr_proto, R8E_ATOM_fill,      builtin_array_fill, 1);
    install_method(ctx, arr_proto, R8E_ATOM_copyWithin,
                   builtin_array_copyWithin, 2);
    install_method(ctx, arr_proto, R8E_ATOM_sort,      builtin_array_sort, 0);
    install_method(ctx, arr_proto, R8E_ATOM_reverse,   builtin_array_reverse, 0);
    install_method(ctx, arr_proto, R8E_ATOM_join,      builtin_array_join, 1);
    install_method(ctx, arr_proto, R8E_ATOM_toString,  builtin_array_toString, 0);
    install_method(ctx, arr_proto, R8E_ATOM_at,        builtin_array_at, 1);
    install_method(ctx, arr_proto, R8E_ATOM_entries,   builtin_array_entries, 0);
    install_method(ctx, arr_proto, R8E_ATOM_keys,      builtin_array_keys, 0);
    install_method(ctx, arr_proto, R8E_ATOM_values,    builtin_array_values, 0);

    /* Array constructor */
    R8EObjTier0 *arr_ctor = r8e_obj_new(ctx);
    install_method(ctx, arr_ctor, R8E_ATOM_isArray, builtin_array_isArray, 1);
    install_method(ctx, arr_ctor, R8E_ATOM_from,    builtin_array_from, 1);
    install_method(ctx, arr_ctor, R8E_ATOM_of,      builtin_array_of, -1);
    install_value(ctx, arr_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(arr_proto));

    /* --- String.prototype --- */
    R8EObjTier0 *str_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    ctx->prototypes[R8E_PROTO_STRING] = str_proto;

    install_method(ctx, str_proto, R8E_ATOM_charAt,     builtin_string_charAt, 1);
    install_method(ctx, str_proto, R8E_ATOM_charCodeAt, builtin_string_charCodeAt, 1);
    install_method(ctx, str_proto, R8E_ATOM_codePointAt,builtin_string_codePointAt, 1);
    install_method(ctx, str_proto, R8E_ATOM_indexOf,    builtin_string_indexOf, 1);
    install_method(ctx, str_proto, R8E_ATOM_lastIndexOf,builtin_string_lastIndexOf, 1);
    install_method(ctx, str_proto, R8E_ATOM_includes,   builtin_string_includes, 1);
    install_method(ctx, str_proto, R8E_ATOM_startsWith, builtin_string_startsWith, 1);
    install_method(ctx, str_proto, R8E_ATOM_endsWith,   builtin_string_endsWith, 1);
    install_method(ctx, str_proto, R8E_ATOM_slice,      builtin_string_slice, 2);
    install_method(ctx, str_proto, R8E_ATOM_substring,  builtin_string_substring, 2);
    install_method(ctx, str_proto, R8E_ATOM_substr,     builtin_string_substr, 2);
    install_method(ctx, str_proto, R8E_ATOM_toUpperCase,builtin_string_toUpperCase, 0);
    install_method(ctx, str_proto, R8E_ATOM_toLowerCase,builtin_string_toLowerCase, 0);
    install_method(ctx, str_proto, R8E_ATOM_trim,       builtin_string_trim, 0);
    install_method(ctx, str_proto, R8E_ATOM_trimStart,  builtin_string_trimStart, 0);
    install_method(ctx, str_proto, R8E_ATOM_trimEnd,    builtin_string_trimEnd, 0);
    install_method(ctx, str_proto, R8E_ATOM_split,      builtin_string_split, 2);
    install_method(ctx, str_proto, R8E_ATOM_replace,    builtin_string_replace, 2);
    install_method(ctx, str_proto, R8E_ATOM_replaceAll, builtin_string_replaceAll, 2);
    install_method(ctx, str_proto, R8E_ATOM_repeat,     builtin_string_repeat, 1);
    install_method(ctx, str_proto, R8E_ATOM_padStart,   builtin_string_padStart, 1);
    install_method(ctx, str_proto, R8E_ATOM_padEnd,     builtin_string_padEnd, 1);
    install_method(ctx, str_proto, R8E_ATOM_concat,     builtin_string_concat, -1);
    install_method(ctx, str_proto, R8E_ATOM_at,         builtin_string_at, 1);
    install_method(ctx, str_proto, R8E_ATOM_match,      builtin_string_match, 1);
    install_method(ctx, str_proto, R8E_ATOM_matchAll,   builtin_string_matchAll, 1);
    install_method(ctx, str_proto, R8E_ATOM_search,     builtin_string_search, 1);
    install_method(ctx, str_proto, R8E_ATOM_normalize,  builtin_string_normalize, 0);
    install_method(ctx, str_proto, R8E_ATOM_toString,   builtin_string_toString, 0);
    install_method(ctx, str_proto, R8E_ATOM_valueOf,    builtin_string_valueOf, 0);

    /* String constructor */
    R8EObjTier0 *str_ctor = r8e_obj_new(ctx);
    install_method(ctx, str_ctor, R8E_ATOM_fromCharCode,
                   builtin_string_fromCharCode, -1);
    install_method(ctx, str_ctor, R8E_ATOM_fromCodePoint,
                   builtin_string_fromCodePoint, -1);
    install_value(ctx, str_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(str_proto));

    /* --- Number.prototype --- */
    R8EObjTier0 *num_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    ctx->prototypes[R8E_PROTO_NUMBER] = num_proto;

    install_method(ctx, num_proto, R8E_ATOM_toFixed,
                   builtin_number_proto_toFixed, 1);
    install_method(ctx, num_proto, R8E_ATOM_toExponential,
                   builtin_number_proto_toExponential, 1);
    install_method(ctx, num_proto, R8E_ATOM_toPrecision,
                   builtin_number_proto_toPrecision, 1);
    install_method(ctx, num_proto, R8E_ATOM_toString,
                   builtin_number_proto_toString, 0);
    install_method(ctx, num_proto, R8E_ATOM_valueOf,
                   builtin_number_proto_valueOf, 0);

    /* Number constructor */
    R8EObjTier0 *num_ctor = r8e_obj_new(ctx);
    install_method(ctx, num_ctor, R8E_ATOM_isFinite,
                   builtin_number_isFinite, 1);
    install_method(ctx, num_ctor, R8E_ATOM_isInteger,
                   builtin_number_isInteger, 1);
    install_method(ctx, num_ctor, R8E_ATOM_isNaN,
                   builtin_number_isNaN, 1);
    install_method(ctx, num_ctor, R8E_ATOM_isSafeInteger,
                   builtin_number_isSafeInteger, 1);
    install_method(ctx, num_ctor, R8E_ATOM_parseInt,
                   builtin_number_parseInt, 2);
    install_method(ctx, num_ctor, R8E_ATOM_parseFloat,
                   builtin_number_parseFloat, 1);
    install_value(ctx, num_ctor, R8E_ATOM_EPSILON,
                  r8e_from_double(DBL_EPSILON));
    install_value(ctx, num_ctor, R8E_ATOM_MAX_SAFE_INTEGER,
                  r8e_from_double(9007199254740991.0));
    install_value(ctx, num_ctor, R8E_ATOM_MIN_SAFE_INTEGER,
                  r8e_from_double(-9007199254740991.0));
    install_value(ctx, num_ctor, R8E_ATOM_MAX_VALUE,
                  r8e_from_double(DBL_MAX));
    install_value(ctx, num_ctor, R8E_ATOM_MIN_VALUE,
                  r8e_from_double(DBL_MIN));
    install_value(ctx, num_ctor, R8E_ATOM_NaN,
                  r8e_from_double(NAN));
    install_value(ctx, num_ctor, R8E_ATOM_POSITIVE_INFINITY,
                  r8e_from_double(INFINITY));
    install_value(ctx, num_ctor, R8E_ATOM_NEGATIVE_INFINITY,
                  r8e_from_double(-INFINITY));
    install_value(ctx, num_ctor, R8E_ATOM_prototype,
                  r8e_from_pointer(num_proto));

    /* --- Boolean.prototype --- */
    R8EObjTier0 *bool_proto = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    ctx->prototypes[R8E_PROTO_BOOLEAN] = bool_proto;

    install_method(ctx, bool_proto, R8E_ATOM_toString,
                   builtin_boolean_proto_toString, 0);
    install_method(ctx, bool_proto, R8E_ATOM_valueOf,
                   builtin_boolean_proto_valueOf, 0);

    /* --- Math object --- */
    R8EObjTier0 *math_obj = r8e_obj_new_with_proto(ctx, R8E_PROTO_OBJECT);
    ctx->prototypes[R8E_PROTO_MATH] = math_obj;

    /* Math constants */
    install_value(ctx, math_obj, R8E_ATOM_E,
                  r8e_from_double(2.718281828459045));
    install_value(ctx, math_obj, R8E_ATOM_PI,
                  r8e_from_double(3.141592653589793));
    install_value(ctx, math_obj, R8E_ATOM_LN2,
                  r8e_from_double(0.6931471805599453));
    install_value(ctx, math_obj, R8E_ATOM_LN10,
                  r8e_from_double(2.302585092994046));
    /* LOG2E, LOG10E, SQRT2, SQRT1_2 use runtime atoms */

    /* Math methods */
    install_method(ctx, math_obj, R8E_ATOM_abs,    builtin_math_abs, 1);
    install_method(ctx, math_obj, R8E_ATOM_ceil,   builtin_math_ceil, 1);
    install_method(ctx, math_obj, R8E_ATOM_floor,  builtin_math_floor, 1);
    install_method(ctx, math_obj, R8E_ATOM_round,  builtin_math_round, 1);
    install_method(ctx, math_obj, R8E_ATOM_trunc,  builtin_math_trunc, 1);
    install_method(ctx, math_obj, R8E_ATOM_sqrt,   builtin_math_sqrt, 1);
    install_method(ctx, math_obj, R8E_ATOM_cbrt,   builtin_math_cbrt, 1);
    install_method(ctx, math_obj, R8E_ATOM_pow,    builtin_math_pow, 2);
    install_method(ctx, math_obj, R8E_ATOM_exp,    builtin_math_exp, 1);
    install_method(ctx, math_obj, R8E_ATOM_log,    builtin_math_log, 1);
    install_method(ctx, math_obj, R8E_ATOM_log2,   builtin_math_log2, 1);
    install_method(ctx, math_obj, R8E_ATOM_log10,  builtin_math_log10, 1);
    install_method(ctx, math_obj, R8E_ATOM_sin,    builtin_math_sin, 1);
    install_method(ctx, math_obj, R8E_ATOM_cos,    builtin_math_cos, 1);
    install_method(ctx, math_obj, R8E_ATOM_tan,    builtin_math_tan, 1);
    install_method(ctx, math_obj, R8E_ATOM_atan,   builtin_math_atan, 1);
    install_method(ctx, math_obj, R8E_ATOM_atan2,  builtin_math_atan2, 2);
    install_method(ctx, math_obj, R8E_ATOM_random, builtin_math_random, 0);
    install_method(ctx, math_obj, R8E_ATOM_max,    builtin_math_max, -1);
    install_method(ctx, math_obj, R8E_ATOM_min,    builtin_math_min, -1);

    /* --- Symbol constructor --- */
    R8EObjTier0 *sym_ctor = r8e_obj_new(ctx);
    /* Install well-known symbol values */
    install_value(ctx, sym_ctor, R8E_ATOM_iterator,
                  r8e_from_symbol(R8E_SYMBOL_ITERATOR));
    install_value(ctx, sym_ctor, R8E_ATOM_hasInstance,
                  r8e_from_symbol(R8E_SYMBOL_HAS_INSTANCE));
    install_value(ctx, sym_ctor, R8E_ATOM_toPrimitive,
                  r8e_from_symbol(R8E_SYMBOL_TO_PRIMITIVE));
    install_value(ctx, sym_ctor, R8E_ATOM_toStringTag,
                  r8e_from_symbol(R8E_SYMBOL_TO_STRING_TAG));
    install_value(ctx, sym_ctor, R8E_ATOM_isConcatSpreadable,
                  r8e_from_symbol(R8E_SYMBOL_IS_CONCAT_SPREADABLE));
    install_value(ctx, sym_ctor, R8E_ATOM_species,
                  r8e_from_symbol(R8E_SYMBOL_SPECIES));
    install_value(ctx, sym_ctor, R8E_ATOM_match,
                  r8e_from_symbol(R8E_SYMBOL_MATCH));
    install_value(ctx, sym_ctor, R8E_ATOM_replace,
                  r8e_from_symbol(R8E_SYMBOL_REPLACE));
    install_value(ctx, sym_ctor, R8E_ATOM_search,
                  r8e_from_symbol(R8E_SYMBOL_SEARCH));
    install_value(ctx, sym_ctor, R8E_ATOM_split,
                  r8e_from_symbol(R8E_SYMBOL_SPLIT));
    install_value(ctx, sym_ctor, R8E_ATOM_unscopables,
                  r8e_from_symbol(R8E_SYMBOL_UNSCOPABLES));

    /* Symbol.for, Symbol.keyFor */
    install_method(ctx, sym_ctor, R8E_ATOM_create /* reuse "for" */,
                   builtin_symbol_for, 1);

    /* --- Install on globalThis --- */
    void *global = ctx->global_object;
    if (!global) {
        global = (void *)r8e_obj_new(ctx);
        ctx->global_object = global;
    }

    /* Constructor objects */
    global = r8e_obj_set(ctx, global, R8E_ATOM_Object,  r8e_from_pointer(obj_ctor));
    global = r8e_obj_set(ctx, global, R8E_ATOM_Array,   r8e_from_pointer(arr_ctor));
    global = r8e_obj_set(ctx, global, R8E_ATOM_String,  r8e_from_pointer(str_ctor));
    global = r8e_obj_set(ctx, global, R8E_ATOM_Number,  r8e_from_pointer(num_ctor));
    global = r8e_obj_set(ctx, global, R8E_ATOM_Boolean, r8e_from_pointer(
                             r8e_obj_new(ctx)));
    global = r8e_obj_set(ctx, global, R8E_ATOM_Symbol,  r8e_from_pointer(sym_ctor));
    global = r8e_obj_set(ctx, global, R8E_ATOM_Math,    r8e_from_pointer(math_obj));

    /* Global values */
    global = r8e_obj_set(ctx, global, R8E_ATOM_undefined, R8E_UNDEFINED);
    global = r8e_obj_set(ctx, global, R8E_ATOM_NaN,
                         r8e_from_double(NAN));
    global = r8e_obj_set(ctx, global, R8E_ATOM_Infinity,
                         r8e_from_double(INFINITY));
    global = r8e_obj_set(ctx, global, R8E_ATOM_globalThis,
                         r8e_from_pointer(global));

    /* Global functions */
    install_method(ctx, global, R8E_ATOM_parseInt,  builtin_global_parseInt, 2);
    install_method(ctx, global, R8E_ATOM_parseFloat,
                   builtin_global_parseFloat, 1);
    install_method(ctx, global, R8E_ATOM_isFinite,  builtin_global_isFinite, 1);
    install_method(ctx, global, R8E_ATOM_isNaN,     builtin_global_isNaN, 1);
    install_method(ctx, global, R8E_ATOM_eval,      builtin_global_eval, 1);

    /* URI functions (stubs) */
    install_method(ctx, global, R8E_ATOM_name /* encodeURI */,
                   builtin_global_encodeURI, 1);

    /* console object */
    R8EObjTier0 *console_obj = r8e_obj_new(ctx);
    install_method(ctx, console_obj, R8E_ATOM_log_method,
                   builtin_console_log, -1);
    install_method(ctx, console_obj, R8E_ATOM_warn,
                   builtin_console_warn, -1);
    install_method(ctx, console_obj, R8E_ATOM_error,
                   builtin_console_error, -1);
    global = r8e_obj_set(ctx, global, R8E_ATOM_console,
                         r8e_from_pointer(console_obj));

    ctx->global_object = global;

    /* --- Freeze all built-in prototypes (Section 11.8) --- */
    if (obj_proto)  r8e_obj_freeze(ctx, obj_proto);
    if (arr_proto)  r8e_obj_freeze(ctx, arr_proto);
    if (str_proto)  r8e_obj_freeze(ctx, str_proto);
    if (num_proto)  r8e_obj_freeze(ctx, num_proto);
    if (bool_proto) r8e_obj_freeze(ctx, bool_proto);
    if (math_obj)   r8e_obj_freeze(ctx, math_obj);
}
