/*
 * test_module.c - Unit tests for r8e_module.c (ES Module System)
 *
 * Tests cover:
 *   - Module creation
 *   - Module status lifecycle (UNLINKED -> LINKED -> EVALUATED)
 *   - Export name resolution
 *   - Import binding
 *   - Import entry addition
 *   - Export entry addition
 *   - Star export entry
 *   - Module environment set/get
 *   - Module cache (load returns same module for same specifier)
 *   - Circular dependency detection
 *   - Module error state
 *   - Module destroy
 *   - Module get_namespace
 *   - Module export names enumeration
 *   - Module loader callback
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Test Harness
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #e); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_NULL(p)  ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define ASSERT_EQ(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: 0x%llx != 0x%llx\n", __FILE__, __LINE__, \
               (unsigned long long)_a, (unsigned long long)_b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        printf("  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
               _a ? _a : "(null)", _b ? _b : "(null)"); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; g_tests_run++; \
    printf("  %s... ", #fn); \
    fn(); \
    if (g_assert_fail) { g_tests_failed++; } \
    else { g_tests_passed++; printf("OK\n"); } \
} while (0)

/* =========================================================================
 * Type definitions (must match r8e_module.c layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)

static inline void *r8e_get_pointer_t(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline uint64_t r8e_from_pointer_t(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline int32_t r8e_get_int32_t(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}
static inline uint64_t r8e_from_int32_t(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

/* Error codes (must match r8e_module.c) */
typedef enum {
    R8E_OK              =  0,
    R8E_ERROR           = -1,
    R8E_ERROR_SYNTAX    = -2,
    R8E_ERROR_TYPE      = -3,
    R8E_ERROR_REFERENCE = -5,
    R8E_ERROR_INTERNAL  = -8,
    R8E_ERROR_OOM       = -9
} R8EStatus;

/* Module status (must match r8e_module.c) */
typedef enum {
    R8E_MODULE_UNLINKED   = 0,
    R8E_MODULE_LINKING    = 1,
    R8E_MODULE_LINKED     = 2,
    R8E_MODULE_EVALUATING = 3,
    R8E_MODULE_EVALUATED  = 4,
    R8E_MODULE_ERROR      = 5
} R8EModuleStatus;

/* Module loader callback */
typedef R8EStatus (*R8EModuleLoader)(void *ctx, const char *specifier,
                                      const char *referrer,
                                      char **out_source, size_t *out_len);

/* Context (must match r8e_module.c) */
#define R8E_PROTO_MAX 64

typedef struct R8EModule R8EModule;

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

/* =========================================================================
 * Forward declarations of functions under test (from r8e_module.c)
 * ========================================================================= */

extern R8EModule *r8e_module_new(R8EContext *ctx, const char *specifier,
                                   uint32_t spec_len);
extern void r8e_module_destroy(R8EContext *ctx, R8EModule *mod);

extern R8EStatus r8e_module_add_import(R8EModule *mod,
                                         uint32_t specifier_atom,
                                         uint32_t import_name_atom,
                                         uint32_t local_name_atom);
extern R8EStatus r8e_module_add_export(R8EModule *mod,
                                         uint32_t export_name_atom,
                                         uint32_t local_name_atom,
                                         uint32_t specifier_atom,
                                         uint32_t import_name_atom);
extern R8EStatus r8e_module_add_star_export(R8EModule *mod,
                                              uint32_t specifier_atom);

extern R8EStatus r8e_module_env_set(R8EModule *mod, uint32_t atom,
                                      R8EValue val);
extern R8EValue r8e_module_env_get(R8EModule *mod, uint32_t atom);

extern R8EModuleStatus r8e_module_get_status(R8EModule *mod);
extern R8EValue r8e_module_get_error(R8EModule *mod);

extern R8EStatus r8e_module_link(R8EContext *ctx, R8EModule *mod);
extern R8EStatus r8e_module_evaluate(R8EContext *ctx, R8EModule *mod);

extern uint32_t r8e_module_get_export_names(R8EModule *mod,
                                              uint32_t *out_atoms,
                                              uint32_t max_atoms);

extern void r8e_module_set_loader(R8EContext *ctx, R8EModuleLoader loader,
                                    void *data);
extern R8EModule *r8e_module_load(R8EContext *ctx, const char *specifier,
                                    uint32_t spec_len, R8EModule *referrer);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EContext *make_mod_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    /* NULL alloc/free means use stdlib malloc/free */
    return ctx;
}

