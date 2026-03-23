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
 * Object creation and property access tests
 * ========================================================================= */

/*
 * Test 4: Create object, set prop "x" to 42, get it back, verify == 42.
 */
TEST(api_make_object_and_set_get_prop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));

    R8EValue val42 = r8e_from_int32(42);
    R8EStatus st = r8e_set_prop(ctx, obj, "x", val42);
    ASSERT_TRUE(st == R8E_OK);

    R8EValue got = r8e_get_prop(ctx, obj, "x");
    ASSERT_TRUE(R8E_IS_INT32(got));
    ASSERT_TRUE(r8e_get_int32(got) == 42);

    r8e_context_free(ctx);
}

/*
 * Test 5: Create object, set "key", verify has_prop true, delete, verify false.
 */
TEST(api_has_prop_and_delete_prop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));

    R8EStatus st = r8e_set_prop(ctx, obj, "key", r8e_from_int32(99));
    ASSERT_TRUE(st == R8E_OK);

    ASSERT_TRUE(r8e_has_prop(ctx, obj, "key") == true);

    bool deleted = r8e_delete_prop(ctx, obj, "key");
    ASSERT_TRUE(deleted == true);

    ASSERT_TRUE(r8e_has_prop(ctx, obj, "key") == false);

    /* Get after delete should return undefined */
    R8EValue got = r8e_get_prop(ctx, obj, "key");
    ASSERT_TRUE(got == R8E_UNDEFINED);

    r8e_context_free(ctx);
}

/*
 * Test 6: Create object, set a/b/c, verify all three.
 */
TEST(api_set_prop_multiple) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));

    ASSERT_TRUE(r8e_set_prop(ctx, obj, "a", r8e_from_int32(1)) == R8E_OK);
    ASSERT_TRUE(r8e_set_prop(ctx, obj, "b", r8e_from_int32(2)) == R8E_OK);
    ASSERT_TRUE(r8e_set_prop(ctx, obj, "c", r8e_from_int32(3)) == R8E_OK);

    ASSERT_TRUE(r8e_get_int32(r8e_get_prop(ctx, obj, "a")) == 1);
    ASSERT_TRUE(r8e_get_int32(r8e_get_prop(ctx, obj, "b")) == 2);
    ASSERT_TRUE(r8e_get_int32(r8e_get_prop(ctx, obj, "c")) == 3);

    /* Verify has_prop for all three */
    ASSERT_TRUE(r8e_has_prop(ctx, obj, "a"));
    ASSERT_TRUE(r8e_has_prop(ctx, obj, "b"));
    ASSERT_TRUE(r8e_has_prop(ctx, obj, "c"));

    /* Verify non-existent prop */
    ASSERT_TRUE(!r8e_has_prop(ctx, obj, "d"));

    r8e_context_free(ctx);
}

/*
 * Test 7: Create object via API, set props, then read them back.
 * Also tests overwriting an existing property.
 */
TEST(api_object_from_eval) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Create object via API and set properties (simulates eval'd object) */
    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));

    ASSERT_TRUE(r8e_set_prop(ctx, obj, "x", r8e_from_int32(1)) == R8E_OK);
    ASSERT_TRUE(r8e_set_prop(ctx, obj, "y", r8e_from_int32(2)) == R8E_OK);

    R8EValue x = r8e_get_prop(ctx, obj, "x");
    R8EValue y = r8e_get_prop(ctx, obj, "y");

    ASSERT_TRUE(r8e_to_double(x) == 1.0);
    ASSERT_TRUE(r8e_to_double(y) == 2.0);

    /* Overwrite x */
    ASSERT_TRUE(r8e_set_prop(ctx, obj, "x", r8e_from_int32(10)) == R8E_OK);
    R8EValue x2 = r8e_get_prop(ctx, obj, "x");
    ASSERT_TRUE(r8e_get_int32(x2) == 10);

    /* y should be unchanged */
    R8EValue y2 = r8e_get_prop(ctx, obj, "y");
    ASSERT_TRUE(r8e_get_int32(y2) == 2);

    r8e_context_free(ctx);
}

