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
#include <sys/wait.h>
#include <unistd.h>

/* =========================================================================
 * Test Infrastructure
 * ========================================================================= */

int g_tests_run    = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_assert_fail  = 0;  /* set within a test to mark failure */

/* Crash recovery: fork-based isolation for each test.
 * Each test runs in a child process. If the child crashes,
 * the parent records a failure and continues. */

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    g_assert_fail = 0;                                              \
    g_tests_run++;                                                  \
    printf("  %-60s ", #name);                                      \
    fflush(stdout);                                                 \
    fflush(stderr);                                                 \
    pid_t pid = fork();                                             \
    if (pid == 0) {                                                 \
        /* Child: run test with 5-second timeout */                 \
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

/* Phase 5: AI primitives */
void run_sse_parser_tests(void);
void run_schema_validator_tests(void);
void run_gguf_tests(void);
void run_tokenizer_tests(void);

/* Phase 5 continued: AI agent and conversations */
void run_agent_tests(void);
void run_conversations_tests(void);

/* Phase AI-2: Sampler, KV cache, chat template */
void run_sampler_tests(void);
void run_kvcache_tests(void);
void run_chat_template_tests(void);

/* Phase 6: MCP (Model Context Protocol) */
void run_mcp_jsonrpc_tests(void);

/* API wiring tests */
void run_api_wire_tests(void);

/* Parser member assignment tests */
void run_parse_member_assign_tests(void);

/* Parser destructuring tests */
void run_parse_destructuring_tests(void);

/* =========================================================================
 * Main
 * ========================================================================= */

/* Run a test suite in a forked child with crash isolation.
 * The child communicates updated test counts back to the parent via pipe. */
static void run_suite_isolated(const char *label, void (*suite_fn)(void)) {
    int pipefd[2];
    printf("[%s]\n", label);
    fflush(stdout);
    fflush(stderr);

    if (pipe(pipefd) != 0) {
        /* pipe failed; run in-process as fallback */
        suite_fn();
        printf("\n");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: run suite, write counts, exit */
        close(pipefd[0]);
        alarm(60); /* 60s timeout for entire suite */
        suite_fn();
        /* Send counts to parent */
        int counts[3] = { g_tests_run, g_tests_passed, g_tests_failed };
        (void)write(pipefd[1], counts, sizeof(counts));
        close(pipefd[1]);
        _exit(0);
    } else if (pid > 0) {
        /* Parent: wait for child, read counts */
        close(pipefd[1]);
        int counts[3] = { 0, 0, 0 };
        ssize_t n = read(pipefd[0], counts, sizeof(counts));
        close(pipefd[0]);
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);

        if (n == (ssize_t)sizeof(counts)) {
            /* Child finished normally - use its counts */
            g_tests_run = counts[0];
            g_tests_passed = counts[1];
            g_tests_failed = counts[2];
        } else if (WIFSIGNALED(wstatus)) {
            /* Child crashed before writing counts */
            fprintf(stderr, "  [%s] SUITE CRASHED (signal %d)\n",
                    label, WTERMSIG(wstatus));
            g_tests_run++;
            g_tests_failed++;
        }
    } else {
        /* fork failed */
        suite_fn();
    }
    printf("\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== r8e unit tests ===\n\n");

    /* Phase 1: Foundation (stable - run in-process) */
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

    /* Phase 2+: Runtime and beyond (fork-isolated to survive crashes) */
    run_suite_isolated("object", run_object_tests);
    run_suite_isolated("array", run_array_tests);
    run_suite_isolated("gc", run_gc_tests);
    run_suite_isolated("closure", run_closure_tests);
    run_suite_isolated("interp", run_interp_tests);
    run_suite_isolated("parse", run_parse_tests);
    run_suite_isolated("function", run_function_tests);
    run_suite_isolated("error", run_error_tests);
    run_suite_isolated("json", run_json_tests);
    run_suite_isolated("bc", run_bc_tests);
    run_suite_isolated("scope", run_scope_tests);
    run_suite_isolated("regexp", run_regexp_tests);
    run_suite_isolated("promise", run_promise_tests);
    run_suite_isolated("integration", run_integration_tests);
    run_suite_isolated("iterator", run_iterator_tests);
    run_suite_isolated("proxy", run_proxy_tests);
    run_suite_isolated("weakref", run_weakref_tests);
    run_suite_isolated("module", run_module_tests);
    run_suite_isolated("security", run_security_tests);
    run_suite_isolated("builtin", run_builtin_tests);
    run_suite_isolated("security-deep", run_security_deep_tests);
    run_suite_isolated("ui", run_ui_tests);
    run_suite_isolated("sse-parser", run_sse_parser_tests);
    run_suite_isolated("schema-validator", run_schema_validator_tests);
    run_suite_isolated("gguf", run_gguf_tests);
    run_suite_isolated("tokenizer", run_tokenizer_tests);
    run_suite_isolated("agent", run_agent_tests);
    run_suite_isolated("conversations", run_conversations_tests);
    run_suite_isolated("sampler", run_sampler_tests);
    run_suite_isolated("kvcache", run_kvcache_tests);
    run_suite_isolated("chat-template", run_chat_template_tests);
    run_suite_isolated("mcp-jsonrpc", run_mcp_jsonrpc_tests);
    run_suite_isolated("api-wire", run_api_wire_tests);
    run_suite_isolated("parse-member-assign", run_parse_member_assign_tests);
    run_suite_isolated("parse-destructuring", run_parse_destructuring_tests);

    printf("=== Summary: %d passed, %d failed out of %d tests ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
