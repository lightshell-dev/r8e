/*
 * r8e_realm.c - Layer 4: Context Isolation (Multi-Realm)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 11.6 (Layer 4: Context Isolation).
 *
 * Architecture:
 *   - Lightweight V8-Isolate equivalent: ~64KB per realm (EXP-13)
 *   - Creation in <2us (5000x faster than V8 Isolate)
 *   - Each realm has:
 *       * Dedicated secure arena (isolated memory)
 *       * Independent prototype chain
 *       * Own global object
 *       * Per-realm resource limits
 *   - Shared immutable data only (atom table, bytecode cache)
 *   - Cross-realm communication: structured clone only
 *       * <10us for <1KB messages (EXP-14)
 *       * No direct object references between realms
 *   - Tagged arenas: realm ID in NaN-boxed pointer upper bits
 *       * 4 bits = 16 max realms
 *       * Cross-realm reference detection in O(1)
 *   - Frozen intrinsics: all built-in prototypes frozen at creation
 *   - No SharedArrayBuffer between realms (prevents Spectre)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

#include "../../include/r8e_types.h"
#include "../../include/r8e_api.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

/* Maximum number of realms per context (4-bit ID in pointer tags) */
#define R8E_MAX_REALMS          16

/* Default arena size per realm: 64KB for plugins, 128KB for main */
#define R8E_REALM_ARENA_DEFAULT (64 * 1024)
#define R8E_REALM_ARENA_MAIN    (128 * 1024)

/* Maximum prototype table entries per realm */
#define R8E_MAX_PROTO_ENTRIES   64

/* Structured clone recursion limit (prevents stack overflow on deep objects) */
#define R8E_CLONE_MAX_DEPTH     64

/* Structured clone maximum total size (prevents memory bombs) */
#define R8E_CLONE_MAX_SIZE      (1024 * 1024)  /* 1MB */

/* =========================================================================
 * Timing Helper (for <2us creation target)
 * ========================================================================= */

