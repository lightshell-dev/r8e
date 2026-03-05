/*
 * r8e_atoms.h - Pre-Interned Atom Table (Common Names)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 4.5 (String Interning / Atom Table).
 *
 * Architecture:
 *   - ~256 common property names pre-interned at startup
 *   - Atom index 0 is reserved (empty/no-property sentinel)
 *   - Comparing two interned strings = comparing two integers
 *   - 256-bit Bloom filter for fast "not present" check
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_ATOMS_H
#define R8E_ATOMS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration: full definition in r8e_types.h */
#ifndef R8E_TYPES_H
struct R8EAtomTable;
typedef struct R8EAtomTable R8EAtomTable;
#endif

/* =========================================================================
 * Pre-Interned Atom IDs
 *
 * These are the well-known atom indices. The atom table is pre-populated
 * with these entries at context creation time. User-defined atoms get
 * indices starting at R8E_ATOM__FIRST_USER.
 *
 * Organized by category for readability.
 * ========================================================================= */

typedef enum {
    /* =====================================================================
     * 0: Reserved sentinel (no property / empty slot)
     * ===================================================================== */
    R8E_ATOM_EMPTY              = 0,

    /* =====================================================================
     * 1-30: Most common property names
     * ===================================================================== */
    R8E_ATOM_length             = 1,
    R8E_ATOM_prototype          = 2,
    R8E_ATOM_constructor        = 3,
    R8E_ATOM_toString           = 4,
    R8E_ATOM_valueOf            = 5,
    R8E_ATOM_hasOwnProperty     = 6,
    R8E_ATOM___proto__          = 7,
    R8E_ATOM_name               = 8,
    R8E_ATOM_message            = 9,
    R8E_ATOM_stack              = 10,
    R8E_ATOM_value              = 11,
    R8E_ATOM_writable           = 12,
    R8E_ATOM_enumerable         = 13,
    R8E_ATOM_configurable       = 14,
    R8E_ATOM_get                = 15,
    R8E_ATOM_set                = 16,
    R8E_ATOM_apply              = 17,
    R8E_ATOM_call               = 18,
    R8E_ATOM_bind               = 19,
    R8E_ATOM_arguments          = 20,
    R8E_ATOM_caller             = 21,
    R8E_ATOM_callee             = 22,
    R8E_ATOM_this               = 23,
    R8E_ATOM_toJSON             = 24,
    R8E_ATOM_toLocaleString     = 25,
    R8E_ATOM_isPrototypeOf      = 26,
    R8E_ATOM_propertyIsEnumerable = 27,
    R8E_ATOM___defineGetter__   = 28,
    R8E_ATOM___defineSetter__   = 29,
    R8E_ATOM___lookupGetter__   = 30,

    /* =====================================================================
     * 31-40: Type names and special literals
     * ===================================================================== */
    R8E_ATOM_undefined          = 31,
    R8E_ATOM_null               = 32,
    R8E_ATOM_true               = 33,
    R8E_ATOM_false              = 34,
    R8E_ATOM_NaN                = 35,
    R8E_ATOM_Infinity           = 36,
    R8E_ATOM_number             = 37,
    R8E_ATOM_string             = 38,
    R8E_ATOM_boolean            = 39,
    R8E_ATOM_object             = 40,
    R8E_ATOM_function           = 41,
    R8E_ATOM_symbol             = 42,
    R8E_ATOM_bigint             = 43,

    /* =====================================================================
     * 44-80: Built-in constructor/global names
     * ===================================================================== */
    R8E_ATOM_Object             = 44,
    R8E_ATOM_Array              = 45,
    R8E_ATOM_String             = 46,
    R8E_ATOM_Number             = 47,
    R8E_ATOM_Boolean            = 48,
    R8E_ATOM_Function           = 49,
    R8E_ATOM_Symbol             = 50,
    R8E_ATOM_Error              = 51,
    R8E_ATOM_TypeError          = 52,
    R8E_ATOM_RangeError         = 53,
    R8E_ATOM_SyntaxError        = 54,
    R8E_ATOM_ReferenceError     = 55,
    R8E_ATOM_URIError           = 56,
    R8E_ATOM_EvalError          = 57,
    R8E_ATOM_AggregateError     = 58,
    R8E_ATOM_RegExp             = 59,
    R8E_ATOM_Date               = 60,
    R8E_ATOM_Map                = 61,
    R8E_ATOM_Set                = 62,
    R8E_ATOM_WeakMap            = 63,
    R8E_ATOM_WeakSet            = 64,
    R8E_ATOM_WeakRef            = 65,
    R8E_ATOM_FinalizationRegistry = 66,
    R8E_ATOM_Promise            = 67,
    R8E_ATOM_Proxy              = 68,
    R8E_ATOM_Reflect            = 69,
    R8E_ATOM_JSON               = 70,
    R8E_ATOM_Math               = 71,
    R8E_ATOM_console            = 72,
    R8E_ATOM_globalThis         = 73,
    R8E_ATOM_ArrayBuffer        = 74,
    R8E_ATOM_SharedArrayBuffer  = 75,
    R8E_ATOM_DataView           = 76,
    R8E_ATOM_Int8Array          = 77,
    R8E_ATOM_Uint8Array         = 78,
    R8E_ATOM_Uint8ClampedArray  = 79,
    R8E_ATOM_Int16Array         = 80,

    /* =====================================================================
     * 81-100: More typed arrays and iterators
     * ===================================================================== */
    R8E_ATOM_Uint16Array        = 81,
    R8E_ATOM_Int32Array         = 82,
    R8E_ATOM_Uint32Array        = 83,
    R8E_ATOM_Float32Array       = 84,
    R8E_ATOM_Float64Array       = 85,
    R8E_ATOM_BigInt64Array      = 86,
    R8E_ATOM_BigUint64Array     = 87,
    R8E_ATOM_Iterator           = 88,
    R8E_ATOM_Generator          = 89,
    R8E_ATOM_GeneratorFunction  = 90,
    R8E_ATOM_AsyncFunction      = 91,
    R8E_ATOM_AsyncGeneratorFunction = 92,

    /* =====================================================================
     * 93-130: Object/Array/String method names
     * ===================================================================== */
    R8E_ATOM_keys               = 93,
    R8E_ATOM_values             = 94,
    R8E_ATOM_entries            = 95,
    R8E_ATOM_freeze             = 96,
    R8E_ATOM_isFrozen           = 97,
    R8E_ATOM_seal               = 98,
    R8E_ATOM_isSealed           = 99,
    R8E_ATOM_create             = 100,
    R8E_ATOM_assign             = 101,
    R8E_ATOM_defineProperty     = 102,
    R8E_ATOM_defineProperties   = 103,
    R8E_ATOM_getOwnPropertyDescriptor  = 104,
    R8E_ATOM_getOwnPropertyDescriptors = 105,
    R8E_ATOM_getOwnPropertyNames       = 106,
    R8E_ATOM_getOwnPropertySymbols     = 107,
    R8E_ATOM_getPrototypeOf    = 108,
    R8E_ATOM_setPrototypeOf    = 109,
    R8E_ATOM_is                = 110,
    R8E_ATOM_isExtensible      = 111,
    R8E_ATOM_preventExtensions = 112,
    R8E_ATOM_fromEntries       = 113,
    R8E_ATOM_push              = 114,
    R8E_ATOM_pop               = 115,
    R8E_ATOM_shift             = 116,
    R8E_ATOM_unshift           = 117,
    R8E_ATOM_splice            = 118,
    R8E_ATOM_slice             = 119,
    R8E_ATOM_concat            = 120,
    R8E_ATOM_join              = 121,
    R8E_ATOM_reverse           = 122,
    R8E_ATOM_sort              = 123,
    R8E_ATOM_indexOf           = 124,
    R8E_ATOM_lastIndexOf       = 125,
    R8E_ATOM_includes          = 126,
    R8E_ATOM_find              = 127,
    R8E_ATOM_findIndex         = 128,
    R8E_ATOM_findLast          = 129,
    R8E_ATOM_findLastIndex     = 130,

    /* =====================================================================
     * 131-160: More Array/String/Iterator methods
     * ===================================================================== */
    R8E_ATOM_filter             = 131,
    R8E_ATOM_map                = 132,
    R8E_ATOM_reduce             = 133,
    R8E_ATOM_reduceRight        = 134,
    R8E_ATOM_forEach            = 135,
    R8E_ATOM_every              = 136,
    R8E_ATOM_some               = 137,
    R8E_ATOM_flat               = 138,
    R8E_ATOM_flatMap            = 139,
    R8E_ATOM_fill               = 140,
    R8E_ATOM_copyWithin         = 141,
    R8E_ATOM_from               = 142,
    R8E_ATOM_of                 = 143,
    R8E_ATOM_isArray            = 144,
    R8E_ATOM_at                 = 145,
    R8E_ATOM_charAt             = 146,
    R8E_ATOM_charCodeAt         = 147,
    R8E_ATOM_codePointAt        = 148,
    R8E_ATOM_substring          = 149,
    R8E_ATOM_substr             = 150,
    R8E_ATOM_trim               = 151,
    R8E_ATOM_trimStart          = 152,
    R8E_ATOM_trimEnd            = 153,
    R8E_ATOM_padStart           = 154,
    R8E_ATOM_padEnd             = 155,
    R8E_ATOM_repeat             = 156,
    R8E_ATOM_replace            = 157,
    R8E_ATOM_replaceAll         = 158,
    R8E_ATOM_split              = 159,
    R8E_ATOM_match              = 160,

    /* =====================================================================
     * 161-180: String/RegExp/Number methods
     * ===================================================================== */
    R8E_ATOM_matchAll           = 161,
    R8E_ATOM_search             = 162,
    R8E_ATOM_startsWith         = 163,
    R8E_ATOM_endsWith           = 164,
    R8E_ATOM_normalize          = 165,
    R8E_ATOM_toUpperCase        = 166,
    R8E_ATOM_toLowerCase        = 167,
    R8E_ATOM_raw                = 168,
    R8E_ATOM_fromCharCode       = 169,
    R8E_ATOM_fromCodePoint      = 170,
    R8E_ATOM_test               = 171,
    R8E_ATOM_exec               = 172,
    R8E_ATOM_source             = 173,
    R8E_ATOM_flags              = 174,
    R8E_ATOM_global             = 175,
    R8E_ATOM_ignoreCase         = 176,
    R8E_ATOM_multiline          = 177,
    R8E_ATOM_dotAll             = 178,
    R8E_ATOM_unicode            = 179,
    R8E_ATOM_sticky             = 180,

    /* =====================================================================
     * 181-200: Number/Math methods and properties
     * ===================================================================== */
    R8E_ATOM_toFixed            = 181,
    R8E_ATOM_toPrecision        = 182,
    R8E_ATOM_toExponential      = 183,
    R8E_ATOM_parseInt           = 184,
    R8E_ATOM_parseFloat         = 185,
    R8E_ATOM_isFinite           = 186,
    R8E_ATOM_isNaN              = 187,
    R8E_ATOM_isInteger          = 188,
    R8E_ATOM_isSafeInteger      = 189,
    R8E_ATOM_MAX_VALUE          = 190,
    R8E_ATOM_MIN_VALUE          = 191,
    R8E_ATOM_MAX_SAFE_INTEGER   = 192,
    R8E_ATOM_MIN_SAFE_INTEGER   = 193,
    R8E_ATOM_EPSILON            = 194,
    R8E_ATOM_POSITIVE_INFINITY  = 195,
    R8E_ATOM_NEGATIVE_INFINITY  = 196,
    R8E_ATOM_PI                 = 197,
    R8E_ATOM_E                  = 198,
    R8E_ATOM_LN2               = 199,
    R8E_ATOM_LN10              = 200,

    /* =====================================================================
     * 201-220: Math methods
     * ===================================================================== */
    R8E_ATOM_abs                = 201,
    R8E_ATOM_ceil               = 202,
    R8E_ATOM_floor              = 203,
    R8E_ATOM_round              = 204,
    R8E_ATOM_trunc              = 205,
    R8E_ATOM_sqrt               = 206,
    R8E_ATOM_cbrt               = 207,
    R8E_ATOM_pow                = 208,
    R8E_ATOM_log                = 209,
    R8E_ATOM_log2               = 210,
    R8E_ATOM_log10              = 211,
    R8E_ATOM_exp                = 212,
    R8E_ATOM_sin                = 213,
    R8E_ATOM_cos                = 214,
    R8E_ATOM_tan                = 215,
    R8E_ATOM_atan               = 216,
    R8E_ATOM_atan2              = 217,
    R8E_ATOM_random             = 218,
    R8E_ATOM_min                = 219,
    R8E_ATOM_max                = 220,

    /* =====================================================================
     * 221-240: Promise/JSON/Date/Symbol methods
     * ===================================================================== */
    R8E_ATOM_then               = 221,
    R8E_ATOM_catch              = 222,
    R8E_ATOM_finally            = 223,
    R8E_ATOM_resolve            = 224,
    R8E_ATOM_reject             = 225,
    R8E_ATOM_all                = 226,
    R8E_ATOM_allSettled         = 227,
    R8E_ATOM_any                = 228,
    R8E_ATOM_race               = 229,
    R8E_ATOM_parse              = 230,
    R8E_ATOM_stringify          = 231,
    R8E_ATOM_now                = 232,
    R8E_ATOM_getTime            = 233,
    R8E_ATOM_setTime            = 234,
    R8E_ATOM_toISOString        = 235,
    R8E_ATOM_iterator           = 236,
    R8E_ATOM_asyncIterator      = 237,
    R8E_ATOM_hasInstance         = 238,
    R8E_ATOM_toPrimitive        = 239,
    R8E_ATOM_toStringTag        = 240,

    /* =====================================================================
     * 241-265: Keywords (for parser use)
     * ===================================================================== */
    R8E_ATOM_var                = 241,
    R8E_ATOM_let                = 242,
    R8E_ATOM_const              = 243,
    R8E_ATOM_if                 = 244,
    R8E_ATOM_else               = 245,
    R8E_ATOM_for                = 246,
    R8E_ATOM_while              = 247,
    R8E_ATOM_do                 = 248,
    R8E_ATOM_switch             = 249,
    R8E_ATOM_case               = 250,
    R8E_ATOM_default            = 251,
    R8E_ATOM_break              = 252,
    R8E_ATOM_continue           = 253,
    R8E_ATOM_return             = 254,
    R8E_ATOM_throw              = 255,
    R8E_ATOM_try                = 256,
    R8E_ATOM_catch_kw           = 257,  /* "catch" - avoid conflict with atom name */
    R8E_ATOM_finally_kw         = 258,  /* "finally" */
    R8E_ATOM_new                = 259,
    R8E_ATOM_delete             = 260,
    R8E_ATOM_typeof_kw          = 261,  /* "typeof" */
    R8E_ATOM_void_kw            = 262,  /* "void" */
    R8E_ATOM_instanceof_kw      = 263,  /* "instanceof" */
    R8E_ATOM_in_kw              = 264,  /* "in" */
    R8E_ATOM_of_kw              = 265,  /* "of" - keyword context (for-of) */

    /* =====================================================================
     * 266-285: More keywords and contextual keywords
     * ===================================================================== */
    R8E_ATOM_class              = 266,
    R8E_ATOM_extends            = 267,
    R8E_ATOM_super              = 268,
    R8E_ATOM_import_kw          = 269,
    R8E_ATOM_export_kw          = 270,
    R8E_ATOM_async              = 271,
    R8E_ATOM_await_kw           = 272,
    R8E_ATOM_yield_kw           = 273,
    R8E_ATOM_static             = 274,
    R8E_ATOM_with               = 275,
    R8E_ATOM_debugger           = 276,
    R8E_ATOM_enum               = 277,
    R8E_ATOM_implements         = 278,
    R8E_ATOM_interface          = 279,
    R8E_ATOM_package            = 280,
    R8E_ATOM_private            = 281,
    R8E_ATOM_protected          = 282,
    R8E_ATOM_public             = 283,

    /* =====================================================================
     * 284-300: Miscellaneous common names
     * ===================================================================== */
    R8E_ATOM_eval               = 284,
    R8E_ATOM_index              = 285,
    R8E_ATOM_input              = 286,
    R8E_ATOM_groups             = 287,
    R8E_ATOM_done               = 288,
    R8E_ATOM_next               = 289,
    R8E_ATOM_return_kw          = 290,  /* "return" as property name */
    R8E_ATOM_throw_kw           = 291,  /* "throw" as property name */
    R8E_ATOM_size               = 292,
    R8E_ATOM_has                = 293,
    R8E_ATOM_add                = 294,
    R8E_ATOM_clear              = 295,
    R8E_ATOM_delete_method      = 296,  /* "delete" as method name */
    R8E_ATOM_species            = 297,
    R8E_ATOM_isConcatSpreadable = 298,
    R8E_ATOM_unscopables        = 299,
    R8E_ATOM_status             = 300,

    /* =====================================================================
     * 301-310: Console/utility methods
     * ===================================================================== */
    R8E_ATOM_log_method         = 301,  /* console.log */
    R8E_ATOM_warn               = 302,
    R8E_ATOM_error              = 303,
    R8E_ATOM_info               = 304,
    R8E_ATOM_debug              = 305,
    R8E_ATOM_assert             = 306,
    R8E_ATOM_trace              = 307,
    R8E_ATOM_dir                = 308,
    R8E_ATOM_time               = 309,
    R8E_ATOM_timeEnd            = 310,

    /* =====================================================================
     * 311-320: Map/Set/WeakRef methods
     * ===================================================================== */
    R8E_ATOM_deref              = 311,
    R8E_ATOM_register           = 312,
    R8E_ATOM_unregister         = 313,
    R8E_ATOM_revocable          = 314,
    R8E_ATOM_revoke             = 315,
    R8E_ATOM_construct          = 316,
    R8E_ATOM_ownKeys            = 317,
    R8E_ATOM_deleteProperty     = 318,
    R8E_ATOM_enumerate          = 319,
    R8E_ATOM_target             = 320,

    /* =====================================================================
     * 321-330: Miscellaneous
     * ===================================================================== */
    R8E_ATOM_reason             = 321,
    R8E_ATOM_fulfilled          = 322,
    R8E_ATOM_rejected           = 323,
    R8E_ATOM_pending            = 324,
    R8E_ATOM_proxy              = 325,
    R8E_ATOM_handler            = 326,
    R8E_ATOM_byteLength         = 327,
    R8E_ATOM_byteOffset         = 328,
    R8E_ATOM_buffer             = 329,
    R8E_ATOM_BYTES_PER_ELEMENT  = 330,

    /* =====================================================================
     * Sentinel: first user-defined atom index
     * ===================================================================== */
    R8E_ATOM__FIRST_USER        = 331,

    /* Total pre-interned atoms */
    R8E_ATOM__BUILTIN_COUNT     = 331

} R8EAtomID;


