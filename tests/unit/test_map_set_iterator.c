/*
 * test_map_set_iterator.c - Unit tests for Map/Set iterator protocol
 *
 * Tests that for...of works with Map and Set, and that .keys(), .values(),
 * .entries() methods return proper iterators.
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

static int32_t eval_int(R8EContext *ctx, const char *source) {
    R8EValue v = r8e_eval(ctx, source, 0);
    return r8e_to_int32(v);
}

/* =========================================================================
 * Test 1: Map for-of iterates entries as [key, value] pairs
 * ========================================================================= */

TEST(map_for_of) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* First check Map basics work */
    ASSERT_EQ_INT(eval_int(ctx,
        "var m = new Map(); m.set('a', 1); m.set('b', 2); m.size"),
        2);

    /* Then check for-of on Map produces entries and count iterations */
    ASSERT_EQ_INT(eval_int(ctx,
        "var m2 = new Map(); m2.set('a', 1); m2.set('b', 2); "
        "var c = 0; for (var e of m2) c++; c"),
        2);

    /* Finally check we can access entry values */
    ASSERT_EQ_INT(eval_int(ctx,
        "var m3 = new Map(); m3.set('a', 1); m3.set('b', 2); "
        "var s = 0; for (var e of m3) s += e[1]; s"),
        3);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 2: Set for-of iterates values
 * ========================================================================= */

TEST(set_for_of) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "var s = new Set(); s.add(10); s.add(20); s.add(30); "
        "var sum = 0; for (var v of s) sum += v; sum"),
        60);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 3: Map.keys() iterator
 * ========================================================================= */

TEST(map_keys) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "var m = new Map(); m.set('x', 5); "
        "var k = []; for (var key of m.keys()) k.push(key); k.length"),
        1);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

void run_map_set_iterator_tests(void) {
    RUN_TEST(map_for_of);
    RUN_TEST(set_for_of);
    RUN_TEST(map_keys);
}