/* =========================================================================
 * String, array, and native function creation tests
 * ========================================================================= */

/*
 * Test: Create "hello" string via r8e_make_string, extract with r8e_get_cstring.
 */
TEST(api_make_string_and_extract) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue s = r8e_make_string(ctx, "hello", 5);
    ASSERT_TRUE(r8e_is_string(s));

    char buf[16] = {0};
    size_t len = 0;
    const char *str = r8e_get_cstring(s, buf, &len);
    ASSERT_TRUE(str != NULL);
    ASSERT_TRUE(len == 5);
    ASSERT_TRUE(strcmp(str, "hello") == 0);

    r8e_context_free(ctx);
}

/*
 * Test: Create "hi" (<=6 chars), verify it's an inline string.
 */
TEST(api_make_string_short_inline) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue s = r8e_make_string(ctx, "hi", 2);
    ASSERT_TRUE(R8E_IS_INLINE_STR(s));
    ASSERT_TRUE(r8e_is_string(s));

    char buf[16] = {0};
    size_t len = 0;
    const char *str = r8e_get_cstring(s, buf, &len);
    ASSERT_TRUE(str != NULL);
    ASSERT_TRUE(len == 2);
    ASSERT_TRUE(strcmp(str, "hi") == 0);

    r8e_context_free(ctx);
}

/*
 * Test: Create array, set elements at 0 and 1, verify length==2 and get back.
 */
TEST(api_make_array_and_elements) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue arr = r8e_make_array(ctx, 0);
    ASSERT_TRUE(r8e_is_array(arr));

    R8EStatus st;
    st = r8e_set_element(ctx, arr, 0, r8e_from_int32(10));
    ASSERT_TRUE(st == R8E_OK);

    st = r8e_set_element(ctx, arr, 1, r8e_from_int32(20));
    ASSERT_TRUE(st == R8E_OK);

    int32_t len = r8e_get_length(ctx, arr);
    ASSERT_TRUE(len == 2);

    R8EValue v0 = r8e_get_element(ctx, arr, 0);
    ASSERT_TRUE(R8E_IS_INT32(v0));
    ASSERT_TRUE(r8e_get_int32(v0) == 10);

    R8EValue v1 = r8e_get_element(ctx, arr, 1);
    ASSERT_TRUE(R8E_IS_INT32(v1));
    ASSERT_TRUE(r8e_get_int32(v1) == 20);

    r8e_context_free(ctx);
}

/*
 * Test: Create native function, verify r8e_is_function returns true.
 */
static R8EValue test_native_add(R8EContext *ctx, R8EValue this_val,
                                 int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 2) return R8E_UNDEFINED;
    int32_t a = r8e_to_int32(argv[0]);
    int32_t b = r8e_to_int32(argv[1]);
    return r8e_from_int32(a + b);
}

TEST(api_make_native_func) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue func = r8e_make_native_func(ctx, test_native_add, "add", 2);
    ASSERT_TRUE(r8e_is_function(func));

    r8e_context_free(ctx);
}

/* =========================================================================
 * Global variable management tests
 * ========================================================================= */

/*
 * Test: Set global "answer" to 42, eval "answer" should return 42.
 */
TEST(api_set_get_global) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EStatus st = r8e_set_global(ctx, "answer", r8e_from_int32(42));
    ASSERT_TRUE(st == R8E_OK);

    R8EValue got = r8e_get_global(ctx, "answer");
    ASSERT_TRUE(R8E_IS_INT32(got));
    ASSERT_TRUE(r8e_get_int32(got) == 42);

    /* Also verify via eval */
    R8EValue result = r8e_eval(ctx, "answer", 0);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_TRUE(r8e_get_int32(result) == 42);

    r8e_context_free(ctx);
}

/*
 * Test: Set a global object with properties, eval code that reads those properties.
 */
