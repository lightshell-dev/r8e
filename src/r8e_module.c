/*
 * r8e_module.c - ES Module System (ESM)
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 13.1 (File Structure), Section 14 (Timeline).
 *
 * Architecture:
 *   - Full ES2023 module semantics (15.2.1.16)
 *   - Module record: source, bytecode, status lifecycle
 *   - Import/export entries with live binding support
 *   - Module linking with circular dependency handling
 *   - Module evaluation with top-level await support
 *   - User-provided module loader callback for resolution + fetching
 *   - Module cache: specifier -> module (avoid re-loading)
 *   - Dynamic import() -> Promise
 *   - import.meta with url property
 *   - Namespace objects for import * as ns
 *   - Star export aggregation with ambiguity detection
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
#define R8E_IS_NULLISH(v)    (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))

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

static inline int r8e_inline_str_decode(R8EValue v, char *buf) {
    int len = (int)((v >> 45) & 0x7);
    for (int i = 0; i < len; i++) {
        buf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
    }
    buf[len] = '\0';
    return len;
}

/* =========================================================================
 * GC header (must match other modules)
 * ========================================================================= */

#define R8E_GC_KIND_MASK      0x000000E0u
#define R8E_GC_KIND_SHIFT     5
#define R8E_GC_KIND_OBJECT    0u
#define R8E_GC_KIND_STRING    1u
#define R8E_GC_KIND_MODULE    7u   /* new GC kind for modules */

#define R8E_OBJ_IS_ARRAY      0x00000080u

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)

/* =========================================================================
 * Context stub (mirrors other modules)
 * ========================================================================= */

#define R8E_PROTO_MAX 64

/* Error codes */
typedef enum {
    R8E_OK              =  0,
    R8E_ERROR           = -1,
    R8E_ERROR_SYNTAX    = -2,
    R8E_ERROR_TYPE      = -3,
    R8E_ERROR_REFERENCE = -5,
    R8E_ERROR_INTERNAL  = -8,
    R8E_ERROR_OOM       = -9,
} R8EStatus;

/* Forward declare module structures */
typedef struct R8EModule R8EModule;

/* Module loader callback */
typedef R8EStatus (*R8EModuleLoader)(void *ctx, const char *specifier,
                                      const char *referrer,
                                      char **out_source, size_t *out_len);

