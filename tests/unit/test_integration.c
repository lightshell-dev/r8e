/*
 * test_integration.c - End-to-end integration tests for the r8e JS engine
 *
 * These tests exercise the full pipeline: source code -> lexer -> single-pass
 * compiler -> bytecode -> interpreter -> result value. Each test validates
 * that a JS program produces the expected result when run through r8e_eval().
 *
 * Coverage targets:
 *   - Basic expressions (arithmetic, comparison, logical)
 *   - Variable binding (let, const, var)
 *   - Function calls (declarations, expressions, arrow, default params)
 *   - Object creation and property access
 *   - Array operations and built-in methods
 *   - Closures and lexical scoping
 *   - ES2015+ classes
 *   - Error handling (try/catch/finally/throw)
 *   - Control flow (for, while, do-while, switch, for-of, for-in)
 *   - String operations and template literals
 *   - Regular expressions
 *   - JSON roundtrip
 *   - Promises and microtask queue
 *   - Destructuring (array and object)
 *   - Spread and rest operators
 *   - Optional chaining and nullish coalescing
 *   - Generators and iterators
 *   - Async/await
 *   - Typeof, instanceof, and in operators
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

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

/* =========================================================================
 * Helpers: evaluate JS and extract result
 * ========================================================================= */

static R8EValue eval_js(R8EContext *ctx, const char *source) {
    return r8e_eval(ctx, source, 0);
}

static int32_t eval_int(R8EContext *ctx, const char *source) {
    R8EValue v = eval_js(ctx, source);
    return r8e_to_int32(v);
}

static double eval_double(R8EContext *ctx, const char *source) {
    R8EValue v = eval_js(ctx, source);
    return r8e_to_double(v);
}

static bool eval_bool(R8EContext *ctx, const char *source) {
    R8EValue v = eval_js(ctx, source);
    return R8E_IS_TRUE(v);
}

static const char *eval_string(R8EContext *ctx, const char *source, char *buf) {
    R8EValue v = eval_js(ctx, source);
    size_t len;
    return r8e_get_cstring(v, buf, &len);
}

/* =========================================================================
 * Test 1: Basic arithmetic expressions
 * ========================================================================= */

TEST(eval_addition) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "1 + 2"), 3);
    r8e_context_free(ctx);
}

TEST(eval_subtraction) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "10 - 7"), 3);
    r8e_context_free(ctx);
}

TEST(eval_multiplication) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "6 * 7"), 42);
    r8e_context_free(ctx);
}

TEST(eval_division) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_DBL(eval_double(ctx, "10 / 3"), 3.3333333333333335, 1e-10);
    r8e_context_free(ctx);
}

TEST(eval_modulo) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "17 % 5"), 2);
    r8e_context_free(ctx);
}

TEST(eval_exponentiation) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "2 ** 10"), 1024);
    r8e_context_free(ctx);
}

TEST(eval_mixed_arithmetic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "2 + 3 * 4"), 14);
    ASSERT_EQ_INT(eval_int(ctx, "(2 + 3) * 4"), 20);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 2: Variable binding
 * ========================================================================= */

TEST(eval_let_binding) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "let x = 5; x * 2"), 10);
    r8e_context_free(ctx);
}

TEST(eval_const_binding) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "const y = 100; y - 1"), 99);
    r8e_context_free(ctx);
}

TEST(eval_var_binding) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "var z = 3; z + z"), 6);
    r8e_context_free(ctx);
}

TEST(eval_multiple_bindings) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "let a = 1, b = 2, c = 3; a + b + c"), 6);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 3: Function calls
 * ========================================================================= */

TEST(eval_function_declaration) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function add(a, b) { return a + b; } add(3, 4)"), 7);
    r8e_context_free(ctx);
}

TEST(eval_function_expression) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "const mul = function(x, y) { return x * y; }; mul(6, 7)"), 42);
    r8e_context_free(ctx);
}

TEST(eval_arrow_function) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "const sq = (x) => x * x; sq(5)"), 25);
    r8e_context_free(ctx);
}

TEST(eval_arrow_no_parens) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "const inc = x => x + 1; inc(9)"), 10);
    r8e_context_free(ctx);
}

TEST(eval_default_params) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function f(a, b = 10) { return a + b; } f(5)"), 15);
    r8e_context_free(ctx);
}

TEST(eval_rest_params) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function sum(...args) { let s = 0; for (let a of args) s += a; return s; }"
        "sum(1, 2, 3, 4)"), 10);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 4: Object creation and property access
 * ========================================================================= */

TEST(eval_object_literal) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let o = {a: 1, b: 2}; o.a + o.b"), 3);
    r8e_context_free(ctx);
}

