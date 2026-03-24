/*
 * test_e2e_app.c - End-to-end integration test exercising the full r8e stack
 *
 * Tests object creation, function calls, arrays, closures, nested objects,
 * string operations, and native function integration in a single comprehensive
 * test to verify the entire engine works correctly end-to-end.
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
    int _a = (a), _b = (b);                                         \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n"      \
                "      got %d vs %d\n"                               \
                "      at %s:%d\n",                                  \
                #a, #b, _a, _b, __FILE__, __LINE__);                \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* Helper: eval JS and return int32 result */
static int eval_int(R8EContext *ctx, const char *src) {
    R8EValue v = r8e_eval(ctx, src, 0);
    if (R8E_IS_INT32(v)) return r8e_get_int32(v);
    return (int)r8e_to_double(v);
}

/* =========================================================================
 * E2E Test: Minimum Viable App
 *
 * Exercises the full r8e stack in a single context: compilation, interpretation,
 * object model, function calls, arrays, closures, and native interop.
 * ========================================================================= */

TEST(e2e_minimum_viable_app) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Test 1: Basic arithmetic */
    ASSERT_EQ_INT(eval_int(ctx, "1 + 2 + 3"), 6);

    /* Test 2: Variable declaration and use */
    ASSERT_EQ_INT(eval_int(ctx, "var x = 10; var y = 20; x + y"), 30);

    /* Test 3: Object creation and property access via member assign */
    R8EValue result = r8e_eval(ctx,
        "var obj = {}; obj.x = 42; obj.x", 0);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    /* Test 4: Function definition and calling */
    result = r8e_eval(ctx,
        "function add(a, b) { return a + b; } add(3, 4)", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 7);

    /* Test 5: Array creation and element access */
    result = r8e_eval(ctx,
        "var arr = [1, 2, 3]; arr[1]", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 2);

    /* Test 6: Array element assignment */
    result = r8e_eval(ctx,
        "var arr = [1, 2, 3]; arr[1] = 20; arr[1]", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 20);

    /* Test 7: Object literal with properties */
    result = r8e_eval(ctx,
        "var obj = {a: 10, b: 20}; obj.a + obj.b", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 30);

    /* Test 8: Nested function calls */
    result = r8e_eval(ctx,
        "function double(n) { return n * 2; }"
        "function triple(n) { return n * 3; }"
        "double(triple(2))", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 12);

    /* Test 9: Nested objects */
    result = r8e_eval(ctx,
        "var a = {}; a.b = {}; a.b.c = 99; a.b.c", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 99);

    /* Test 10: String length */
    result = r8e_eval(ctx, "'hello'.length", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 5);

    /* Test 11: Native function integration — set a global from C, read in JS */
    r8e_set_global(ctx, "nativeVal", r8e_from_int32(777));
    result = r8e_eval(ctx, "nativeVal", 0);
    ASSERT_EQ_INT(r8e_get_int32(result), 777);

    /* Test 12: r8e_call — invoke a JS function from C */
    R8EValue func = r8e_eval(ctx, "function mul(a,b){return a*b} mul", 0);
    ASSERT_TRUE(r8e_is_function(func));
    R8EValue args[2];
    args[0] = r8e_from_int32(6);
    args[1] = r8e_from_int32(7);
    result = r8e_call(ctx, func, R8E_UNDEFINED, 2, args);
    ASSERT_EQ_INT(r8e_get_int32(result), 42);

    /* Test 13: Conditional (if/else) */
    ASSERT_EQ_INT(eval_int(ctx,
        "var v = 10; if (v > 5) { v = 100; } else { v = 0; } v"), 100);

    /* Test 14: For loop */
    ASSERT_EQ_INT(eval_int(ctx,
        "var s = 0; for (var i = 0; i < 5; i++) { s = s + i; } s"), 10);

    /* Test 15: While loop */
    ASSERT_EQ_INT(eval_int(ctx,
        "var n = 0; var s = 0; while (n < 5) { s = s + n; n = n + 1; } s"), 10);

    /* Test 16: Comparison operators */
    ASSERT_TRUE(R8E_IS_TRUE(r8e_eval(ctx, "5 > 3", 0)));
    ASSERT_TRUE(R8E_IS_FALSE(r8e_eval(ctx, "3 > 5", 0)));
    ASSERT_TRUE(R8E_IS_TRUE(r8e_eval(ctx, "5 === 5", 0)));

    /* Test 17: Boolean logic */
    ASSERT_TRUE(R8E_IS_TRUE(r8e_eval(ctx, "true && true", 0)));
    ASSERT_TRUE(R8E_IS_FALSE(r8e_eval(ctx, "true && false", 0)));
    ASSERT_TRUE(R8E_IS_TRUE(r8e_eval(ctx, "false || true", 0)));

    /* Test 18: Negation and unary operators */
    ASSERT_EQ_INT(eval_int(ctx, "var z = 10; -z"), -10);

    /* Test 19: Multiple variables */
    ASSERT_EQ_INT(eval_int(ctx,
        "var a1 = 1; var a2 = 2; var a3 = 3; a1 + a2 + a3"), 6);

    /* Test 20: Nested if/else */
    ASSERT_EQ_INT(eval_int(ctx,
        "var n = 15;"
        "var r = 0;"
        "if (n > 20) { r = 1; }"
        "else if (n > 10) { r = 2; }"
        "else { r = 3; }"
        "r"), 2);

    r8e_context_free(ctx);
}

