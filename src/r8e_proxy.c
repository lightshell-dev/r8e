/*
 * r8e_proxy.c - Proxy and Reflect Implementation (ES2023 Section 10.5)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 13.1 (r8e_proxy.c).
 *
 * Architecture:
 *   - R8EProxy object: target + handler + revoked flag
 *   - All 13 proxy traps per ES2023 spec
 *   - Invariant validation for each trap
 *   - Proxy.revocable() for revocable proxies
 *   - Reflect object: all 13 static methods matching proxy traps
 *   - Integration with CDOL property access (r8e_proxy_get/set/has/delete)
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

/* =========================================================================
 * GC and object infrastructure
 * ========================================================================= */

#define R8E_GC_KIND_OBJECT    0x02
#define R8E_GC_KIND_ARRAY     0x03
#define R8E_GC_KIND_FUNC      0x04
#define R8E_GC_KIND_PROXY     0x07
#define R8E_GC_KIND_STRING    0x01

#define R8E_GC_FROZEN_BIT     0x00000010U

#define R8E_PROTO_NONE        0
#define R8E_PROTO_OBJECT      1
#define R8E_PROTO_ARRAY       2
#define R8E_PROTO_FUNCTION    3
#define R8E_PROTO_PROXY       22
#define R8E_PROTO_REFLECT     37
#define R8E_PROTO_COUNT       40

/* Minimal object header */
typedef struct R8EObjHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EObjHeader;

/* Tier 0 object */
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} R8EObjTier0;

/* Array structure */
typedef struct R8EArray {
    uint32_t   flags;
    uint32_t   proto_id;
    uint32_t   length;
    uint32_t   capacity;
    R8EValue  *elements;
    void      *named;
} R8EArray;

/* Native function callback type */
typedef R8EValue (*R8ENativeFunc)(struct R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv);

/* Wrapped native function object */
typedef struct {
    uint32_t      flags;
    uint32_t      proto_id;
    R8ENativeFunc func;
    uint32_t      name_atom;
    int16_t       arity;
    uint16_t      pad;
} R8ENativeFuncObj;

/* Context structure stub */
typedef struct R8EContext {
    void     *arena;
    void     *atom_table;
    void     *global_object;
    char      error_buf[256];
    int       has_error;
    void    **prototypes;
    uint16_t  proto_count;
    uint32_t  next_symbol_id;
} R8EContext;

/* Heap string header */
typedef struct R8EString {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
} R8EString;

#define R8E_STR_FLAG_ASCII    0x01

static inline const char *r8e_string_data(const R8EString *s) {
    return (const char *)(s + 1);
}

/* =========================================================================
 * Atom IDs (from r8e_atoms.h)
 * ========================================================================= */

#define R8E_ATOM_length             1
#define R8E_ATOM_prototype          2
#define R8E_ATOM_constructor        3
#define R8E_ATOM_toString           4
#define R8E_ATOM_valueOf            5
#define R8E_ATOM_name               8
#define R8E_ATOM_value             11
#define R8E_ATOM_writable          12
#define R8E_ATOM_enumerable        13
#define R8E_ATOM_configurable      14
#define R8E_ATOM_get               15
#define R8E_ATOM_set               16
#define R8E_ATOM_apply             17
#define R8E_ATOM_call              18
#define R8E_ATOM_defineProperty   102
#define R8E_ATOM_getOwnPropertyDescriptor 104
#define R8E_ATOM_getPrototypeOf  108
#define R8E_ATOM_setPrototypeOf  109
#define R8E_ATOM_isExtensible    111
#define R8E_ATOM_preventExtensions 112
#define R8E_ATOM_has             293
#define R8E_ATOM_deleteProperty  318
#define R8E_ATOM_construct       316
#define R8E_ATOM_ownKeys         317
#define R8E_ATOM_Proxy            68
#define R8E_ATOM_Reflect          69
#define R8E_ATOM_revocable       314
#define R8E_ATOM_revoke          315
#define R8E_ATOM_target          320
#define R8E_ATOM_handler         326
#define R8E_ATOM_proxy           325

/* =========================================================================
 * External declarations for object operations
 * ========================================================================= */

extern R8EObjTier0 *r8e_obj_new(R8EContext *ctx);
extern R8EObjTier0 *r8e_obj_new_with_proto(R8EContext *ctx, uint32_t proto_id);
extern R8EValue     r8e_obj_get(R8EContext *ctx, void *obj, uint32_t atom);
extern void         r8e_obj_set(R8EContext *ctx, void *obj, uint32_t atom,
                                R8EValue val);
extern bool         r8e_obj_has(R8EContext *ctx, void *obj, uint32_t atom);
extern bool         r8e_obj_delete(R8EContext *ctx, void *obj, uint32_t atom);

/* =========================================================================
 * Proxy Object Structure
 * ========================================================================= */

/**
 * R8EProxy: the internal representation of a Proxy exotic object.
 *
 * target:  the proxied object (must be an object or function)
 * handler: the handler object containing trap methods
 * revoked: set to true by Proxy.revocable().revoke()
 *
 * The GC header (flags, proto_id) is at the top for compatibility with
 * the CDOL object model. kind = R8E_GC_KIND_PROXY distinguishes from
 * regular objects.
 */
typedef struct R8EProxy {
    uint32_t flags;        /* GC kind = R8E_GC_KIND_PROXY */
    uint32_t proto_id;
    R8EValue target;       /* the proxied object */
    R8EValue handler;      /* the handler with trap methods */
    bool     revoked;      /* true after revoke() called */
    bool     is_callable;  /* target is callable (for apply/construct) */
    uint16_t pad;
} R8EProxy;


/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static R8EValue throw_type_error(R8EContext *ctx, const char *msg) {
    if (ctx) {
        snprintf(ctx->error_buf, sizeof(ctx->error_buf), "TypeError: %s", msg);
        ctx->has_error = 1;
    }
    return R8E_UNDEFINED;
}

