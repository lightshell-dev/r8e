/*
 * test_raf.c - Unit tests for requestAnimationFrame / cancelAnimationFrame
 *
 * Tests cover:
 *   1. requestAnimationFrame returns non-zero ID
 *   2. Callback fires on tick
 *   3. cancelAnimationFrame prevents callback from firing
 *   4. Callback fires only once (not on second tick)
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
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
 * Test 1: requestAnimationFrame returns non-zero ID
 * ========================================================================= */

TEST(raf_request_returns_id) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_raf_reset();
    r8e_raf_init(ctx);

    /* Create a simple callback via eval */
    R8EValue cb = r8e_eval(ctx, "function cb(){} cb", 0);
    ASSERT_TRUE(r8e_is_function(cb));

    uint32_t id = r8e_raf_request(ctx, cb);
    ASSERT_TRUE(id != 0);

    /* Second request should return a different ID */
    uint32_t id2 = r8e_raf_request(ctx, cb);
    ASSERT_TRUE(id2 != 0);
    ASSERT_TRUE(id2 != id);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 2: Callback fires on tick
 * ========================================================================= */

TEST(raf_callback_fires) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_raf_reset();
    r8e_raf_init(ctx);

    /* Set up a global counter and a callback that increments it */
    r8e_eval(ctx, "var rafCount = 0", 0);
    R8EValue cb = r8e_eval(ctx, "(function() { rafCount = rafCount + 1 })", 0);
    ASSERT_TRUE(r8e_is_function(cb));

    r8e_raf_request(ctx, cb);
    r8e_raf_tick(ctx);

    /* Check that rafCount is now 1 */
    R8EValue count = r8e_eval(ctx, "rafCount", 0);
    double d = r8e_to_double(count);
    ASSERT_EQ_INT((int)d, 1);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 3: cancelAnimationFrame prevents callback from firing
 * ========================================================================= */

TEST(raf_cancel_prevents_fire) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_raf_reset();
    r8e_raf_init(ctx);

    r8e_eval(ctx, "var rafCancelCount = 0", 0);
    R8EValue cb = r8e_eval(ctx, "(function() { rafCancelCount = rafCancelCount + 1 })", 0);
    ASSERT_TRUE(r8e_is_function(cb));

    uint32_t id = r8e_raf_request(ctx, cb);
    r8e_raf_cancel(ctx, id);
    r8e_raf_tick(ctx);

    /* Should NOT have been called */
    R8EValue count = r8e_eval(ctx, "rafCancelCount", 0);
    double d = r8e_to_double(count);
    ASSERT_EQ_INT((int)d, 0);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 4: Callback fires only once (not on second tick)
 * ========================================================================= */

TEST(raf_fires_once) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    r8e_raf_reset();
    r8e_raf_init(ctx);

    r8e_eval(ctx, "var rafOnceCount = 0", 0);
    R8EValue cb = r8e_eval(ctx, "(function() { rafOnceCount = rafOnceCount + 1 })", 0);
    ASSERT_TRUE(r8e_is_function(cb));

    r8e_raf_request(ctx, cb);

    /* First tick fires callback */
    r8e_raf_tick(ctx);
    R8EValue count1 = r8e_eval(ctx, "rafOnceCount", 0);
    ASSERT_EQ_INT((int)r8e_to_double(count1), 1);

    /* Second tick should NOT fire it again */
    r8e_raf_tick(ctx);
    R8EValue count2 = r8e_eval(ctx, "rafOnceCount", 0);
    ASSERT_EQ_INT((int)r8e_to_double(count2), 1);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_raf_tests(void) {
    RUN_TEST(raf_request_returns_id);
    RUN_TEST(raf_callback_fires);
    RUN_TEST(raf_cancel_prevents_fire);
    RUN_TEST(raf_fires_once);
}