TEST(eval_object_computed_property) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let key = 'x'; let o = {[key]: 42}; o.x"), 42);
    r8e_context_free(ctx);
}

TEST(eval_object_shorthand) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = 5, b = 10; let o = {a, b}; o.a + o.b"), 15);
    r8e_context_free(ctx);
}

TEST(eval_object_method) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let o = { val: 3, get() { return this.val; } }; o.get()"), 3);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 5: Array operations
 * ========================================================================= */

TEST(eval_array_literal) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = [10, 20, 30]; a[0] + a[1] + a[2]"), 60);
    r8e_context_free(ctx);
}

TEST(eval_array_map) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* [1,2,3].map(x => x*2) should produce [2,4,6]; check sum = 12 */
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = [1,2,3].map(x => x*2); a[0] + a[1] + a[2]"), 12);
    r8e_context_free(ctx);
}

TEST(eval_array_filter) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "[1,2,3,4,5].filter(x => x > 3).length"), 2);
    r8e_context_free(ctx);
}

TEST(eval_array_reduce) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "[1,2,3,4,5].reduce((s, x) => s + x, 0)"), 15);
    r8e_context_free(ctx);
}

TEST(eval_array_push_pop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = [1]; a.push(2); a.push(3); a.pop()"), 3);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 6: Closures
 * ========================================================================= */

TEST(eval_closure_basic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function f() { let x = 0; return () => ++x; }"
        "let c = f(); c(); c()"), 2);
    r8e_context_free(ctx);
}

TEST(eval_closure_shared_state) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function counter() {"
        "  let n = 0;"
        "  return { inc: () => ++n, get: () => n };"
        "}"
        "let c = counter(); c.inc(); c.inc(); c.inc(); c.get()"), 3);
    r8e_context_free(ctx);
}

TEST(eval_closure_over_loop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* Verify that let in for-loop creates a new binding per iteration */
    ASSERT_EQ_INT(eval_int(ctx,
        "let fns = [];"
        "for (let i = 0; i < 3; i++) {"
        "  fns.push(() => i);"
        "}"
        "fns[0]() + fns[1]() + fns[2]()"), 3); /* 0+1+2 = 3 */
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 7: Classes
 * ========================================================================= */

TEST(eval_class_basic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "class Foo {"
        "  constructor(x) { this.x = x; }"
        "  get() { return this.x; }"
        "}"
        "new Foo(42).get()"), 42);
    r8e_context_free(ctx);
}

TEST(eval_class_inheritance) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "class Animal {"
        "  constructor(name) { this.name = name; }"
        "  sound() { return 0; }"
        "}"
        "class Dog extends Animal {"
        "  sound() { return 42; }"
        "}"
        "new Dog('Rex').sound()"), 42);
    r8e_context_free(ctx);
}

TEST(eval_class_getter_setter) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "class Box {"
        "  constructor() { this._v = 0; }"
        "  get value() { return this._v; }"
        "  set value(v) { this._v = v; }"
        "}"
        "let b = new Box(); b.value = 99; b.value"), 99);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 8: Error handling
 * ========================================================================= */

TEST(eval_try_catch) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx,
        "try { throw new Error('x'); } catch(e) { e.message }", buf);
    ASSERT_EQ_STR(s, "x");
    r8e_context_free(ctx);
}

TEST(eval_try_finally) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let r = 0;"
        "try { r = 1; } finally { r = 2; }"
        "r"), 2);
    r8e_context_free(ctx);
}

TEST(eval_try_catch_finally) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let r = 0;"
        "try { throw 1; } catch(e) { r = e; } finally { r += 10; }"
        "r"), 11);
    r8e_context_free(ctx);
}

TEST(eval_custom_error) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx,
        "let caught = false;"
        "try { throw new TypeError('bad'); }"
        "catch(e) { caught = e instanceof TypeError; }"
        "caught"));
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 9: Control flow
 * ========================================================================= */

TEST(eval_for_loop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let s = 0; for (let i = 0; i < 10; i++) s += i; s"), 45);
    r8e_context_free(ctx);
}

TEST(eval_while_loop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let n = 0, s = 0; while (n < 5) { s += n; n++; } s"), 10);
    r8e_context_free(ctx);
}

TEST(eval_do_while_loop) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let n = 0; do { n++; } while (n < 3); n"), 3);
    r8e_context_free(ctx);
}

TEST(eval_for_of) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let s = 0; for (let x of [10, 20, 30]) s += x; s"), 60);
    r8e_context_free(ctx);
}