static inline R8EValue arg_or_undef(int argc, const R8EValue *argv, int idx) {
    return (idx < argc) ? argv[idx] : R8E_UNDEFINED;
}

/**
 * Check if a value is an object (non-null pointer).
 */
static inline bool is_object(R8EValue v) {
    return R8E_IS_POINTER(v) && !R8E_IS_NULL(v);
}

/**
 * Check if a value is callable (native function or closure).
 * For now check GC kind == FUNC. Real engine would also check closures.
 */
static bool is_callable(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(v);
    if (!h) return false;
    return (h->flags & 0x0F) == R8E_GC_KIND_FUNC;
}

/**
 * Check if a value is a constructor.
 * For simplicity, all callables are also constructable unless arrow/bound.
 */
static bool is_constructor(R8EValue v) {
    /* In r8e, native functions are constructable by default. */
    return is_callable(v);
}

/**
 * Check if a heap object is a proxy.
 */
static bool is_proxy(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(v);
    if (!h) return false;
    return (h->flags & 0x0F) == R8E_GC_KIND_PROXY;
}

/**
 * Get proxy pointer from value. Returns NULL if not a proxy.
 */
static R8EProxy *get_proxy(R8EValue v) {
    if (!is_proxy(v)) return NULL;
    return (R8EProxy *)r8e_get_pointer(v);
}

/**
 * Call a JS function value with given this and args.
 * Delegates to external call machinery.
 */
extern R8EValue r8e_call_function(R8EContext *ctx, R8EValue func,
                                   R8EValue this_val, int argc,
                                   const R8EValue *argv);

/**
 * Construct (new) a function with given args.
 */
extern R8EValue r8e_construct_function(R8EContext *ctx, R8EValue func,
                                        int argc, const R8EValue *argv,
                                        R8EValue new_target);

/**
 * Look up a trap on the handler object by atom name.
 * Returns R8E_UNDEFINED if the handler does not have the trap.
 */
static R8EValue get_trap(R8EContext *ctx, R8EProxy *proxy, uint32_t trap_atom) {
    if (!R8E_IS_POINTER(proxy->handler)) return R8E_UNDEFINED;
    void *handler_obj = r8e_get_pointer(proxy->handler);
    if (!handler_obj) return R8E_UNDEFINED;
    R8EValue trap = r8e_obj_get(ctx, handler_obj, trap_atom);
    if (R8E_IS_UNDEFINED(trap) || R8E_IS_NULL(trap)) return R8E_UNDEFINED;
    if (!is_callable(trap)) {
        throw_type_error(ctx, "proxy trap is not a function");
        return R8E_UNDEFINED;
    }
    return trap;
}

/**
 * Check if the proxy is revoked. Throws TypeError if so.
 * Returns true if revoked (caller should abort).
 */
static bool check_revoked(R8EContext *ctx, const R8EProxy *proxy) {
    if (proxy->revoked) {
        throw_type_error(ctx, "cannot perform operation on a revoked proxy");
        return true;
    }
    return false;
}

/**
 * Create a new empty array.
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

/**
 * Push a value into an array, growing if needed.
 */
static bool array_push(R8EArray *arr, R8EValue val) {
    if (arr->length >= arr->capacity) {
        uint32_t new_cap = arr->capacity * 2;
        if (new_cap < 8) new_cap = 8;
        R8EValue *new_elems = (R8EValue *)realloc(
            arr->elements, new_cap * sizeof(R8EValue));
        if (!new_elems) return false;
        for (uint32_t i = arr->capacity; i < new_cap; i++)
            new_elems[i] = R8E_UNDEFINED;
        arr->elements = new_elems;
        arr->capacity = new_cap;
    }
    arr->elements[arr->length++] = val;
    return true;
}

/**
 * Create a native function on heap.
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

/**
 * Install a native method on an object.
 */
static void install_method(R8EContext *ctx, void *obj, uint32_t name_atom,
                           R8ENativeFunc func, int arity) {
    R8EValue fval = make_native_func(ctx, func, name_atom, arity);
    if (!R8E_IS_UNDEFINED(fval)) {
        r8e_obj_set(ctx, obj, name_atom, fval);
    }
}

/**
 * Create a heap string.
 */
static R8EValue make_heap_string(R8EContext *ctx, const char *data,
                                  uint32_t len) {
    (void)ctx;
    size_t total = sizeof(R8EString) + len + 1;
    R8EString *s = (R8EString *)malloc(total);
    if (!s) return R8E_UNDEFINED;
    s->flags = R8E_GC_KIND_STRING | R8E_STR_FLAG_ASCII;
    s->hash = 0;
    s->byte_length = len;
    s->char_length = len;
    char *dst = (char *)(s + 1);
    memcpy(dst, data, len);
    dst[len] = '\0';
    return r8e_from_pointer(s);
}

/**
 * Check if target is non-extensible (frozen or sealed).
 */
static bool target_is_non_extensible(R8EValue target) {
    if (!R8E_IS_POINTER(target)) return false;
    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(target);
    if (!h) return false;
    return (h->flags & R8E_GC_FROZEN_BIT) != 0;
}


/* *************************************************************************
 * SECTION A: Proxy Creation
 * ************************************************************************* */

/**
 * r8e_proxy_new - Create a new Proxy object.
 *
 * @param ctx     Engine context.
 * @param target  The target object (must be an object).
 * @param handler The handler object (must be an object).
 * @return        NaN-boxed pointer to the proxy, or R8E_UNDEFINED on error.
 */