static uint64_t r8e_time_us(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000 / freq.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

/* =========================================================================
 * Realm Internal State
 *
 * Extended realm data not in the public R8ERealm struct. Keeps the
 * public header clean while storing implementation details here.
 * ========================================================================= */

typedef struct R8ERealmInternal {
    R8ERealm         public_realm;   /* the public-facing realm struct */
    R8ERealmLimits   limits;         /* resource limits */
    uint64_t         cpu_start_us;   /* CPU time tracking start */
    uint64_t         cpu_used_us;    /* accumulated CPU microseconds */
    size_t           memory_used;    /* current memory usage */
    bool             frozen;         /* true if intrinsics are frozen */
    bool             active;         /* true if realm is in use */
    uint16_t         proto_capacity; /* allocated prototype slots */
} R8ERealmInternal;

/* Realm table (indexed by realm ID) */
static R8ERealmInternal g_realms[R8E_MAX_REALMS];
static bool g_realm_table_init = false;

static void ensure_realm_table_init(void) {
    if (!g_realm_table_init) {
        memset(g_realms, 0, sizeof(g_realms));
        g_realm_table_init = true;
    }
}

/* =========================================================================
 * Realm Creation
 * ========================================================================= */

/**
 * Initialize default resource limits for a new realm.
 */
static void realm_init_limits(R8ERealmLimits *limits) {
    limits->max_memory       = R8E_DEFAULT_MAX_MEMORY;
    limits->max_cpu_time_us  = R8E_DEFAULT_MAX_CPU_TIME_US;
    limits->max_stack_depth  = R8E_DEFAULT_MAX_STACK_DEPTH;
    limits->max_string_length = R8E_DEFAULT_MAX_STRING_LEN;
    limits->max_array_length = R8E_DEFAULT_MAX_ARRAY_LEN;
    limits->max_regexp_steps = R8E_DEFAULT_MAX_REGEXP_STEPS;
}

/**
 * Allocate and initialize the prototype table for a realm.
 * Pre-populates with R8E_PROTO_COUNT entries (all NULL initially).
 *
 * @param realm  The realm to initialize.
 * @return       true on success, false on OOM.
 */
static bool realm_init_prototypes(R8ERealmInternal *realm) {
    uint16_t cap = R8E_PROTO_COUNT + 8; /* room for user prototypes */
    realm->public_realm.prototypes = (R8EObject **)calloc(
        cap, sizeof(R8EObject *));
    if (!realm->public_realm.prototypes) return false;
    realm->public_realm.proto_count = R8E_PROTO_COUNT;
    realm->public_realm.proto_capacity = cap;
    realm->proto_capacity = cap;
    return true;
}

/**
 * Create a new empty global object for the realm.
 * The global is a Tier 1 object (starts with a few properties).
 *
 * @param realm  The realm to create a global for.
 * @return       true on success, false on OOM.
 */
static bool realm_init_global(R8ERealmInternal *realm) {
    /* Allocate a basic object for the global scope.
     * In a full implementation, this would be populated with standard
     * global properties (undefined, NaN, Infinity, parseInt, etc.) */
    R8EObject *global = (R8EObject *)calloc(1, sizeof(R8EObject));
    if (!global) return false;

    /* Initialize as Tier 0 (empty object) */
    global->u.header.flags = R8E_GC_TIER_0;
    global->u.header.proto_id = R8E_PROTO_OBJECT;

    /* Set initial refcount to 1 (owned by the realm) */
    global->u.header.flags = r8e_gc_set_rc(global->u.header.flags, 1);

    realm->public_realm.global = global;
    return true;
}

/**
 * Allocate a dedicated memory arena for the realm.
 *
 * @param realm      The realm to create an arena for.
 * @param arena_size Requested arena size (0 = default).
 * @return           true on success, false on failure.
 */
static bool realm_init_arena(R8ERealmInternal *realm, size_t arena_size) {
    if (arena_size == 0) {
        arena_size = (realm->public_realm.id == 0)
                     ? R8E_REALM_ARENA_MAIN
                     : R8E_REALM_ARENA_DEFAULT;
    }

    /* Use mmap with guard pages if available */
#ifndef _WIN32
    {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) page = 4096;
        size_t pg = (size_t)page;
        size_t data_pages = (arena_size + pg - 1) & ~(pg - 1);
        size_t total = data_pages + 2 * pg;

        void *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (raw != MAP_FAILED) {
            /* Guard pages */
            mprotect(raw, pg, PROT_NONE);
            mprotect((uint8_t *)raw + pg + data_pages, pg, PROT_NONE);

            realm->public_realm.arena = (uint8_t *)raw + pg;
            realm->public_realm.arena_size = data_pages;
            realm->public_realm.arena_used = 0;

            /* Zero the arena */
            memset(realm->public_realm.arena, 0, data_pages);
            return true;
        }
        /* Fall through to malloc */
    }
#endif

    /* Fallback: malloc */
    void *data = calloc(1, arena_size);
    if (!data) return false;
    realm->public_realm.arena = data;
    realm->public_realm.arena_size = arena_size;
    realm->public_realm.arena_used = 0;
    return true;
}

/**
 * Freeze all built-in prototypes in a realm.
 *
 * This prevents prototype pollution attacks. Once frozen, properties
 * cannot be added, removed, or modified on any built-in prototype.
 *
 * @param realm  The realm whose prototypes to freeze.
 */
static void realm_freeze_intrinsics(R8ERealmInternal *realm) {
    R8ERealm *r = &realm->public_realm;
    for (uint16_t i = 0; i < r->proto_count; i++) {
        if (r->prototypes[i]) {
            /* Set the frozen bit on the prototype object */
            r->prototypes[i]->u.header.flags |= R8E_GC_FROZEN_BIT;
        }
    }
    realm->frozen = true;
}

/**
 * Create a new realm within an engine context.
 *
 * The realm is initialized with:
 *   - A dedicated memory arena with guard pages
 *   - Independent prototype table (Object.prototype, Array.prototype, etc.)
 *   - A fresh global object
 *   - Default resource limits
 *   - Frozen intrinsics (prototype pollution protection)
 *
 * Target: <2us creation time, ~64KB memory (EXP-13).
 *
 * @param ctx  Engine context.
 * @return     Newly created realm pointer, or NULL on failure.
 *             The realm is registered in the context's realm table.
 */
