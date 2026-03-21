/*
 * test_api_wire.c - Tests for r8e_call (function invocation through public API)
 *
 * Validates that native C code can invoke JavaScript functions via r8e_call()
 * after obtaining function values from r8e_eval().
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/r8e_types.h"
#include "../../include/r8e_api.h"

/* =========================================================================
 * Test Infrastructure (shared with test_runner.c)
 * ========================================================================= */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;
extern int g_assert_fail;

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
            fprintf(stderr, "    CRASHED (signal)\n");              \
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
} while (0)

#define ASSERT_TRUE(expr) do {                                      \
    if (!(expr)) {                                                  \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n"              \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* =========================================================================
 * Tests
 * ========================================================================= */

/*
 * Test 1: Eval a function, call it with two int args, verify result.
 *   JS: function add(a,b){return a+b}  add
 *   Call: r8e_call(add, undefined, [3, 4])
 *   Expected: 7
 */
TEST(api_call_simple_function) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Eval returns the function value (last expression is `add`) */
    R8EValue func = r8e_eval(ctx, "function add(a,b){return a+b} add", 0);
    ASSERT_TRUE(r8e_is_function(func));

    /* Call with two integer arguments */
    R8EValue args[2];
    args[0] = r8e_from_int32(3);
    args[1] = r8e_from_int32(4);
    R8EValue result = r8e_call(ctx, func, R8E_UNDEFINED, 2, args);

    /* Result should be 7 (int or double) */
    double d = r8e_to_double(result);
    ASSERT_TRUE(d == 7.0);

    r8e_context_free(ctx);
}

/*
 * Test 2: Call a function with no arguments.
 *   JS: function f(){return 42} f
 *   Call: r8e_call(f, undefined, 0, NULL)
 *   Expected: 42
 */
TEST(api_call_no_args) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue func = r8e_eval(ctx, "function f(){return 42} f", 0);
    ASSERT_TRUE(r8e_is_function(func));

    R8EValue result = r8e_call(ctx, func, R8E_UNDEFINED, 0, NULL);

    double d = r8e_to_double(result);
    ASSERT_TRUE(d == 42.0);

    r8e_context_free(ctx);
}

/*
 * Test 3: Call a function that returns a string.
 *   JS: function greet(){return "hello"} greet
 *   Call: r8e_call(greet, undefined, 0, NULL)
 *   Expected: "hello" (inline string)
 */
TEST(api_call_with_string) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue func = r8e_eval(ctx, "function greet(){return \"hello\"} greet", 0);
    ASSERT_TRUE(r8e_is_function(func));

    R8EValue result = r8e_call(ctx, func, R8E_UNDEFINED, 0, NULL);

    /* "hello" is 5 chars, fits in inline string */
    char buf[16] = {0};
    size_t len = 0;
    const char *str = r8e_get_cstring(result, buf, &len);
    ASSERT_TRUE(str != NULL);
    ASSERT_TRUE(len == 5);
    ASSERT_TRUE(strcmp(str, "hello") == 0);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void run_api_wire_tests(void) {
    RUN_TEST(api_call_simple_function);
    RUN_TEST(api_call_no_args);
    RUN_TEST(api_call_with_string);
}