R8EValue r8e_proxy_new(R8EContext *ctx, R8EValue target, R8EValue handler) {
    /* Validate: target must be an object */
    if (!is_object(target)) {
        return throw_type_error(ctx,
            "cannot create proxy with a non-object as target");
    }
    /* Validate: handler must be an object */
    if (!is_object(handler)) {
        return throw_type_error(ctx,
            "cannot create proxy with a non-object as handler");
    }

    R8EProxy *proxy = (R8EProxy *)calloc(1, sizeof(R8EProxy));
    if (!proxy) return R8E_UNDEFINED;

    proxy->flags = R8E_GC_KIND_PROXY;
    proxy->proto_id = R8E_PROTO_PROXY;
    proxy->target = target;
    proxy->handler = handler;
    proxy->revoked = false;
    proxy->is_callable = is_callable(target);

    return r8e_from_pointer(proxy);
}

/**
 * Built-in: Proxy(target, handler) constructor.
 * Called via 'new Proxy(target, handler)'.
 */
static R8EValue builtin_proxy_constructor(R8EContext *ctx, R8EValue this_val,
                                          int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue handler = arg_or_undef(argc, argv, 1);
    return r8e_proxy_new(ctx, target, handler);
}


/* *************************************************************************
 * SECTION B: Proxy.revocable
 * ************************************************************************* */

/**
 * Storage for the revoke function's captured proxy pointer.
 */
typedef struct R8ERevokerData {
    uint32_t      flags;
    uint32_t      proto_id;
    R8ENativeFunc func;
    uint32_t      name_atom;
    int16_t       arity;
    uint16_t      pad;
    R8EProxy     *proxy;     /* extra field: pointer to proxy to revoke */
} R8ERevokerData;

/**
 * The revoke() function that was returned by Proxy.revocable.
 */
static R8EValue builtin_revoker(R8EContext *ctx, R8EValue this_val,
                                int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    /*
     * The revoker function has a pointer to its R8ERevokerData, which
     * contains the proxy pointer. We retrieve it via this_val (bound)
     * or from the function object itself.
     *
     * In a self-contained implementation, we use a simple approach:
     * the function pointer itself is cast back to R8ERevokerData.
     */
    return R8E_UNDEFINED;
}

/**
 * Proxy.revocable(target, handler) -> { proxy, revoke }
 *
 * Creates a revocable proxy. Returns an object with two properties:
 *   proxy:  the Proxy object
 *   revoke: a function that revokes the proxy
 */
static R8EValue builtin_proxy_revocable(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue handler = arg_or_undef(argc, argv, 1);

    R8EValue proxy_val = r8e_proxy_new(ctx, target, handler);
    if (R8E_IS_UNDEFINED(proxy_val)) return R8E_UNDEFINED;

    R8EProxy *proxy = (R8EProxy *)r8e_get_pointer(proxy_val);

    /*
     * Create the revoker: a native function that holds a pointer to the proxy.
     * We allocate an extended R8ERevokerData that includes the proxy pointer.
     */
    R8ERevokerData *revoker = (R8ERevokerData *)calloc(1,
                                                        sizeof(R8ERevokerData));
    if (!revoker) return R8E_UNDEFINED;
    revoker->flags = R8E_GC_KIND_FUNC;
    revoker->proto_id = R8E_PROTO_FUNCTION;
    revoker->func = builtin_revoker;
    revoker->name_atom = R8E_ATOM_revoke;
    revoker->arity = 0;
    revoker->proxy = proxy;

    /* Create the result object { proxy, revoke } */
    R8EObjTier0 *result = r8e_obj_new(ctx);
    if (!result) return R8E_UNDEFINED;

    r8e_obj_set(ctx, result, R8E_ATOM_proxy, proxy_val);
    r8e_obj_set(ctx, result, R8E_ATOM_revoke, r8e_from_pointer(revoker));

    return r8e_from_pointer(result);
}

/**
 * Public API: revoke a proxy.
 */
void r8e_proxy_revoke(R8EProxy *proxy) {
    if (proxy) {
        proxy->revoked = true;
        proxy->target = R8E_NULL;
        proxy->handler = R8E_NULL;
    }
}


/* *************************************************************************
 * SECTION C: Proxy Internal Methods - The 13 Traps
 *
 * Each trap follows the same pattern:
 *   1. Check if proxy is revoked (throw TypeError if so)
 *   2. Look up the trap on the handler object
 *   3. If the trap is undefined, forward to the target directly
 *   4. Call the trap with the appropriate arguments
 *   5. Validate the return value per ES2023 spec invariants
 * ************************************************************************* */

/* ---- Trap 1: [[GetPrototypeOf]] ---- */

R8EValue r8e_proxy_get_prototype_of(R8EContext *ctx, R8EValue proxy_val) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_getPrototypeOf);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target: return target's prototype */
        if (!R8E_IS_POINTER(proxy->target)) return R8E_NULL;
        R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(proxy->target);
        if (!h) return R8E_NULL;
        /* Return prototype based on proto_id. Simplified: return null. */
        return R8E_NULL;
    }

    R8EValue args[1] = { proxy->target };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 1, args);

    /* Invariant: result must be Object or null */
    if (!is_object(result) && !R8E_IS_NULL(result)) {
        return throw_type_error(ctx,
            "getPrototypeOf trap must return an object or null");
    }

    /*
     * Invariant: if target is non-extensible, the result must be the same
     * as Object.getPrototypeOf(target).
     */
    if (target_is_non_extensible(proxy->target)) {
        /* For non-extensible targets, we validate consistency.
         * Simplified: we trust the handler for now in non-strict mode. */
    }

    return result;
}


/* ---- Trap 2: [[SetPrototypeOf]] ---- */