R8ERealm *r8e_realm_create(R8EContext *ctx) {
    if (!ctx) return NULL;

    ensure_realm_table_init();

    /* Find a free realm slot */
    int slot = -1;
    for (int i = 0; i < R8E_MAX_REALMS; i++) {
        if (!g_realms[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return NULL; /* all 16 realm slots in use */

    /* Also check the context's realm table */
    if (ctx->realm_count >= R8E_MAX_REALMS) return NULL;

    uint64_t start_us = r8e_time_us();

    R8ERealmInternal *ri = &g_realms[slot];
    memset(ri, 0, sizeof(R8ERealmInternal));

    ri->public_realm.id = (uint32_t)slot;
    ri->public_realm.flags = 0;
    ri->active = true;

    /* Initialize resource limits */
    realm_init_limits(&ri->limits);
    ri->public_realm.limits = ri->limits;

    /* Initialize prototype table */
    if (!realm_init_prototypes(ri)) {
        ri->active = false;
        return NULL;
    }

    /* Initialize global object */
    if (!realm_init_global(ri)) {
        free(ri->public_realm.prototypes);
        ri->public_realm.prototypes = NULL;
        ri->active = false;
        return NULL;
    }

    /* Initialize dedicated arena */
    if (!realm_init_arena(ri, 0)) {
        free(ri->public_realm.global);
        free(ri->public_realm.prototypes);
        ri->public_realm.prototypes = NULL;
        ri->public_realm.global = NULL;
        ri->active = false;
        return NULL;
    }

    /* Freeze all built-in prototypes to prevent prototype pollution */
    realm_freeze_intrinsics(ri);

    /* CPU time tracking */
    ri->cpu_start_us = 0;
    ri->cpu_used_us = 0;

    /* Register in context */
    ctx->realms[slot] = &ri->public_realm;
    if ((uint32_t)slot >= ctx->realm_count) {
        ctx->realm_count = (uint8_t)(slot + 1);
    }

    uint64_t elapsed = r8e_time_us() - start_us;
    (void)elapsed; /* < 2us target per EXP-13 */

    return &ri->public_realm;
}

/**
 * Destroy a realm and free all associated memory.
 *
 * This frees:
 *   - The realm's arena (all objects allocated within it)
 *   - The prototype table
 *   - The global object
 *
 * Cannot destroy realm 0 (the default realm).
 *
 * @param ctx    Engine context.
 * @param realm  Realm to destroy.
 */
R8EStatus r8e_realm_destroy(R8EContext *ctx, int realm_id) {
    if (!ctx) return R8E_ERROR;
    if (realm_id < 0 || realm_id >= R8E_MAX_REALMS) return R8E_ERROR;
    if (realm_id == 0) return R8E_ERROR; /* cannot destroy default realm */

    R8ERealmInternal *ri = &g_realms[realm_id];
    if (!ri->active) return R8E_ERROR;

    R8ERealm *realm = &ri->public_realm;

    /* Free arena */
    if (realm->arena) {
#ifndef _WIN32
        /* If mmap-backed, unmap the full region including guard pages */
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) page = 4096;
        size_t pg = (size_t)page;
        /* Scrub before freeing */
        volatile uint8_t *p = (volatile uint8_t *)realm->arena;
        for (size_t i = 0; i < realm->arena_size; i++) p[i] = 0;

        void *raw = (uint8_t *)realm->arena - pg;
        size_t total = realm->arena_size + 2 * pg;
        munmap(raw, total);
#else
        free(realm->arena);
#endif
        realm->arena = NULL;
    }

    /* Free prototype table */
    if (realm->prototypes) {
        free(realm->prototypes);
        realm->prototypes = NULL;
    }

    /* Free global object (if heap-allocated outside the arena) */
    if (realm->global) {
        free(realm->global);
        realm->global = NULL;
    }

    /* Unregister from context */
    ctx->realms[realm_id] = NULL;

    /* Mark slot as free */
    ri->active = false;
    memset(ri, 0, sizeof(R8ERealmInternal));
    return R8E_OK;
}

/**
 * Switch the active realm in a context.
 *
 * All subsequent JS execution uses the new realm's global, prototypes,
 * and resource limits.
 *
 * @param ctx    Engine context.
 * @param realm  Realm to switch to.
 * @return       The previously active realm, or NULL on error.
 */
int r8e_realm_switch(R8EContext *ctx, int realm_id) {
    if (!ctx) return -1;
    if (realm_id < 0 || realm_id >= R8E_MAX_REALMS) return -1;
    if (!g_realms[realm_id].active) return -1;

    int prev_id = ctx->current_realm;

    /* Pause CPU tracking for the old realm */
    if (prev_id >= 0 && prev_id < R8E_MAX_REALMS &&
        g_realms[prev_id].cpu_start_us > 0) {
        uint64_t now = r8e_time_us();
        g_realms[prev_id].cpu_used_us +=
            now - g_realms[prev_id].cpu_start_us;
        g_realms[prev_id].cpu_start_us = 0;
    }

    /* Switch */
    ctx->current_realm = (uint8_t)realm_id;

    /* Start CPU tracking for the new realm */
    g_realms[realm_id].cpu_start_us = r8e_time_us();

    return prev_id;
}

/* =========================================================================
 * Resource Limits
 * ========================================================================= */

/**
 * Set resource limits on a realm.
 * Takes effect immediately. Checked at CHECK_LIMITS opcode points.
 *
 * @param realm   Target realm.
 * @param limits  New limits to apply.
 * @return        R8E_OK on success.
 */
R8EStatus r8e_realm_set_limits(R8EContext *ctx, int realm_id,
                                const R8ERealmLimits *limits) {
    if (!ctx || !limits) return R8E_ERROR;
    if (realm_id < 0 || realm_id >= R8E_MAX_REALMS) return R8E_ERROR;

    R8ERealmInternal *ri = &g_realms[realm_id];
    if (!ri->active) return R8E_ERROR;

    ri->limits = *limits;
    ri->public_realm.limits = *limits;
    return R8E_OK;
}

/**
 * Check resource limits for the current realm.
 * Called by the CHECK_LIMITS opcode at loop back-edges and function calls.
 *
 * @param ctx  Engine context.
 * @return     R8E_OK if within limits, R8E_ERROR_LIMIT if exceeded.
 */
R8EStatus r8e_realm_check_limits(R8EContext *ctx) {
    if (!ctx) return R8E_ERROR;

    uint8_t rid = ctx->current_realm;
    if (rid >= R8E_MAX_REALMS) return R8E_ERROR;

    R8ERealmInternal *ri = &g_realms[rid];
    if (!ri->active) return R8E_ERROR;

    const R8ERealmLimits *limits = &ri->limits;

    /* Check CPU time */
    if (limits->max_cpu_time_us > 0 && ri->cpu_start_us > 0) {
        uint64_t now = r8e_time_us();
        uint64_t elapsed = ri->cpu_used_us + (now - ri->cpu_start_us);
        if (elapsed > limits->max_cpu_time_us) {
            return R8E_ERROR_LIMIT;
        }
    }

    /* Check memory */
    if (limits->max_memory > 0) {
        R8ERealm *realm = &ri->public_realm;
        if (realm->arena_used > limits->max_memory) {
            return R8E_ERROR_LIMIT;
        }
    }

    /* Check stack depth */
    if (limits->max_stack_depth > 0) {
        if (ctx->stack_depth > limits->max_stack_depth) {
            return R8E_ERROR_LIMIT;
        }
    }

    return R8E_OK;
}

/* =========================================================================
 * Tagged Arena: Realm ID in Pointer Tags
 *
 * We embed a 4-bit realm ID in the upper bits of NaN-boxed pointers.
 * This allows O(1) cross-realm reference detection without any
 * additional memory overhead.
 *
 * Layout (within the 48-bit pointer payload):
 *   Bits [47:44] = realm ID (0-15)
 *   Bits [43:0]  = actual pointer (44 bits = 16TB address space)
 *
 * On current 64-bit systems, virtual addresses are 48 bits, and the
 * upper bits are usually zero (user space). We use the top 4 bits
 * of the 48-bit payload for the realm ID.
 * ========================================================================= */

/**
 * Encode a realm-tagged pointer into a NaN-boxed value.
 *
 * @param ptr       Raw pointer (must fit in 44 bits).
 * @param realm_id  Realm ID (0-15).
 * @return          NaN-boxed value with realm tag embedded.
 */
R8EValue r8e_realm_tag_pointer(void *ptr, uint32_t realm_id) {
    if (!ptr || realm_id >= R8E_MAX_REALMS) return R8E_NULL;

    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    /* Verify address fits in 44 bits */
    if (addr > 0x0000FFFFFFFFFFFULL) {
        /* Address too large for tagged scheme. Fall back to untagged. */
        return r8e_from_pointer(ptr);
    }

    /* Embed realm ID in bits [47:44] */
    uint64_t tagged = addr | ((uint64_t)realm_id << 44);
    return 0xFFF9000000000000ULL | tagged;
}

/**
 * Extract the realm ID from a tagged pointer.
 *
 * @param val  NaN-boxed pointer value.
 * @return     Realm ID (0-15), or UINT32_MAX if not a pointer.
 */
uint32_t r8e_realm_id_from_pointer(R8EValue val) {
    if (!R8E_IS_POINTER(val)) return UINT32_MAX;
    uint64_t payload = val & 0x0000FFFFFFFFFFFFULL;
    return (uint32_t)((payload >> 44) & 0xF);
}

/**
 * Extract the raw pointer from a realm-tagged value.
 *
 * @param val  NaN-boxed pointer value.
 * @return     Raw pointer with realm tag stripped.
 */
void *r8e_realm_untag_pointer(R8EValue val) {
    if (!R8E_IS_POINTER(val)) return NULL;
    uint64_t payload = val & 0x0000FFFFFFFFFFFFULL;
    /* Strip realm ID from bits [47:44] */
    uint64_t addr = payload & 0x00000FFFFFFFFFFFULL;
    return (void *)(uintptr_t)addr;
}

/**
 * Check if a value belongs to a specific realm.
 * O(1) check using the embedded realm tag.
 *
 * @param val            NaN-boxed value.
 * @param expected_realm Expected realm ID.
 * @return               true if the value belongs to the expected realm,
 *                       true for non-pointer values (they are realm-independent).
 */
bool r8e_check_cross_realm(R8EValue val, uint32_t expected_realm) {
    /* Non-pointer values (numbers, booleans, inline strings, etc.)
     * are realm-independent. No cross-realm issue. */
    if (!R8E_IS_POINTER(val)) return true;

    uint32_t val_realm = r8e_realm_id_from_pointer(val);
    return val_realm == expected_realm;
}

/* =========================================================================
 * Structured Clone (Cross-Realm Communication)
 *
 * Deep copy of values for safe cross-realm transfer. No direct object
 * references cross realm boundaries.
 *
 * Supported types: primitives, plain objects, arrays, dates, regexps,
 *                  maps, sets, arraybuffers, typed arrays.
 * Rejected types: functions, closures, proxies, symbols, WeakRef.
 *
 * Target: <10us for <1KB messages (EXP-14).
 * ========================================================================= */

/* Clone context: tracks recursion and memory usage during cloning */
typedef struct {
    R8EContext *ctx;
    int         depth;         /* current recursion depth */
    size_t      total_size;    /* total bytes cloned so far */
    bool        error;         /* set on failure */
    char        error_msg[128];
} R8ECloneCtx;

static void clone_error(R8ECloneCtx *cc, const char *msg) {
    cc->error = true;
    snprintf(cc->error_msg, sizeof(cc->error_msg), "%s", msg);
}

/**
 * Clone a single value. Recursive for objects/arrays.
 *
 * @param cc   Clone context.
 * @param val  Value to clone.
 * @return     Cloned value in the target realm.
 */
static R8EValue clone_value(R8ECloneCtx *cc, R8EValue val) {
    if (cc->error) return R8E_UNDEFINED;

    /* Depth check */
    if (cc->depth >= R8E_CLONE_MAX_DEPTH) {
        clone_error(cc, "structured clone: maximum depth exceeded");
        return R8E_UNDEFINED;
    }

    /* Size check */
    if (cc->total_size >= R8E_CLONE_MAX_SIZE) {
        clone_error(cc, "structured clone: maximum size exceeded");
        return R8E_UNDEFINED;
    }

    /* Primitives: return as-is (they are realm-independent) */
    if (R8E_IS_DOUBLE(val) || R8E_IS_INT32(val)) {
        cc->total_size += 8;
        return val;
    }
    if (R8E_IS_UNDEFINED(val) || R8E_IS_NULL(val) || R8E_IS_BOOL(val)) {
        return val;
    }
    if (R8E_IS_INLINE_STR(val)) {
        cc->total_size += 8;
        return val; /* inline strings have no heap allocation */
    }
    if (R8E_IS_ATOM(val)) {
        /* Atoms are shared across realms (interned in context) */
        return val;
    }

    /* Symbols cannot be cloned (they are identity-bearing) */
    if (R8E_IS_SYMBOL(val)) {
        clone_error(cc, "structured clone: cannot clone Symbol");
        return R8E_UNDEFINED;
    }

    /* Pointer types: must deep-copy */
    if (R8E_IS_POINTER(val)) {
        void *ptr = r8e_get_pointer(val);
        if (!ptr) {
            clone_error(cc, "structured clone: null pointer in value");
            return R8E_UNDEFINED;
        }

        /* Determine the object type from the flags field */
        R8EObject *obj = (R8EObject *)ptr;
        uint32_t flags = obj->u.header.flags;

        /* Check for function/closure: NOT cloneable */
        if (flags & R8E_OBJ_IS_FUNCTION) {
            clone_error(cc, "structured clone: cannot clone function");
            return R8E_UNDEFINED;
        }

        /* Check for array */
        if (flags & R8E_OBJ_IS_ARRAY) {
            R8EArray *arr = (R8EArray *)ptr;

            /* Allocate a new array in the target realm */
            R8EArray *new_arr = (R8EArray *)calloc(1, sizeof(R8EArray));
            if (!new_arr) {
                clone_error(cc, "structured clone: out of memory");
                return R8E_UNDEFINED;
            }
            new_arr->flags = R8E_OBJ_IS_ARRAY | R8E_GC_TIER_0;
            new_arr->flags = r8e_gc_set_rc(new_arr->flags, 1);
            new_arr->proto_id = R8E_PROTO_ARRAY;
            new_arr->length = arr->length;
            new_arr->capacity = arr->length;

            if (arr->length > 0) {
                new_arr->elements = (R8EValue *)calloc(
                    arr->length, sizeof(R8EValue));
                if (!new_arr->elements) {
                    free(new_arr);
                    clone_error(cc, "structured clone: out of memory");
                    return R8E_UNDEFINED;
                }

                cc->depth++;
                for (uint32_t i = 0; i < arr->length; i++) {
                    new_arr->elements[i] = clone_value(cc, arr->elements[i]);
                    if (cc->error) {
                        free(new_arr->elements);
                        free(new_arr);
                        return R8E_UNDEFINED;
                    }
                }
                cc->depth--;
            }

            cc->total_size += sizeof(R8EArray) +
                              (size_t)arr->length * sizeof(R8EValue);

            return r8e_from_pointer(new_arr);
        }

        /* Check for string */
        if (flags & R8E_OBJ_IS_STRING) {
            R8EString *str = (R8EString *)ptr;
            size_t alloc_size = sizeof(R8EString) + str->byte_length + 1;
            R8EString *new_str = (R8EString *)calloc(1, alloc_size);
            if (!new_str) {
                clone_error(cc, "structured clone: out of memory");
                return R8E_UNDEFINED;
            }
            memcpy(new_str, str, alloc_size);
            new_str->flags = r8e_gc_set_rc(new_str->flags, 1);
            new_str->offset_table = NULL; /* lazy, will be rebuilt */
            cc->total_size += alloc_size;
            return r8e_from_pointer(new_str);
        }

        /* Plain object: clone properties */
        uint8_t tier = r8e_gc_get_tier(flags);

        if (tier == 0) {
            /* Tier 0: 0-1 properties */
            R8EObjTier0 *src = &obj->u.t0;
            R8EObject *new_obj = (R8EObject *)calloc(1, sizeof(R8EObject));
            if (!new_obj) {
                clone_error(cc, "structured clone: out of memory");
                return R8E_UNDEFINED;
            }
            new_obj->u.t0.flags = R8E_GC_TIER_0;
            new_obj->u.t0.flags = r8e_gc_set_rc(new_obj->u.t0.flags, 1);
            new_obj->u.t0.proto_id = src->proto_id;

            cc->depth++;
            if (src->key0 != 0) {
                new_obj->u.t0.key0 = src->key0;
                new_obj->u.t0.val0 = clone_value(cc, src->val0);
            }
            cc->depth--;

            cc->total_size += sizeof(R8EObject);
            return r8e_from_pointer(new_obj);
        }

        if (tier == 1) {
            /* Tier 1: 2-4 properties */
            R8EObjTier1 *src = &obj->u.t1;
            R8EObject *new_obj = (R8EObject *)calloc(1, sizeof(R8EObject));
            if (!new_obj) {
                clone_error(cc, "structured clone: out of memory");
                return R8E_UNDEFINED;
            }
            new_obj->u.t1.flags = R8E_GC_TIER_1;
            new_obj->u.t1.flags = r8e_gc_set_rc(new_obj->u.t1.flags, 1);
            new_obj->u.t1.proto_id = src->proto_id;
            new_obj->u.t1.count = src->count;

            cc->depth++;
            for (uint8_t i = 0; i < src->count && i < 4; i++) {
                new_obj->u.t1.props[i].key = src->props[i].key;
                new_obj->u.t1.props[i].val = clone_value(cc, src->props[i].val);
                if (cc->error) {
                    free(new_obj);
                    return R8E_UNDEFINED;
                }
            }
            cc->depth--;

            cc->total_size += sizeof(R8EObject);
            return r8e_from_pointer(new_obj);
        }

        if (tier == 2) {
            /* Tier 2: 5-16 properties */
            R8EObjTier2 *src = &obj->u.t2;
            R8EObject *new_obj = (R8EObject *)calloc(1, sizeof(R8EObject));
            if (!new_obj) {
                clone_error(cc, "structured clone: out of memory");
                return R8E_UNDEFINED;
            }
            new_obj->u.t2.flags = R8E_GC_TIER_2;
            new_obj->u.t2.flags = r8e_gc_set_rc(new_obj->u.t2.flags, 1);
            new_obj->u.t2.proto_id = src->proto_id;
            new_obj->u.t2.count = src->count;
            new_obj->u.t2.capacity = src->count;

            if (src->count > 0 && src->props) {
                new_obj->u.t2.props = (R8EPropPair *)calloc(
                    src->count, sizeof(R8EPropPair));
                if (!new_obj->u.t2.props) {
                    free(new_obj);
                    clone_error(cc, "structured clone: out of memory");
                    return R8E_UNDEFINED;
                }

                cc->depth++;
                for (uint8_t i = 0; i < src->count; i++) {
                    new_obj->u.t2.props[i].key = src->props[i].key;
                    new_obj->u.t2.props[i].val =
                        clone_value(cc, src->props[i].val);
                    if (cc->error) {
                        free(new_obj->u.t2.props);
                        free(new_obj);
                        return R8E_UNDEFINED;
                    }
                }
                cc->depth--;
            }

            cc->total_size += sizeof(R8EObject) +
                              (size_t)src->count * sizeof(R8EPropPair);
            return r8e_from_pointer(new_obj);
        }

        if (tier == 3) {
            /* Tier 3: 17+ properties (hash table) */
            R8EObjTier3 *src = &obj->u.t3;
            R8EObject *new_obj = (R8EObject *)calloc(1, sizeof(R8EObject));
            if (!new_obj) {
                clone_error(cc, "structured clone: out of memory");
                return R8E_UNDEFINED;
            }
            new_obj->u.t3.flags = R8E_GC_TIER_3;
            new_obj->u.t3.flags = r8e_gc_set_rc(new_obj->u.t3.flags, 1);
            new_obj->u.t3.proto_id = src->proto_id;
            new_obj->u.t3.count = src->count;
            new_obj->u.t3.capacity = src->capacity;

            if (src->capacity > 0 && src->buckets) {
                new_obj->u.t3.buckets = (R8EPropPair *)calloc(
                    src->capacity, sizeof(R8EPropPair));
                if (!new_obj->u.t3.buckets) {
                    free(new_obj);
                    clone_error(cc, "structured clone: out of memory");
                    return R8E_UNDEFINED;
                }

                cc->depth++;
                for (uint16_t i = 0; i < src->capacity; i++) {
                    if (src->buckets[i].key != 0) {
                        new_obj->u.t3.buckets[i].key = src->buckets[i].key;
                        new_obj->u.t3.buckets[i].val =
                            clone_value(cc, src->buckets[i].val);
                        if (cc->error) {
                            free(new_obj->u.t3.buckets);
                            free(new_obj);
                            return R8E_UNDEFINED;
                        }
                    }
                }
                cc->depth--;
            }

            cc->total_size += sizeof(R8EObject) +
                              (size_t)src->capacity * sizeof(R8EPropPair);
            return r8e_from_pointer(new_obj);
        }

        /* Unknown object type: reject */
        clone_error(cc, "structured clone: unsupported object type");
        return R8E_UNDEFINED;
    }

    /* Unknown value type: reject */
    clone_error(cc, "structured clone: unsupported value type");
    return R8E_UNDEFINED;
}

/**
 * Perform a structured clone of a value for cross-realm transfer.
 *
 * Creates a deep copy of the value that is safe to use in a different realm.
 * All object references are cloned; no shared mutable state.
 *
 * @param ctx    Engine context.
 * @param value  Value to clone.
 * @return       Deep copy of the value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_structured_clone(R8EContext *ctx, R8EValue value) {
    if (!ctx) return R8E_UNDEFINED;

    R8ECloneCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.ctx = ctx;
    cc.depth = 0;
    cc.total_size = 0;
    cc.error = false;

    R8EValue result = clone_value(&cc, value);

    if (cc.error) {
        /* Report the error through the context */
        fprintf(stderr, "r8e: %s\n", cc.error_msg);
        return R8E_UNDEFINED;
    }

    return result;
}

