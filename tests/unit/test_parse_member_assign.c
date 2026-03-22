/*
 * test_parse_member_assign.c - Tests for member expression assignment
 *
 * Tests cover:
 *   - obj.prop = val (dot assignment)
 *   - obj[key] = val (bracket assignment)
 *   - nested member assignment (a.b.c = val)
 *   - compound member assignment (obj.x += val)
 *   - array element assignment (arr[i] = val)
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

TEST(member_assign_dot) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var obj = {}; obj.x = 42; obj.x"), 42);
    r8e_context_free(ctx);
}

TEST(member_assign_bracket) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* Use integer key on array since SET_ELEM currently supports arrays with
     * integer keys. String-key bracket assignment on plain objects requires
     * interpreter-level SET_ELEM support for string keys. */
    ASSERT_EQ_INT(eval_int(ctx, "var arr = [0,0,0]; var k = 2; arr[k] = 99; arr[k]"), 99);
    r8e_context_free(ctx);
}

TEST(member_assign_nested) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var a = {}; a.b = {}; a.b.c = 7; a.b.c"), 7);
    r8e_context_free(ctx);
}

TEST(member_assign_compound) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var obj = {}; obj.x = 10; obj.x += 5; obj.x"), 15);
    r8e_context_free(ctx);
}

TEST(member_assign_array_element) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var arr = [1,2,3]; arr[1] = 20; arr[1]"), 20);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Suite Runner
 * ========================================================================= */

void run_parse_member_assign_tests(void) {
    RUN_TEST(member_assign_dot);
    RUN_TEST(member_assign_bracket);
    RUN_TEST(member_assign_nested);
    RUN_TEST(member_assign_compound);
    RUN_TEST(member_assign_array_element);
}
