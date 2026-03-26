/*
 * test_html.c - Unit tests for the r8e HTML parser
 *
 * Tests cover:
 *   - Simple element parsing
 *   - Nested element parsing
 *   - Attribute parsing (quoted, boolean)
 *   - Style block extraction
 *   - Inline script extraction
 *   - External script (src) extraction
 *   - Self-closing / void tags
 *   - Full page parsing with DOCTYPE, head, body
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

/* =========================================================================
 * Minimal type definitions (self-contained, matching source files)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* =========================================================================
 * Test infrastructure (shared globals from test_runner.c)
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n" \
                "      at %s:%d\n", #e, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n" \
                "      got %lld vs %lld\n" \
                "      at %s:%d\n", #a, #b, _a, _b, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "    ASSERT_EQ_STR failed: %s == %s\n" \
                "      got \"%s\" vs \"%s\"\n" \
                "      at %s:%d\n", #a, #b, \
                _a ? _a : "(null)", _b ? _b : "(null)", \
                __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

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

/* =========================================================================
 * HTML parser API (from r8e_html.h)
 * ========================================================================= */

typedef struct {
    char *css;
    uint32_t css_len;
    struct {
        char *content;
        char *src;
        uint32_t content_len;
    } *scripts;
    uint32_t script_count;
    void *root_node;
} R8EHTMLResult;

R8EHTMLResult *r8e_html_parse(const char *html, uint32_t len);
void r8e_html_result_free(R8EHTMLResult *result);

/* =========================================================================
 * Tests
 * ========================================================================= */

TEST(html_parse_simple_div) {
    R8EHTMLResult *r = r8e_html_parse("<div>hello</div>", 16);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->root_node != NULL);
    r8e_html_result_free(r);
}

TEST(html_parse_nested) {
    const char *html = "<div><span>inner</span></div>";
    R8EHTMLResult *r = r8e_html_parse(html, (uint32_t)strlen(html));
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->root_node != NULL);
    r8e_html_result_free(r);
}

TEST(html_parse_attributes) {
    const char *html = "<div class=\"foo\" id=\"bar\">text</div>";
    R8EHTMLResult *r = r8e_html_parse(html, (uint32_t)strlen(html));
    ASSERT_TRUE(r != NULL);
    r8e_html_result_free(r);
}

TEST(html_extract_style) {
    const char *html = "<style>body{color:red}</style><div>hi</div>";
    R8EHTMLResult *r = r8e_html_parse(html, (uint32_t)strlen(html));
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->css != NULL);
    ASSERT_TRUE(r->css_len > 0);
    ASSERT_TRUE(strstr(r->css, "color:red") != NULL);
    r8e_html_result_free(r);
}

TEST(html_extract_script_inline) {
    const char *html = "<script>var x = 1;</script>";
    R8EHTMLResult *r = r8e_html_parse(html, (uint32_t)strlen(html));
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->script_count == 1);
    ASSERT_TRUE(r->scripts[0].content != NULL);
    ASSERT_TRUE(strstr(r->scripts[0].content, "var x") != NULL);
    r8e_html_result_free(r);
}

TEST(html_extract_script_src) {
    const char *html = "<script src=\"app.js\"></script>";
    R8EHTMLResult *r = r8e_html_parse(html, (uint32_t)strlen(html));
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->script_count == 1);
    ASSERT_TRUE(r->scripts[0].src != NULL);
    ASSERT_TRUE(strcmp(r->scripts[0].src, "app.js") == 0);
    r8e_html_result_free(r);
}

TEST(html_self_closing_tags) {
    const char *html = "<div><br><img src=\"x.png\"><hr></div>";
    R8EHTMLResult *r = r8e_html_parse(html, (uint32_t)strlen(html));
    ASSERT_TRUE(r != NULL);
    r8e_html_result_free(r);
}

TEST(html_full_page) {
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<style>body{margin:0}</style>"
        "</head><body>"
        "<div id=\"app\"><h1>Hello</h1><p>World</p></div>"
        "<script>console.log('hi')</script>"
        "</body></html>";
    uint32_t hlen = (uint32_t)strlen(html);
    R8EHTMLResult *r = r8e_html_parse(html, hlen);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->css != NULL);
    ASSERT_TRUE(r->css_len > 0);
    ASSERT_TRUE(r->script_count == 1);
    ASSERT_TRUE(r->root_node != NULL);
    r8e_html_result_free(r);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_html_tests(void) {
    RUN_TEST(html_parse_simple_div);
    RUN_TEST(html_parse_nested);
    RUN_TEST(html_parse_attributes);
    RUN_TEST(html_extract_style);
    RUN_TEST(html_extract_script_inline);
    RUN_TEST(html_extract_script_src);
    RUN_TEST(html_self_closing_tags);
    RUN_TEST(html_full_page);
    fflush(stdout);
    fflush(stderr);
}