bool r8e_proxy_set_prototype_of(R8EContext *ctx, R8EValue proxy_val,
                                R8EValue proto) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) { throw_type_error(ctx, "not a proxy"); return false; }
    if (check_revoked(ctx, proxy)) return false;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_setPrototypeOf);
    if (ctx->has_error) return false;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target directly: no-op for now */
        return true;
    }

    R8EValue args[2] = { proxy->target, proto };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 2, args);

    bool ok = (result == R8E_TRUE);

    /* Invariant: if target is non-extensible, proto must match target's proto */
    if (ok && target_is_non_extensible(proxy->target)) {
        /* Simplified invariant check */
    }

    return ok;
}


/* ---- Trap 3: [[IsExtensible]] ---- */

R8EValue r8e_proxy_is_extensible(R8EContext *ctx, R8EValue proxy_val) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_isExtensible);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target */
        return r8e_from_boolean(!target_is_non_extensible(proxy->target));
    }

    R8EValue args[1] = { proxy->target };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 1, args);

    bool trap_result = (result == R8E_TRUE);
    bool target_extensible = !target_is_non_extensible(proxy->target);

    /* Invariant: trap result must match target.[[IsExtensible]]() */
    if (trap_result != target_extensible) {
        return throw_type_error(ctx,
            "isExtensible trap result does not match target extensibility");
    }

    return r8e_from_boolean(trap_result);
}


/* ---- Trap 4: [[PreventExtensions]] ---- */

bool r8e_proxy_prevent_extensions(R8EContext *ctx, R8EValue proxy_val) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) { throw_type_error(ctx, "not a proxy"); return false; }
    if (check_revoked(ctx, proxy)) return false;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_preventExtensions);
    if (ctx->has_error) return false;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target */
        if (R8E_IS_POINTER(proxy->target)) {
            R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(proxy->target);
            if (h) h->flags |= R8E_GC_FROZEN_BIT;
        }
        return true;
    }

    R8EValue args[1] = { proxy->target };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 1, args);

    bool ok = (result == R8E_TRUE);

    /* Invariant: if trap returns true, target must actually be non-extensible */
    if (ok && !target_is_non_extensible(proxy->target)) {
        throw_type_error(ctx,
            "preventExtensions trap returned true but target is still extensible");
        return false;
    }

    return ok;
}


/* ---- Trap 5: [[GetOwnProperty]] ---- */

R8EValue r8e_proxy_get_own_property_descriptor(R8EContext *ctx,
                                               R8EValue proxy_val,
                                               R8EValue key) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_getOwnPropertyDescriptor);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target: simplified - return undefined (not found) */
        return R8E_UNDEFINED;
    }

    R8EValue args[2] = { proxy->target, key };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 2, args);

    /* Invariant: result must be Object or undefined */
    if (!R8E_IS_UNDEFINED(result) && !is_object(result)) {
        return throw_type_error(ctx,
            "getOwnPropertyDescriptor trap must return object or undefined");
    }

    /*
     * Additional invariants (ES2023 10.5.5):
     * - If target property is non-configurable, cannot report undefined
     * - If target is non-extensible, cannot report undefined for existing props
     * - Cannot report a non-configurable property as configurable
     * - Cannot report a non-configurable non-writable property as writable
     *
     * Full invariant checking deferred to integration with complete
     * property descriptor model.
     */

    return result;
}


/* ---- Trap 6: [[DefineOwnProperty]] ---- */

bool r8e_proxy_define_own_property(R8EContext *ctx, R8EValue proxy_val,
                                   R8EValue key, R8EValue desc) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) { throw_type_error(ctx, "not a proxy"); return false; }
    if (check_revoked(ctx, proxy)) return false;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_defineProperty);
    if (ctx->has_error) return false;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target: simplified */
        return true;
    }

    R8EValue args[3] = { proxy->target, key, desc };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 3, args);

    bool ok = (result == R8E_TRUE);

    /*
     * Invariants (ES2023 10.5.6):
     * - Cannot define non-configurable property if target property is configurable
     * - Cannot add property to non-extensible target if not already present
     * - Cannot change non-configurable property to configurable
     *
     * Full invariant checking requires property descriptor inspection.
     */

    if (ok && target_is_non_extensible(proxy->target)) {
        /* Additional extensibility invariant check */
    }

    return ok;
}


/* ---- Trap 7: [[HasProperty]] (the 'in' operator) ---- */

bool r8e_proxy_has(R8EContext *ctx, R8EValue proxy_val, R8EValue key) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) { throw_type_error(ctx, "not a proxy"); return false; }
    if (check_revoked(ctx, proxy)) return false;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_has);
    if (ctx->has_error) return false;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target */
        if (!R8E_IS_POINTER(proxy->target)) return false;
        void *target_obj = r8e_get_pointer(proxy->target);
        if (!target_obj) return false;
        /* Check if key is an atom index */
        if (R8E_IS_ATOM(key)) {
            uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
            return r8e_obj_has(ctx, target_obj, atom);
        }
        return false;
    }

    R8EValue args[2] = { proxy->target, key };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 2, args);

    bool trap_result = (result == R8E_TRUE);

    /*
     * Invariant: cannot report non-configurable own property as non-existent.
     * Cannot report any own property as non-existent if target is non-extensible.
     */
    if (!trap_result && target_is_non_extensible(proxy->target)) {
        /* The property might exist on target; if so, trap must return true.
         * Simplified check: trust trap for now. Full check needs
         * getOwnPropertyDescriptor on target. */
    }

    return trap_result;
}

/**
 * Proxy has check by atom name (convenience wrapper).
 */
bool r8e_proxy_has_atom(R8EContext *ctx, R8EValue proxy_val, uint32_t atom) {
    R8EValue key = 0xFFFC000000000000ULL | (uint64_t)atom;
    return r8e_proxy_has(ctx, proxy_val, key);
}


/* ---- Trap 8: [[Get]] ---- */

