/*
 * r8e_api.c - Public API implementation (glue between parser and interpreter)
 *
 * Wires up the full eval pipeline:
 *   JS source -> r8e_compile_script (parser) -> bytecode
 *   bytecode  -> r8e_interpret (interpreter) -> result value
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../include/r8e_types.h"
#include "../include/r8e_api.h"

/* =========================================================================
 * Forward declarations for internal types and functions
 *
 * R8EBytecodeBuffer is defined in r8e_bc.c (included by r8e_parse.c).
 * R8EInterpContext is defined in r8e_interp.c.
 * We replicate the struct layouts here for access to their fields.
 * ========================================================================= */

/* --- Bytecode buffer (must match r8e_bc.c layout exactly) --- */
typedef struct R8EBytecodeBuffer {
    uint8_t  *code;
    uint32_t  length;
    uint32_t  capacity;

    R8EValue *constants;
    uint16_t  const_count;
    uint16_t  const_capacity;

    uint32_t *line_table;
    uint16_t  line_count;
    uint16_t  line_capacity;

    struct R8EFuncDesc {
        struct R8EBytecodeBuffer *bc;
        uint32_t  name_atom;
        uint16_t  param_count;
        uint16_t  local_count;
        uint16_t  capture_count;
        uint16_t  stack_size;
        uint8_t   is_strict;
        uint8_t   is_arrow;
        uint8_t   is_generator;
        uint8_t   is_async;
        uint16_t  source_line;
        struct R8ECaptureInfo {
            uint32_t atom;
            uint8_t  src_reg;
            uint8_t  src_depth;
            uint8_t  is_mutable;
        } *captures;
    } *functions;
    uint16_t  func_count;
    uint16_t  func_capacity;

    uint16_t  max_stack;
    uint16_t  cur_stack;
} R8EBytecodeBuffer;

/* --- Interpreter context (opaque, from r8e_interp.c) --- */
typedef struct R8EInterpContext R8EInterpContext;

/* --- External functions from r8e_parse.c --- */
extern R8EBytecodeBuffer *r8e_compile_script(R8EAtomTable *atoms,
                                              const char *source,
                                              uint32_t length,
                                              const char *filename);
/* r8e_bc_destroy is static in r8e_bc.c (included by r8e_parse.c) so we
 * cannot call it directly. We'll free bytecode buffer fields manually. */

/* --- External functions from r8e_interp.c --- */
extern R8EInterpContext *r8e_interp_context_new(void);
extern void              r8e_interp_context_free(R8EInterpContext *ctx);
extern void              r8e_interp_set_global(R8EInterpContext *ctx,
                                               R8EValue global);
extern R8EValue          r8e_interpret(R8EInterpContext *ctx,
                                       const uint8_t *bytecode,
                                       uint32_t bytecode_len,
                                       R8EValue *constants,
                                       uint16_t num_constants);
extern R8EValue          r8e_interpret_ex(R8EInterpContext *ctx,
                                           const uint8_t *bytecode,
                                           uint32_t bytecode_len,
                                           R8EValue *constants,
                                           uint16_t num_constants,
                                           uint16_t func_base);
extern bool              r8e_interp_has_exception(const R8EInterpContext *ctx);
extern R8EValue          r8e_interp_get_exception(const R8EInterpContext *ctx);
extern R8EValue          r8e_interp_clear_exception(R8EInterpContext *ctx);
extern R8EValue          r8e_call_function(R8EInterpContext *ctx,
                                            R8EValue func, R8EValue this_val,
                                            const R8EValue *args, int argc);

/* --- External functions from r8e_atom.c ---
 * Note: r8e_atom.c has its own R8EContext definition ({ void *arena; }).
 * The functions accept a pointer but don't use it for atom operations
 * (they use a global g_atom_table). We declare them as taking void*. */
extern int r8e_atom_table_init(R8EAtomTable *table);
extern void r8e_atom_table_destroy(R8EAtomTable *table);

/* --- Weak stubs for module functions (overridden by r8e_module.c if linked) --- */
__attribute__((weak))
R8EValue r8e_compile_module(R8EContext *ctx, const char *source,
                             size_t len, const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}
__attribute__((weak))
R8EValue r8e_exec_function(R8EContext *ctx, R8EValue func) {
    (void)ctx; (void)func;
    return R8E_UNDEFINED;
}

/* --- External functions from r8e_weakref.c (Map/Set) --- */
extern R8EValue r8e_map_new(R8EContext *ctx);
extern R8EValue r8e_map_get(R8EContext *ctx, R8EValue map, R8EValue key);
extern bool     r8e_map_set(R8EContext *ctx, R8EValue map,
                              R8EValue key, R8EValue value);
extern bool     r8e_map_has(R8EContext *ctx, R8EValue map, R8EValue key);
extern bool     r8e_map_delete(R8EContext *ctx, R8EValue map, R8EValue key);
extern void     r8e_map_clear(R8EContext *ctx, R8EValue map);
extern uint32_t r8e_map_size(R8EContext *ctx, R8EValue map);
extern R8EValue r8e_set_new(R8EContext *ctx);
extern R8EValue r8e_set_add(R8EContext *ctx, R8EValue set, R8EValue value);
extern bool     r8e_set_has(R8EContext *ctx, R8EValue set, R8EValue value);
extern bool     r8e_set_delete(R8EContext *ctx, R8EValue set, R8EValue value);
extern void     r8e_set_clear(R8EContext *ctx, R8EValue set);
extern uint32_t r8e_set_size(R8EContext *ctx, R8EValue set);
extern void     r8e_map_for_each(R8EContext *ctx, R8EValue map,
                                  R8EValue callback, R8EValue this_arg);
extern void     r8e_set_for_each(R8EContext *ctx, R8EValue set,
                                  R8EValue callback, R8EValue this_arg);

/* =========================================================================
 * Context lifecycle
 *
 * r8e_context_new allocates a large block (matching the stub's 4096 bytes)
 * to be compatible with other modules that have their own R8EContext layout.
 * ========================================================================= */

#define R8E_CTX_ALLOC_SIZE 4096

/* Map/Set constructor callbacks for r8e_eval (ctx pointer is unused) */
static R8EValue api_map_constructor(R8EContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return r8e_map_new(ctx);
}

static R8EValue api_set_constructor(R8EContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return r8e_set_new(ctx);
}

/* =========================================================================
 * Object/Array static methods & global functions for Svelte support
 *
 * These native callbacks receive R8EInterpContext* disguised as R8EContext*.
 * They must NOT use R8EContext-specific fields; work only with NaN-boxed values.
 * ========================================================================= */

/* Forward declare atom-to-string lookup */
extern const char *r8e_atom_get_str(void *ctx, uint32_t atom, uint32_t *out_len);

/* Forward declare types used by builtin implementations */
typedef struct { R8EValue key; R8EValue val; } ApiPropPairFwd;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} ApiObjTier0Fwd;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t   count;
    uint8_t   pad[7];
    ApiPropPairFwd props[4];
} ApiObjTier1Fwd;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t   count;
    uint8_t   capacity;
    uint8_t   pad[6];
    ApiPropPairFwd *props;
} ApiObjTier2Fwd;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *elements;
    void     *named;
} ApiArrayInterpFwd;

/* Helper: get string data from an R8EValue (inline or heap) */
static const char *api_get_string(R8EValue v, char *buf, uint32_t *out_len) {
    if (R8E_IS_INLINE_STR(v)) {
        int len = r8e_inline_str_len(v);
        r8e_inline_str_decode(v, buf);
        *out_len = (uint32_t)len;
        return buf;
    }
    if (((v) >> 48) == 0xFFFCU) { /* atom */
        uint32_t atom_id = (uint32_t)(v & 0xFFFFFFFFULL);
        const char *s = r8e_atom_get_str(NULL, atom_id, out_len);
        if (s) return s;
    }
    if (R8E_IS_POINTER(v)) {
        void *ptr = r8e_get_pointer(v);
        if (ptr) {
            uint32_t flags = *(uint32_t *)ptr;
            uint8_t kind = (flags >> 5) & 0x7;
            if (kind == 1) { /* STRING */
                uint32_t *words = (uint32_t *)ptr;
                *out_len = words[2]; /* byte_length */
                /* Data follows the struct: flags(4) + hash(4) + byte_length(4) +
                   char_length(4) + offset_table_ptr(8) = 24 bytes */
                return (const char *)ptr + 24;
            }
        }
    }
    *out_len = 0;
    return "";
}