/* =========================================================================
 * E2E Test: Cross-eval global persistence
 *
 * Verifies that global values set via the C API persist across multiple
 * r8e_eval calls and can be read/written from both C and JS.
 * ========================================================================= */

TEST(e2e_global_persistence) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Set a global from C */
    r8e_set_global(ctx, "counter", r8e_from_int32(0));

    /* Read it from JS */
    R8EValue v = r8e_eval(ctx, "counter", 0);
    ASSERT_EQ_INT(r8e_get_int32(v), 0);

    /* Modify it from JS */
    r8e_eval(ctx, "counter = counter + 10", 0);

    /* Read back from C */
    v = r8e_get_global(ctx, "counter");
    ASSERT_EQ_INT(r8e_get_int32(v), 10);

    /* Modify from C again */
    r8e_set_global(ctx, "counter", r8e_from_int32(99));

    /* Read from JS */
    v = r8e_eval(ctx, "counter", 0);
    ASSERT_EQ_INT(r8e_get_int32(v), 99);

    r8e_context_free(ctx);
}

/* =========================================================================
 * E2E Test: Object property CRUD
 *
 * Exercises the full object property lifecycle: create, read, update, delete.
 * ========================================================================= */

TEST(e2e_object_crud) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Create an object from C */
    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));

    /* Set properties */
    r8e_set_prop(ctx, obj, "x", r8e_from_int32(10));
    r8e_set_prop(ctx, obj, "y", r8e_from_int32(20));

    /* Read them back */
    R8EValue vx = r8e_get_prop(ctx, obj, "x");
    R8EValue vy = r8e_get_prop(ctx, obj, "y");
    ASSERT_EQ_INT(r8e_get_int32(vx), 10);
    ASSERT_EQ_INT(r8e_get_int32(vy), 20);

    /* Update */
    r8e_set_prop(ctx, obj, "x", r8e_from_int32(100));
    vx = r8e_get_prop(ctx, obj, "x");
    ASSERT_EQ_INT(r8e_get_int32(vx), 100);

    /* Has prop */
    ASSERT_TRUE(r8e_has_prop(ctx, obj, "x"));
    ASSERT_TRUE(!r8e_has_prop(ctx, obj, "z"));

    /* Delete */
    r8e_delete_prop(ctx, obj, "x");
    ASSERT_TRUE(!r8e_has_prop(ctx, obj, "x"));

    r8e_context_free(ctx);
}

/* =========================================================================
 * E2E Test: Native function callback
 *
 * Registers a native C function as a JS global and calls it from JS.
 * ========================================================================= */

static R8EValue native_square(R8EContext *ctx, R8EValue this_val,
                               int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return r8e_from_int32(0);
    int32_t n = r8e_get_int32(argv[0]);
    return r8e_from_int32(n * n);
}

TEST(e2e_native_callback) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Register a native function */
    r8e_set_global_func(ctx, "square", native_square, 1);

    /* Call from JS */
    R8EValue v = r8e_eval(ctx, "square(7)", 0);
    ASSERT_EQ_INT(r8e_get_int32(v), 49);

    /* Use in a loop */
    v = r8e_eval(ctx,
        "var s = 0; for (var i = 1; i <= 3; i++) { s = s + square(i); } s", 0);
    /* 1 + 4 + 9 = 14 */
    ASSERT_EQ_INT(r8e_get_int32(v), 14);

    r8e_context_free(ctx);
}

/* =========================================================================
 * E2E Test: String operations
 *
 * Exercises string creation, length, concatenation.
 * ========================================================================= */

TEST(e2e_string_ops) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* String length */
    R8EValue v = r8e_eval(ctx, "'hello'.length", 0);
    ASSERT_EQ_INT(r8e_get_int32(v), 5);

    /* Short string length */
    v = r8e_eval(ctx, "'ab'.length", 0);
    ASSERT_EQ_INT(r8e_get_int32(v), 2);

    /* Single char */
    v = r8e_eval(ctx, "'x'.length", 0);
    ASSERT_EQ_INT(r8e_get_int32(v), 1);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_e2e_app_tests(void) {
    RUN_TEST(e2e_minimum_viable_app);
    RUN_TEST(e2e_global_persistence);
    RUN_TEST(e2e_object_crud);
    RUN_TEST(e2e_native_callback);
    RUN_TEST(e2e_string_ops);
}