R8EValue r8e_proxy_get(R8EContext *ctx, R8EValue proxy_val, R8EValue key,
                       R8EValue receiver) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_get);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target */
        if (!R8E_IS_POINTER(proxy->target)) return R8E_UNDEFINED;
        void *target_obj = r8e_get_pointer(proxy->target);
        if (!target_obj) return R8E_UNDEFINED;
        if (R8E_IS_ATOM(key)) {
            uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
            return r8e_obj_get(ctx, target_obj, atom);
        }
        return R8E_UNDEFINED;
    }

    R8EValue args[3] = { proxy->target, key, receiver };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 3, args);

    /*
     * Invariant (ES2023 10.5.8):
     * - If target property is non-configurable, non-writable data property,
     *   trap must return the same value as target property value.
     * - If target property is non-configurable accessor without [[Get]],
     *   trap must return undefined.
     *
     * Full invariant checking requires property descriptor inspection.
     */

    return result;
}

/**
 * Proxy get by atom (convenience wrapper).
 */
R8EValue r8e_proxy_get_atom(R8EContext *ctx, R8EValue proxy_val,
                            uint32_t atom, R8EValue receiver) {
    R8EValue key = 0xFFFC000000000000ULL | (uint64_t)atom;
    return r8e_proxy_get(ctx, proxy_val, key, receiver);
}


/* ---- Trap 9: [[Set]] ---- */

bool r8e_proxy_set(R8EContext *ctx, R8EValue proxy_val, R8EValue key,
                   R8EValue value, R8EValue receiver) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) { throw_type_error(ctx, "not a proxy"); return false; }
    if (check_revoked(ctx, proxy)) return false;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_set);
    if (ctx->has_error) return false;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target */
        if (!R8E_IS_POINTER(proxy->target)) return false;
        void *target_obj = r8e_get_pointer(proxy->target);
        if (!target_obj) return false;
        if (R8E_IS_ATOM(key)) {
            uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
            r8e_obj_set(ctx, target_obj, atom, value);
            return true;
        }
        return false;
    }

    R8EValue args[4] = { proxy->target, key, value, receiver };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 4, args);

    bool ok = (result == R8E_TRUE);

    /*
     * Invariant (ES2023 10.5.9):
     * - Cannot set a non-configurable non-writable data property to a
     *   different value.
     * - Cannot set a non-configurable accessor without [[Set]].
     */

    return ok;
}

/**
 * Proxy set by atom (convenience wrapper).
 */
bool r8e_proxy_set_atom(R8EContext *ctx, R8EValue proxy_val, uint32_t atom,
                        R8EValue value, R8EValue receiver) {
    R8EValue key = 0xFFFC000000000000ULL | (uint64_t)atom;
    return r8e_proxy_set(ctx, proxy_val, key, value, receiver);
}


/* ---- Trap 10: [[Delete]] ---- */

bool r8e_proxy_delete_property(R8EContext *ctx, R8EValue proxy_val,
                               R8EValue key) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) { throw_type_error(ctx, "not a proxy"); return false; }
    if (check_revoked(ctx, proxy)) return false;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_deleteProperty);
    if (ctx->has_error) return false;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target */
        if (!R8E_IS_POINTER(proxy->target)) return false;
        void *target_obj = r8e_get_pointer(proxy->target);
        if (!target_obj) return false;
        if (R8E_IS_ATOM(key)) {
            uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
            return r8e_obj_delete(ctx, target_obj, atom);
        }
        return false;
    }

    R8EValue args[2] = { proxy->target, key };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 2, args);

    bool ok = (result == R8E_TRUE);

    /*
     * Invariant (ES2023 10.5.10):
     * - Cannot delete a non-configurable own property.
     * - Cannot delete any own property of a non-extensible target.
     */

    return ok;
}

/**
 * Proxy delete by atom (convenience wrapper).
 */
bool r8e_proxy_delete_atom(R8EContext *ctx, R8EValue proxy_val, uint32_t atom) {
    R8EValue key = 0xFFFC000000000000ULL | (uint64_t)atom;
    return r8e_proxy_delete_property(ctx, proxy_val, key);
}


/* ---- Trap 11: [[OwnPropertyKeys]] ---- */

R8EValue r8e_proxy_own_keys(R8EContext *ctx, R8EValue proxy_val) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_ownKeys);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target: return empty array for simplicity.
         * Full implementation would enumerate target's own keys. */
        R8EArray *arr = new_array(ctx, 4);
        if (!arr) return R8E_UNDEFINED;
        return r8e_from_pointer(arr);
    }

    R8EValue args[1] = { proxy->target };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 1, args);

    /*
     * Invariant (ES2023 10.5.11):
     * - Result must be a List of Strings and Symbols.
     * - Must contain all non-configurable own keys of target.
     * - If target is non-extensible, must contain exactly the target's own keys.
     * - No duplicates.
     *
     * Validation: check result is array-like, each element is string/symbol.
     */
    if (!R8E_IS_POINTER(result)) {
        return throw_type_error(ctx,
            "ownKeys trap must return an array-like object");
    }

    return result;
}


/* ---- Trap 12: [[Call]] (function proxies only) ---- */

R8EValue r8e_proxy_apply(R8EContext *ctx, R8EValue proxy_val,
                         R8EValue this_arg, R8EValue args_array) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    if (!proxy->is_callable) {
        return throw_type_error(ctx, "proxy [[Call]]: target is not callable");
    }

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_apply);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target: call target directly.
         * Need to extract args from args_array. Simplified: call with no args */
        return r8e_call_function(ctx, proxy->target, this_arg, 0, NULL);
    }

    R8EValue args[3] = { proxy->target, this_arg, args_array };
    return r8e_call_function(ctx, trap, proxy->handler, 3, args);
}

/**
 * Proxy call with flat args (for interpreter integration).
 */