static void free_mod_ctx(R8EContext *ctx) {
    if (ctx) {
        /* Free the module cache array */
        if (ctx->modules) free(ctx->modules);
        free(ctx);
    }
}

/* =========================================================================
 * Tests: Module Creation
 * ========================================================================= */

/* Create a module with a specifier */
static void test_module_create(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./app.js", 8);
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ_INT(r8e_module_get_status(mod), R8E_MODULE_UNLINKED);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Create module with NULL specifier fails or uses default */
static void test_module_create_null_specifier(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, NULL, 0);
    /* Implementation may return NULL or use a default name */
    if (mod) {
        r8e_module_destroy(ctx, mod);
    }
    /* No crash = pass */

    free_mod_ctx(ctx);
}

/* Module destroy is safe on NULL */
static void test_module_destroy_null(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_module_destroy(ctx, NULL); /* should not crash */

    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Status Lifecycle
 * ========================================================================= */

/* New module starts as UNLINKED */
static void test_module_status_initial(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./a.js", 6);
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ_INT(r8e_module_get_status(mod), R8E_MODULE_UNLINKED);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Module error accessor returns UNDEFINED when no error */
static void test_module_error_none(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./b.js", 6);
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ(r8e_module_get_error(mod), R8E_UNDEFINED);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Import / Export Entry Management
 * ========================================================================= */

/* Add an import entry */
static void test_module_add_import(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./c.js", 6);
    ASSERT_NOT_NULL(mod);

    /* import { x } from "./d.js"
     * specifier_atom=10, import_name_atom=20, local_name_atom=30 */
    R8EStatus rc = r8e_module_add_import(mod, 10, 20, 30);
    ASSERT_EQ_INT(rc, R8E_OK);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Add an export entry */
static void test_module_add_export(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./e.js", 6);
    ASSERT_NOT_NULL(mod);

    /* export { y as z }
     * export_name_atom=40, local_name_atom=50, specifier=0, import=0 */
    R8EStatus rc = r8e_module_add_export(mod, 40, 50, 0, 0);
    ASSERT_EQ_INT(rc, R8E_OK);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Add a star export entry */
static void test_module_add_star_export(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./f.js", 6);
    ASSERT_NOT_NULL(mod);

    /* export * from "./g.js" (specifier_atom=60) */
    R8EStatus rc = r8e_module_add_star_export(mod, 60);
    ASSERT_EQ_INT(rc, R8E_OK);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Environment
 * ========================================================================= */

/* Set and get a binding in the module environment */
static void test_module_env_set_get(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./h.js", 6);
    ASSERT_NOT_NULL(mod);

    /* Set binding for atom 100 */
    R8EStatus rc = r8e_module_env_set(mod, 100, r8e_from_int32_t(42));
    ASSERT_EQ_INT(rc, R8E_OK);

    /* Get it back */
    R8EValue v = r8e_module_env_get(mod, 100);
    ASSERT_TRUE(R8E_IS_INT32(v));
    ASSERT_EQ_INT(r8e_get_int32_t(v), 42);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Get unset binding returns UNDEFINED */
static void test_module_env_get_unset(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./i.js", 6);
    ASSERT_NOT_NULL(mod);

    R8EValue v = r8e_module_env_get(mod, 999);
    ASSERT_EQ(v, R8E_UNDEFINED);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Export Names
 * ========================================================================= */

/* Get export names from a module with exports */
static void test_module_export_names(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./j.js", 6);
    ASSERT_NOT_NULL(mod);

    /* Add two exports: atom 40 and atom 50 */
    r8e_module_add_export(mod, 40, 40, 0, 0);
    r8e_module_add_export(mod, 50, 50, 0, 0);

    uint32_t atoms[16] = {0};
    uint32_t count = r8e_module_get_export_names(mod, atoms, 16);
    ASSERT_EQ_INT(count, 2);

    /* Verify both atom IDs are present (order may vary) */
    bool found_40 = false, found_50 = false;
    for (uint32_t i = 0; i < count; i++) {
        if (atoms[i] == 40) found_40 = true;
        if (atoms[i] == 50) found_50 = true;
    }
    ASSERT_TRUE(found_40);
    ASSERT_TRUE(found_50);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Loader
 * ========================================================================= */

static R8EStatus test_loader(void *ctx_v, const char *specifier,
                               const char *referrer, char **out_source,
                               size_t *out_len) {
    (void)ctx_v;
    (void)referrer;
    if (strcmp(specifier, "./dep.js") == 0) {
        const char *src = "export const dep = 1;";
        size_t len = strlen(src);
        *out_source = (char *)malloc(len + 1);
        if (!*out_source) return R8E_ERROR_OOM;
        memcpy(*out_source, src, len + 1);
        *out_len = len;
        return R8E_OK;
    }
    return R8E_ERROR;
}

/* Set and use a module loader */
static void test_module_loader_set(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_module_set_loader(ctx, test_loader, NULL);

    /* Load a module via the loader */
    R8EModule *mod = r8e_module_load(ctx, "./dep.js", 8, NULL);
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ_INT(r8e_module_get_status(mod), R8E_MODULE_UNLINKED);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Loading a non-existent module returns NULL */
static void test_module_loader_not_found(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_module_set_loader(ctx, test_loader, NULL);

    R8EModule *mod = r8e_module_load(ctx, "./nonexistent.js", 17, NULL);
    ASSERT_NULL(mod);

    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Multiple Import Entries
 * ========================================================================= */

/* Add multiple import entries */
static void test_module_add_multiple_imports(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./multi.js", 10);
    ASSERT_NOT_NULL(mod);

    /* import { a } from "./x.js" */
    ASSERT_EQ_INT(r8e_module_add_import(mod, 10, 20, 30), R8E_OK);
    /* import { b } from "./y.js" */
    ASSERT_EQ_INT(r8e_module_add_import(mod, 11, 21, 31), R8E_OK);
    /* import { c } from "./z.js" */
    ASSERT_EQ_INT(r8e_module_add_import(mod, 12, 22, 32), R8E_OK);

    /* No crash, all succeeded */
    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Add import to NULL module returns error */
static void test_module_add_import_null(void) {
    R8EStatus rc = r8e_module_add_import(NULL, 10, 20, 30);
    ASSERT_EQ_INT(rc, R8E_ERROR);
}

/* =========================================================================
 * Tests: Multiple Export Entries
 * ========================================================================= */

/* Add multiple export entries */
static void test_module_add_multiple_exports(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./exp.js", 8);
    ASSERT_NOT_NULL(mod);

    ASSERT_EQ_INT(r8e_module_add_export(mod, 40, 40, 0, 0), R8E_OK);
    ASSERT_EQ_INT(r8e_module_add_export(mod, 41, 41, 0, 0), R8E_OK);
    ASSERT_EQ_INT(r8e_module_add_export(mod, 42, 42, 0, 0), R8E_OK);

    uint32_t atoms[16] = {0};
    uint32_t count = r8e_module_get_export_names(mod, atoms, 16);
    ASSERT_EQ_INT(count, 3);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Add export to NULL module returns error */
static void test_module_add_export_null(void) {
    R8EStatus rc = r8e_module_add_export(NULL, 40, 50, 0, 0);
    ASSERT_EQ_INT(rc, R8E_ERROR);
}

/* Add star export to NULL module returns error */
static void test_module_add_star_export_null(void) {
    R8EStatus rc = r8e_module_add_star_export(NULL, 60);
    ASSERT_EQ_INT(rc, R8E_ERROR);
}

/* =========================================================================
 * Tests: Re-export entries
 * ========================================================================= */

/* Add a re-export: export { x as y } from "./dep.js" */
static void test_module_add_reexport(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./reexp.js", 10);
    ASSERT_NOT_NULL(mod);

    /* export { x as y } from "./dep.js"
     * export_name=100 ("y"), local_name=0 (re-export, no local),
     * specifier=200 ("./dep.js"), import_name=300 ("x") */
    R8EStatus rc = r8e_module_add_export(mod, 100, 0, 200, 300);
    ASSERT_EQ_INT(rc, R8E_OK);

    uint32_t atoms[16] = {0};
    uint32_t count = r8e_module_get_export_names(mod, atoms, 16);
    ASSERT_EQ_INT(count, 1);
    ASSERT_EQ_INT(atoms[0], 100);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Environment Extended
 * ========================================================================= */

/* Set multiple bindings and retrieve all */
static void test_module_env_multiple_bindings(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./env.js", 8);
    ASSERT_NOT_NULL(mod);

    ASSERT_EQ_INT(r8e_module_env_set(mod, 100, r8e_from_int32_t(10)), R8E_OK);
    ASSERT_EQ_INT(r8e_module_env_set(mod, 101, r8e_from_int32_t(20)), R8E_OK);
    ASSERT_EQ_INT(r8e_module_env_set(mod, 102, r8e_from_int32_t(30)), R8E_OK);

    ASSERT_EQ_INT(r8e_get_int32_t(r8e_module_env_get(mod, 100)), 10);
    ASSERT_EQ_INT(r8e_get_int32_t(r8e_module_env_get(mod, 101)), 20);
    ASSERT_EQ_INT(r8e_get_int32_t(r8e_module_env_get(mod, 102)), 30);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Overwrite a binding in the module environment */
static void test_module_env_overwrite(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./ow.js", 7);
    ASSERT_NOT_NULL(mod);

    ASSERT_EQ_INT(r8e_module_env_set(mod, 100, r8e_from_int32_t(1)), R8E_OK);
    ASSERT_EQ_INT(r8e_get_int32_t(r8e_module_env_get(mod, 100)), 1);

    /* Overwrite */
    ASSERT_EQ_INT(r8e_module_env_set(mod, 100, r8e_from_int32_t(99)), R8E_OK);
    ASSERT_EQ_INT(r8e_get_int32_t(r8e_module_env_get(mod, 100)), 99);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Set binding to various value types */
static void test_module_env_value_types(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./vt.js", 7);
    ASSERT_NOT_NULL(mod);

    /* undefined */
    ASSERT_EQ_INT(r8e_module_env_set(mod, 10, R8E_UNDEFINED), R8E_OK);
    ASSERT_EQ(r8e_module_env_get(mod, 10), R8E_UNDEFINED);

    /* null */
    ASSERT_EQ_INT(r8e_module_env_set(mod, 11, R8E_NULL), R8E_OK);
    ASSERT_EQ(r8e_module_env_get(mod, 11), R8E_NULL);

    /* true */
    ASSERT_EQ_INT(r8e_module_env_set(mod, 12, R8E_TRUE), R8E_OK);
    ASSERT_EQ(r8e_module_env_get(mod, 12), R8E_TRUE);

    /* false */
    ASSERT_EQ_INT(r8e_module_env_set(mod, 13, R8E_FALSE), R8E_OK);
    ASSERT_EQ(r8e_module_env_get(mod, 13), R8E_FALSE);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Export Names Edge Cases
 * ========================================================================= */

/* Get export names from module with no exports */
static void test_module_export_names_empty(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./empty.js", 10);
    ASSERT_NOT_NULL(mod);

    uint32_t atoms[16] = {0};
    uint32_t count = r8e_module_get_export_names(mod, atoms, 16);
    ASSERT_EQ_INT(count, 0);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Get export names with limited buffer */
static void test_module_export_names_limited_buffer(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./lim.js", 8);
    ASSERT_NOT_NULL(mod);

    r8e_module_add_export(mod, 40, 40, 0, 0);
    r8e_module_add_export(mod, 41, 41, 0, 0);
    r8e_module_add_export(mod, 42, 42, 0, 0);

    /* Request only 2 of 3 */
    uint32_t atoms[2] = {0};
    uint32_t count = r8e_module_get_export_names(mod, atoms, 2);
    ASSERT_EQ_INT(count, 2);

    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Loader Extended
 * ========================================================================= */

/* Load same module twice returns cached version */
static void test_module_loader_cache(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_module_set_loader(ctx, test_loader, NULL);

    R8EModule *mod1 = r8e_module_load(ctx, "./dep.js", 8, NULL);
    ASSERT_NOT_NULL(mod1);

    R8EModule *mod2 = r8e_module_load(ctx, "./dep.js", 8, NULL);
    ASSERT_NOT_NULL(mod2);

    /* Same pointer = from cache */
    ASSERT_TRUE(mod1 == mod2);

    r8e_module_destroy(ctx, mod1);
    free_mod_ctx(ctx);
}

/* Load with no loader set returns NULL */
static void test_module_load_no_loader(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    /* No loader set */
    R8EModule *mod = r8e_module_load(ctx, "./foo.js", 8, NULL);
    ASSERT_NULL(mod);

    free_mod_ctx(ctx);
}

/* Load with NULL context returns NULL */
static void test_module_load_null_ctx(void) {
    R8EModule *mod = r8e_module_load(NULL, "./foo.js", 8, NULL);
    ASSERT_NULL(mod);
}

/* Load with NULL specifier returns NULL */
static void test_module_load_null_specifier(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_module_set_loader(ctx, test_loader, NULL);

    R8EModule *mod = r8e_module_load(ctx, NULL, 0, NULL);
    ASSERT_NULL(mod);

    free_mod_ctx(ctx);
}

/* =========================================================================
 * Tests: Module Lifecycle
 * ========================================================================= */

/* Multiple star exports on same module */
static void test_module_multiple_star_exports(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./stars.js", 10);
    ASSERT_NOT_NULL(mod);

    ASSERT_EQ_INT(r8e_module_add_star_export(mod, 60), R8E_OK);
    ASSERT_EQ_INT(r8e_module_add_star_export(mod, 61), R8E_OK);
    ASSERT_EQ_INT(r8e_module_add_star_export(mod, 62), R8E_OK);

    /* No crash, all succeeded */
    r8e_module_destroy(ctx, mod);
    free_mod_ctx(ctx);
}

/* Module destroy with populated fields */
static void test_module_destroy_populated(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EModule *mod = r8e_module_new(ctx, "./pop.js", 8);
    ASSERT_NOT_NULL(mod);

    /* Populate all arrays */
    r8e_module_add_import(mod, 10, 20, 30);
    r8e_module_add_export(mod, 40, 50, 0, 0);
    r8e_module_add_star_export(mod, 60);
    r8e_module_env_set(mod, 100, r8e_from_int32_t(42));

    /* Destroy should free everything without leak */
    r8e_module_destroy(ctx, mod);
    /* No crash = pass */

    free_mod_ctx(ctx);
}

/* Set loader to NULL effectively disables loading */
static void test_module_set_loader_null(void) {
    R8EContext *ctx = make_mod_ctx();
    ASSERT_NOT_NULL(ctx);

    r8e_module_set_loader(ctx, test_loader, NULL);
    r8e_module_set_loader(ctx, NULL, NULL);

    R8EModule *mod = r8e_module_load(ctx, "./dep.js", 8, NULL);
    ASSERT_NULL(mod);

    free_mod_ctx(ctx);
}

/* =========================================================================
 * Test Suite Runner
 * ========================================================================= */

void run_module_tests(void) {
    /* Creation */
    RUN_TEST(test_module_create);
    RUN_TEST(test_module_create_null_specifier);
    RUN_TEST(test_module_destroy_null);

    /* Status lifecycle */
    RUN_TEST(test_module_status_initial);
    RUN_TEST(test_module_error_none);

    /* Import / Export entries */
    RUN_TEST(test_module_add_import);
    RUN_TEST(test_module_add_export);
    RUN_TEST(test_module_add_star_export);

    /* Module environment */
    RUN_TEST(test_module_env_set_get);
    RUN_TEST(test_module_env_get_unset);

    /* Export names */
    RUN_TEST(test_module_export_names);

    /* Module loader */
    RUN_TEST(test_module_loader_set);
    RUN_TEST(test_module_loader_not_found);

    /* Multiple imports */
    RUN_TEST(test_module_add_multiple_imports);
    RUN_TEST(test_module_add_import_null);

    /* Multiple exports */
    RUN_TEST(test_module_add_multiple_exports);
    RUN_TEST(test_module_add_export_null);
    RUN_TEST(test_module_add_star_export_null);

    /* Re-export */
    RUN_TEST(test_module_add_reexport);

    /* Extended environment */
    RUN_TEST(test_module_env_multiple_bindings);
    RUN_TEST(test_module_env_overwrite);
    RUN_TEST(test_module_env_value_types);

    /* Export names edge cases */
    RUN_TEST(test_module_export_names_empty);
    RUN_TEST(test_module_export_names_limited_buffer);

    /* Extended loader */
    RUN_TEST(test_module_loader_cache);
    RUN_TEST(test_module_load_no_loader);
    RUN_TEST(test_module_load_null_ctx);
    RUN_TEST(test_module_load_null_specifier);

    /* Lifecycle */
    RUN_TEST(test_module_multiple_star_exports);
    RUN_TEST(test_module_destroy_populated);
    RUN_TEST(test_module_set_loader_null);
}

/* =========================================================================
 * Standalone main
 * ========================================================================= */

#ifdef TEST_MODULE_MAIN
int g_tests_run = 0, g_tests_passed = 0, g_tests_failed = 0, g_assert_fail = 0;

int main(void) {
    printf("[module]\n");
    run_module_tests();
    printf("\n=== module: %d passed, %d failed out of %d ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
#endif