TEST(eval_for_in) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let o = {a:1, b:2, c:3}; let s = 0;"
        "for (let k in o) s += o[k]; s"), 6);
    r8e_context_free(ctx);
}

TEST(eval_switch) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let x = 2, r = 0;"
        "switch(x) {"
        "  case 1: r = 10; break;"
        "  case 2: r = 20; break;"
        "  default: r = 30;"
        "}"
        "r"), 20);
    r8e_context_free(ctx);
}

TEST(eval_break_continue) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* Sum only even numbers < 10 using continue */
    ASSERT_EQ_INT(eval_int(ctx,
        "let s = 0;"
        "for (let i = 0; i < 10; i++) {"
        "  if (i % 2 !== 0) continue;"
        "  s += i;"
        "}"
        "s"), 20); /* 0+2+4+6+8 */
    r8e_context_free(ctx);
}

TEST(eval_ternary) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "true ? 42 : 0"), 42);
    ASSERT_EQ_INT(eval_int(ctx, "false ? 0 : 99"), 99);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 10: String operations
 * ========================================================================= */

TEST(eval_string_split_join) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx,
        "'hello world'.split(' ').join('-')", buf);
    ASSERT_EQ_STR(s, "hello-world");
    r8e_context_free(ctx);
}

TEST(eval_string_length) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx, "'hello'.length"), 5);
    r8e_context_free(ctx);
}

TEST(eval_string_concat) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx, "'foo' + 'bar'", buf);
    ASSERT_EQ_STR(s, "foobar");
    r8e_context_free(ctx);
}

TEST(eval_string_includes) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx, "'hello world'.includes('world')"));
    ASSERT_FALSE(eval_bool(ctx, "'hello world'.includes('xyz')"));
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 11: Regular expressions
 * ========================================================================= */

TEST(eval_regexp_test) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx, "/\\d+/.test('abc123')"));
    ASSERT_FALSE(eval_bool(ctx, "/\\d+/.test('abcdef')"));
    r8e_context_free(ctx);
}

TEST(eval_regexp_match) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx,
        "'hello123'.match(/\\d+/)[0]", buf);
    ASSERT_EQ_STR(s, "123");
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 12: JSON roundtrip
 * ========================================================================= */

TEST(eval_json_roundtrip) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let o = JSON.parse(JSON.stringify({a: 1})); o.a"), 1);
    r8e_context_free(ctx);
}

TEST(eval_json_array) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = JSON.parse(JSON.stringify([1,2,3])); a[0] + a[1] + a[2]"), 6);
    r8e_context_free(ctx);
}

TEST(eval_json_nested) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let o = JSON.parse(JSON.stringify({a: {b: 42}})); o.a.b"), 42);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 13: Promise and microtask queue
 * ========================================================================= */

TEST(eval_promise_resolve) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* Test that resolved promise callbacks fire via microtask queue */
    ASSERT_EQ_INT(eval_int(ctx,
        "let result = 0;"
        "Promise.resolve(42).then(v => { result = v; });"
        "result"), 42);
    r8e_context_free(ctx);
}

TEST(eval_promise_chain) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let r = 0;"
        "Promise.resolve(1)"
        "  .then(v => v + 1)"
        "  .then(v => v * 10)"
        "  .then(v => { r = v; });"
        "r"), 20);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 14: Destructuring
 * ========================================================================= */

TEST(eval_destructure_object) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let {a, b} = {a: 1, b: 2}; a + b"), 3);
    r8e_context_free(ctx);
}

TEST(eval_destructure_array) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let [x, y, z] = [10, 20, 30]; x + y + z"), 60);
    r8e_context_free(ctx);
}

TEST(eval_destructure_nested) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let {a: {b}} = {a: {b: 42}}; b"), 42);
    r8e_context_free(ctx);
}

TEST(eval_destructure_default) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let {x = 10, y = 20} = {x: 5}; x + y"), 25);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 15: Spread operator
 * ========================================================================= */

TEST(eval_spread_array) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = [1, 2]; let b = [...a, 3]; b.length"), 3);
    r8e_context_free(ctx);
}

TEST(eval_spread_object) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let a = {x: 1}; let b = {...a, y: 2}; b.x + b.y"), 3);
    r8e_context_free(ctx);
}

TEST(eval_spread_function_call) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function add(a, b, c) { return a + b + c; }"
        "add(...[1, 2, 3])"), 6);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 16: Optional chaining
 * ========================================================================= */

TEST(eval_optional_chaining) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let o = {a: {b: 1}}; o?.a?.b"), 1);
    r8e_context_free(ctx);
}

TEST(eval_optional_chaining_null) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    R8EValue v = eval_js(ctx, "let o = null; o?.a?.b");
    ASSERT_TRUE(R8E_IS_UNDEFINED(v));
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 17: Nullish coalescing
 * ========================================================================= */