/* Helper: make an interp-compatible string value */
static R8EValue api_make_string(const char *data, uint32_t len) {
    /* Try inline (0-6 ASCII chars) */
    if (len <= 6) {
        bool all_ascii = true;
        for (uint32_t i = 0; i < len; i++) {
            if ((uint8_t)data[i] > 127) { all_ascii = false; break; }
        }
        if (all_ascii) {
            uint64_t v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)len << 45);
            for (uint32_t i = 0; i < len; i++)
                v |= ((uint64_t)(uint8_t)data[i] << (38 - i * 7));
            return v;
        }
    }
    /* Heap string: flags(4) + hash(4) + byte_length(4) + char_length(4) +
       offset_table_ptr(8) + data */
    size_t hdr_size = 24;
    void *s = calloc(1, hdr_size + len + 1);
    if (!s) return R8E_UNDEFINED;
    uint32_t *words = (uint32_t *)s;
    words[0] = (1u << 5); /* kind = STRING */
    words[1] = 0; /* hash */
    words[2] = len; /* byte_length */
    words[3] = len; /* char_length (ASCII assumption) */
    /* offset_table pointer at offset 16 = NULL */
    memcpy((char *)s + hdr_size, data, len);
    ((char *)s)[hdr_size + len] = '\0';
    return r8e_from_pointer(s);
}

/* Helper: create an interp-compatible array */
static R8EValue api_make_interp_arr(uint32_t cap) {
    if (cap == 0) cap = 4;
    ApiArrayInterpFwd *arr = (ApiArrayInterpFwd *)calloc(1, sizeof(ApiArrayInterpFwd));
    if (!arr) return R8E_UNDEFINED;
    arr->flags = (2u << 5); /* ARRAY kind */
    arr->proto_id = 2;
    arr->length = 0;
    arr->capacity = cap;
    arr->elements = (R8EValue *)calloc(cap, sizeof(R8EValue));
    if (!arr->elements) { free(arr); return R8E_UNDEFINED; }
    arr->named = NULL;
    return r8e_from_pointer(arr);
}

/* Helper: push element to interp array */
static void api_array_push(R8EValue arr_val, R8EValue elem) {
    ApiArrayInterpFwd *arr = (ApiArrayInterpFwd *)r8e_get_pointer(arr_val);
    if (!arr) return;
    if (arr->length >= arr->capacity) {
        uint32_t new_cap = arr->capacity * 2;
        R8EValue *new_el = (R8EValue *)realloc(arr->elements,
            new_cap * sizeof(R8EValue));
        if (!new_el) return;
        arr->elements = new_el;
        arr->capacity = new_cap;
    }
    arr->elements[arr->length++] = elem;
}

/* Helper: iterate own properties of an object, calling fn for each key/value */
typedef void (*api_prop_iter_fn)(uint32_t atom, R8EValue key_tagged,
                                  R8EValue val, void *userdata);

static void api_iter_own_props(R8EValue obj, api_prop_iter_fn fn, void *ud) {
    if (!R8E_IS_POINTER(obj)) return;
    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return;
    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> 5) & 0x7;
    if (kind != 0) return; /* not an object */
    uint8_t tier = flags & 0x03;

    if (tier == 0) {
        ApiObjTier0Fwd *t0 = (ApiObjTier0Fwd *)ptr;
        if (t0->key0 != 0) {
            uint32_t atom = (uint32_t)(t0->key0 & 0xFFFFFFFFULL);
            fn(atom, t0->key0, t0->val0, ud);
        }
    } else if (tier == 1) {
        ApiObjTier1Fwd *t1 = (ApiObjTier1Fwd *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            uint32_t atom = (uint32_t)(t1->props[i].key & 0xFFFFFFFFULL);
            fn(atom, t1->props[i].key, t1->props[i].val, ud);
        }
    } else if (tier == 2) {
        ApiObjTier2Fwd *t2 = (ApiObjTier2Fwd *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                uint32_t atom = (uint32_t)(t2->props[i].key & 0xFFFFFFFFULL);
                fn(atom, t2->props[i].key, t2->props[i].val, ud);
            }
        }
    }
}

/* Callback for Object.keys: collect key names */
static void obj_keys_cb(uint32_t atom, R8EValue key_tagged,
                          R8EValue val, void *ud) {
    (void)key_tagged; (void)val;
    R8EValue arr = *(R8EValue *)ud;
    uint32_t name_len;
    const char *name = r8e_atom_get_str(NULL, atom, &name_len);
    if (name) {
        api_array_push(arr, api_make_string(name, name_len));
    }
}

static R8EValue api_object_keys(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return api_make_interp_arr(0);
    R8EValue target = argv[0];
    R8EValue result = api_make_interp_arr(8);
    api_iter_own_props(target, obj_keys_cb, &result);
    return result;
}

/* Callback for Object.values */
static void obj_values_cb(uint32_t atom, R8EValue key_tagged,
                            R8EValue val, void *ud) {
    (void)atom; (void)key_tagged;
    api_array_push(*(R8EValue *)ud, val);
}

static R8EValue api_object_values(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return api_make_interp_arr(0);
    R8EValue result = api_make_interp_arr(8);
    api_iter_own_props(argv[0], obj_values_cb, &result);
    return result;
}

/* Callback for Object.entries */
static void obj_entries_cb(uint32_t atom, R8EValue key_tagged,
                             R8EValue val, void *ud) {
    (void)key_tagged;
    R8EValue arr = *(R8EValue *)ud;
    uint32_t name_len;
    const char *name = r8e_atom_get_str(NULL, atom, &name_len);
    if (name) {
        R8EValue pair = api_make_interp_arr(2);
        api_array_push(pair, api_make_string(name, name_len));
        api_array_push(pair, val);
        api_array_push(arr, pair);
    }
}

static R8EValue api_object_entries(R8EContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return api_make_interp_arr(0);
    R8EValue result = api_make_interp_arr(8);
    api_iter_own_props(argv[0], obj_entries_cb, &result);
    return result;
}

/* Object.assign(target, ...sources) */
static void obj_assign_cb(uint32_t atom, R8EValue key_tagged,
                            R8EValue val, void *ud) {
    R8EValue target = *(R8EValue *)ud;
    if (!R8E_IS_POINTER(target)) return;
    void *ptr = r8e_get_pointer(target);
    if (!ptr) return;
    uint32_t flags = *(uint32_t *)ptr;
    uint8_t tier = flags & 0x03;

    if (tier == 0) {
        ApiObjTier0Fwd *t0 = (ApiObjTier0Fwd *)ptr;
        if (t0->key0 == key_tagged || t0->key0 == 0) {
            t0->key0 = key_tagged;
            t0->val0 = val;
        }
        /* Tier 0 full - cannot add more properties without realloc */
        return;
    } else if (tier == 1) {
        ApiObjTier1Fwd *t1 = (ApiObjTier1Fwd *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key_tagged) {
                t1->props[i].val = val;
                return;
            }
        }
        if (t1->count < 4) {
            t1->props[t1->count].key = key_tagged;
            t1->props[t1->count].val = val;
            t1->count++;
            return;
        }
        /* Tier 1 full - promote to tier 2 */
        {
            uint8_t old_count = t1->count;
            uint8_t new_cap = 16;
            ApiPropPairFwd *new_props = (ApiPropPairFwd *)calloc(new_cap, sizeof(ApiPropPairFwd));
            if (!new_props) return;
            for (uint8_t i = 0; i < old_count; i++)
                new_props[i] = t1->props[i];
            new_props[old_count].key = key_tagged;
            new_props[old_count].val = val;
            ApiObjTier2Fwd *t2 = (ApiObjTier2Fwd *)ptr;
            t2->flags = (0u << 5) | 2u;
            t2->count = old_count + 1;
            t2->capacity = new_cap;
            t2->props = new_props;
        }
    } else if (tier == 2) {
        ApiObjTier2Fwd *t2 = (ApiObjTier2Fwd *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key_tagged) {
                    t2->props[i].val = val;
                    return;
                }
            }
            if (t2->count < t2->capacity) {
                t2->props[t2->count].key = key_tagged;
                t2->props[t2->count].val = val;
                t2->count++;
            }
        }
    }
    (void)atom;
}

static R8EValue api_object_assign(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return R8E_UNDEFINED;
    R8EValue target = argv[0];
    for (int i = 1; i < argc; i++) {
        api_iter_own_props(argv[i], obj_assign_cb, &target);
    }
    return target;
}

/* Object.freeze(obj) - set frozen bit */
static R8EValue api_object_freeze(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1 || !R8E_IS_POINTER(argv[0])) return argc >= 1 ? argv[0] : R8E_UNDEFINED;
    void *ptr = r8e_get_pointer(argv[0]);
    if (ptr) {
        uint32_t *flags = (uint32_t *)ptr;
        *flags |= 0x10u; /* frozen bit */
    }
    return argv[0];
}

