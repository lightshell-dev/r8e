/*
 * test_parse_destructuring.c - Tests for destructuring declarations
 *
 * Tests cover:
 *   - Object destructuring: const { a, b } = obj
 *   - Array destructuring: const [x, y] = arr
 *   - Rename syntax: const { x: myX } = obj
 *   - Default values: const { a, b = 99 } = obj
 *   - Array holes (skip): const [, second] = arr
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
    pid_t _pid = fork();                                            \
    if (_pid == 0) {                                                \
        alarm(5);                                                   \
        test_##name();                                              \
        _exit(g_assert_fail ? 1 : 0);                              \
    } else if (_pid > 0) {                                         \
        int _wstatus = 0;                                           \
        waitpid(_pid, &_wstatus, 0);                               \
        if (WIFSIGNALED(_wstatus)) {                               \
            g_assert_fail = 1;                                      \
            fprintf(stderr, "    CRASHED (signal)\n");              \
        } else if (WIFEXITED(_wstatus) &&                          \
                   WEXITSTATUS(_wstatus) != 0) {                   \
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

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EValue eval_js(R8EContext *ctx, const char *source) {
    return r8e_eval(ctx, source, 0);
}

static int32_t eval_int(R8EContext *ctx, const char *source) {
    R8EValue v = eval_js(ctx, source);
    return r8e_to_int32(v);
}

/* =========================================================================
 * Tests
 * ========================================================================= */

TEST(destructure_object_basic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var obj = {a: 1, b: 2}; const {a, b} = obj; a + b"), 3);
    r8e_context_free(ctx);
}

TEST(destructure_array_basic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var arr = [10, 20, 30]; const [x, y, z] = arr; y"), 20);
    r8e_context_free(ctx);
}

TEST(destructure_with_rename) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var obj = {x: 7}; const {x: myX} = obj; myX"), 7);
    r8e_context_free(ctx);
}

TEST(destructure_with_defaults) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var obj = {a: 5}; const {a, b = 99} = obj; b"), 99);
    r8e_context_free(ctx);
}

TEST(destructure_array_skip) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var arr = [1, 2, 3]; const [, second] = arr; second"), 2);
    r8e_context_free(ctx);
}

TEST(destructure_object_let) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var obj = {x: 10, y: 20}; let {x, y} = obj; x + y"), 30);
    r8e_context_free(ctx);
}

TEST(destructure_array_let) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var arr = [5, 15]; let [a, b] = arr; a + b"), 20);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Suite Entry Point
 * ========================================================================= */

void run_parse_destructuring_tests(void) {
    RUN_TEST(destructure_object_basic);
    RUN_TEST(destructure_array_basic);
    RUN_TEST(destructure_with_rename);
    RUN_TEST(destructure_with_defaults);
    RUN_TEST(destructure_array_skip);
    RUN_TEST(destructure_object_let);
    RUN_TEST(destructure_array_let);
}