TEST(eval_nullish_coalescing) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx, "null ?? 'default'", buf);
    ASSERT_EQ_STR(s, "default");
    r8e_context_free(ctx);
}

TEST(eval_nullish_coalescing_zero) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* 0 is not nullish, so should return 0 */
    ASSERT_EQ_INT(eval_int(ctx, "0 ?? 42"), 0);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 18: Template literals
 * ========================================================================= */

TEST(eval_template_literal) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx,
        "let x = 5; `value is ${x}`", buf);
    ASSERT_EQ_STR(s, "value is 5");
    r8e_context_free(ctx);
}

TEST(eval_template_literal_expr) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    const char *s = eval_string(ctx,
        "`${1 + 2} items`", buf);
    ASSERT_EQ_STR(s, "3 items");
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 19: Generators
 * ========================================================================= */

TEST(eval_generator_basic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* function* g() { yield 1; yield 2; } let a = [...g()]; a[0] + a[1] */
    ASSERT_EQ_INT(eval_int(ctx,
        "function* g() { yield 1; yield 2; }"
        "let a = [...g()]; a[0] + a[1]"), 3);
    r8e_context_free(ctx);
}

TEST(eval_generator_for_of) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function* range(n) { for (let i = 0; i < n; i++) yield i; }"
        "let s = 0; for (let v of range(5)) s += v; s"), 10);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 20: Async/await
 * ========================================================================= */

TEST(eval_async_basic) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let r = 0;"
        "async function f() { return 42; }"
        "f().then(v => { r = v; });"
        "r"), 42);
    r8e_context_free(ctx);
}

TEST(eval_async_await) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "let r = 0;"
        "async function f() {"
        "  let v = await Promise.resolve(10);"
        "  return v * 2;"
        "}"
        "f().then(v => { r = v; });"
        "r"), 20);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 21: typeof, instanceof, in operators
 * ========================================================================= */

TEST(eval_typeof) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    char buf[64];
    ASSERT_EQ_STR(eval_string(ctx, "typeof 42", buf), "number");
    ASSERT_EQ_STR(eval_string(ctx, "typeof 'hi'", buf), "string");
    ASSERT_EQ_STR(eval_string(ctx, "typeof true", buf), "boolean");
    ASSERT_EQ_STR(eval_string(ctx, "typeof undefined", buf), "undefined");
    ASSERT_EQ_STR(eval_string(ctx, "typeof {}", buf), "object");
    ASSERT_EQ_STR(eval_string(ctx, "typeof (() => {})", buf), "function");
    r8e_context_free(ctx);
}

TEST(eval_instanceof) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx, "[] instanceof Array"));
    ASSERT_TRUE(eval_bool(ctx, "new Error() instanceof Error"));
    ASSERT_FALSE(eval_bool(ctx, "42 instanceof Number"));
    r8e_context_free(ctx);
}

TEST(eval_in_operator) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx, "'a' in {a: 1}"));
    ASSERT_FALSE(eval_bool(ctx, "'b' in {a: 1}"));
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 22: Comparison and logical operators
 * ========================================================================= */

TEST(eval_comparisons) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx, "5 > 3"));
    ASSERT_TRUE(eval_bool(ctx, "3 < 5"));
    ASSERT_TRUE(eval_bool(ctx, "5 >= 5"));
    ASSERT_TRUE(eval_bool(ctx, "5 <= 5"));
    ASSERT_TRUE(eval_bool(ctx, "5 === 5"));
    ASSERT_FALSE(eval_bool(ctx, "5 === '5'"));
    ASSERT_TRUE(eval_bool(ctx, "5 == '5'"));
    r8e_context_free(ctx);
}

TEST(eval_logical_operators) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(eval_bool(ctx, "true && true"));
    ASSERT_FALSE(eval_bool(ctx, "true && false"));
    ASSERT_TRUE(eval_bool(ctx, "false || true"));
    ASSERT_FALSE(eval_bool(ctx, "false || false"));
    ASSERT_TRUE(eval_bool(ctx, "!false"));
    ASSERT_FALSE(eval_bool(ctx, "!true"));
    r8e_context_free(ctx);
}

/* =========================================================================
 * Test 23: Scoping
 * ========================================================================= */

TEST(eval_block_scoping) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    /* let is block-scoped; accessing it outside the block should fail */
    ASSERT_EQ_INT(eval_int(ctx,
        "let r = 0;"
        "{ let x = 10; r = x; }"
        "r"), 10);
    r8e_context_free(ctx);
}