/* Object.create(proto) */
static R8EValue api_object_create(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    /* Create a new empty object; proto linkage is not fully supported
       but we return a valid object */
    ApiObjTier1Fwd *obj = (ApiObjTier1Fwd *)calloc(1, sizeof(ApiObjTier1Fwd));
    if (!obj) return R8E_UNDEFINED;
    obj->flags = (0u << 5) | 1u; /* kind=OBJECT, tier=1 */
    obj->proto_id = 1; /* PROTO_OBJECT */
    obj->count = 0;
    (void)argc; (void)argv;
    return r8e_from_pointer(obj);
}

/* Array.isArray(val) */
static R8EValue api_array_isArray(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return R8E_FALSE;
    R8EValue val = argv[0];
    if (!R8E_IS_POINTER(val)) return R8E_FALSE;
    void *ptr = r8e_get_pointer(val);
    if (!ptr) return R8E_FALSE;
    uint32_t flags = *(uint32_t *)ptr;
    uint8_t kind = (flags >> 5) & 0x7;
    return (kind == 2) ? R8E_TRUE : R8E_FALSE; /* 2 = ARRAY */
}

/* Array.from(iterable) */
static R8EValue api_array_from(R8EContext *ctx, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return api_make_interp_arr(0);
    R8EValue src = argv[0];
    /* If already an array, copy it */
    if (R8E_IS_POINTER(src)) {
        void *ptr = r8e_get_pointer(src);
        if (ptr) {
            uint32_t flags = *(uint32_t *)ptr;
            uint8_t kind = (flags >> 5) & 0x7;
            if (kind == 2) { /* ARRAY */
                ApiArrayInterpFwd *sarr = (ApiArrayInterpFwd *)ptr;
                R8EValue result = api_make_interp_arr(sarr->length > 0 ? sarr->length : 4);
                ApiArrayInterpFwd *darr = (ApiArrayInterpFwd *)r8e_get_pointer(result);
                if (darr) {
                    for (uint32_t i = 0; i < sarr->length; i++) {
                        api_array_push(result, sarr->elements[i]);
                    }
                }
                return result;
            }
        }
    }
    /* If it's a string, convert to array of chars */
    if (R8E_IS_INLINE_STR(src) || (((src) >> 48) == 0xFFFCU) ||
        (R8E_IS_POINTER(src) && r8e_get_pointer(src) &&
         ((*(uint32_t *)r8e_get_pointer(src) >> 5) & 0x7) == 1)) {
        char sbuf[8]; uint32_t slen;
        const char *sdata = api_get_string(src, sbuf, &slen);
        R8EValue result = api_make_interp_arr(slen > 0 ? slen : 4);
        for (uint32_t i = 0; i < slen; i++) {
            api_array_push(result, api_make_string(sdata + i, 1));
        }
        return result;
    }
    return api_make_interp_arr(0);
}

/* parseInt(str, radix) */
static R8EValue api_global_parseInt(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return r8e_from_double(NAN);
    R8EValue val = argv[0];
    /* If already a number, truncate */
    if (R8E_IS_INT32(val)) return val;
    if (R8E_IS_DOUBLE(val)) {
        double d;
        memcpy(&d, &val, 8);
        return r8e_from_int32((int32_t)d);
    }
    /* String parsing */
    char sbuf[8]; uint32_t slen;
    const char *s = api_get_string(val, sbuf, &slen);
    if (slen == 0) return r8e_from_double(NAN);
    /* Skip whitespace */
    uint32_t i = 0;
    while (i < slen && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
    if (i >= slen) return r8e_from_double(NAN);
    int sign = 1;
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') { i++; }
    int radix = 10;
    if (argc >= 2 && R8E_IS_INT32(argv[1])) {
        radix = (int)r8e_get_int32(argv[1]);
    }
    if (radix == 0) radix = 10;
    if (i + 1 < slen && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X') && radix == 16) {
        i += 2;
    } else if (i + 1 < slen && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X') && radix == 10) {
        radix = 16; i += 2;
    }
    long result = 0;
    bool has_digit = false;
    while (i < slen) {
        int digit = -1;
        if (s[i] >= '0' && s[i] <= '9') digit = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') digit = s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F') digit = s[i] - 'A' + 10;
        if (digit < 0 || digit >= radix) break;
        result = result * radix + digit;
        has_digit = true;
        i++;
    }
    if (!has_digit) return r8e_from_double(NAN);
    return r8e_from_int32((int32_t)(sign * result));
}

/* parseFloat(str) */
static R8EValue api_global_parseFloat(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return r8e_from_double(NAN);
    R8EValue val = argv[0];
    if (R8E_IS_INT32(val)) return r8e_from_double((double)r8e_get_int32(val));
    if (R8E_IS_DOUBLE(val)) return val;
    char sbuf[8]; uint32_t slen;
    const char *s = api_get_string(val, sbuf, &slen);
    if (slen == 0) return r8e_from_double(NAN);
    char *end = NULL;
    /* Need null-terminated copy for strtod */
    char tmp[256];
    uint32_t copy_len = slen < 255 ? slen : 255;
    memcpy(tmp, s, copy_len);
    tmp[copy_len] = '\0';
    double d = strtod(tmp, &end);
    if (end == tmp) return r8e_from_double(NAN);
    return r8e_from_double(d);
}

/* isNaN(val) */
static R8EValue api_global_isNaN(R8EContext *ctx, R8EValue this_val,
                                   int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return R8E_TRUE;
    R8EValue val = argv[0];
    if (R8E_IS_INT32(val)) return R8E_FALSE;
    if (R8E_IS_DOUBLE(val)) {
        double d; memcpy(&d, &val, 8);
        return isnan(d) ? R8E_TRUE : R8E_FALSE;
    }
    return R8E_TRUE; /* non-numeric values -> NaN */
}

/* isFinite(val) */
static R8EValue api_global_isFinite(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return R8E_FALSE;
    R8EValue val = argv[0];
    if (R8E_IS_INT32(val)) return R8E_TRUE;
    if (R8E_IS_DOUBLE(val)) {
        double d; memcpy(&d, &val, 8);
        return isfinite(d) ? R8E_TRUE : R8E_FALSE;
    }
    return R8E_FALSE;
}

/* encodeURIComponent(str) - basic implementation */
static R8EValue api_global_encodeURIComponent(R8EContext *ctx, R8EValue this_val,
                                                int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return api_make_string("undefined", 9);
    char sbuf[8]; uint32_t slen;
    const char *s = api_get_string(argv[0], sbuf, &slen);
    /* Allocate worst case (3x) */
    char *buf = (char *)malloc(slen * 3 + 1);
    if (!buf) return argv[0];
    uint32_t wp = 0;
    for (uint32_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') {
            buf[wp++] = c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            buf[wp++] = '%';
            buf[wp++] = hex[c >> 4];
            buf[wp++] = hex[c & 0xF];
        }
    }
    buf[wp] = '\0';
    R8EValue result = api_make_string(buf, wp);
    free(buf);
    return result;
}