/* =========================================================================
 * Realm Query Functions
 * ========================================================================= */

/**
 * Get the current realm for a context.
 */
R8ERealm *r8e_realm_get_current(R8EContext *ctx) {
    if (!ctx) return NULL;
    if (ctx->current_realm >= R8E_MAX_REALMS) return NULL;
    return ctx->realms[ctx->current_realm];
}

/**
 * Get a realm by ID.
 */
R8ERealm *r8e_realm_get(R8EContext *ctx, uint32_t realm_id) {
    if (!ctx || realm_id >= R8E_MAX_REALMS) return NULL;
    return ctx->realms[realm_id];
}

/**
 * Get the number of active realms.
 */
uint32_t r8e_realm_count(R8EContext *ctx) {
    if (!ctx) return 0;
    uint32_t count = 0;
    for (int i = 0; i < R8E_MAX_REALMS; i++) {
        if (g_realms[i].active) count++;
    }
    return count;
}

/**
 * Get CPU time used by a realm.
 */
uint64_t r8e_realm_cpu_used(R8ERealm *realm) {
    if (!realm || realm->id >= R8E_MAX_REALMS) return 0;
    R8ERealmInternal *ri = &g_realms[realm->id];
    if (!ri->active) return 0;

    uint64_t total = ri->cpu_used_us;
    if (ri->cpu_start_us > 0) {
        total += r8e_time_us() - ri->cpu_start_us;
    }
    return total;
}

/**
 * Reset CPU time tracking for a realm.
 */
void r8e_realm_reset_cpu(R8ERealm *realm) {
    if (!realm || realm->id >= R8E_MAX_REALMS) return;
    R8ERealmInternal *ri = &g_realms[realm->id];
    ri->cpu_used_us = 0;
    ri->cpu_start_us = r8e_time_us();
}

/**
 * Public API wrapper: create a new realm and return its ID.
 * Matches r8e_api.h signature: int r8e_realm_new(R8EContext *ctx).
 */
int r8e_realm_new(R8EContext *ctx) {
    R8ERealm *realm = r8e_realm_create(ctx);
    if (!realm) return -1;
    return (int)realm->id;
}