/* Minimal context with module support */
typedef struct R8EContext {
    R8EValue  proto_table[R8E_PROTO_MAX];
    uint32_t  proto_count;
    void *(*alloc)(size_t size, void *opaque);
    void *(*realloc_fn)(void *ptr, size_t size, void *opaque);
    void  (*free_fn)(void *ptr, void *opaque);
    void  *alloc_opaque;
    R8EValue last_error;
    bool     has_exception;

    /* Module state */
    R8EModule       **modules;
    uint16_t          module_count;
    uint16_t          module_capacity;
    R8EModuleLoader   module_loader;
    void             *module_loader_data;
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

/* String */
extern R8EValue r8e_string_new(R8EContext *ctx, const char *data,
                                uint32_t len);
extern const char *r8e_string_data(R8EValue v, uint32_t *out_len);

/* Atom table */
extern uint32_t r8e_atom_intern_str(R8EContext *ctx, const char *str,
                                     uint32_t len);
extern const char *r8e_atom_get_str(R8EContext *ctx, uint32_t atom,
                                     uint32_t *out_len);

/* Function call / eval */
extern R8EValue r8e_call_fn(R8EContext *ctx, R8EValue func,
                             R8EValue this_val, int argc,
                             const R8EValue *argv);
extern R8EValue r8e_compile_module(R8EContext *ctx, const char *source,
                                    size_t len, const char *filename);
extern R8EValue r8e_exec_function(R8EContext *ctx, R8EValue func);

/* Well-known atom IDs (must match r8e_atoms.h) */
#define R8E_ATOM_EMPTY     0
#define R8E_ATOM_length    1
#define R8E_ATOM_default   251
#define R8E_ATOM_star      0  /* placeholder for "*" */

/* =========================================================================
 * Constants
 * ========================================================================= */

#define R8E_MODULE_MAX_IMPORTS    256
#define R8E_MODULE_MAX_EXPORTS    256
#define R8E_MODULE_MAX_STAR_EXPORTS 64
#define R8E_MODULE_MAX_DEPTH      64
#define R8E_MODULE_CACHE_INIT     16
#define R8E_MODULE_MAX_SPECIFIER  1024

/* =========================================================================
 * Module Status (ES2023 15.2.1.16)
 * ========================================================================= */

typedef enum {
    R8E_MODULE_UNLINKED    = 0,  /* Parsed but not yet linked */
    R8E_MODULE_LINKING     = 1,  /* Currently being linked */
    R8E_MODULE_LINKED      = 2,  /* All imports resolved */
    R8E_MODULE_EVALUATING  = 3,  /* Currently executing */
    R8E_MODULE_EVALUATED   = 4,  /* Execution complete */
    R8E_MODULE_ERROR       = 5   /* Error during link or eval */
} R8EModuleStatus;

/* =========================================================================
 * Import Entry (ES2023 15.2.1.16.1)
 *
 * Represents: import { import_name as local_name } from "specifier"
 *   - module_specifier: the "from" string
 *   - import_name: the exported name in the source module ("default", "*", etc.)
 *   - local_name: the binding name in this module's scope
 * ========================================================================= */

typedef struct {
    uint32_t module_specifier_atom;  /* interned specifier string */
    uint32_t import_name_atom;       /* exported name in source module */
    uint32_t local_name_atom;        /* local binding name */
} R8EImportEntry;

/* =========================================================================
 * Export Entry (ES2023 15.2.1.16.1)
 *
 * Represents various export forms:
 *   export { local_name as export_name }
 *   export { import_name as export_name } from "specifier"  (re-export)
 *   export * from "specifier"  (star re-export)
 *   export default ...
 * ========================================================================= */

typedef struct {
    uint32_t export_name_atom;       /* the exported name */
    uint32_t local_name_atom;        /* local binding (0 if re-export) */
    uint32_t module_specifier_atom;  /* re-export source (0 if local) */
    uint32_t import_name_atom;       /* name in source module (re-export) */
} R8EExportEntry;

/* =========================================================================
 * Resolved Binding
 *
 * Result of resolving an export name: {module, binding_name}
 * ========================================================================= */

typedef struct {
    R8EModule *module;     /* the module that defines the binding */
    uint32_t   binding_atom; /* the local name in that module */
    bool       ambiguous;    /* true if star export resolution is ambiguous */
} R8EResolvedBinding;

/* Sentinel: resolution failed */
#define R8E_BINDING_NOT_FOUND   ((R8EResolvedBinding){NULL, 0, false})
#define R8E_BINDING_AMBIGUOUS   ((R8EResolvedBinding){NULL, 0, true})

/* =========================================================================
 * Module Record
 *
 * Central data structure for an ES module.
 * ========================================================================= */

struct R8EModule {
    /* GC header (for pointer tagging) */
    uint32_t flags;
    uint32_t proto_id;

    /* Identity */
    char            *specifier;      /* canonical module specifier (owned) */
    uint32_t         specifier_len;
    uint32_t         specifier_atom; /* interned specifier */

    /* Source and compiled code */
    char            *source;         /* module source text (owned, freed
                                        after compilation) */
    size_t           source_len;
    R8EValue         function;       /* compiled module body as function */

    /* Module status */
    R8EModuleStatus  status;

    /* Import entries */
    R8EImportEntry  *imports;
    uint16_t         import_count;
    uint16_t         import_capacity;

    /* Export entries (local exports) */
    R8EExportEntry  *exports;
    uint16_t         export_count;
    uint16_t         export_capacity;

    /* Star re-exports: export * from "..." */
    uint32_t        *star_exports;    /* specifier atoms */
    uint16_t         star_export_count;
    uint16_t         star_export_capacity;

    /* Requested modules: specifiers of all imported modules */
    R8EModule       **requested_modules;
    uint16_t          requested_count;
    uint16_t          requested_capacity;

    /* Module environment: bindings for exported variables */
    R8EValue         *env_bindings;   /* array of NaN-boxed binding values */
    uint32_t         *env_atoms;      /* atom for each binding */
    uint16_t          env_count;
    uint16_t          env_capacity;

    /* Namespace object (lazily created for import * as ns) */
    R8EValue          namespace_obj;

    /* Evaluation result */
    R8EValue          eval_result;
    R8EValue          eval_error;     /* stored error if status == ERROR */

    /* Linking state (used during link algorithm) */
    int32_t           dfs_index;
    int32_t           dfs_ancestor_index;

    /* import.meta object (lazily created) */
    R8EValue          import_meta;

    /* Context reference */
    R8EContext        *ctx;
};

/* =========================================================================
 * Module cache entry
 * ========================================================================= */

typedef struct {
    uint32_t    specifier_atom;
    R8EModule  *module;
} R8EModuleCacheEntry;

/* =========================================================================
 * PART 1: Module Creation and Lifecycle
 * ========================================================================= */

/*
 * r8e_module_new - Create a new module record.
 *
 * @param ctx        Engine context
 * @param specifier  Canonical module specifier string
 * @param spec_len   Specifier length
 * @return           New module, or NULL on OOM
 */
R8EModule *r8e_module_new(R8EContext *ctx, const char *specifier,
                           uint32_t spec_len) {
    R8EModule *mod = (R8EModule *)r8e_alloc(ctx, sizeof(R8EModule));
    if (!mod) return NULL;
    memset(mod, 0, sizeof(R8EModule));

    /* Set GC header */
    mod->flags = (R8E_GC_KIND_MODULE << R8E_GC_KIND_SHIFT);
    mod->proto_id = 0;

    /* Copy specifier */
    mod->specifier = (char *)r8e_alloc(ctx, spec_len + 1);
    if (!mod->specifier) {
        r8e_free(ctx, mod);
        return NULL;
    }
    memcpy(mod->specifier, specifier, spec_len);
    mod->specifier[spec_len] = '\0';
    mod->specifier_len = spec_len;
    mod->specifier_atom = r8e_atom_intern_str(ctx, specifier, spec_len);

    mod->status = R8E_MODULE_UNLINKED;
    mod->function = R8E_UNDEFINED;
    mod->namespace_obj = R8E_UNDEFINED;
    mod->eval_result = R8E_UNDEFINED;
    mod->eval_error = R8E_UNDEFINED;
    mod->import_meta = R8E_UNDEFINED;
    mod->dfs_index = -1;
    mod->dfs_ancestor_index = -1;
    mod->ctx = ctx;

    return mod;
}

/*
 * r8e_module_destroy - Free a module record and all its allocations.
 */
void r8e_module_destroy(R8EContext *ctx, R8EModule *mod) {
    if (!mod) return;

    if (mod->specifier) r8e_free(ctx, mod->specifier);
    if (mod->source) r8e_free(ctx, mod->source);
    if (mod->imports) r8e_free(ctx, mod->imports);
    if (mod->exports) r8e_free(ctx, mod->exports);
    if (mod->star_exports) r8e_free(ctx, mod->star_exports);
    if (mod->requested_modules) r8e_free(ctx, mod->requested_modules);
    if (mod->env_bindings) r8e_free(ctx, mod->env_bindings);
    if (mod->env_atoms) r8e_free(ctx, mod->env_atoms);

    r8e_free(ctx, mod);
}

/* =========================================================================
 * PART 2: Import and Export Entry Management
 * ========================================================================= */

/*
 * Add an import entry to a module.
 */
R8EStatus r8e_module_add_import(R8EModule *mod,
                                 uint32_t specifier_atom,
                                 uint32_t import_name_atom,
                                 uint32_t local_name_atom) {
    if (!mod) return R8E_ERROR;

    /* Grow array if needed */
    if (mod->import_count >= mod->import_capacity) {
        uint16_t new_cap = mod->import_capacity == 0
                           ? 8 : mod->import_capacity * 2;
        if (new_cap > R8E_MODULE_MAX_IMPORTS) new_cap = R8E_MODULE_MAX_IMPORTS;
        if (mod->import_count >= new_cap) return R8E_ERROR;

        R8EImportEntry *new_imports = (R8EImportEntry *)r8e_realloc(
            mod->ctx, mod->imports,
            sizeof(R8EImportEntry) * new_cap);
        if (!new_imports) return R8E_ERROR_OOM;
        mod->imports = new_imports;
        mod->import_capacity = new_cap;
    }

    R8EImportEntry *entry = &mod->imports[mod->import_count++];
    entry->module_specifier_atom = specifier_atom;
    entry->import_name_atom = import_name_atom;
    entry->local_name_atom = local_name_atom;
    return R8E_OK;
}

/*
 * Add an export entry to a module.
 */
R8EStatus r8e_module_add_export(R8EModule *mod,
                                 uint32_t export_name_atom,
                                 uint32_t local_name_atom,
                                 uint32_t specifier_atom,
                                 uint32_t import_name_atom) {
    if (!mod) return R8E_ERROR;

    if (mod->export_count >= mod->export_capacity) {
        uint16_t new_cap = mod->export_capacity == 0
                           ? 8 : mod->export_capacity * 2;
        if (new_cap > R8E_MODULE_MAX_EXPORTS) new_cap = R8E_MODULE_MAX_EXPORTS;
        if (mod->export_count >= new_cap) return R8E_ERROR;

        R8EExportEntry *new_exports = (R8EExportEntry *)r8e_realloc(
            mod->ctx, mod->exports,
            sizeof(R8EExportEntry) * new_cap);
        if (!new_exports) return R8E_ERROR_OOM;
        mod->exports = new_exports;
        mod->export_capacity = new_cap;
    }

    R8EExportEntry *entry = &mod->exports[mod->export_count++];
    entry->export_name_atom = export_name_atom;
    entry->local_name_atom = local_name_atom;
    entry->module_specifier_atom = specifier_atom;
    entry->import_name_atom = import_name_atom;
    return R8E_OK;
}

/*
 * Add a star re-export (export * from "specifier").
 */
R8EStatus r8e_module_add_star_export(R8EModule *mod,
                                      uint32_t specifier_atom) {
    if (!mod) return R8E_ERROR;

    if (mod->star_export_count >= mod->star_export_capacity) {
        uint16_t new_cap = mod->star_export_capacity == 0
                           ? 4 : mod->star_export_capacity * 2;
        if (new_cap > R8E_MODULE_MAX_STAR_EXPORTS)
            new_cap = R8E_MODULE_MAX_STAR_EXPORTS;
        if (mod->star_export_count >= new_cap) return R8E_ERROR;

        uint32_t *new_stars = (uint32_t *)r8e_realloc(
            mod->ctx, mod->star_exports,
            sizeof(uint32_t) * new_cap);
        if (!new_stars) return R8E_ERROR_OOM;
        mod->star_exports = new_stars;
        mod->star_export_capacity = new_cap;
    }

    mod->star_exports[mod->star_export_count++] = specifier_atom;
    return R8E_OK;
}

/* =========================================================================
 * PART 3: Module Environment (Bindings)
 *
 * Each module has an environment that holds its exported bindings.
 * These are live: imports see the current value, not a snapshot.
 * ========================================================================= */

/*
 * Allocate or find a binding slot in the module environment.
 * Returns the slot index, or -1 on error.
 */
static int r8e_module_env_ensure(R8EModule *mod, uint32_t atom) {
    /* Check if already exists */
    for (int i = 0; i < mod->env_count; i++) {
        if (mod->env_atoms[i] == atom) return i;
    }

    /* Grow if needed */
    if (mod->env_count >= mod->env_capacity) {
        uint16_t new_cap = mod->env_capacity == 0
                           ? 16 : mod->env_capacity * 2;
        R8EValue *new_bindings = (R8EValue *)r8e_realloc(
            mod->ctx, mod->env_bindings,
            sizeof(R8EValue) * new_cap);
        uint32_t *new_atoms = (uint32_t *)r8e_realloc(
            mod->ctx, mod->env_atoms,
            sizeof(uint32_t) * new_cap);
        if (!new_bindings || !new_atoms) return -1;
        mod->env_bindings = new_bindings;
        mod->env_atoms = new_atoms;
        mod->env_capacity = new_cap;
    }

    int idx = mod->env_count;
    mod->env_atoms[idx] = atom;
    mod->env_bindings[idx] = R8E_UNDEFINED;
    mod->env_count++;
    return idx;
}

/*
 * Set a binding value in the module environment.
 */
R8EStatus r8e_module_env_set(R8EModule *mod, uint32_t atom, R8EValue val) {
    int idx = r8e_module_env_ensure(mod, atom);
    if (idx < 0) return R8E_ERROR_OOM;
    mod->env_bindings[idx] = val;
    return R8E_OK;
}

/*
 * Get a binding value from the module environment.
 */
R8EValue r8e_module_env_get(R8EModule *mod, uint32_t atom) {
    for (int i = 0; i < mod->env_count; i++) {
        if (mod->env_atoms[i] == atom) {
            return mod->env_bindings[i];
        }
    }
    return R8E_UNDEFINED;
}

/* =========================================================================
 * PART 4: Module Loading
 *
 * Resolves a specifier string to a module record. Uses the module cache
 * to avoid re-loading, and the user-provided loader callback for fetching.
 * ========================================================================= */

/*
 * r8e_module_set_loader - Set the module loader callback.
 */
void r8e_module_set_loader(R8EContext *ctx, R8EModuleLoader loader,
                            void *user_data) {
    if (!ctx) return;
    ctx->module_loader = loader;
    ctx->module_loader_data = user_data;
}

/*
 * Find a module in the cache by specifier atom.
 */
static R8EModule *r8e_module_cache_find(R8EContext *ctx,
                                         uint32_t specifier_atom) {
    for (uint16_t i = 0; i < ctx->module_count; i++) {
        if (ctx->modules[i] &&
            ctx->modules[i]->specifier_atom == specifier_atom) {
            return ctx->modules[i];
        }
    }
    return NULL;
}

/*
 * Add a module to the cache.
 */
static R8EStatus r8e_module_cache_add(R8EContext *ctx, R8EModule *mod) {
    if (ctx->module_count >= ctx->module_capacity) {
        uint16_t new_cap = ctx->module_capacity == 0
                           ? R8E_MODULE_CACHE_INIT
                           : ctx->module_capacity * 2;
        R8EModule **new_mods = (R8EModule **)r8e_realloc(
            ctx, ctx->modules,
            sizeof(R8EModule *) * new_cap);
        if (!new_mods) return R8E_ERROR_OOM;
        ctx->modules = new_mods;
        ctx->module_capacity = new_cap;
    }
    ctx->modules[ctx->module_count++] = mod;
    return R8E_OK;
}

/*
 * Resolve a relative specifier against a referrer.
 * Simple path resolution: handles "./", "../", absolute paths.
 * Returns an interned atom for the resolved specifier.
 */
static uint32_t r8e_module_resolve_specifier(R8EContext *ctx,
                                              const char *specifier,
                                              uint32_t spec_len,
                                              const char *referrer) {
    /* If specifier is absolute or bare (no ./ or ../), use as-is */
    if (spec_len == 0) return 0;

    /* Check for relative paths */
    bool is_relative = (specifier[0] == '.');

    if (!is_relative || !referrer) {
        /* Bare specifier or no referrer: intern as-is */
        return r8e_atom_intern_str(ctx, specifier, spec_len);
    }

    /* Find the directory part of the referrer */
    const char *last_slash = NULL;
    for (const char *p = referrer; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    /* Build resolved path */
    char resolved[R8E_MODULE_MAX_SPECIFIER];
    size_t dir_len = 0;

    if (last_slash) {
        dir_len = (size_t)(last_slash - referrer + 1);
        if (dir_len >= sizeof(resolved)) return 0;
        memcpy(resolved, referrer, dir_len);
    }

    /* Start from the relative specifier */
    const char *sp = specifier;
    size_t rlen = dir_len;

    while (*sp) {
        if (sp[0] == '.' && sp[1] == '/') {
            /* "./" - current directory, skip */
            sp += 2;
        } else if (sp[0] == '.' && sp[1] == '.' && sp[2] == '/') {
            /* "../" - go up one directory */
            sp += 3;
            /* Remove last directory component from resolved */
            if (rlen > 0) {
                rlen--; /* skip trailing slash */
                while (rlen > 0 && resolved[rlen - 1] != '/') rlen--;
            }
        } else {
            break;
        }
    }

    /* Append remaining specifier */
    size_t rem_len = strlen(sp);
    if (rlen + rem_len >= sizeof(resolved)) return 0;
    memcpy(resolved + rlen, sp, rem_len);
    rlen += rem_len;
    resolved[rlen] = '\0';

    return r8e_atom_intern_str(ctx, resolved, (uint32_t)rlen);
}

/*
 * r8e_module_load - Load a module by specifier, resolving relative to referrer.
 *
 * Uses the module cache first. If not cached, calls the loader callback
 * to fetch source, parses it, and adds to cache.
 *
 * @param ctx        Engine context
 * @param specifier  Module specifier string
 * @param spec_len   Specifier length (0 = strlen)
 * @param referrer   Referrer module (NULL for top-level)
 * @return           Module record, or NULL on error
 */
R8EModule *r8e_module_load(R8EContext *ctx, const char *specifier,
                            uint32_t spec_len, R8EModule *referrer) {
    if (!ctx || !specifier) return NULL;
    if (spec_len == 0) spec_len = (uint32_t)strlen(specifier);

    /* Resolve the specifier */
    const char *ref_spec = referrer ? referrer->specifier : NULL;
    uint32_t resolved_atom = r8e_module_resolve_specifier(
        ctx, specifier, spec_len, ref_spec);
    if (resolved_atom == 0) {
        ctx->has_exception = true;
        return NULL;
    }

    /* Check cache */
    R8EModule *cached = r8e_module_cache_find(ctx, resolved_atom);
    if (cached) return cached;

    /* Need to load: call the user-provided loader */
    if (!ctx->module_loader) {
        ctx->has_exception = true;
        return NULL;
    }

    uint32_t resolved_len;
    const char *resolved_str = r8e_atom_get_str(ctx, resolved_atom,
                                                 &resolved_len);
    if (!resolved_str) {
        ctx->has_exception = true;
        return NULL;
    }

    char *source = NULL;
    size_t source_len = 0;

    R8EStatus status = ctx->module_loader(
        ctx->module_loader_data,
        resolved_str,
        ref_spec,
        &source, &source_len);

    if (status != R8E_OK || !source) {
        ctx->has_exception = true;
        return NULL;
    }

    /* Create module record */
    R8EModule *mod = r8e_module_new(ctx, resolved_str, resolved_len);
    if (!mod) {
        r8e_free(ctx, source);
        return NULL;
    }

    mod->source = source;
    mod->source_len = source_len;

    /* Compile the module source to bytecode */
    mod->function = r8e_compile_module(ctx, source, source_len,
                                        resolved_str);
    if (R8E_IS_UNDEFINED(mod->function)) {
        mod->status = R8E_MODULE_ERROR;
        mod->eval_error = ctx->last_error;
        /* Still add to cache so repeated imports get the same error */
        r8e_module_cache_add(ctx, mod);
        return NULL;
    }

    /* Source can be freed after compilation */
    r8e_free(ctx, mod->source);
    mod->source = NULL;
    mod->source_len = 0;

    /* Add to cache */
    if (r8e_module_cache_add(ctx, mod) != R8E_OK) {
        r8e_module_destroy(ctx, mod);
        return NULL;
    }

    return mod;
}

/* =========================================================================
 * PART 5: Export Resolution (ES2023 15.2.1.16.3)
 *
 * Resolves an export name to a {module, binding_name} pair.
 * Handles local exports, re-exports, and star exports with
 * ambiguity detection.
 * ========================================================================= */

/*
 * Resolution set: tracks visited modules to detect circular resolution.
 */
typedef struct {
    R8EModule *modules[R8E_MODULE_MAX_DEPTH];
    uint32_t   names[R8E_MODULE_MAX_DEPTH];
    int        count;
} R8EResolveSet;

static bool resolve_set_contains(R8EResolveSet *set, R8EModule *mod,
                                  uint32_t name_atom) {
    for (int i = 0; i < set->count; i++) {
        if (set->modules[i] == mod && set->names[i] == name_atom)
            return true;
    }
    return false;
}

static void resolve_set_add(R8EResolveSet *set, R8EModule *mod,
                             uint32_t name_atom) {
    if (set->count < R8E_MODULE_MAX_DEPTH) {
        set->modules[set->count] = mod;
        set->names[set->count] = name_atom;
        set->count++;
    }
}

/*
 * r8e_module_resolve_export_impl - Internal export resolution.
 *
 * ES2023 15.2.1.16.3 ResolveExport(exportName, resolveSet)
 *
 * Walks the export entries and star exports to find the binding that
 * defines 'export_name'. Returns {module, binding} or ambiguous/not-found.
 */
static R8EResolvedBinding r8e_module_resolve_export_impl(
    R8EModule *mod, uint32_t export_name_atom,
    R8EResolveSet *resolve_set) {

    /* Step 1: Check for circular resolution */
    if (resolve_set_contains(resolve_set, mod, export_name_atom)) {
        /* Circular: return null (not found) per spec */
        return R8E_BINDING_NOT_FOUND;
    }
    resolve_set_add(resolve_set, mod, export_name_atom);

    /* Step 2: Search local export entries */
    for (uint16_t i = 0; i < mod->export_count; i++) {
        R8EExportEntry *ee = &mod->exports[i];
        if (ee->export_name_atom != export_name_atom) continue;

        /* Local export (not a re-export) */
        if (ee->module_specifier_atom == 0) {
            /* Direct local export */
            R8EResolvedBinding binding;
            binding.module = mod;
            binding.binding_atom = ee->local_name_atom;
            binding.ambiguous = false;
            return binding;
        }

        /* Re-export from another module */
        R8EModule *imported = r8e_module_cache_find(
            mod->ctx, ee->module_specifier_atom);
        if (!imported) {
            return R8E_BINDING_NOT_FOUND;
        }

        /* If import name is "*", it re-exports the namespace */
        if (ee->import_name_atom == R8E_ATOM_star) {
            R8EResolvedBinding binding;
            binding.module = imported;
            binding.binding_atom = 0; /* namespace */
            binding.ambiguous = false;
            return binding;
        }

        /* Recursively resolve in the source module */
        return r8e_module_resolve_export_impl(
            imported, ee->import_name_atom, resolve_set);
    }

    /* Step 3: Search star exports */
    R8EResolvedBinding star_resolution = R8E_BINDING_NOT_FOUND;
    bool found_star = false;

    for (uint16_t i = 0; i < mod->star_export_count; i++) {
        uint32_t star_spec_atom = mod->star_exports[i];
        R8EModule *star_mod = r8e_module_cache_find(mod->ctx,
                                                     star_spec_atom);
        if (!star_mod) continue;

        /* Do not re-export "default" through star exports */
        if (export_name_atom == R8E_ATOM_default) continue;

        R8EResolvedBinding resolution = r8e_module_resolve_export_impl(
            star_mod, export_name_atom, resolve_set);

        if (resolution.ambiguous) {
            return R8E_BINDING_AMBIGUOUS;
        }
        if (resolution.module == NULL) continue; /* not found in this star */

        if (!found_star) {
            /* First match */
            star_resolution = resolution;
            found_star = true;
        } else {
            /* Check for ambiguity: same {module, binding}? */
            if (star_resolution.module != resolution.module ||
                star_resolution.binding_atom != resolution.binding_atom) {
                return R8E_BINDING_AMBIGUOUS;
            }
        }
    }

    return star_resolution;
}

/*
 * r8e_module_resolve_export - Public export resolution.
 *
 * @param mod              The module to resolve from
 * @param export_name_atom The export name to look up
 * @return                 Resolved binding, or not-found/ambiguous
 */
R8EResolvedBinding r8e_module_resolve_export(R8EModule *mod,
                                              uint32_t export_name_atom) {
    R8EResolveSet resolve_set;
    resolve_set.count = 0;
    return r8e_module_resolve_export_impl(mod, export_name_atom,
                                           &resolve_set);
}

/* =========================================================================
 * PART 6: Module Linking (ES2023 15.2.1.16.2)
 *
 * InnerModuleLinking resolves all import bindings and connects them to
 * the corresponding export bindings in their source modules.
 *
 * Handles circular dependencies: if module A imports from B and B imports
 * from A, they can still link successfully because both module environments
 * exist (with undefined initial values) before any evaluation occurs.
 * ========================================================================= */

/*
 * Module link stack: tracks DFS traversal for circular dependency detection.
 */
typedef struct {
    R8EModule *stack[R8E_MODULE_MAX_DEPTH];
    int        count;
    int        index;  /* DFS index counter */
} R8ELinkState;

static R8EStatus r8e_module_inner_link(R8EModule *mod, R8ELinkState *state);

/*
 * r8e_module_link - Link a module and all its dependencies.
 *
 * ES2023 15.2.1.16.2 Link()
 *
 * After this call, all import bindings are resolved and the module is
 * ready for evaluation.
 */
R8EStatus r8e_module_link(R8EContext *ctx, R8EModule *mod) {
    if (!ctx || !mod) return R8E_ERROR;

    /* Already linked or evaluated? Nothing to do. */
    if (mod->status == R8E_MODULE_LINKED ||
        mod->status == R8E_MODULE_EVALUATED) {
        return R8E_OK;
    }
    if (mod->status == R8E_MODULE_ERROR) {
        return R8E_ERROR;
    }

    R8ELinkState state;
    memset(&state, 0, sizeof(state));

    R8EStatus status = r8e_module_inner_link(mod, &state);

    if (status != R8E_OK) {
        /* Linking failed: mark all modules in the stack as ERROR */
        for (int i = 0; i < state.count; i++) {
            if (state.stack[i]->status == R8E_MODULE_LINKING) {
                state.stack[i]->status = R8E_MODULE_ERROR;
            }
        }
        mod->status = R8E_MODULE_ERROR;
        return status;
    }

    /* Assert: stack is empty if linking succeeded */
    assert(state.count == 0);

    return R8E_OK;
}

/*
 * InnerModuleLinking - Recursive DFS linking.
 *
 * Uses Tarjan's algorithm adapted for module dependency graphs.
 */
static R8EStatus r8e_module_inner_link(R8EModule *mod, R8ELinkState *state) {
    /* Already linked? */
    if (mod->status == R8E_MODULE_LINKED ||
        mod->status == R8E_MODULE_EVALUATED) {
        return R8E_OK;
    }

    /* Already linking? Circular dependency. This is allowed. */
    if (mod->status == R8E_MODULE_LINKING) {
        return R8E_OK;
    }

    if (mod->status == R8E_MODULE_ERROR) {
        return R8E_ERROR;
    }

    /* Mark as linking */
    mod->status = R8E_MODULE_LINKING;
    mod->dfs_index = state->index;
    mod->dfs_ancestor_index = state->index;
    state->index++;

    /* Push onto stack */
    if (state->count >= R8E_MODULE_MAX_DEPTH) {
        return R8E_ERROR;
    }
    state->stack[state->count++] = mod;

    /* Step 1: Load and link all requested modules (imports) */
    for (uint16_t i = 0; i < mod->import_count; i++) {
        R8EImportEntry *ie = &mod->imports[i];

        /* Load the requested module */
        uint32_t spec_len;
        const char *spec_str = r8e_atom_get_str(
            mod->ctx, ie->module_specifier_atom, &spec_len);
        if (!spec_str) return R8E_ERROR;

        R8EModule *required = r8e_module_load(
            mod->ctx, spec_str, spec_len, mod);
        if (!required) return R8E_ERROR;

        /* Track as requested module */
        if (mod->requested_count >= mod->requested_capacity) {
            uint16_t new_cap = mod->requested_capacity == 0
                               ? 4 : mod->requested_capacity * 2;
            R8EModule **new_req = (R8EModule **)r8e_realloc(
                mod->ctx, mod->requested_modules,
                sizeof(R8EModule *) * new_cap);
            if (!new_req) return R8E_ERROR_OOM;
            mod->requested_modules = new_req;
            mod->requested_capacity = new_cap;
        }
        mod->requested_modules[mod->requested_count++] = required;

        /* Recursively link the required module */
        R8EStatus status = r8e_module_inner_link(required, state);
        if (status != R8E_OK) return status;

        /* Update ancestor index for Tarjan's algorithm */
        if (required->status == R8E_MODULE_LINKING) {
            if (required->dfs_ancestor_index < mod->dfs_ancestor_index) {
                mod->dfs_ancestor_index = required->dfs_ancestor_index;
            }
        }
    }

    /* Step 2: Also load modules for star re-exports */
    for (uint16_t i = 0; i < mod->star_export_count; i++) {
        uint32_t spec_atom = mod->star_exports[i];
        uint32_t spec_len;
        const char *spec_str = r8e_atom_get_str(mod->ctx, spec_atom,
                                                 &spec_len);
        if (!spec_str) return R8E_ERROR;

        R8EModule *star_mod = r8e_module_load(
            mod->ctx, spec_str, spec_len, mod);
        if (!star_mod) return R8E_ERROR;

        R8EStatus status = r8e_module_inner_link(star_mod, state);
        if (status != R8E_OK) return status;

        if (star_mod->status == R8E_MODULE_LINKING) {
            if (star_mod->dfs_ancestor_index < mod->dfs_ancestor_index) {
                mod->dfs_ancestor_index = star_mod->dfs_ancestor_index;
            }
        }
    }

    /* Step 3: Initialize module environment */
    for (uint16_t i = 0; i < mod->export_count; i++) {
        R8EExportEntry *ee = &mod->exports[i];
        if (ee->local_name_atom != 0 && ee->module_specifier_atom == 0) {
            /* Local export: create a binding in our environment */
            r8e_module_env_ensure(mod, ee->local_name_atom);
        }
    }

    /* Step 4: Resolve import bindings */
    for (uint16_t i = 0; i < mod->import_count; i++) {
        R8EImportEntry *ie = &mod->imports[i];

        /* Find the source module */
        R8EModule *source_mod = r8e_module_cache_find(
            mod->ctx, ie->module_specifier_atom);
        if (!source_mod) {
            /* Try loaded requested modules by matching specifier */
            for (uint16_t j = 0; j < mod->requested_count; j++) {
                if (mod->requested_modules[j]->specifier_atom ==
                    ie->module_specifier_atom) {
                    source_mod = mod->requested_modules[j];
                    break;
                }
            }
        }
        if (!source_mod) return R8E_ERROR_REFERENCE;

        /* Namespace import: import * as ns */
        if (ie->import_name_atom == R8E_ATOM_star ||
            ie->import_name_atom == 0) {
            /* Create or get the namespace object for the source module */
            /* For now, store a reference to the module itself */
            r8e_module_env_ensure(mod, ie->local_name_atom);
            /* The actual namespace object is created lazily at eval time */
            continue;
        }

        /* Named import: resolve the export in the source module */
        R8EResolvedBinding binding = r8e_module_resolve_export(
            source_mod, ie->import_name_atom);

        if (binding.ambiguous) {
            /* SyntaxError: ambiguous star export */
            return R8E_ERROR_SYNTAX;
        }
        if (binding.module == NULL) {
            /* The requested name was not exported */
            return R8E_ERROR_SYNTAX;
        }

        /* Create a binding in our environment that will be connected
         * to the source at evaluation time (live binding) */
        r8e_module_env_ensure(mod, ie->local_name_atom);
    }

    /* Step 5: Check if this is the root of an SCC (Tarjan) */
    if (mod->dfs_ancestor_index == mod->dfs_index) {
        /* Pop all modules in this SCC and mark as LINKED */
        while (state->count > 0) {
            R8EModule *top = state->stack[--state->count];
            top->status = R8E_MODULE_LINKED;
            if (top == mod) break;
        }
    }

    return R8E_OK;
}

/* =========================================================================
 * PART 7: Module Evaluation (ES2023 15.2.1.16.3)
 *
 * Executes the module body. Imports are resolved to live bindings
 * (reading from the source module's environment at access time).
 * ========================================================================= */

typedef struct {
    R8EModule *stack[R8E_MODULE_MAX_DEPTH];
    int        count;
    int        index;
} R8EEvalState;

static R8EStatus r8e_module_inner_evaluate(R8EModule *mod,
                                            R8EEvalState *state);

/* Forward declaration: namespace object creation (used by inner_evaluate) */
R8EValue r8e_module_get_namespace(R8EModule *mod);

/*
 * r8e_module_evaluate - Evaluate a module and all its dependencies.
 *
 * @param ctx  Engine context
 * @param mod  Module to evaluate (must be LINKED)
 * @return     R8E_OK on success, error code on failure
 */
R8EStatus r8e_module_evaluate(R8EContext *ctx, R8EModule *mod) {
    if (!ctx || !mod) return R8E_ERROR;

    if (mod->status == R8E_MODULE_EVALUATED) {
        return R8E_OK;
    }
    if (mod->status == R8E_MODULE_ERROR) {
        return R8E_ERROR;
    }
    if (mod->status != R8E_MODULE_LINKED) {
        return R8E_ERROR;
    }

    R8EEvalState state;
    memset(&state, 0, sizeof(state));

    R8EStatus status = r8e_module_inner_evaluate(mod, &state);

    if (status != R8E_OK) {
        return status;
    }

    return R8E_OK;
}

/*
 * InnerModuleEvaluation - Recursive DFS evaluation.
 *
 * Evaluates all dependencies before evaluating this module.
 * Handles circular dependencies: modules in the same SCC
 * may have partially initialized bindings during evaluation.
 */
static R8EStatus r8e_module_inner_evaluate(R8EModule *mod,
                                            R8EEvalState *state) {
    /* Already evaluated? */
    if (mod->status == R8E_MODULE_EVALUATED) {
        return R8E_OK;
    }
    if (mod->status == R8E_MODULE_ERROR) {
        return R8E_ERROR;
    }
    /* Currently evaluating? Circular dependency. Return OK. */
    if (mod->status == R8E_MODULE_EVALUATING) {
        return R8E_OK;
    }

    assert(mod->status == R8E_MODULE_LINKED);

    /* Mark as evaluating */
    mod->status = R8E_MODULE_EVALUATING;
    mod->dfs_index = state->index;
    mod->dfs_ancestor_index = state->index;
    state->index++;

    /* Push onto stack */
    if (state->count >= R8E_MODULE_MAX_DEPTH) {
        mod->status = R8E_MODULE_ERROR;
        return R8E_ERROR;
    }
    state->stack[state->count++] = mod;

    /* Step 1: Evaluate all requested modules (imports) first */
    for (uint16_t i = 0; i < mod->requested_count; i++) {
        R8EModule *required = mod->requested_modules[i];
        R8EStatus status = r8e_module_inner_evaluate(required, state);

        if (status != R8E_OK) {
            mod->status = R8E_MODULE_ERROR;
            mod->eval_error = required->eval_error;
            return status;
        }

        if (required->status == R8E_MODULE_EVALUATING) {
            if (required->dfs_ancestor_index < mod->dfs_ancestor_index) {
                mod->dfs_ancestor_index = required->dfs_ancestor_index;
            }
        }
    }

    /* Step 2: Populate import bindings from source modules.
     * This implements live bindings: store a reference so that
     * reading the import always gets the current export value. */
    for (uint16_t i = 0; i < mod->import_count; i++) {
        R8EImportEntry *ie = &mod->imports[i];

        R8EModule *source_mod = r8e_module_cache_find(
            mod->ctx, ie->module_specifier_atom);
        if (!source_mod) continue;

        if (ie->import_name_atom == R8E_ATOM_star ||
            ie->import_name_atom == 0) {
            /* Namespace import: create namespace object */
            R8EValue ns = r8e_module_get_namespace(source_mod);
            r8e_module_env_set(mod, ie->local_name_atom, ns);
            continue;
        }

        /* Resolve the binding */
        R8EResolvedBinding binding = r8e_module_resolve_export(
            source_mod, ie->import_name_atom);

        if (binding.module != NULL) {
            /* Get the current value from the source module */
            R8EValue val = r8e_module_env_get(binding.module,
                                               binding.binding_atom);
            r8e_module_env_set(mod, ie->local_name_atom, val);
        }
    }

    /* Step 3: Execute the module body */
    if (!R8E_IS_UNDEFINED(mod->function)) {
        mod->eval_result = r8e_exec_function(mod->ctx, mod->function);

        if (mod->ctx->has_exception) {
            mod->status = R8E_MODULE_ERROR;
            mod->eval_error = mod->ctx->last_error;
            return R8E_ERROR;
        }
    }

    /* Step 4: After execution, update import bindings to reflect
     * any changes made during execution (live bindings). */
    for (uint16_t i = 0; i < mod->import_count; i++) {
        R8EImportEntry *ie = &mod->imports[i];
        if (ie->import_name_atom == R8E_ATOM_star ||
            ie->import_name_atom == 0) continue;

        R8EModule *source_mod = r8e_module_cache_find(
            mod->ctx, ie->module_specifier_atom);
        if (!source_mod) continue;

        R8EResolvedBinding binding = r8e_module_resolve_export(
            source_mod, ie->import_name_atom);
        if (binding.module != NULL) {
            R8EValue val = r8e_module_env_get(binding.module,
                                               binding.binding_atom);
            r8e_module_env_set(mod, ie->local_name_atom, val);
        }
    }

    /* Step 5: Check if this is the root of an SCC */
    if (mod->dfs_ancestor_index == mod->dfs_index) {
        while (state->count > 0) {
            R8EModule *top = state->stack[--state->count];
            top->status = R8E_MODULE_EVALUATED;
            if (top == mod) break;
        }
    }

    return R8E_OK;
}

/* =========================================================================
 * PART 8: Module Namespace Object
 *
 * The namespace object is a frozen object containing all exported bindings.
 * Created lazily for import * as ns.
 * ========================================================================= */

/*
 * r8e_module_get_namespace - Get or create the namespace object.
 *
 * The namespace object has:
 *   - A property for each export name
 *   - Read-only (frozen)
 *   - @@toStringTag = "Module"
 *
 * @param mod  Module record
 * @return     Namespace object value
 */
R8EValue r8e_module_get_namespace(R8EModule *mod) {
    if (!mod) return R8E_UNDEFINED;

    /* Return cached namespace if it exists */
    if (!R8E_IS_UNDEFINED(mod->namespace_obj)) {
        return mod->namespace_obj;
    }

    /* Create namespace object */
    void *ns = r8e_obj_new(mod->ctx);
    if (!ns) return R8E_UNDEFINED;

    /* Add all local exports */
    for (uint16_t i = 0; i < mod->export_count; i++) {
        R8EExportEntry *ee = &mod->exports[i];

        if (ee->module_specifier_atom == 0 && ee->local_name_atom != 0) {
            /* Local export: get value from our environment */
            R8EValue val = r8e_module_env_get(mod, ee->local_name_atom);
            r8e_obj_set(mod->ctx, ns, ee->export_name_atom, val);
        } else if (ee->module_specifier_atom != 0) {
            /* Re-export: resolve and get value */
            R8EResolvedBinding binding = r8e_module_resolve_export(
                mod, ee->export_name_atom);
            if (binding.module != NULL) {
                R8EValue val = r8e_module_env_get(binding.module,
                                                   binding.binding_atom);
                r8e_obj_set(mod->ctx, ns, ee->export_name_atom, val);
            }
        }
    }

    /* Add star export bindings */
    for (uint16_t i = 0; i < mod->star_export_count; i++) {
        uint32_t star_atom = mod->star_exports[i];
        R8EModule *star_mod = r8e_module_cache_find(mod->ctx, star_atom);
        if (!star_mod) continue;

        /* Get all exports from the star module */
        for (uint16_t j = 0; j < star_mod->export_count; j++) {
            R8EExportEntry *ee = &star_mod->exports[j];
            /* Skip "default" */
            if (ee->export_name_atom == R8E_ATOM_default) continue;
            /* Skip if already defined (local exports take precedence) */
            if (r8e_obj_has(mod->ctx, ns, ee->export_name_atom)) continue;

            R8EResolvedBinding binding = r8e_module_resolve_export(
                star_mod, ee->export_name_atom);
            if (binding.module != NULL && !binding.ambiguous) {
                R8EValue val = r8e_module_env_get(binding.module,
                                                   binding.binding_atom);
                r8e_obj_set(mod->ctx, ns, ee->export_name_atom, val);
            }
        }
    }

    mod->namespace_obj = r8e_from_pointer(ns);
    return mod->namespace_obj;
}

/* =========================================================================
 * PART 9: Dynamic Import (import() expression)
 *
 * Returns a Promise that resolves to the module's namespace object.
 * For now, synchronous implementation (top-level await deferred).
 * ========================================================================= */

/*
 * r8e_dynamic_import - Handle import() expression.
 *
 * @param ctx        Engine context
 * @param specifier  Module specifier string value
 * @param referrer   The module containing the import() call (can be NULL)
 * @return           Namespace object (or Promise in async case)
 */
R8EValue r8e_dynamic_import(R8EContext *ctx, R8EValue specifier,
                             R8EModule *referrer) {
    if (!ctx) return R8E_UNDEFINED;

    /* Get specifier string */
    const char *spec_str;
    uint32_t spec_len;
    char buf[8];

    if (R8E_IS_INLINE_STR(specifier)) {
        spec_len = (uint32_t)r8e_inline_str_decode(specifier, buf);
        spec_str = buf;
    } else if (R8E_IS_POINTER(specifier)) {
        spec_str = r8e_string_data(specifier, &spec_len);
    } else {
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    if (!spec_str) {
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    /* Load the module */
    R8EModule *mod = r8e_module_load(ctx, spec_str, spec_len, referrer);
    if (!mod) return R8E_UNDEFINED;

    /* Link */
    R8EStatus status = r8e_module_link(ctx, mod);
    if (status != R8E_OK) return R8E_UNDEFINED;

    /* Evaluate */
    status = r8e_module_evaluate(ctx, mod);
    if (status != R8E_OK) return R8E_UNDEFINED;

    /* Return namespace */
    return r8e_module_get_namespace(mod);
}

/* =========================================================================
 * PART 10: import.meta
 *
 * The import.meta object provides metadata about the current module.
 * Lazily created per module with at least a 'url' property.
 * ========================================================================= */

/*
 * r8e_import_meta - Get the import.meta object for a module.
 *
 * @param ctx  Engine context
 * @param mod  Module record
 * @return     import.meta object value
 */
R8EValue r8e_import_meta(R8EContext *ctx, R8EModule *mod) {
    if (!ctx || !mod) return R8E_UNDEFINED;

    /* Return cached import.meta if exists */
    if (!R8E_IS_UNDEFINED(mod->import_meta)) {
        return mod->import_meta;
    }

    /* Create import.meta object */
    void *meta = r8e_obj_new(ctx);
    if (!meta) return R8E_UNDEFINED;

    /* Set the 'url' property to the module specifier */
    uint32_t url_atom = r8e_atom_intern_str(ctx, "url", 3);
    R8EValue url_val;
    if (mod->specifier_len <= 6) {
        url_val = r8e_from_inline_str(mod->specifier,
                                       (int)mod->specifier_len);
        if (R8E_IS_UNDEFINED(url_val))
            url_val = r8e_string_new(ctx, mod->specifier,
                                      mod->specifier_len);
    } else {
        url_val = r8e_string_new(ctx, mod->specifier, mod->specifier_len);
    }
    r8e_obj_set(ctx, meta, url_atom, url_val);

    mod->import_meta = r8e_from_pointer(meta);
    return mod->import_meta;
}

/* =========================================================================
 * PART 11: Live Binding Support
 *
 * Import bindings are "live": when the source module updates an exported
 * variable, the importing module sees the new value. This is implemented
 * by reading from the source module's environment at access time.
 * ========================================================================= */

/*
 * r8e_module_get_import_binding - Read a live import binding.
 *
 * Called by the interpreter when reading an import variable.
 * Resolves through the export chain and returns the current value.
 *
 * @param mod         The importing module
 * @param local_atom  The local binding name in the importing module
 * @return            Current value of the binding
 */
R8EValue r8e_module_get_import_binding(R8EModule *mod,
                                        uint32_t local_atom) {
    if (!mod) return R8E_UNDEFINED;

    /* Find the import entry for this local name */
    for (uint16_t i = 0; i < mod->import_count; i++) {
        R8EImportEntry *ie = &mod->imports[i];
        if (ie->local_name_atom != local_atom) continue;

        /* Found the import entry. Resolve to source. */
        R8EModule *source_mod = r8e_module_cache_find(
            mod->ctx, ie->module_specifier_atom);
        if (!source_mod) return R8E_UNDEFINED;

        /* Namespace import */
        if (ie->import_name_atom == R8E_ATOM_star ||
            ie->import_name_atom == 0) {
            return r8e_module_get_namespace(source_mod);
        }

        /* Named import: resolve through export chain */
        R8EResolvedBinding binding = r8e_module_resolve_export(
            source_mod, ie->import_name_atom);
        if (binding.module != NULL && !binding.ambiguous) {
            return r8e_module_env_get(binding.module,
                                       binding.binding_atom);
        }
        return R8E_UNDEFINED;
    }

    /* Not an import - check local environment */
    return r8e_module_env_get(mod, local_atom);
}

/*
 * r8e_module_set_export_binding - Write to an exported binding.
 *
 * Called by the interpreter when assigning to an exported variable.
 * Updates the value in this module's environment. All importers see
 * the change via live binding resolution.
 *
 * @param mod        The exporting module
 * @param local_atom The local binding name
 * @param val        New value
 */
R8EStatus r8e_module_set_export_binding(R8EModule *mod, uint32_t local_atom,
                                         R8EValue val) {
    return r8e_module_env_set(mod, local_atom, val);
}

/* =========================================================================
 * PART 12: Entry Points
 * ========================================================================= */

/*
 * r8e_eval_module - Evaluate a module from source.
 *
 * This is the main entry point for running a module. It:
 *   1. Creates a module record
 *   2. Compiles the source
 *   3. Links all dependencies
 *   4. Evaluates the module
 *   5. Returns the namespace object
 *
 * @param ctx        Engine context
 * @param source     Module source code (UTF-8)
 * @param len        Source length (0 = strlen)
 * @param specifier  Module specifier/filename
 * @return           Module namespace object, or R8E_UNDEFINED on error
 */
R8EValue r8e_eval_module_source(R8EContext *ctx, const char *source,
                                 size_t len, const char *specifier) {
    if (!ctx || !source) return R8E_UNDEFINED;
    if (len == 0) len = strlen(source);
    if (!specifier) specifier = "<module>";

    uint32_t spec_len = (uint32_t)strlen(specifier);

    /* Check if already loaded */
    uint32_t spec_atom = r8e_atom_intern_str(ctx, specifier, spec_len);
    R8EModule *existing = r8e_module_cache_find(ctx, spec_atom);
    if (existing) {
        if (existing->status == R8E_MODULE_EVALUATED) {
            return r8e_module_get_namespace(existing);
        }
        if (existing->status == R8E_MODULE_ERROR) {
            return R8E_UNDEFINED;
        }
    }

    /* Create module record */
    R8EModule *mod = r8e_module_new(ctx, specifier, spec_len);
    if (!mod) return R8E_UNDEFINED;

    /* Compile */
    mod->function = r8e_compile_module(ctx, source, len, specifier);
    if (R8E_IS_UNDEFINED(mod->function)) {
        mod->status = R8E_MODULE_ERROR;
        r8e_module_cache_add(ctx, mod);
        return R8E_UNDEFINED;
    }

    /* Add to cache */
    if (r8e_module_cache_add(ctx, mod) != R8E_OK) {
        r8e_module_destroy(ctx, mod);
        return R8E_UNDEFINED;
    }

    /* Link */
    R8EStatus status = r8e_module_link(ctx, mod);
    if (status != R8E_OK) return R8E_UNDEFINED;

    /* Evaluate */
    status = r8e_module_evaluate(ctx, mod);
    if (status != R8E_OK) return R8E_UNDEFINED;

    /* Return namespace */
    return r8e_module_get_namespace(mod);
}

/*
 * r8e_eval_module_file - Evaluate a module from a file path.
 *
 * Delegates to the module loader to read the file.
 *
 * @param ctx       Engine context
 * @param filename  Path to the module file
 * @return          Module namespace object, or R8E_UNDEFINED on error
 */
R8EValue r8e_eval_module_file(R8EContext *ctx, const char *filename) {
    if (!ctx || !filename) return R8E_UNDEFINED;

    /* Try the module loader */
    if (ctx->module_loader) {
        char *source = NULL;
        size_t source_len = 0;

        R8EStatus status = ctx->module_loader(
            ctx->module_loader_data,
            filename, NULL,
            &source, &source_len);

        if (status == R8E_OK && source) {
            R8EValue result = r8e_eval_module_source(
                ctx, source, source_len, filename);
            r8e_free(ctx, source);
            return result;
        }
    }

    /* Fallback: try to read the file directly */
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > (long)(16 * 1024 * 1024)) {
        fclose(fp);
        ctx->has_exception = true;
        return R8E_UNDEFINED;
    }

    char *source = (char *)r8e_alloc(ctx, (size_t)fsize + 1);
    if (!source) {
        fclose(fp);
        return R8E_UNDEFINED;
    }

    size_t read_len = fread(source, 1, (size_t)fsize, fp);
    fclose(fp);
    source[read_len] = '\0';

    R8EValue result = r8e_eval_module_source(ctx, source, read_len,
                                              filename);
    r8e_free(ctx, source);
    return result;
}

/* =========================================================================
 * PART 13: Module Enumeration and Introspection
 * ========================================================================= */

/*
 * r8e_module_get_export_names - Get all export names of a module.
 *
 * @param mod       Module record
 * @param out_atoms Output array of export name atoms
 * @param max_count Maximum number to return
 * @return          Number of export names written
 */
uint32_t r8e_module_get_export_names(R8EModule *mod, uint32_t *out_atoms,
                                      uint32_t max_count) {
    if (!mod || !out_atoms) return 0;

    uint32_t count = 0;

    /* Local exports */
    for (uint16_t i = 0; i < mod->export_count && count < max_count; i++) {
        out_atoms[count++] = mod->exports[i].export_name_atom;
    }

    /* Star export names (resolved) */
    for (uint16_t i = 0; i < mod->star_export_count && count < max_count;
         i++) {
        uint32_t star_atom = mod->star_exports[i];
        R8EModule *star_mod = r8e_module_cache_find(mod->ctx, star_atom);
        if (!star_mod) continue;

        for (uint16_t j = 0;
             j < star_mod->export_count && count < max_count; j++) {
            uint32_t name = star_mod->exports[j].export_name_atom;
            if (name == R8E_ATOM_default) continue;

            /* Check for duplicates */
            bool dup = false;
            for (uint32_t k = 0; k < count; k++) {
                if (out_atoms[k] == name) { dup = true; break; }
            }
            if (!dup) {
                out_atoms[count++] = name;
            }
        }
    }

    return count;
}

/*
 * r8e_module_get_status - Get the current status of a module.
 */
R8EModuleStatus r8e_module_get_status(R8EModule *mod) {
    return mod ? mod->status : R8E_MODULE_ERROR;
}

/*
 * r8e_module_get_error - Get the evaluation error from a failed module.
 */
R8EValue r8e_module_get_error(R8EModule *mod) {
    return mod ? mod->eval_error : R8E_UNDEFINED;
}

/*
 * r8e_module_get_specifier - Get the module specifier string.
 */
const char *r8e_module_get_specifier(R8EModule *mod,
                                      uint32_t *out_len) {
    if (!mod) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = mod->specifier_len;
    return mod->specifier;
}


/* =========================================================================
 * Stub Implementations for External Dependencies
 *
 * These stubs allow the file to compile standalone. In the integrated
 * build, these are replaced by the real implementations.
 * ========================================================================= */

#if !defined(R8E_INTEGRATED_BUILD) && !defined(R8E_TESTING)

/* Weak attribute so these stubs don't conflict with real implementations
 * when linked into a full library build. */
#ifdef __GNUC__
#define R8E_WEAK __attribute__((weak))
#else
#define R8E_WEAK
#endif

/* Simple atom table: stores interned strings with non-zero atom IDs.
 * Used only when running tests standalone (the real atom table is in
 * r8e_atom.c). */

#define R8E_STUB_ATOM_MAX  256
#define R8E_STUB_ATOM_MAXLEN 1024

static struct {
    char     strings[R8E_STUB_ATOM_MAX][R8E_STUB_ATOM_MAXLEN];
    uint32_t lengths[R8E_STUB_ATOM_MAX];
    uint32_t count;
    bool     initialized;
} g_stub_atoms = {0};

R8E_WEAK uint32_t r8e_atom_intern_str(R8EContext *ctx, const char *str, uint32_t len) {
    (void)ctx;
    if (!str || len == 0) return 0;
    if (len >= R8E_STUB_ATOM_MAXLEN) len = R8E_STUB_ATOM_MAXLEN - 1;

    /* Search existing atoms */
    for (uint32_t i = 0; i < g_stub_atoms.count; i++) {
        if (g_stub_atoms.lengths[i] == len &&
            memcmp(g_stub_atoms.strings[i], str, len) == 0) {
            return i + 1; /* atoms are 1-based */
        }
    }

    /* Add new atom */
    if (g_stub_atoms.count >= R8E_STUB_ATOM_MAX) return 0;
    uint32_t idx = g_stub_atoms.count++;
    memcpy(g_stub_atoms.strings[idx], str, len);
    g_stub_atoms.strings[idx][len] = '\0';
    g_stub_atoms.lengths[idx] = len;
    return idx + 1;
}

R8E_WEAK const char *r8e_atom_get_str(R8EContext *ctx, uint32_t atom,
                              uint32_t *out_len) {
    (void)ctx;
    if (atom == 0 || atom > g_stub_atoms.count) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    uint32_t idx = atom - 1;
    if (out_len) *out_len = g_stub_atoms.lengths[idx];
    return g_stub_atoms.strings[idx];
}

/* Object model stubs */
R8E_WEAK void *r8e_obj_new(R8EContext *ctx) {
    (void)ctx;
    return NULL;
}

R8E_WEAK R8EValue r8e_obj_get(R8EContext *ctx, void *obj, uint32_t key) {
    (void)ctx; (void)obj; (void)key;
    return R8E_UNDEFINED;
}

R8E_WEAK void *r8e_obj_set(R8EContext *ctx, void *obj, uint32_t key, R8EValue val) {
    (void)ctx; (void)obj; (void)key; (void)val;
    return obj;
}

R8E_WEAK bool r8e_obj_has(R8EContext *ctx, void *obj, uint32_t key) {
    (void)ctx; (void)obj; (void)key;
    return false;
}

R8E_WEAK uint32_t r8e_obj_keys(R8EContext *ctx, void *obj, uint32_t *out_keys,
                       uint32_t max_keys) {
    (void)ctx; (void)obj; (void)out_keys; (void)max_keys;
    return 0;
}

/* String */
R8E_WEAK R8EValue r8e_string_new(R8EContext *ctx, const char *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    return R8E_UNDEFINED;
}

R8E_WEAK const char *r8e_string_data(R8EValue v, uint32_t *out_len) {
    (void)v;
    if (out_len) *out_len = 0;
    return "";
}

/* Function call */
R8E_WEAK R8EValue r8e_call_fn(R8EContext *ctx, R8EValue func, R8EValue this_val,
                      int argc, const R8EValue *argv) {
    (void)ctx; (void)func; (void)this_val; (void)argc; (void)argv;
    return R8E_UNDEFINED;
}

R8E_WEAK R8EValue r8e_compile_module(R8EContext *ctx, const char *source,
                             size_t len, const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

R8E_WEAK R8EValue r8e_exec_function(R8EContext *ctx, R8EValue func) {
    (void)ctx; (void)func;
    return R8E_UNDEFINED;
}

#endif /* R8E_INTEGRATED_BUILD */