/* decodeURIComponent(str) - basic implementation */
static R8EValue api_global_decodeURIComponent(R8EContext *ctx, R8EValue this_val,
                                                int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return api_make_string("undefined", 9);
    char sbuf[8]; uint32_t slen;
    const char *s = api_get_string(argv[0], sbuf, &slen);
    char *buf = (char *)malloc(slen + 1);
    if (!buf) return argv[0];
    uint32_t wp = 0;
    for (uint32_t i = 0; i < slen; i++) {
        if (s[i] == '%' && i + 2 < slen) {
            int hi = -1, lo = -1;
            if (s[i+1] >= '0' && s[i+1] <= '9') hi = s[i+1] - '0';
            else if (s[i+1] >= 'A' && s[i+1] <= 'F') hi = s[i+1] - 'A' + 10;
            else if (s[i+1] >= 'a' && s[i+1] <= 'f') hi = s[i+1] - 'a' + 10;
            if (s[i+2] >= '0' && s[i+2] <= '9') lo = s[i+2] - '0';
            else if (s[i+2] >= 'A' && s[i+2] <= 'F') lo = s[i+2] - 'A' + 10;
            else if (s[i+2] >= 'a' && s[i+2] <= 'f') lo = s[i+2] - 'a' + 10;
            if (hi >= 0 && lo >= 0) {
                buf[wp++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[wp++] = s[i];
    }
    buf[wp] = '\0';
    R8EValue result = api_make_string(buf, wp);
    free(buf);
    return result;
}

R8EContext *r8e_context_new(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, R8E_CTX_ALLOC_SIZE);
    if (!ctx) return NULL;

    /* Initialize the global atom table (idempotent - safe to call multiple times) */
    r8e_atom_table_init(NULL);

    /* Set defaults */
    ctx->realm_count = 0;
    ctx->current_realm = 0;
    ctx->stack_depth = 0;
    ctx->max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    ctx->epoch_threshold = R8E_DEFAULT_SUSPECT_THRESHOLD;
    ctx->error.has_exception = false;
    ctx->error.exception = R8E_UNDEFINED;

    /* Create a persistent global object for cross-eval global access */
    ctx->global_obj = r8e_make_object(ctx);

    /* Register Map and Set constructors on the global object */
    r8e_set_global_func(ctx, "Map", api_map_constructor, 0);
    r8e_set_global_func(ctx, "Set", api_set_constructor, 0);

    /* --- Object constructor with static methods --- */
    {
        R8EValue obj_ctor = r8e_make_object(ctx);
        r8e_set_prop(ctx, obj_ctor, "keys",    r8e_make_native_func(ctx, api_object_keys, "keys", 1));
        r8e_set_prop(ctx, obj_ctor, "values",  r8e_make_native_func(ctx, api_object_values, "values", 1));
        r8e_set_prop(ctx, obj_ctor, "entries", r8e_make_native_func(ctx, api_object_entries, "entries", 1));
        r8e_set_prop(ctx, obj_ctor, "assign",  r8e_make_native_func(ctx, api_object_assign, "assign", 2));
        r8e_set_prop(ctx, obj_ctor, "freeze",  r8e_make_native_func(ctx, api_object_freeze, "freeze", 1));
        r8e_set_prop(ctx, obj_ctor, "create",  r8e_make_native_func(ctx, api_object_create, "create", 1));
        r8e_set_global(ctx, "Object", obj_ctor);
    }

    /* --- Array constructor with static methods --- */
    {
        R8EValue arr_ctor = r8e_make_object(ctx);
        r8e_set_prop(ctx, arr_ctor, "isArray", r8e_make_native_func(ctx, api_array_isArray, "isArray", 1));
        r8e_set_prop(ctx, arr_ctor, "from",    r8e_make_native_func(ctx, api_array_from, "from", 1));
        r8e_set_global(ctx, "Array", arr_ctor);
    }

    /* --- Global utility functions --- */
    r8e_set_global_func(ctx, "parseInt",  api_global_parseInt, 2);
    r8e_set_global_func(ctx, "parseFloat", api_global_parseFloat, 1);
    r8e_set_global_func(ctx, "isNaN",     api_global_isNaN, 1);
    r8e_set_global_func(ctx, "isFinite",  api_global_isFinite, 1);
    r8e_set_global_func(ctx, "encodeURIComponent", api_global_encodeURIComponent, 1);
    r8e_set_global_func(ctx, "decodeURIComponent", api_global_decodeURIComponent, 1);

    return ctx;
}

void r8e_context_free(R8EContext *ctx) {
    if (!ctx) return;
    /* Free any allocated realms */
    for (int i = 0; i < 16; i++) {
        if (ctx->realms[i]) {
            free(ctx->realms[i]);
            ctx->realms[i] = NULL;
        }
    }
    /* Don't destroy the global atom table here - it's shared across all
     * contexts and managed separately. Other tests may still need it. */
    free(ctx);
}

/* =========================================================================
 * Helper: free a bytecode buffer
 *
 * r8e_bc_destroy is static in r8e_bc.c, so we inline the destruction here.
 * Must match the r8e_bc_destroy logic exactly.
 * ========================================================================= */

static void bc_free(R8EBytecodeBuffer *bc) {
    if (!bc) return;

    free(bc->code);
    free(bc->constants);

    /* Free nested function descriptors */
    for (uint16_t i = 0; i < bc->func_count; i++) {
        if (bc->functions[i].bc) {
            bc_free(bc->functions[i].bc);
            free(bc->functions[i].bc);
        }
        free(bc->functions[i].captures);
    }
    free(bc->functions);
    free(bc->line_table);
}

/* =========================================================================
 * Script evaluation: the full pipeline
 * ========================================================================= */

/* Opcode constants needed for bytecode patching */
#define R8E_OP_DROP  0x11
#define R8E_OP_HALT  0xFF
#define R8E_OP_NOP   0xFE

/* GC kind constants matching the interpreter */
#define API_GC_KIND_SHIFT    5
#define API_GC_KIND_CLOSURE  3u
#define API_GC_KIND_FUNCTION 5u

/* Interpreter-compatible function/closure structs (must match r8e_interp.c) */
typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t  *bytecode;
    uint32_t  bytecode_len;
    uint16_t  param_count;
    uint16_t  local_count;
    uint16_t  capture_count;
    uint16_t  stack_size;
    uint32_t  name_atom;
    R8EValue *constants;
    uint16_t  const_count;
    uint8_t   is_strict;
    uint8_t   is_arrow;
    uint8_t   is_generator;
    uint8_t   is_async;
    uint16_t  source_line;
} ApiFunctionInterp;

typedef struct {
    uint8_t  kind;
    uint8_t  index;
    uint16_t pad;
} ApiCaptureMetaInterp;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  refcount;
    uint16_t  slot_count;
    uint16_t  reserved;
    R8EValue *slots;
} ApiEnvFrameInterp;

typedef struct {
    uint32_t              flags;
    uint32_t              proto_id;
    ApiFunctionInterp    *func;
    uint8_t               capture_count;
    uint8_t               capture_mode;
    uint16_t              reserved;
    union {
        struct { R8EValue inline_captures[2]; } inl;
        struct {
            R8EValue              *values;
            ApiCaptureMetaInterp  *meta;
            ApiEnvFrameInterp     *env;
        } arr;
        struct { ApiEnvFrameInterp *env_frame; } frm;
    } storage;
} ApiClosureInterp;

/**
 * Build interpreter-compatible closure objects from the parser's
 * function descriptors. Returns a malloc'd array of R8EValue pointers
 * (NaN-boxed closure pointers). Caller must free.
 */
static R8EValue *build_function_closures(R8EBytecodeBuffer *bc,
                                          uint16_t *out_count) {
    *out_count = bc->func_count;
    if (bc->func_count == 0) return NULL;

    R8EValue *closures = (R8EValue *)calloc(bc->func_count, sizeof(R8EValue));
    if (!closures) return NULL;

    for (uint16_t i = 0; i < bc->func_count; i++) {
        struct R8EFuncDesc *fd = &bc->functions[i];

        /* Allocate function object */
        ApiFunctionInterp *func = (ApiFunctionInterp *)calloc(
            1, sizeof(ApiFunctionInterp));
        if (!func) { closures[i] = R8E_UNDEFINED; continue; }

        func->flags = (API_GC_KIND_FUNCTION << API_GC_KIND_SHIFT);
        func->proto_id = 3; /* PROTO_FUNCTION */
        func->param_count = fd->param_count;
        func->local_count = fd->local_count;
        func->capture_count = fd->capture_count;
        func->stack_size = fd->stack_size > 0 ? fd->stack_size : 256;
        func->name_atom = fd->name_atom;
        func->is_strict = fd->is_strict;
        func->is_arrow = fd->is_arrow;
        func->is_generator = fd->is_generator;
        func->is_async = fd->is_async;
        func->source_line = fd->source_line;

        if (fd->bc) {
            func->bytecode = fd->bc->code;
            func->bytecode_len = fd->bc->length;
            func->constants = fd->bc->constants;
            func->const_count = fd->bc->const_count;

            /* Recursively build closures for nested functions */
            if (fd->bc->func_count > 0) {
                uint16_t nested_count;
                R8EValue *nested = build_function_closures(fd->bc,
                                                            &nested_count);
                if (nested) {
                    /* Expand the inner function's constants to include
                     * nested closures. The inner OP_NEW_FUNCTION indices
                     * are relative to the inner function's constants. */
                    uint16_t total = func->const_count + nested_count;
                    R8EValue *expanded = (R8EValue *)calloc(
                        total, sizeof(R8EValue));
                    if (expanded) {
                        if (func->constants && func->const_count > 0)
                            memcpy(expanded, func->constants,
                                   func->const_count * sizeof(R8EValue));
                        memcpy(expanded + func->const_count, nested,
                               nested_count * sizeof(R8EValue));
                        func->constants = expanded;
                        func->const_count = total;
                    }
                    free(nested);
                }
            }
        }

        /* Allocate closure wrapping the function */
        ApiClosureInterp *cl = (ApiClosureInterp *)calloc(
            1, sizeof(ApiClosureInterp));
        if (!cl) { closures[i] = R8E_UNDEFINED; continue; }

        cl->flags = (API_GC_KIND_CLOSURE << API_GC_KIND_SHIFT);
        cl->proto_id = 3; /* PROTO_FUNCTION */
        cl->func = func;
        cl->capture_count = 0;
        cl->capture_mode = 0; /* INLINE */

        closures[i] = r8e_from_pointer(cl);
    }

    return closures;
}