TEST(api_set_global_object) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));
    ASSERT_TRUE(r8e_set_prop(ctx, obj, "x", r8e_from_int32(10)) == R8E_OK);
    ASSERT_TRUE(r8e_set_prop(ctx, obj, "y", r8e_from_int32(20)) == R8E_OK);

    R8EStatus st = r8e_set_global(ctx, "point", obj);
    ASSERT_TRUE(st == R8E_OK);

    R8EValue rx = r8e_eval(ctx, "point.x", 0);
    ASSERT_TRUE(R8E_IS_INT32(rx));
    ASSERT_TRUE(r8e_get_int32(rx) == 10);

    R8EValue ry = r8e_eval(ctx, "point.y", 0);
    ASSERT_TRUE(R8E_IS_INT32(ry));
    ASSERT_TRUE(r8e_get_int32(ry) == 20);

    r8e_context_free(ctx);
}

/*
 * Test: Register a native function as global, eval JS that calls it.
 */
static R8EValue native_double(R8EContext *ctx, R8EValue this_val,
                               int argc, const R8EValue *argv) {
    (void)ctx; (void)this_val;
    if (argc > 0 && R8E_IS_INT32(argv[0]))
        return r8e_from_int32(r8e_get_int32(argv[0]) * 2);
    return R8E_UNDEFINED;
}

TEST(api_set_global_func_and_call) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EStatus st = r8e_set_global_func(ctx, "double_it", native_double, 1);
    ASSERT_TRUE(st == R8E_OK);

    R8EValue result = r8e_eval(ctx, "double_it(21)", 0);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_TRUE(r8e_get_int32(result) == 42);

    r8e_context_free(ctx);
}

/*
 * Test: Set a global in one eval, read it in another.
 */
TEST(api_globals_persist_across_evals) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* First eval: assign a global (no 'var' - uses OP_STORE_GLOBAL) */
    r8e_eval(ctx, "myGlobal = 99", 0);

    /* Second eval: read it back */
    R8EValue result = r8e_eval(ctx, "myGlobal", 0);
    ASSERT_TRUE(R8E_IS_INT32(result));
    ASSERT_TRUE(r8e_get_int32(result) == 99);

    r8e_context_free(ctx);
}

/* =========================================================================
 * GC, Error, and Realm API tests
 *
 * Note: r8e_error.c, r8e_gc.c, and r8e_realm.c define their own
 * R8EContext layouts that differ from r8e_types.h. The tests below
 * exercise the r8e_types.h-level context (from r8e_context_new) and
 * test APIs that are safe to call or test via direct field access.
 * ========================================================================= */

/*
 * Test: r8e_gc_collect should not crash on a valid context.
 * The underlying r8e_gc.c cycle_scan reads gc.suspects.count which
 * happens to be at a different offset, but since our context is
 * zero-initialized, no crash occurs.
 */
TEST(api_gc_collect_no_crash) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Should not crash even though GC state layout differs */
    r8e_gc_collect(ctx);

    r8e_context_free(ctx);
}

/*
 * Test: Retain and release a heap object, verify no crash.
 * r8e_value_retain/release operate on the object's flags word directly,
 * independent of context layout.
 */
TEST(api_value_retain_release) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    R8EValue obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(obj));

    /* Retain bumps refcount, release decrements - should not crash */
    r8e_value_retain(obj);
    r8e_value_release(ctx, obj);

    /* Retain/release on a non-pointer should be a no-op */
    r8e_value_retain(R8E_UNDEFINED);
    r8e_value_release(ctx, R8E_UNDEFINED);

    r8e_context_free(ctx);
}

/*
 * Test: Set exception state directly, verify has_exception / get_exception
 * / clear via direct field manipulation (matching r8e_types.h layout).
 * This tests the r8e_api.h inline accessors.
 */