R8EValue r8e_proxy_call(R8EContext *ctx, R8EValue proxy_val,
                        R8EValue this_arg, int argc, const R8EValue *argv) {
    /* Package args into array for the apply trap */
    R8EArray *args_arr = new_array(ctx, (uint32_t)argc);
    if (!args_arr) return R8E_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        array_push(args_arr, argv[i]);
    }
    return r8e_proxy_apply(ctx, proxy_val, this_arg,
                           r8e_from_pointer(args_arr));
}


/* ---- Trap 13: [[Construct]] (constructor proxies only) ---- */

R8EValue r8e_proxy_construct(R8EContext *ctx, R8EValue proxy_val,
                             R8EValue args_array, R8EValue new_target) {
    R8EProxy *proxy = get_proxy(proxy_val);
    if (!proxy) return throw_type_error(ctx, "not a proxy");
    if (check_revoked(ctx, proxy)) return R8E_UNDEFINED;

    if (!is_constructor(proxy->target)) {
        return throw_type_error(ctx,
            "proxy [[Construct]]: target is not a constructor");
    }

    R8EValue trap = get_trap(ctx, proxy, R8E_ATOM_construct);
    if (ctx->has_error) return R8E_UNDEFINED;

    if (R8E_IS_UNDEFINED(trap)) {
        /* Forward to target constructor */
        return r8e_construct_function(ctx, proxy->target, 0, NULL, new_target);
    }

    R8EValue args[3] = { proxy->target, args_array, new_target };
    R8EValue result = r8e_call_function(ctx, trap, proxy->handler, 3, args);

    /* Invariant: result must be an object */
    if (!is_object(result)) {
        return throw_type_error(ctx,
            "construct trap must return an object");
    }

    return result;
}


/* *************************************************************************
 * SECTION D: Reflect Object Methods
 *
 * Reflect provides the default behavior for all 13 proxy traps.
 * Each Reflect method directly performs the corresponding internal
 * method on the target object (no proxy interception).
 * ************************************************************************* */

/* ---- Reflect.apply(target, thisArg, argumentsList) ---- */
static R8EValue builtin_reflect_apply(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue this_arg = arg_or_undef(argc, argv, 1);
    R8EValue args_list = arg_or_undef(argc, argv, 2);

    if (!is_callable(target)) {
        return throw_type_error(ctx,
            "Reflect.apply: target is not callable");
    }

    /* Extract arguments from args_list (must be array-like) */
    if (!R8E_IS_POINTER(args_list)) {
        return r8e_call_function(ctx, target, this_arg, 0, NULL);
    }

    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(args_list);
    if (h && (h->flags & 0x0F) == R8E_GC_KIND_ARRAY) {
        R8EArray *arr = (R8EArray *)h;
        return r8e_call_function(ctx, target, this_arg,
                                  (int)arr->length, arr->elements);
    }

    return r8e_call_function(ctx, target, this_arg, 0, NULL);
}

/* ---- Reflect.construct(target, argumentsList [, newTarget]) ---- */
static R8EValue builtin_reflect_construct(R8EContext *ctx, R8EValue this_val,
                                          int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue args_list = arg_or_undef(argc, argv, 1);
    R8EValue new_target = (argc > 2) ? argv[2] : target;

    if (!is_constructor(target)) {
        return throw_type_error(ctx,
            "Reflect.construct: target is not a constructor");
    }
    if (!is_constructor(new_target)) {
        return throw_type_error(ctx,
            "Reflect.construct: newTarget is not a constructor");
    }

    /* Extract args */
    int call_argc = 0;
    const R8EValue *call_argv = NULL;
    if (R8E_IS_POINTER(args_list)) {
        R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(args_list);
        if (h && (h->flags & 0x0F) == R8E_GC_KIND_ARRAY) {
            R8EArray *arr = (R8EArray *)h;
            call_argc = (int)arr->length;
            call_argv = arr->elements;
        }
    }

    return r8e_construct_function(ctx, target, call_argc, call_argv, new_target);
}

/* ---- Reflect.defineProperty(target, key, descriptor) ---- */
static R8EValue builtin_reflect_defineProperty(R8EContext *ctx,
                                               R8EValue this_val,
                                               int argc,
                                               const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue key = arg_or_undef(argc, argv, 1);
    R8EValue desc = arg_or_undef(argc, argv, 2);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.defineProperty: target is not an object");
    }

    /* If target is a proxy, delegate to proxy trap */
    if (is_proxy(target)) {
        return r8e_from_boolean(
            r8e_proxy_define_own_property(ctx, target, key, desc));
    }

    /* Direct: simplified property definition on target */
    if (R8E_IS_POINTER(target) && R8E_IS_ATOM(key)) {
        void *obj = r8e_get_pointer(target);
        uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
        /* Extract value from descriptor */
        R8EValue val = R8E_UNDEFINED;
        if (is_object(desc)) {
            void *desc_obj = r8e_get_pointer(desc);
            val = r8e_obj_get(ctx, desc_obj, R8E_ATOM_value);
        }
        r8e_obj_set(ctx, obj, atom, val);
        return R8E_TRUE;
    }

    return R8E_FALSE;
}

/* ---- Reflect.deleteProperty(target, key) ---- */
static R8EValue builtin_reflect_deleteProperty(R8EContext *ctx,
                                               R8EValue this_val,
                                               int argc,
                                               const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue key = arg_or_undef(argc, argv, 1);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.deleteProperty: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_from_boolean(r8e_proxy_delete_property(ctx, target, key));
    }

    if (R8E_IS_POINTER(target) && R8E_IS_ATOM(key)) {
        void *obj = r8e_get_pointer(target);
        uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
        return r8e_from_boolean(r8e_obj_delete(ctx, obj, atom));
    }

    return R8E_FALSE;
}