/**
 * Patch the compiled bytecode so that the last DROP before HALT is removed.
 *
 * The parser emits OP_DROP after every expression statement to pop the
 * value off the stack. For r8e_eval(), we want to keep the last
 * expression's value on the stack so OP_HALT can return it.
 *
 * We scan backwards from the HALT looking for the last DROP and NOP it out.
 */
static void patch_eval_bytecode(uint8_t *code, uint32_t length) {
    if (length < 2) return;

    /* Find the HALT (should be last byte) */
    if (code[length - 1] != R8E_OP_HALT) return;

    /* Walk backwards to find the last DROP before HALT */
    for (int32_t i = (int32_t)(length - 2); i >= 0; i--) {
        if (code[i] == R8E_OP_DROP) {
            code[i] = R8E_OP_NOP;
            return;
        }
        /* Stop if we hit another control flow instruction */
        if (code[i] == R8E_OP_HALT) return;
    }
}

R8EValue r8e_eval(R8EContext *ctx, const char *source, size_t len) {
    if (!ctx || !source) return R8E_UNDEFINED;

    if (len == 0) len = strlen(source);

    /* Ensure atom table is initialized */
    r8e_atom_table_init(NULL);

    /* Step 1: Compile source to bytecode */
    R8EBytecodeBuffer *bc = r8e_compile_script(
        NULL, source, (uint32_t)len, NULL);
    if (!bc) {
        ctx->error.has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Step 2: Patch bytecode - remove last DROP so HALT returns the value */
    patch_eval_bytecode(bc->code, bc->length);

    /* Step 3: Build function closures and combined constants array.
     *
     * The parser emits OP_NEW_FUNCTION <func_idx> where func_idx is a
     * 0-based index into bc->functions[]. The interpreter's OP_NEW_FUNCTION
     * reads constants[func_base + func_idx] where func_base is the offset
     * where function closures start in the combined constants array.
     *
     * Layout: [original_constants...] [function_closures...]
     *         ^index 0                ^func_base
     */
    R8EValue *eval_constants = bc->constants;
    uint16_t  eval_const_count = bc->const_count;
    uint16_t  func_base = bc->const_count;
    R8EValue *func_closures = NULL;
    R8EValue *combined_constants = NULL;

    if (bc->func_count > 0) {
        uint16_t func_count = 0;
        func_closures = build_function_closures(bc, &func_count);

        if (func_closures && func_count > 0) {
            uint16_t total = bc->const_count + func_count;
            combined_constants = (R8EValue *)calloc(total, sizeof(R8EValue));
            if (combined_constants) {
                if (bc->constants && bc->const_count > 0)
                    memcpy(combined_constants, bc->constants,
                           bc->const_count * sizeof(R8EValue));
                memcpy(combined_constants + bc->const_count, func_closures,
                       func_count * sizeof(R8EValue));
                eval_constants = combined_constants;
                eval_const_count = total;
            }
        }
    }

    /* Step 4: Create interpreter context and execute */
    R8EInterpContext *interp = r8e_interp_context_new();
    if (!interp) {
        free(combined_constants);
        free(func_closures);
        free(bc);
        return R8E_UNDEFINED;
    }

    /* Inject the persistent global object so globals survive across evals */
    if (R8E_IS_POINTER(ctx->global_obj)) {
        r8e_interp_set_global(interp, ctx->global_obj);
    }

    R8EValue result = r8e_interpret_ex(
        interp, bc->code, bc->length,
        eval_constants, eval_const_count, func_base);

    /* Step 5: Propagate exceptions */
    if (r8e_interp_has_exception(interp)) {
        ctx->error.has_exception = true;
        ctx->error.exception = r8e_interp_get_exception(interp);
    }

    /* Step 6: Clean up */
    r8e_interp_context_free(interp);
    free(combined_constants);
    free(func_closures);
    /* Note: we don't free bc->code or bc->constants because function
     * closures reference them. This is a deliberate leak until proper
     * GC integration is done. */
    free(bc);

    return result;
}

R8EValue r8e_eval_file(R8EContext *ctx, const char *source, size_t len,
                        const char *filename) {
    (void)filename;
    return r8e_eval(ctx, source, len);
}

R8EValue r8e_compile(R8EContext *ctx, const char *source, size_t len,
                      const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

/* =========================================================================
 * Module System APIs
 * ========================================================================= */

void r8e_set_module_loader(R8EContext *ctx, R8EModuleLoader loader) {
    if (!ctx) return;
    ctx->module_loader = (void *)loader;
}

R8EValue r8e_eval_module(R8EContext *ctx, const char *source, size_t len,
                          const char *filename) {
    if (!ctx || !source) return R8E_UNDEFINED;
    if (len == 0) len = strlen(source);
    if (!filename) filename = "<module>";

    /* Use the same compilation + execution path as r8e_eval,
     * but via the module compiler (r8e_compile_module).
     * Since r8e_compile_module may be a stub that returns R8E_UNDEFINED,
     * we handle that gracefully. */
    R8EValue compiled = r8e_compile_module(ctx, source, len, filename);
    if (R8E_IS_UNDEFINED(compiled)) {
        /* Module compilation not available or failed - fall back to
         * regular eval so caller at least gets some result */
        return r8e_eval(ctx, source, len);
    }

    /* Execute the compiled module function */
    return r8e_exec_function(ctx, compiled);
}

R8EValue r8e_call(R8EContext *ctx, R8EValue func, R8EValue this_val,
                   int argc, const R8EValue *argv) {
    if (!ctx) return R8E_UNDEFINED;

    /* The function value must be a pointer to a closure or native function */
    if (!R8E_IS_POINTER(func)) {
        ctx->error.has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Create a fresh interpreter context for the call */
    R8EInterpContext *interp = r8e_interp_context_new();
    if (!interp) {
        ctx->error.has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Inject the persistent global object so the callback can access globals */
    if (R8E_IS_POINTER(ctx->global_obj)) {
        r8e_interp_set_global(interp, ctx->global_obj);
    }

    /* Invoke the function through the interpreter's call mechanism */
    R8EValue result = r8e_call_function(interp, func, this_val, argv, argc);

    /* Propagate exceptions from the interpreter to the public context */
    if (r8e_interp_has_exception(interp)) {
        ctx->error.has_exception = true;
        ctx->error.exception = r8e_interp_get_exception(interp);
    }

    /* Clean up the interpreter context */
    r8e_interp_context_free(interp);

    return result;
}

/* =========================================================================
 * Global variable management
 *
 * Globals are stored as properties on ctx->global_obj, which persists
 * across r8e_eval() calls. r8e_eval() injects this object into each
 * interpreter context so JS code can access globals via OP_LOAD_GLOBAL
 * and OP_STORE_GLOBAL.
 * ========================================================================= */

R8EStatus r8e_set_global(R8EContext *ctx, const char *name, R8EValue val) {
    if (!ctx || !name) return R8E_ERROR;
    if (!R8E_IS_POINTER(ctx->global_obj)) return R8E_ERROR;
    return r8e_set_prop(ctx, ctx->global_obj, name, val);
}

R8EValue r8e_get_global(R8EContext *ctx, const char *name) {
    if (!ctx || !name) return R8E_UNDEFINED;
    if (!R8E_IS_POINTER(ctx->global_obj)) return R8E_UNDEFINED;
    return r8e_get_prop(ctx, ctx->global_obj, name);
}

R8EStatus r8e_set_global_func(R8EContext *ctx, const char *name,
                               R8ENativeFunc func, int argc) {
    if (!ctx || !name || !func) return R8E_ERROR;
    R8EValue fn = r8e_make_native_func(ctx, func, name, argc);
    if (R8E_IS_UNDEFINED(fn)) return R8E_ERROR_OOM;
    return r8e_set_global(ctx, name, fn);
}

/* =========================================================================
 * Value extraction: r8e_get_cstring
 * ========================================================================= */

const char *r8e_get_cstring(R8EValue v, char *buf, size_t *outlen) {
    /* Handle inline short strings */
    if (R8E_IS_INLINE_STR(v)) {
        int len = r8e_inline_str_decode(v, buf);
        if (outlen) *outlen = (size_t)len;
        return buf;
    }

    /* Handle heap strings (pointer to R8EString or R8EStringInterp) */
    if (R8E_IS_POINTER(v)) {
        void *ptr = r8e_get_pointer(v);
        if (ptr) {
            uint32_t flags = *(uint32_t *)ptr;
            uint8_t kind_interp = (flags >> 5) & 0x7;
            if (kind_interp == 1) {
                /* Interpreter-compatible string: data follows the struct
                 * (ApiStringInterp / R8EStringInterp layout with offset_table
                 * pointer). Cast to get byte_length field at offset 8. */
                uint32_t *words = (uint32_t *)ptr;
                uint32_t byte_len = words[2]; /* byte_length at offset 8 */
                /* Data follows the struct header (5 words + 1 pointer).
                 * struct size: flags(4) + hash(4) + byte_length(4) +
                 * char_length(4) + offset_table(8) = 24 bytes on 64-bit */
                const char *data = (const char *)ptr +
                    sizeof(uint32_t) * 4 + sizeof(void *);
                if (outlen) *outlen = byte_len;
                return data;
            }
            /* r8e_types.h uses R8E_OBJ_IS_STRING = 0x60 in bits [7:5] = 3 */
            if ((flags & 0xE0U) == 0x60U) {
                R8EString *s = (R8EString *)ptr;
                if (outlen) *outlen = s->byte_length;
                return s->data;
            }
        }
    }

    /* Not a string */
    if (buf) buf[0] = '\0';
    if (outlen) *outlen = 0;
    return buf ? buf : "";
}

/* =========================================================================
 * Other API functions
 * ========================================================================= */

bool r8e_is_string(R8EValue v) {
    if (R8E_IS_INLINE_STR(v)) return true;
    if (R8E_IS_POINTER(v)) {
        R8EString *s = (R8EString *)r8e_get_pointer(v);
        if (s) {
            uint8_t kind = (s->flags >> 5) & 0x7;
            return kind == 1;
        }
    }
    return false;
}

bool r8e_is_function(R8EValue v) {
    if (R8E_IS_POINTER(v)) {
        uint32_t *flags = (uint32_t *)r8e_get_pointer(v);
        if (flags) {
            uint8_t kind = (*flags >> 5) & 0x7;
            return kind == 3 /* closure */ || kind == 5 /* function */
                || kind == 6 /* native function */;
        }
    }
    return false;
}

bool r8e_is_array(R8EValue v) {
    if (R8E_IS_POINTER(v)) {
        uint32_t *flags = (uint32_t *)r8e_get_pointer(v);
        if (flags) {
            uint8_t kind = (*flags >> 5) & 0x7;
            return kind == 2; /* R8E_GC_KIND_ARRAY */
        }
    }
    return false;
}

bool r8e_to_bool(R8EValue v) {
    if (v == R8E_FALSE || v == R8E_NULL || v == R8E_UNDEFINED) return false;
    if (v == R8E_TRUE) return true;
    if (R8E_IS_INT32(v)) return r8e_get_int32(v) != 0;
    if (R8E_IS_DOUBLE(v)) {
        double d = r8e_get_double(v);
        return d != 0.0 && d == d; /* false for 0 and NaN */
    }
    if (R8E_IS_INLINE_STR(v)) return r8e_inline_str_len(v) > 0;
    if (R8E_IS_POINTER(v)) return true;
    return false;
}

R8EValue r8e_make_number(double d) {
    if (d == (double)(int32_t)d && (d != 0.0 || (1.0 / d > 0))) {
        return r8e_from_int32((int32_t)d);
    }
    return r8e_from_double(d);
}

/* =========================================================================
 * Object creation and property access
 *
 * These work with the interpreter's object layout (GC header with kind in
 * bits [7:5], tier in bits [1:0]).  Keys are atom-tagged NaN-boxed values:
 *   0xFFFC000000000000 | atom_id
 *
 * We create Tier 1 objects (up to 4 properties) matching OP_NEW_OBJECT.
 * ========================================================================= */

/* GC kind for objects (bits [7:5] = 0) */
#define API_OBJ_GC_KIND_OBJECT  0u
#define API_OBJ_GC_KIND_SHIFT   5

/* Atom-tagged key encoding used by the interpreter */
#define API_ATOM_TAG  0xFFFC000000000000ULL

/* Interpreter-compatible object tier structs (must match r8e_interp.c) */
typedef struct { R8EValue key; R8EValue val; } ApiPropPair;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} ApiObjTier0;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t   count;
    uint8_t   pad[7];
    ApiPropPair props[4];
} ApiObjTier1;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t   count;
    uint8_t   capacity;
    uint8_t   pad[6];
    ApiPropPair *props;
} ApiObjTier2;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint16_t  count;
    uint16_t  capacity;
    uint32_t  pad;
    ApiPropPair *buckets;
} ApiObjTier3;

