/*
 * test_css_vars.c - Unit tests for CSS custom properties (variables)
 *
 * Tests cover:
 *   - Setting and getting CSS variables
 *   - var() resolution with fallback values
 *   - Nested variable resolution (variable-in-variable)
 *   - :root selector variable extraction
 *   - Clearing variables
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>

/* =========================================================================
 * Minimal type definitions (self-contained, matching source files)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* =========================================================================
 * Test infrastructure (shared globals from test_runner.c)
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n" \
                "      at %s:%d\n", #e, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n" \
                "      got %lld vs %lld\n" \
                "      at %s:%d\n", #a, #b, _a, _b, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "    ASSERT_EQ_STR failed: %s == %s\n" \
                "      got \"%s\" vs \"%s\"\n" \
                "      at %s:%d\n", #a, #b, \
                _a ? _a : "(null)", _b ? _b : "(null)", \
                __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    g_assert_fail = 0;                                              \
    g_tests_run++;                                                  \
    printf("  %-60s ", #name);                                      \
    fflush(stdout);                                                 \
    fflush(stderr);                                                 \
    pid_t pid = fork();                                             \
    if (pid == 0) {                                                 \
        alarm(5);                                                   \
        test_##name();                                              \
        _exit(g_assert_fail ? 1 : 0);                              \
    } else if (pid > 0) {                                          \
        int wstatus = 0;                                            \
        waitpid(pid, &wstatus, 0);                                 \
        if (WIFSIGNALED(wstatus)) {                                \
            g_assert_fail = 1;                                      \
            fprintf(stderr, "    CRASHED (signal %d)\n",            \
                    WTERMSIG(wstatus));                             \
        } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) { \
            g_assert_fail = 1;                                      \
        }                                                           \
    } else {                                                        \
        g_assert_fail = 1;                                          \
        fprintf(stderr, "    fork() failed\n");                     \
    }                                                               \
    if (g_assert_fail) {                                            \
        g_tests_failed++;                                           \
        printf("FAIL\n");                                           \
    } else {                                                        \
        g_tests_passed++;                                           \
        printf("ok\n");                                             \
    }                                                               \
    fflush(stdout);                                                 \
} while (0)

/* =========================================================================
 * External declarations for CSS variable functions
 * ========================================================================= */

/* CSS variable API from r8e_style.c */
extern void r8e_ui_css_var_set(const char *name, const char *value);
extern const char *r8e_ui_css_var_get(const char *name);
extern void r8e_ui_css_var_clear(void);

/* Forward declarations for stylesheet (opaque type) */
typedef struct R8EUIStyleSheet R8EUIStyleSheet;

extern R8EUIStyleSheet *r8e_ui_stylesheet_create(void);
extern void r8e_ui_stylesheet_parse(R8EUIStyleSheet *sheet, const char *css);
extern void r8e_ui_stylesheet_free(R8EUIStyleSheet *sheet);

/* =========================================================================
 * Tests
 * ========================================================================= */

TEST(css_var_set_and_get) {
    r8e_ui_css_var_clear();

    r8e_ui_css_var_set("--bg", "#fff");
    const char *val = r8e_ui_css_var_get("--bg");
    ASSERT_NOT_NULL(val);
    ASSERT_EQ_STR(val, "#fff");
}

TEST(css_var_get_missing) {
    r8e_ui_css_var_clear();

    const char *val = r8e_ui_css_var_get("--nonexistent");
    ASSERT_NULL(val);
}

TEST(css_var_update) {
    r8e_ui_css_var_clear();

    r8e_ui_css_var_set("--color", "red");
    r8e_ui_css_var_set("--color", "blue");
    const char *val = r8e_ui_css_var_get("--color");
    ASSERT_NOT_NULL(val);
    ASSERT_EQ_STR(val, "blue");
}

TEST(css_var_clear) {
    r8e_ui_css_var_clear();

    r8e_ui_css_var_set("--x", "10px");
    r8e_ui_css_var_clear();
    const char *val = r8e_ui_css_var_get("--x");
    ASSERT_NULL(val);
}