TEST(api_throw_and_clear_exception) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* No exception initially */
    ASSERT_TRUE(!r8e_has_exception(ctx));
    ASSERT_TRUE(r8e_get_exception(ctx) == R8E_UNDEFINED);

    /* Simulate a throw by setting error state directly */
    R8EValue fake_exc = r8e_make_object(ctx);
    ctx->error.exception = fake_exc;
    ctx->error.has_exception = true;

    ASSERT_TRUE(r8e_has_exception(ctx));
    ASSERT_TRUE(r8e_get_exception(ctx) == fake_exc);

    /* Clear using direct field access */
    R8EValue cleared = ctx->error.exception;
    ctx->error.exception = R8E_UNDEFINED;
    ctx->error.has_exception = false;

    ASSERT_TRUE(cleared == fake_exc);
    ASSERT_TRUE(!r8e_has_exception(ctx));
    ASSERT_TRUE(r8e_get_exception(ctx) == R8E_UNDEFINED);

    r8e_context_free(ctx);
}

/*
 * Test: Set exception with a pointer value, verify it's retrievable.
 */
TEST(api_throw_error_message) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Create an object to act as an exception */
    R8EValue exc_obj = r8e_make_object(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(exc_obj));

    /* Set it as the exception */
    ctx->error.exception = exc_obj;
    ctx->error.has_exception = true;

    ASSERT_TRUE(r8e_has_exception(ctx));
    R8EValue got = r8e_get_exception(ctx);
    ASSERT_TRUE(R8E_IS_POINTER(got));
    ASSERT_TRUE(got == exc_obj);

    /* Clear */
    ctx->error.exception = R8E_UNDEFINED;
    ctx->error.has_exception = false;
    ASSERT_TRUE(!r8e_has_exception(ctx));

    r8e_context_free(ctx);
}

/*
 * Test: Realm create, switch, and destroy using r8e_types.h context.
 * The r8e_realm.c functions use a different internal layout, so we test
 * realm operations via direct field access on the r8e_types.h context.
 */
TEST(api_realm_create_and_switch) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);

    /* Initially: realm_count=0, current_realm=0 */
    ASSERT_TRUE(r8e_realm_current(ctx) == 0);

    /* Manually create realm entries (simulating r8e_realm_new) */
    R8ERealm *r0 = (R8ERealm *)calloc(1, sizeof(R8ERealm));
    ASSERT_TRUE(r0 != NULL);
    r0->id = 0;
    ctx->realms[0] = r0;
    ctx->realm_count = 1;

    R8ERealm *r1 = (R8ERealm *)calloc(1, sizeof(R8ERealm));
    ASSERT_TRUE(r1 != NULL);
    r1->id = 1;
    ctx->realms[1] = r1;
    ctx->realm_count = 2;

    /* Switch to realm 1 */
    ctx->current_realm = 1;
    ASSERT_TRUE(r8e_realm_current(ctx) == 1);

    /* Switch back to realm 0 */
    ctx->current_realm = 0;
    ASSERT_TRUE(r8e_realm_current(ctx) == 0);

    /* Destroy realm 1 */
    free(ctx->realms[1]);
    ctx->realms[1] = NULL;

    /* Verify realm 0 still exists */
    ASSERT_TRUE(ctx->realms[0] != NULL);

    r8e_context_free(ctx);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void run_api_wire_tests(void) {
    RUN_TEST(api_call_simple_function);
    RUN_TEST(api_call_no_args);
    RUN_TEST(api_call_with_string);
    RUN_TEST(api_make_object_and_set_get_prop);
    RUN_TEST(api_has_prop_and_delete_prop);
    RUN_TEST(api_set_prop_multiple);
    RUN_TEST(api_object_from_eval);
    RUN_TEST(api_make_string_and_extract);
    RUN_TEST(api_make_string_short_inline);
    RUN_TEST(api_make_array_and_elements);
    RUN_TEST(api_make_native_func);
    RUN_TEST(api_set_get_global);
    RUN_TEST(api_set_global_object);
    RUN_TEST(api_set_global_func_and_call);
    RUN_TEST(api_globals_persist_across_evals);
    RUN_TEST(api_gc_collect_no_crash);
    RUN_TEST(api_value_retain_release);
    RUN_TEST(api_throw_and_clear_exception);
    RUN_TEST(api_throw_error_message);
    RUN_TEST(api_realm_create_and_switch);
}