/* GC kind for accessor descriptors (bits [7:5] = 7) */
#define API_GC_KIND_ACCESSOR    7u

/* Accessor property descriptor: holds getter and setter function values */
typedef struct {
    uint32_t  flags;       /* GC header: kind = API_GC_KIND_ACCESSOR */
    uint32_t  reserved;
    R8EValue  getter;      /* getter function value, or R8E_UNDEFINED */
    R8EValue  setter;      /* setter function value, or R8E_UNDEFINED */
} ApiAccessorDesc;

/* Check if a value is an accessor descriptor */
static inline bool api_is_accessor(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    uint32_t *flags = (uint32_t *)(void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
    if (!flags) return false;
    uint8_t kind = (*flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    return kind == API_GC_KIND_ACCESSOR;
}

/* Forward declaration for native function struct (defined later in this file) */
typedef struct ApiNativeFuncInterp ApiNativeFuncInterp;

/* Invoke a native function callback directly (avoids creating interp context) */
static R8EValue api_call_native(R8EContext *ctx, R8EValue func, R8EValue this_val,
                                 int argc, const R8EValue *argv);


/* Extern: atom interning */
extern uint32_t r8e_atom_intern_cstr(void *ctx, const char *cstr);

R8EValue r8e_make_object(R8EContext *ctx) {
    (void)ctx;
    /* Allocate a Tier 1 object matching OP_NEW_OBJECT in r8e_interp.c */
    ApiObjTier1 *t1 = (ApiObjTier1 *)calloc(1, sizeof(ApiObjTier1));
    if (!t1) return R8E_UNDEFINED;

    t1->flags = (API_OBJ_GC_KIND_OBJECT << API_OBJ_GC_KIND_SHIFT) | 1u; /* tier 1 */
    t1->proto_id = 1; /* PROTO_OBJECT */
    t1->count = 0;

    return r8e_from_pointer(t1);
}

R8EValue r8e_get_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    (void)ctx;
    if (!name || !R8E_IS_POINTER(obj)) return R8E_UNDEFINED;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return R8E_UNDEFINED;

    return r8e_get_prop_atom(ctx, obj, atom);
}

R8EValue r8e_get_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom) {
    if (!R8E_IS_POINTER(obj)) return R8E_UNDEFINED;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return R8E_UNDEFINED;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return R8E_UNDEFINED;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;
    R8EValue raw = R8E_UNDEFINED;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        if (t0->key0 == key) raw = t0->val0;
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) { raw = t1->props[i].val; break; }
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) { raw = t2->props[i].val; break; }
            }
        }
    } else if (tier == 3) {
        ApiObjTier3 *t3 = (ApiObjTier3 *)ptr;
        if (t3->buckets && t3->capacity > 0) {
            uint32_t mask = t3->capacity - 1;
            uint32_t idx = atom & mask;
            for (uint16_t probe = 0; probe < t3->capacity; probe++) {
                R8EValue bkey = t3->buckets[idx].key;
                if (bkey == key) { raw = t3->buckets[idx].val; break; }
                if (bkey == 0) break; /* empty slot */
                idx = (idx + 1) & mask;
            }
        }
    }

    /* If the value is an accessor descriptor, invoke the getter */
    if (api_is_accessor(raw)) {
        ApiAccessorDesc *acc = (ApiAccessorDesc *)(void *)(uintptr_t)(raw & 0x0000FFFFFFFFFFFFULL);
        if (acc && !R8E_IS_UNDEFINED(acc->getter)) {
            return api_call_native(ctx, acc->getter, obj, 0, NULL);
        }
        return R8E_UNDEFINED; /* no getter defined */
    }

    return raw;
}