/* =========================================================================
 * Static String Table
 *
 * Maps atom ID -> C string for initialization of the atom table.
 * This table is used once at context creation to pre-populate the
 * atom hash table and Bloom filter.
 * ========================================================================= */

#ifdef R8E_ATOMS_IMPL

static const char *r8e_atom_strings[R8E_ATOM__BUILTIN_COUNT] = {
    /* 0 */   "",                        /* EMPTY sentinel */
    /* 1 */   "length",
    /* 2 */   "prototype",
    /* 3 */   "constructor",
    /* 4 */   "toString",
    /* 5 */   "valueOf",
    /* 6 */   "hasOwnProperty",
    /* 7 */   "__proto__",
    /* 8 */   "name",
    /* 9 */   "message",
    /* 10 */  "stack",
    /* 11 */  "value",
    /* 12 */  "writable",
    /* 13 */  "enumerable",
    /* 14 */  "configurable",
    /* 15 */  "get",
    /* 16 */  "set",
    /* 17 */  "apply",
    /* 18 */  "call",
    /* 19 */  "bind",
    /* 20 */  "arguments",
    /* 21 */  "caller",
    /* 22 */  "callee",
    /* 23 */  "this",
    /* 24 */  "toJSON",
    /* 25 */  "toLocaleString",
    /* 26 */  "isPrototypeOf",
    /* 27 */  "propertyIsEnumerable",
    /* 28 */  "__defineGetter__",
    /* 29 */  "__defineSetter__",
    /* 30 */  "__lookupGetter__",
    /* 31 */  "undefined",
    /* 32 */  "null",
    /* 33 */  "true",
    /* 34 */  "false",
    /* 35 */  "NaN",
    /* 36 */  "Infinity",
    /* 37 */  "number",
    /* 38 */  "string",
    /* 39 */  "boolean",
    /* 40 */  "object",
    /* 41 */  "function",
    /* 42 */  "symbol",
    /* 43 */  "bigint",
    /* 44 */  "Object",
    /* 45 */  "Array",
    /* 46 */  "String",
    /* 47 */  "Number",
    /* 48 */  "Boolean",
    /* 49 */  "Function",
    /* 50 */  "Symbol",
    /* 51 */  "Error",
    /* 52 */  "TypeError",
    /* 53 */  "RangeError",
    /* 54 */  "SyntaxError",
    /* 55 */  "ReferenceError",
    /* 56 */  "URIError",
    /* 57 */  "EvalError",
    /* 58 */  "AggregateError",
    /* 59 */  "RegExp",
    /* 60 */  "Date",
    /* 61 */  "Map",
    /* 62 */  "Set",
    /* 63 */  "WeakMap",
    /* 64 */  "WeakSet",
    /* 65 */  "WeakRef",
    /* 66 */  "FinalizationRegistry",
    /* 67 */  "Promise",
    /* 68 */  "Proxy",
    /* 69 */  "Reflect",
    /* 70 */  "JSON",
    /* 71 */  "Math",
    /* 72 */  "console",
    /* 73 */  "globalThis",
    /* 74 */  "ArrayBuffer",
    /* 75 */  "SharedArrayBuffer",
    /* 76 */  "DataView",
    /* 77 */  "Int8Array",
    /* 78 */  "Uint8Array",
    /* 79 */  "Uint8ClampedArray",
    /* 80 */  "Int16Array",
    /* 81 */  "Uint16Array",
    /* 82 */  "Int32Array",
    /* 83 */  "Uint32Array",
    /* 84 */  "Float32Array",
    /* 85 */  "Float64Array",
    /* 86 */  "BigInt64Array",
    /* 87 */  "BigUint64Array",
    /* 88 */  "Iterator",
    /* 89 */  "Generator",
    /* 90 */  "GeneratorFunction",
    /* 91 */  "AsyncFunction",
    /* 92 */  "AsyncGeneratorFunction",
    /* 93 */  "keys",
    /* 94 */  "values",
    /* 95 */  "entries",
    /* 96 */  "freeze",
    /* 97 */  "isFrozen",
    /* 98 */  "seal",
    /* 99 */  "isSealed",
    /* 100 */ "create",
    /* 101 */ "assign",
    /* 102 */ "defineProperty",
    /* 103 */ "defineProperties",
    /* 104 */ "getOwnPropertyDescriptor",
    /* 105 */ "getOwnPropertyDescriptors",
    /* 106 */ "getOwnPropertyNames",
    /* 107 */ "getOwnPropertySymbols",
    /* 108 */ "getPrototypeOf",
    /* 109 */ "setPrototypeOf",
    /* 110 */ "is",
    /* 111 */ "isExtensible",
    /* 112 */ "preventExtensions",
    /* 113 */ "fromEntries",
    /* 114 */ "push",
    /* 115 */ "pop",
    /* 116 */ "shift",
    /* 117 */ "unshift",
    /* 118 */ "splice",
    /* 119 */ "slice",
    /* 120 */ "concat",
    /* 121 */ "join",
    /* 122 */ "reverse",
    /* 123 */ "sort",
    /* 124 */ "indexOf",
    /* 125 */ "lastIndexOf",
    /* 126 */ "includes",
    /* 127 */ "find",
    /* 128 */ "findIndex",
    /* 129 */ "findLast",
    /* 130 */ "findLastIndex",
    /* 131 */ "filter",
    /* 132 */ "map",
    /* 133 */ "reduce",
    /* 134 */ "reduceRight",
    /* 135 */ "forEach",
    /* 136 */ "every",
    /* 137 */ "some",
    /* 138 */ "flat",
    /* 139 */ "flatMap",
    /* 140 */ "fill",
    /* 141 */ "copyWithin",
    /* 142 */ "from",
    /* 143 */ "of",
    /* 144 */ "isArray",
    /* 145 */ "at",
    /* 146 */ "charAt",
    /* 147 */ "charCodeAt",
    /* 148 */ "codePointAt",
    /* 149 */ "substring",
    /* 150 */ "substr",
    /* 151 */ "trim",
    /* 152 */ "trimStart",
    /* 153 */ "trimEnd",
    /* 154 */ "padStart",
    /* 155 */ "padEnd",
    /* 156 */ "repeat",
    /* 157 */ "replace",
    /* 158 */ "replaceAll",
    /* 159 */ "split",
    /* 160 */ "match",
    /* 161 */ "matchAll",
    /* 162 */ "search",
    /* 163 */ "startsWith",
    /* 164 */ "endsWith",
    /* 165 */ "normalize",
    /* 166 */ "toUpperCase",
    /* 167 */ "toLowerCase",
    /* 168 */ "raw",
    /* 169 */ "fromCharCode",
    /* 170 */ "fromCodePoint",
    /* 171 */ "test",
    /* 172 */ "exec",
    /* 173 */ "source",
    /* 174 */ "flags",
    /* 175 */ "global",
    /* 176 */ "ignoreCase",
    /* 177 */ "multiline",
    /* 178 */ "dotAll",
    /* 179 */ "unicode",
    /* 180 */ "sticky",
    /* 181 */ "toFixed",
    /* 182 */ "toPrecision",
    /* 183 */ "toExponential",
    /* 184 */ "parseInt",
    /* 185 */ "parseFloat",
    /* 186 */ "isFinite",
    /* 187 */ "isNaN",
    /* 188 */ "isInteger",
    /* 189 */ "isSafeInteger",
    /* 190 */ "MAX_VALUE",
    /* 191 */ "MIN_VALUE",
    /* 192 */ "MAX_SAFE_INTEGER",
    /* 193 */ "MIN_SAFE_INTEGER",
    /* 194 */ "EPSILON",
    /* 195 */ "POSITIVE_INFINITY",
    /* 196 */ "NEGATIVE_INFINITY",
    /* 197 */ "PI",
    /* 198 */ "E",
    /* 199 */ "LN2",
    /* 200 */ "LN10",
    /* 201 */ "abs",
    /* 202 */ "ceil",
    /* 203 */ "floor",
    /* 204 */ "round",
    /* 205 */ "trunc",
    /* 206 */ "sqrt",
    /* 207 */ "cbrt",
    /* 208 */ "pow",
    /* 209 */ "log",
    /* 210 */ "log2",
    /* 211 */ "log10",
    /* 212 */ "exp",
    /* 213 */ "sin",
    /* 214 */ "cos",
    /* 215 */ "tan",
    /* 216 */ "atan",
    /* 217 */ "atan2",
    /* 218 */ "random",
    /* 219 */ "min",
    /* 220 */ "max",
    /* 221 */ "then",
    /* 222 */ "catch",
    /* 223 */ "finally",
    /* 224 */ "resolve",
    /* 225 */ "reject",
    /* 226 */ "all",
    /* 227 */ "allSettled",
    /* 228 */ "any",
    /* 229 */ "race",
    /* 230 */ "parse",
    /* 231 */ "stringify",
    /* 232 */ "now",
    /* 233 */ "getTime",
    /* 234 */ "setTime",
    /* 235 */ "toISOString",
    /* 236 */ "Symbol.iterator",
    /* 237 */ "Symbol.asyncIterator",
    /* 238 */ "Symbol.hasInstance",
    /* 239 */ "Symbol.toPrimitive",
    /* 240 */ "Symbol.toStringTag",
    /* 241 */ "var",
    /* 242 */ "let",
    /* 243 */ "const",
    /* 244 */ "if",
    /* 245 */ "else",
    /* 246 */ "for",
    /* 247 */ "while",
    /* 248 */ "do",
    /* 249 */ "switch",
    /* 250 */ "case",
    /* 251 */ "default",
    /* 252 */ "break",
    /* 253 */ "continue",
    /* 254 */ "return",
    /* 255 */ "throw",
    /* 256 */ "try",
    /* 257 */ "catch",
    /* 258 */ "finally",
    /* 259 */ "new",
    /* 260 */ "delete",
    /* 261 */ "typeof",
    /* 262 */ "void",
    /* 263 */ "instanceof",
    /* 264 */ "in",
    /* 265 */ "of",
    /* 266 */ "class",
    /* 267 */ "extends",
    /* 268 */ "super",
    /* 269 */ "import",
    /* 270 */ "export",
    /* 271 */ "async",
    /* 272 */ "await",
    /* 273 */ "yield",
    /* 274 */ "static",
    /* 275 */ "with",
    /* 276 */ "debugger",
    /* 277 */ "enum",
    /* 278 */ "implements",
    /* 279 */ "interface",
    /* 280 */ "package",
    /* 281 */ "private",
    /* 282 */ "protected",
    /* 283 */ "public",
    /* 284 */ "eval",
    /* 285 */ "index",
    /* 286 */ "input",
    /* 287 */ "groups",
    /* 288 */ "done",
    /* 289 */ "next",
    /* 290 */ "return",
    /* 291 */ "throw",
    /* 292 */ "size",
    /* 293 */ "has",
    /* 294 */ "add",
    /* 295 */ "clear",
    /* 296 */ "delete",
    /* 297 */ "Symbol.species",
    /* 298 */ "Symbol.isConcatSpreadable",
    /* 299 */ "Symbol.unscopables",
    /* 300 */ "status",
    /* 301 */ "log",
    /* 302 */ "warn",
    /* 303 */ "error",
    /* 304 */ "info",
    /* 305 */ "debug",
    /* 306 */ "assert",
    /* 307 */ "trace",
    /* 308 */ "dir",
    /* 309 */ "time",
    /* 310 */ "timeEnd",
    /* 311 */ "deref",
    /* 312 */ "register",
    /* 313 */ "unregister",
    /* 314 */ "revocable",
    /* 315 */ "revoke",
    /* 316 */ "construct",
    /* 317 */ "ownKeys",
    /* 318 */ "deleteProperty",
    /* 319 */ "enumerate",
    /* 320 */ "target",
    /* 321 */ "reason",
    /* 322 */ "fulfilled",
    /* 323 */ "rejected",
    /* 324 */ "pending",
    /* 325 */ "proxy",
    /* 326 */ "handler",
    /* 327 */ "byteLength",
    /* 328 */ "byteOffset",
    /* 329 */ "buffer",
    /* 330 */ "BYTES_PER_ELEMENT",
};