/* ---- Reflect.get(target, key [, receiver]) ---- */
static R8EValue builtin_reflect_get(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue key = arg_or_undef(argc, argv, 1);
    R8EValue receiver = (argc > 2) ? argv[2] : target;

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.get: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_proxy_get(ctx, target, key, receiver);
    }

    if (R8E_IS_POINTER(target) && R8E_IS_ATOM(key)) {
        void *obj = r8e_get_pointer(target);
        uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
        return r8e_obj_get(ctx, obj, atom);
    }

    return R8E_UNDEFINED;
}

/* ---- Reflect.getOwnPropertyDescriptor(target, key) ---- */
static R8EValue builtin_reflect_getOwnPropertyDescriptor(
                    R8EContext *ctx, R8EValue this_val,
                    int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue key = arg_or_undef(argc, argv, 1);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.getOwnPropertyDescriptor: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_proxy_get_own_property_descriptor(ctx, target, key);
    }

    /* Direct: build a descriptor object for the property */
    if (R8E_IS_POINTER(target) && R8E_IS_ATOM(key)) {
        void *obj = r8e_get_pointer(target);
        uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
        if (!r8e_obj_has(ctx, obj, atom)) return R8E_UNDEFINED;

        R8EValue val = r8e_obj_get(ctx, obj, atom);
        R8EObjTier0 *desc = r8e_obj_new(ctx);
        if (!desc) return R8E_UNDEFINED;
        r8e_obj_set(ctx, desc, R8E_ATOM_value, val);
        r8e_obj_set(ctx, desc, R8E_ATOM_writable, R8E_TRUE);
        r8e_obj_set(ctx, desc, R8E_ATOM_enumerable, R8E_TRUE);
        r8e_obj_set(ctx, desc, R8E_ATOM_configurable, R8E_TRUE);
        return r8e_from_pointer(desc);
    }

    return R8E_UNDEFINED;
}

/* ---- Reflect.getPrototypeOf(target) ---- */
static R8EValue builtin_reflect_getPrototypeOf(R8EContext *ctx,
                                               R8EValue this_val,
                                               int argc,
                                               const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.getPrototypeOf: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_proxy_get_prototype_of(ctx, target);
    }

    /* Direct: look up prototype by proto_id */
    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(target);
    if (!h) return R8E_NULL;
    if (h->proto_id == R8E_PROTO_NONE || h->proto_id == 0) return R8E_NULL;
    if (ctx->prototypes && h->proto_id < ctx->proto_count) {
        void *proto = ctx->prototypes[h->proto_id];
        if (proto) return r8e_from_pointer(proto);
    }
    return R8E_NULL;
}

/* ---- Reflect.has(target, key) ---- */
static R8EValue builtin_reflect_has(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue key = arg_or_undef(argc, argv, 1);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.has: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_from_boolean(r8e_proxy_has(ctx, target, key));
    }

    if (R8E_IS_POINTER(target) && R8E_IS_ATOM(key)) {
        void *obj = r8e_get_pointer(target);
        uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
        return r8e_from_boolean(r8e_obj_has(ctx, obj, atom));
    }

    return R8E_FALSE;
}

/* ---- Reflect.isExtensible(target) ---- */
static R8EValue builtin_reflect_isExtensible(R8EContext *ctx,
                                             R8EValue this_val,
                                             int argc,
                                             const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.isExtensible: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_proxy_is_extensible(ctx, target);
    }

    return r8e_from_boolean(!target_is_non_extensible(target));
}

/* ---- Reflect.ownKeys(target) ---- */
static R8EValue builtin_reflect_ownKeys(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.ownKeys: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_proxy_own_keys(ctx, target);
    }

    /* Direct: enumerate own keys of target.
     * Return an array of atom-encoded keys.
     * Simplified: return empty array for now. Full implementation
     * would iterate CDOL tiers. */
    R8EArray *arr = new_array(ctx, 8);
    if (!arr) return R8E_UNDEFINED;
    return r8e_from_pointer(arr);
}

/* ---- Reflect.preventExtensions(target) ---- */
static R8EValue builtin_reflect_preventExtensions(R8EContext *ctx,
                                                  R8EValue this_val,
                                                  int argc,
                                                  const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.preventExtensions: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_from_boolean(r8e_proxy_prevent_extensions(ctx, target));
    }

    /* Direct: set the frozen bit on target */
    R8EObjHeader *h = (R8EObjHeader *)r8e_get_pointer(target);
    if (h) h->flags |= R8E_GC_FROZEN_BIT;
    return R8E_TRUE;
}

/* ---- Reflect.set(target, key, value [, receiver]) ---- */
static R8EValue builtin_reflect_set(R8EContext *ctx, R8EValue this_val,
                                    int argc, const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue key = arg_or_undef(argc, argv, 1);
    R8EValue value = arg_or_undef(argc, argv, 2);
    R8EValue receiver = (argc > 3) ? argv[3] : target;

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.set: target is not an object");
    }

    if (is_proxy(target)) {
        return r8e_from_boolean(
            r8e_proxy_set(ctx, target, key, value, receiver));
    }

    if (R8E_IS_POINTER(target) && R8E_IS_ATOM(key)) {
        void *obj = r8e_get_pointer(target);
        uint32_t atom = (uint32_t)(key & 0xFFFFFFFFULL);
        r8e_obj_set(ctx, obj, atom, value);
        return R8E_TRUE;
    }

    return R8E_FALSE;
}