R8EStatus r8e_set_prop(R8EContext *ctx, R8EValue obj, const char *name,
                       R8EValue val) {
    if (!name || !R8E_IS_POINTER(obj)) return R8E_ERROR;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return R8E_ERROR;

    return r8e_set_prop_atom(ctx, obj, atom, val);
}

R8EStatus r8e_set_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom,
                            R8EValue val) {
    if (!R8E_IS_POINTER(obj)) return R8E_ERROR;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return R8E_ERROR;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return R8E_ERROR;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    /* Helper macro: check if existing value is an accessor and invoke setter */
    #define CHECK_ACCESSOR_SET(existing_val) do {                          \
        if (api_is_accessor(existing_val)) {                               \
            ApiAccessorDesc *acc = (ApiAccessorDesc *)(void *)(uintptr_t)  \
                ((existing_val) & 0x0000FFFFFFFFFFFFULL);                  \
            if (acc && !R8E_IS_UNDEFINED(acc->setter)) {                   \
                api_call_native(ctx, acc->setter, obj, 1, &val);           \
                return R8E_OK;                                             \
            }                                                              \
            return R8E_ERROR; /* no setter defined */                       \
        }                                                                  \
    } while (0)

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        if (t0->key0 == key) {
            CHECK_ACCESSOR_SET(t0->val0);
            t0->val0 = val;
            return R8E_OK;
        }
        if (t0->key0 == 0) {
            t0->key0 = key;
            t0->val0 = val;
            return R8E_OK;
        }
        return R8E_ERROR; /* tier 0 full, no promotion from API */
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        /* Check for existing key */
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) {
                CHECK_ACCESSOR_SET(t1->props[i].val);
                t1->props[i].val = val;
                return R8E_OK;
            }
        }
        /* Add new property */
        if (t1->count < 4) {
            t1->props[t1->count].key = key;
            t1->props[t1->count].val = val;
            t1->count++;
            return R8E_OK;
        }
        /* Tier 1 full - promote to tier 2 in-place.
         * ApiObjTier2 is: flags(4) + proto_id(4) + count(1) + capacity(1) + pad(6) + props_ptr(8) = 24 bytes
         * ApiObjTier1 is: flags(4) + proto_id(4) + count(1) + pad(7) + props[4](64) = 80 bytes
         * Since tier1 is larger, we can reinterpret in-place */
        {
            uint8_t old_count = t1->count;  /* 4 */
            uint8_t new_cap = 16;
            ApiPropPair *new_props = (ApiPropPair *)calloc(new_cap, sizeof(ApiPropPair));
            if (!new_props) return R8E_ERROR;
            /* Copy existing props */
            for (uint8_t i = 0; i < old_count; i++) {
                new_props[i] = t1->props[i];
            }
            /* Add the new prop */
            new_props[old_count].key = key;
            new_props[old_count].val = val;
            /* Reinterpret as tier 2 */
            ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
            t2->flags = (API_OBJ_GC_KIND_OBJECT << API_OBJ_GC_KIND_SHIFT) | 2u;
            t2->count = old_count + 1;
            t2->capacity = new_cap;
            t2->props = new_props;
            return R8E_OK;
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) {
                    CHECK_ACCESSOR_SET(t2->props[i].val);
                    t2->props[i].val = val;
                    return R8E_OK;
                }
            }
            if (t2->count < t2->capacity) {
                t2->props[t2->count].key = key;
                t2->props[t2->count].val = val;
                t2->count++;
                return R8E_OK;
            }
            /* Grow the props array */
            uint8_t new_cap = t2->capacity * 2;
            if (new_cap < t2->capacity) new_cap = 255;
            ApiPropPair *new_props = (ApiPropPair *)realloc(
                t2->props, new_cap * sizeof(ApiPropPair));
            if (!new_props) return R8E_ERROR_OOM;
            t2->props = new_props;
            t2->capacity = new_cap;
            t2->props[t2->count].key = key;
            t2->props[t2->count].val = val;
            t2->count++;
            return R8E_OK;
        }
        return R8E_ERROR;
    }

    #undef CHECK_ACCESSOR_SET

    return R8E_ERROR;
}

bool r8e_has_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    if (!name || !R8E_IS_POINTER(obj)) return false;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return false;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return false;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return false;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    (void)ctx;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        return t0->key0 == key;
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) return true;
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) return true;
            }
        }
    }

    return false;
}

bool r8e_delete_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    if (!name || !R8E_IS_POINTER(obj)) return false;

    uint32_t atom = r8e_atom_intern_cstr(NULL, name);
    if (atom == 0) return false;

    void *ptr = r8e_get_pointer(obj);
    if (!ptr) return false;

    uint32_t flags = *(uint32_t *)ptr;
    uint32_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != API_OBJ_GC_KIND_OBJECT) return false;

    uint8_t tier = flags & 0x03;
    R8EValue key = API_ATOM_TAG | (uint64_t)atom;

    (void)ctx;

    if (tier == 0) {
        ApiObjTier0 *t0 = (ApiObjTier0 *)ptr;
        if (t0->key0 == key) {
            t0->key0 = 0;
            t0->val0 = R8E_UNDEFINED;
            return true;
        }
    } else if (tier == 1) {
        ApiObjTier1 *t1 = (ApiObjTier1 *)ptr;
        for (uint8_t i = 0; i < t1->count; i++) {
            if (t1->props[i].key == key) {
                /* Shift remaining props down */
                for (uint8_t j = i; j < t1->count - 1; j++) {
                    t1->props[j] = t1->props[j + 1];
                }
                t1->count--;
                t1->props[t1->count].key = 0;
                t1->props[t1->count].val = R8E_UNDEFINED;
                return true;
            }
        }
    } else if (tier == 2) {
        ApiObjTier2 *t2 = (ApiObjTier2 *)ptr;
        if (t2->props) {
            for (uint8_t i = 0; i < t2->count; i++) {
                if (t2->props[i].key == key) {
                    for (uint8_t j = i; j < t2->count - 1; j++) {
                        t2->props[j] = t2->props[j + 1];
                    }
                    t2->count--;
                    return true;
                }
            }
        }
    }

    return false;
}

/* =========================================================================
 * String creation: r8e_make_string, r8e_make_cstring
 *
 * Short ASCII strings (<=6 chars) are stored inline in the NaN-box.
 * Longer strings are heap-allocated with GC kind STRING (bits [7:5] = 1).
 * ========================================================================= */

/* Interpreter-compatible string struct (must match r8e_interp.c R8EStringInterp) */
typedef struct {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    void    *offset_table;
    /* char data[] follows via (struct + 1) */
} ApiStringInterp;

R8EValue r8e_make_string(R8EContext *ctx, const char *str, size_t len) {
    (void)ctx;
    if (!str) return R8E_UNDEFINED;
    if (len == 0) len = strlen(str);

    /* Try inline encoding for short ASCII strings */
    if (len <= 6) {
        R8EValue v = r8e_from_inline_str(str, (int)len);
        if (v != R8E_UNDEFINED) return v;
    }

    /* Heap-allocate interpreter-compatible string */
    size_t alloc_sz = sizeof(ApiStringInterp) + len + 1;
    ApiStringInterp *s = (ApiStringInterp *)calloc(1, alloc_sz);
    if (!s) return R8E_UNDEFINED;

    s->flags = (1u << API_OBJ_GC_KIND_SHIFT) | 0x01u; /* STRING kind + ASCII flag */
    s->hash = 0;
    s->byte_length = (uint32_t)len;
    s->char_length = (uint32_t)len;
    s->offset_table = NULL;
    char *dst = (char *)(s + 1);
    memcpy(dst, str, len);
    dst[len] = '\0';

    return r8e_from_pointer(s);
}

R8EValue r8e_make_cstring(R8EContext *ctx, const char *str) {
    if (!str) return R8E_UNDEFINED;
    return r8e_make_string(ctx, str, strlen(str));
}

/* =========================================================================
 * Array creation: r8e_make_array
 *
 * Creates interpreter-compatible array with GC kind ARRAY (bits [7:5] = 2).
 * ========================================================================= */

/* Interpreter-compatible array struct (must match r8e_interp.c R8EArrayInterp) */
typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *elements;
    void     *named;
} ApiArrayInterp;

