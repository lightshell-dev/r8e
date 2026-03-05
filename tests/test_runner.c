/*
 * test_runner.c - Minimal test harness for the r8e JavaScript engine
 *
 * Provides test macros, assertion helpers, and a main() that runs all
 * test suites and prints a summary.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Test Infrastructure
 * ========================================================================= */

int g_tests_run    = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_assert_fail  = 0;  /* set within a test to mark failure */

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    g_assert_fail = 0;                                              \
    g_tests_run++;                                                  \
    printf("  %-60s ", #name);                                      \
    test_##name();                                                  \
    if (g_assert_fail) {                                            \
        g_tests_failed++;                                           \
        printf("FAIL\n");                                           \
    } else {                                                        \
        g_tests_passed++;                                           \
        printf("ok\n");                                             \
    }                                                               \
} while (0)

#define ASSERT_TRUE(expr) do {                                      \
    if (!(expr)) {                                                  \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n"              \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_FALSE(expr) do {                                     \
    if (expr) {                                                     \
        fprintf(stderr, "    ASSERT_FALSE failed: %s\n"             \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ(a, b) do {                                        \
    uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);               \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ failed: %s == %s\n"          \
                "      got 0x%llx vs 0x%llx\n"                      \
                "      at %s:%d\n",                                 \
                #a, #b,                                             \
                (unsigned long long)_a, (unsigned long long)_b,     \
                __FILE__, __LINE__);                                 \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_NE(a, b) do {                                        \
    uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);               \
    if (_a == _b) {                                                 \
        fprintf(stderr, "    ASSERT_NE failed: %s != %s\n"          \
                "      both are 0x%llx\n"                           \
                "      at %s:%d\n",                                 \
                #a, #b, (unsigned long long)_a,                     \
                __FILE__, __LINE__);                                 \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ_INT(a, b) do {                                    \
    long long _a = (long long)(a), _b = (long long)(b);             \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n"      \
                "      got %lld vs %lld\n"                           \
                "      at %s:%d\n",                                 \
                #a, #b, _a, _b, __FILE__, __LINE__);                \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ_DBL(a, b, eps) do {                               \
    double _a = (double)(a), _b = (double)(b), _e = (double)(eps);  \
    if (fabs(_a - _b) > _e) {                                       \
        fprintf(stderr, "    ASSERT_EQ_DBL failed: %s ~= %s\n"      \
                "      got %.17g vs %.17g (eps=%.17g)\n"             \
                "      at %s:%d\n",                                 \
                #a, #b, _a, _b, _e, __FILE__, __LINE__);            \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ_STR(a, b) do {                                    \
    const char *_a = (a), *_b = (b);                                \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {           \
        fprintf(stderr, "    ASSERT_EQ_STR failed: %s == %s\n"      \
                "      got \"%s\" vs \"%s\"\n"                       \
                "      at %s:%d\n",                                 \
                #a, #b,                                              \
                _a ? _a : "(null)", _b ? _b : "(null)",              \
                __FILE__, __LINE__);                                 \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_VALUE_EQ(a, b) do {                                  \
    R8EValue _a = (a), _b = (b);                                    \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_VALUE_EQ failed: %s == %s\n"     \
                "      got 0x%016llx vs 0x%016llx\n"                 \
                "      at %s:%d\n",                                 \
                #a, #b,                                              \
                (unsigned long long)_a, (unsigned long long)_b,      \
                __FILE__, __LINE__);                                 \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_NAN(v) do {                                          \
    R8EValue _v = (v);                                              \
    if (!R8E_IS_DOUBLE(_v) || !isnan(r8e_get_double(_v))) {         \
        fprintf(stderr, "    ASSERT_NAN failed: %s\n"                \
                "      value is 0x%016llx\n"                         \
                "      at %s:%d\n",                                 \
                #v, (unsigned long long)_v, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* =========================================================================
 * External test suite declarations
 * ========================================================================= */

/* Phase 1: Foundation modules */
void test_suite_value(void);
void test_suite_alloc(void);
void test_suite_number(void);
void test_suite_string(void);
void test_suite_atom(void);

/* Phase 1 continued: Lexer */
void run_token_tests(void);

/* Phase 2: Runtime modules */
void run_object_tests(void);
void run_array_tests(void);
void run_gc_tests(void);
void run_closure_tests(void);
void run_interp_tests(void);
void run_parse_tests(void);
void run_function_tests(void);
void run_error_tests(void);
void run_json_tests(void);

/* Phase 2 continued: Bytecode buffer and scope */
void run_bc_tests(void);
void run_scope_tests(void);

/* Phase 3: Full language + Security */
void run_regexp_tests(void);
void run_promise_tests(void);
void run_module_tests(void);
void run_security_tests(void);

/* Phase 3 continued: ES2023 features */
void run_iterator_tests(void);
void run_proxy_tests(void);
void run_weakref_tests(void);
void run_builtin_tests(void);

/* Phase 4: Integration, deep security, and UI */
void run_integration_tests(void);
void run_security_deep_tests(void);
void run_ui_tests(void);

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== r8e unit tests ===\n\n");

    /* Phase 1: Foundation */
    printf("[value]\n");
    test_suite_value();
    printf("\n");

    printf("[alloc]\n");
    test_suite_alloc();
    printf("\n");

    printf("[number]\n");
    test_suite_number();
    printf("\n");

    printf("[string]\n");
    test_suite_string();
    printf("\n");

    printf("[atom]\n");
    test_suite_atom();
    printf("\n");

    printf("[token]\n");
    run_token_tests();
    printf("\n");

    /* Phase 2: Runtime */
    printf("[object]\n");
    run_object_tests();
    printf("\n");

    printf("[array]\n");
    run_array_tests();
    printf("\n");

    printf("[gc]\n");
    run_gc_tests();
    printf("\n");

    printf("[closure]\n");
    run_closure_tests();
    printf("\n");

    printf("[interp]\n");
    run_interp_tests();
    printf("\n");

    printf("[parse]\n");
    run_parse_tests();
    printf("\n");

    printf("[function]\n");
    run_function_tests();
    printf("\n");

    printf("[error]\n");
    run_error_tests();
    printf("\n");

    printf("[json]\n");
    run_json_tests();
    printf("\n");

    printf("[bc]\n");
    run_bc_tests();
    printf("\n");

    printf("[scope]\n");
    run_scope_tests();
    printf("\n");

    /* Phase 3: Full language + Security */
    printf("[regexp]\n");
    run_regexp_tests();
    printf("\n");

    printf("[promise]\n");
    run_promise_tests();
    printf("\n");

    printf("[module]\n");
    run_module_tests();
    printf("\n");

    printf("[security]\n");
    run_security_tests();
    printf("\n");

    /* Phase 3 continued: ES2023 features */
    printf("[iterator]\n");
    run_iterator_tests();
    printf("\n");

    printf("[proxy]\n");
    run_proxy_tests();
    printf("\n");

    printf("[weakref]\n");
    run_weakref_tests();
    printf("\n");

    printf("[builtin]\n");
    run_builtin_tests();
    printf("\n");

    /* Phase 4: Integration, deep security, and UI */
    printf("[integration]\n");
    run_integration_tests();
    printf("\n");

    printf("[security-deep]\n");
    run_security_deep_tests();
    printf("\n");

    printf("[ui]\n");
    run_ui_tests();
    printf("\n");

    printf("=== Summary: %d passed, %d failed out of %d tests ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