#else
/* Declaration only: defined in the compilation unit that defines R8E_ATOMS_IMPL */
extern const char *r8e_atom_strings[R8E_ATOM__BUILTIN_COUNT];
#endif /* R8E_ATOMS_IMPL */


/* =========================================================================
 * Atom Table Initialization API
 * ========================================================================= */

/**
 * Initialize an atom table with pre-interned atoms.
 * Called once at context creation.
 */
int r8e_atom_table_init(R8EAtomTable *table);

/**
 * Free an atom table and all its entries.
 */
void r8e_atom_table_free(R8EAtomTable *table);

/**
 * Intern a string: look up or insert, return atom index.
 * The string is copied if it needs to be added.
 */
uint32_t r8e_atom_intern(R8EAtomTable *table, const char *str, uint32_t len);

/**
 * Look up an atom by string. Returns atom index or 0 (R8E_ATOM_EMPTY) if not found.
 * Uses Bloom filter for fast rejection.
 */
uint32_t r8e_atom_find(const R8EAtomTable *table, const char *str, uint32_t len);

/**
 * Get the string for an atom index.
 * Returns NULL if index is out of range.
 */
const char *r8e_atom_str(const R8EAtomTable *table, uint32_t atom);


/* =========================================================================
 * String Hashing (used by atom table and string comparisons)
 * ========================================================================= */

/**
 * FNV-1a hash function for strings.
 * Fast, good distribution, simple to implement.
 */
static inline uint32_t r8e_string_hash(const char *str, uint32_t len) {
    uint32_t hash = 2166136261U;  /* FNV offset basis */
    for (uint32_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619U;  /* FNV prime */
    }
    return hash;
}

/**
 * Set a bit in the 256-bit Bloom filter.
 */
static inline void r8e_bloom_add(uint64_t bloom[4], uint32_t hash) {
    uint8_t bit = (uint8_t)(hash & 0xFF);
    bloom[bit >> 6] |= (1ULL << (bit & 63));
}

/**
 * Test a bit in the 256-bit Bloom filter.
 * Returns true if the bit is set (string MIGHT be present).
 * Returns false if the bit is not set (string DEFINITELY not present).
 */
static inline bool r8e_bloom_test(const uint64_t bloom[4], uint32_t hash) {
    uint8_t bit = (uint8_t)(hash & 0xFF);
    return (bloom[bit >> 6] & (1ULL << (bit & 63))) != 0;
}


#ifdef __cplusplus
}
#endif

#endif /* R8E_ATOMS_H */