TEST(eval_function_scoping) {
    R8EContext *ctx = r8e_context_new();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ_INT(eval_int(ctx,
        "function f() { var x = 42; return x; } f()"), 42);
    r8e_context_free(ctx);
}

/* =========================================================================
 * Suite Runner
 * ========================================================================= */

void run_integration_tests(void) {
    /* Basic expressions */
    RUN_TEST(eval_addition);
    RUN_TEST(eval_subtraction);
    RUN_TEST(eval_multiplication);
    RUN_TEST(eval_division);
    RUN_TEST(eval_modulo);
    RUN_TEST(eval_exponentiation);
    RUN_TEST(eval_mixed_arithmetic);

    /* Variable binding */
    RUN_TEST(eval_let_binding);
    RUN_TEST(eval_const_binding);
    RUN_TEST(eval_var_binding);
    RUN_TEST(eval_multiple_bindings);

    /* Function calls */
    RUN_TEST(eval_function_declaration);
    RUN_TEST(eval_function_expression);
    RUN_TEST(eval_arrow_function);
    RUN_TEST(eval_arrow_no_parens);
    RUN_TEST(eval_default_params);
    RUN_TEST(eval_rest_params);

    /* Object creation */
    RUN_TEST(eval_object_literal);
    RUN_TEST(eval_object_computed_property);
    RUN_TEST(eval_object_shorthand);
    RUN_TEST(eval_object_method);

    /* Array operations */
    RUN_TEST(eval_array_literal);
    RUN_TEST(eval_array_map);
    RUN_TEST(eval_array_filter);
    RUN_TEST(eval_array_reduce);
    RUN_TEST(eval_array_push_pop);

    /* Closures */
    RUN_TEST(eval_closure_basic);
    RUN_TEST(eval_closure_shared_state);
    RUN_TEST(eval_closure_over_loop);

    /* Classes */
    RUN_TEST(eval_class_basic);
    RUN_TEST(eval_class_inheritance);
    RUN_TEST(eval_class_getter_setter);

    /* Error handling */
    RUN_TEST(eval_try_catch);
    RUN_TEST(eval_try_finally);
    RUN_TEST(eval_try_catch_finally);
    RUN_TEST(eval_custom_error);

    /* Control flow */
    RUN_TEST(eval_for_loop);
    RUN_TEST(eval_while_loop);
    RUN_TEST(eval_do_while_loop);
    RUN_TEST(eval_for_of);
    RUN_TEST(eval_for_in);
    RUN_TEST(eval_switch);
    RUN_TEST(eval_break_continue);
    RUN_TEST(eval_ternary);

    /* String operations */
    RUN_TEST(eval_string_split_join);
    RUN_TEST(eval_string_length);
    RUN_TEST(eval_string_concat);
    RUN_TEST(eval_string_includes);

    /* Regular expressions */
    RUN_TEST(eval_regexp_test);
    RUN_TEST(eval_regexp_match);

    /* JSON roundtrip */
    RUN_TEST(eval_json_roundtrip);
    RUN_TEST(eval_json_array);
    RUN_TEST(eval_json_nested);

    /* Promise */
    RUN_TEST(eval_promise_resolve);
    RUN_TEST(eval_promise_chain);

    /* Destructuring */
    RUN_TEST(eval_destructure_object);
    RUN_TEST(eval_destructure_array);
    RUN_TEST(eval_destructure_nested);
    RUN_TEST(eval_destructure_default);

    /* Spread */
    RUN_TEST(eval_spread_array);
    RUN_TEST(eval_spread_object);
    RUN_TEST(eval_spread_function_call);

    /* Optional chaining */
    RUN_TEST(eval_optional_chaining);
    RUN_TEST(eval_optional_chaining_null);

    /* Nullish coalescing */
    RUN_TEST(eval_nullish_coalescing);
    RUN_TEST(eval_nullish_coalescing_zero);

    /* Template literals */
    RUN_TEST(eval_template_literal);
    RUN_TEST(eval_template_literal_expr);

    /* Generators */
    RUN_TEST(eval_generator_basic);
    RUN_TEST(eval_generator_for_of);

    /* Async/await */
    RUN_TEST(eval_async_basic);
    RUN_TEST(eval_async_await);

    /* typeof, instanceof, in */
    RUN_TEST(eval_typeof);
    RUN_TEST(eval_instanceof);
    RUN_TEST(eval_in_operator);

    /* Comparison and logical */
    RUN_TEST(eval_comparisons);
    RUN_TEST(eval_logical_operators);

    /* Scoping */
    RUN_TEST(eval_block_scoping);
    RUN_TEST(eval_function_scoping);
}