R8EValue r8e_make_array(R8EContext *ctx, uint32_t capacity) {
    (void)ctx;
    ApiArrayInterp *arr = (ApiArrayInterp *)calloc(1, sizeof(ApiArrayInterp));
    if (!arr) return R8E_UNDEFINED;

    arr->flags = (2u << API_OBJ_GC_KIND_SHIFT); /* ARRAY kind */
    arr->proto_id = 2; /* PROTO_ARRAY */
    arr->length = 0;
    arr->named = NULL;

    uint32_t cap = capacity > 0 ? capacity : 4;
    arr->elements = (R8EValue *)calloc(cap, sizeof(R8EValue));
    if (!arr->elements) {
        free(arr);
        return R8E_UNDEFINED;
    }
    arr->capacity = cap;

    /* Initialize elements to undefined */
    for (uint32_t i = 0; i < cap; i++)
        arr->elements[i] = R8E_UNDEFINED;

    return r8e_from_pointer(arr);
}

/* =========================================================================
 * Array element access: r8e_get_element, r8e_set_element, r8e_get_length
 * ========================================================================= */

R8EValue r8e_get_element(R8EContext *ctx, R8EValue arr, uint32_t index) {
    (void)ctx;
    if (!R8E_IS_POINTER(arr)) return R8E_UNDEFINED;

    ApiArrayInterp *a = (ApiArrayInterp *)r8e_get_pointer(arr);
    if (!a) return R8E_UNDEFINED;

    uint8_t kind = (a->flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != 2) return R8E_UNDEFINED; /* not an array */

    if (index >= a->length) return R8E_UNDEFINED;
    return a->elements[index];
}

R8EStatus r8e_set_element(R8EContext *ctx, R8EValue arr, uint32_t index,
                           R8EValue val) {
    (void)ctx;
    if (!R8E_IS_POINTER(arr)) return R8E_ERROR;

    ApiArrayInterp *a = (ApiArrayInterp *)r8e_get_pointer(arr);
    if (!a) return R8E_ERROR;

    uint8_t kind = (a->flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    if (kind != 2) return R8E_ERROR; /* not an array */

    /* Grow if needed */
    if (index >= a->capacity) {
        uint32_t new_cap = a->capacity;
        while (new_cap <= index) {
            uint32_t next = new_cap * 2;
            if (next < new_cap) { new_cap = index + 1; break; }
            new_cap = next;
        }
        R8EValue *new_elems = (R8EValue *)realloc(
            a->elements, new_cap * sizeof(R8EValue));
        if (!new_elems) return R8E_ERROR_OOM;
        for (uint32_t i = a->capacity; i < new_cap; i++)
            new_elems[i] = R8E_UNDEFINED;
        a->elements = new_elems;
        a->capacity = new_cap;
    }

    a->elements[index] = val;
    if (index >= a->length)
        a->length = index + 1;

    return R8E_OK;
}

int32_t r8e_get_length(R8EContext *ctx, R8EValue v) {
    (void)ctx;

    /* Inline strings */
    if (R8E_IS_INLINE_STR(v))
        return (int32_t)r8e_inline_str_len(v);

    if (!R8E_IS_POINTER(v)) return -1;

    void *ptr = r8e_get_pointer(v);
    if (!ptr) return -1;

    uint32_t flags = *(uint32_t *)ptr;
    uint8_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;

    if (kind == 2) { /* ARRAY */
        ApiArrayInterp *a = (ApiArrayInterp *)ptr;
        return (int32_t)a->length;
    }
    if (kind == 1) { /* STRING */
        ApiStringInterp *s = (ApiStringInterp *)ptr;
        return (int32_t)s->char_length;
    }

    return -1;
}

/* =========================================================================
 * Native function creation: r8e_make_native_func
 *
 * Creates an R8ENativeFunction with GC kind NATIVE_FUNC (bits [7:5] = 6).
 * ========================================================================= */

/* Interpreter-compatible native function struct */
struct ApiNativeFuncInterp {
    uint32_t      flags;
    uint32_t      proto_id;
    R8ENativeFunc callback;
    uint32_t      name_atom;
    int16_t       arity;
    uint8_t       func_flags;
    uint8_t       reserved;
};

R8EValue r8e_make_native_func(R8EContext *ctx, R8ENativeFunc func,
                               const char *name, int argc) {
    (void)ctx;
    if (!func) return R8E_UNDEFINED;

    ApiNativeFuncInterp *nf = (ApiNativeFuncInterp *)calloc(
        1, sizeof(ApiNativeFuncInterp));
    if (!nf) return R8E_UNDEFINED;

    nf->flags = (5u << API_OBJ_GC_KIND_SHIFT); /* R8E_GC_KIND_FUNCTION (native) */
    nf->proto_id = 3; /* PROTO_FUNCTION */
    nf->callback = func;
    nf->name_atom = name ? r8e_atom_intern_cstr(NULL, name) : 0;
    nf->arity = (int16_t)argc;
    nf->func_flags = 0;
    nf->reserved = 0;

    return r8e_from_pointer(nf);
}


/* =========================================================================
 * Accessor property support: api_call_native + r8e_define_accessor
 * ========================================================================= */

/* Invoke a native function callback directly */
static R8EValue api_call_native(R8EContext *ctx, R8EValue func, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    if (!R8E_IS_POINTER(func)) return R8E_UNDEFINED;
    void *ptr = (void *)(uintptr_t)(func & 0x0000FFFFFFFFFFFFULL);
    if (!ptr) return R8E_UNDEFINED;
    uint32_t flags = *(uint32_t *)ptr;
    uint8_t kind = (flags >> API_OBJ_GC_KIND_SHIFT) & 0x7;
    /* kind 5 = native function (from r8e_make_native_func) */
    if (kind == 5) {
        struct ApiNativeFuncInterp *nf = (struct ApiNativeFuncInterp *)ptr;
        if (nf->callback)
            return nf->callback(ctx, this_val, argc, argv);
    }
    /* For closures/JS functions, fall back to r8e_call */
    return r8e_call(ctx, func, this_val, argc, argv);
}

R8EStatus r8e_define_accessor(R8EContext *ctx, R8EValue obj, const char *name,
                                R8EValue getter, R8EValue setter) {
    if (!ctx || !name || !R8E_IS_POINTER(obj)) return R8E_ERROR;

    /* Allocate an accessor descriptor */
    ApiAccessorDesc *desc = (ApiAccessorDesc *)calloc(1, sizeof(ApiAccessorDesc));
    if (!desc) return R8E_ERROR_OOM;

    desc->flags = (API_GC_KIND_ACCESSOR << API_OBJ_GC_KIND_SHIFT);
    desc->getter = getter;
    desc->setter = setter;

    /* Store the accessor descriptor as the property value */
    R8EValue desc_val = r8e_from_pointer(desc);
    return r8e_set_prop(ctx, obj, name, desc_val);
}


/* =========================================================================
 * GC API wrappers
 * ========================================================================= */

/* Inline refcount is stored in bits [31:16] of the flags word (first uint32_t
 * of every heap-allocated object). */
#define API_GC_RC_INLINE_MASK   0xFFFF0000u
#define API_GC_RC_INLINE_SHIFT  16

/* r8e_gc_collect is defined in r8e_gc.c — do not redefine here */

void r8e_gc_stats(const R8EContext *ctx, R8EMemStats *stats) {
    if (!ctx || !stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->total_allocated = ctx->total_allocated;
    stats->total_freed = ctx->total_freed;
    stats->current_usage = ctx->total_allocated - ctx->total_freed;
    stats->epoch_threshold = ctx->epoch_threshold;
}

void r8e_value_retain(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return;
    uint32_t *flags = (uint32_t *)(void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
    if (!flags) return;
    uint32_t rc = (*flags & API_GC_RC_INLINE_MASK) >> API_GC_RC_INLINE_SHIFT;
    if (rc < 0xFFFF) {
        rc++;
        *flags = (*flags & ~API_GC_RC_INLINE_MASK)
               | (rc << API_GC_RC_INLINE_SHIFT);
    }
}

void r8e_value_release(R8EContext *ctx, R8EValue v) {
    (void)ctx;
    if (!R8E_IS_POINTER(v)) return;
    uint32_t *flags = (uint32_t *)(void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
    if (!flags) return;
    uint32_t rc = (*flags & API_GC_RC_INLINE_MASK) >> API_GC_RC_INLINE_SHIFT;
    if (rc > 0) {
        rc--;
        *flags = (*flags & ~API_GC_RC_INLINE_MASK)
               | (rc << API_GC_RC_INLINE_SHIFT);
    }
    /* Note: full release (freeing children + the object) is not done here
     * since the API layer objects are managed by the interpreter's GC.
     * This just decrements the refcount. */
}


/* Error throwing, clear_exception, gc_collect, and realm management
 * functions are provided by r8e_error.c, r8e_gc.c, and r8e_realm.c
 * respectively. They use their own internal R8EContext layouts.
 * See r8e_test_stubs.c for weak fallback stubs used in test builds. */