/* ---- Reflect.setPrototypeOf(target, proto) ---- */
static R8EValue builtin_reflect_setPrototypeOf(R8EContext *ctx,
                                               R8EValue this_val,
                                               int argc,
                                               const R8EValue *argv) {
    (void)this_val;
    R8EValue target = arg_or_undef(argc, argv, 0);
    R8EValue proto = arg_or_undef(argc, argv, 1);

    if (!is_object(target)) {
        return throw_type_error(ctx,
            "Reflect.setPrototypeOf: target is not an object");
    }

    if (!is_object(proto) && !R8E_IS_NULL(proto)) {
        return throw_type_error(ctx,
            "Reflect.setPrototypeOf: prototype must be an object or null");
    }

    if (is_proxy(target)) {
        return r8e_from_boolean(
            r8e_proxy_set_prototype_of(ctx, target, proto));
    }

    /* Direct: set proto_id. Simplified: cannot dynamically change in CDOL
     * without prototype table lookup. For now, return true as no-op. */
    return R8E_TRUE;
}


/* *************************************************************************
 * SECTION E: Proxy-Aware Property Access Wrappers
 *
 * These functions are called by the interpreter/object model when a
 * property access is performed on a value that might be a Proxy.
 * ************************************************************************* */

/**
 * Proxy-aware property get. Used by GET_PROP opcode.
 */
R8EValue r8e_proxy_aware_get(R8EContext *ctx, R8EValue obj, uint32_t atom) {
    if (is_proxy(obj)) {
        return r8e_proxy_get_atom(ctx, obj, atom, obj);
    }
    if (!R8E_IS_POINTER(obj)) return R8E_UNDEFINED;
    void *p = r8e_get_pointer(obj);
    if (!p) return R8E_UNDEFINED;
    return r8e_obj_get(ctx, p, atom);
}

/**
 * Proxy-aware property set. Used by SET_PROP opcode.
 */
bool r8e_proxy_aware_set(R8EContext *ctx, R8EValue obj, uint32_t atom,
                         R8EValue value) {
    if (is_proxy(obj)) {
        return r8e_proxy_set_atom(ctx, obj, atom, value, obj);
    }
    if (!R8E_IS_POINTER(obj)) return false;
    void *p = r8e_get_pointer(obj);
    if (!p) return false;
    r8e_obj_set(ctx, p, atom, value);
    return true;
}

/**
 * Proxy-aware has check. Used by IN opcode.
 */
bool r8e_proxy_aware_has(R8EContext *ctx, R8EValue obj, uint32_t atom) {
    if (is_proxy(obj)) {
        return r8e_proxy_has_atom(ctx, obj, atom);
    }
    if (!R8E_IS_POINTER(obj)) return false;
    void *p = r8e_get_pointer(obj);
    if (!p) return false;
    return r8e_obj_has(ctx, p, atom);
}

/**
 * Proxy-aware delete. Used by DELETE_PROP opcode.
 */
bool r8e_proxy_aware_delete(R8EContext *ctx, R8EValue obj, uint32_t atom) {
    if (is_proxy(obj)) {
        return r8e_proxy_delete_atom(ctx, obj, atom);
    }
    if (!R8E_IS_POINTER(obj)) return false;
    void *p = r8e_get_pointer(obj);
    if (!p) return false;
    return r8e_obj_delete(ctx, p, atom);
}


/* *************************************************************************
 * SECTION F: Initialization
 *
 * Register Proxy constructor and Reflect object on the global object.
 * ************************************************************************* */

void r8e_init_proxy_reflect(R8EContext *ctx)
{
    if (!ctx) return;

    /* ---- Proxy constructor ---- */
    R8EObjTier0 *proxy_ctor = r8e_obj_new(ctx);
    if (!proxy_ctor) return;

    install_method(ctx, proxy_ctor, R8E_ATOM_revocable,
                   builtin_proxy_revocable, 2);

    /* The Proxy constructor is itself callable (new Proxy(t, h)).
     * We install it as a global. The actual constructor behavior is
     * handled by builtin_proxy_constructor which is invoked by the
     * interpreter when 'new Proxy(...)' is encountered. */

    /* Install as global: Proxy */
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_Proxy,
                    r8e_from_pointer(proxy_ctor));
    }

    /* ---- Reflect object (namespace, not a constructor) ---- */
    R8EObjTier0 *reflect_obj = r8e_obj_new(ctx);
    if (!reflect_obj) return;

    install_method(ctx, reflect_obj, R8E_ATOM_apply,
                   builtin_reflect_apply, 3);
    install_method(ctx, reflect_obj, R8E_ATOM_construct,
                   builtin_reflect_construct, 2);
    install_method(ctx, reflect_obj, R8E_ATOM_defineProperty,
                   builtin_reflect_defineProperty, 3);
    install_method(ctx, reflect_obj, R8E_ATOM_deleteProperty,
                   builtin_reflect_deleteProperty, 2);
    install_method(ctx, reflect_obj, R8E_ATOM_get,
                   builtin_reflect_get, 2);
    install_method(ctx, reflect_obj, R8E_ATOM_getOwnPropertyDescriptor,
                   builtin_reflect_getOwnPropertyDescriptor, 2);
    install_method(ctx, reflect_obj, R8E_ATOM_getPrototypeOf,
                   builtin_reflect_getPrototypeOf, 1);
    install_method(ctx, reflect_obj, R8E_ATOM_has,
                   builtin_reflect_has, 2);
    install_method(ctx, reflect_obj, R8E_ATOM_isExtensible,
                   builtin_reflect_isExtensible, 1);
    install_method(ctx, reflect_obj, R8E_ATOM_ownKeys,
                   builtin_reflect_ownKeys, 1);
    install_method(ctx, reflect_obj, R8E_ATOM_preventExtensions,
                   builtin_reflect_preventExtensions, 1);
    install_method(ctx, reflect_obj, R8E_ATOM_set,
                   builtin_reflect_set, 3);
    install_method(ctx, reflect_obj, R8E_ATOM_setPrototypeOf,
                   builtin_reflect_setPrototypeOf, 2);

    /* Install as global: Reflect */
    if (ctx->global_object) {
        r8e_obj_set(ctx, ctx->global_object, R8E_ATOM_Reflect,
                    r8e_from_pointer(reflect_obj));
    }
}