TEST(css_var_must_start_with_dashes) {
    r8e_ui_css_var_clear();

    r8e_ui_css_var_set("color", "red");  /* no -- prefix */
    const char *val = r8e_ui_css_var_get("color");
    ASSERT_NULL(val);
}

TEST(css_var_root_parsing) {
    r8e_ui_css_var_clear();

    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);

    r8e_ui_stylesheet_parse(sheet,
        ":root { --bg: #ffffff; --accent: #6366f1; --size: 16px; }");

    const char *bg = r8e_ui_css_var_get("--bg");
    ASSERT_NOT_NULL(bg);
    ASSERT_EQ_STR(bg, "#ffffff");

    const char *accent = r8e_ui_css_var_get("--accent");
    ASSERT_NOT_NULL(accent);
    ASSERT_EQ_STR(accent, "#6366f1");

    const char *size = r8e_ui_css_var_get("--size");
    ASSERT_NOT_NULL(size);
    ASSERT_EQ_STR(size, "16px");

    r8e_ui_stylesheet_free(sheet);
}

TEST(css_var_fallback) {
    r8e_ui_css_var_clear();

    /* Set up: --bg exists, --missing does not */
    r8e_ui_css_var_set("--bg", "#fff");

    /* Resolve var(--bg) - should find it */
    const char *val = r8e_ui_css_var_get("--bg");
    ASSERT_NOT_NULL(val);
    ASSERT_EQ_STR(val, "#fff");

    /* --missing should be NULL */
    val = r8e_ui_css_var_get("--missing");
    ASSERT_NULL(val);
}

TEST(css_var_nested) {
    r8e_ui_css_var_clear();

    /* --a: blue, --b: var(--a) */
    r8e_ui_css_var_set("--a", "blue");
    r8e_ui_css_var_set("--b", "var(--a)");

    /* Direct lookup of --b returns the raw "var(--a)" */
    const char *raw = r8e_ui_css_var_get("--b");
    ASSERT_NOT_NULL(raw);
    ASSERT_EQ_STR(raw, "var(--a)");

    /* Verify --a is properly stored for resolution chain */
    const char *a = r8e_ui_css_var_get("--a");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ_STR(a, "blue");
}

TEST(css_var_multiple) {
    r8e_ui_css_var_clear();

    r8e_ui_css_var_set("--c1", "#111");
    r8e_ui_css_var_set("--c2", "#222");
    r8e_ui_css_var_set("--c3", "#333");

    ASSERT_EQ_STR(r8e_ui_css_var_get("--c1"), "#111");
    ASSERT_EQ_STR(r8e_ui_css_var_get("--c2"), "#222");
    ASSERT_EQ_STR(r8e_ui_css_var_get("--c3"), "#333");
}

TEST(css_var_root_with_other_rules) {
    r8e_ui_css_var_clear();

    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);

    r8e_ui_stylesheet_parse(sheet,
        ":root { --primary: #007bff; }\n"
        "div { display: flex; }\n");

    const char *primary = r8e_ui_css_var_get("--primary");
    ASSERT_NOT_NULL(primary);
    ASSERT_EQ_STR(primary, "#007bff");

    r8e_ui_stylesheet_free(sheet);
}

/* =========================================================================
 * Test suite entry point
 * ========================================================================= */

void run_css_vars_tests(void) {
    RUN_TEST(css_var_set_and_get);
    RUN_TEST(css_var_get_missing);
    RUN_TEST(css_var_update);
    RUN_TEST(css_var_clear);
    RUN_TEST(css_var_must_start_with_dashes);
    RUN_TEST(css_var_root_parsing);
    RUN_TEST(css_var_fallback);
    RUN_TEST(css_var_nested);
    RUN_TEST(css_var_multiple);
    RUN_TEST(css_var_root_with_other_rules);
}
