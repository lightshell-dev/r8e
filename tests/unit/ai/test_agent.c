/*
 * test_agent.c - Unit tests for r8e_agent.c
 *
 * Tests cover:
 *   - Parse tool calls from Anthropic-style JSON responses
 *   - Format tool results into message JSON
 *   - Detect tool calls in response JSON
 *   - Build complete message arrays with system prompt and tools
 *   - Extract text content from responses
 *   - Extract stop reason from responses
 *   - Edge cases: empty content, malformed JSON, buffer overflow
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../../include/r8e_agent.h"

/* =========================================================================
 * Test Infrastructure (imports from test_runner.c)
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
    if ((expr)) {                                                   \
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

#define ASSERT_STRN_EQ(a, alen, b) do {                             \
    const char *_a = (a); int _alen = (alen);                       \
    const char *_b = (b); int _blen = (int)strlen(_b);              \
    if (_a == NULL || _alen != _blen ||                              \
        memcmp(_a, _b, _alen) != 0) {                               \
        fprintf(stderr, "    ASSERT_STRN_EQ failed: %s == %s\n"     \
                "      got \"%.*s\" (len=%d) vs \"%s\" (len=%d)\n"   \
                "      at %s:%d\n",                                 \
                #a, #b,                                              \
                _a ? _alen : 6, _a ? _a : "(null)", _alen,          \
                _b, _blen, __FILE__, __LINE__);                      \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* =========================================================================
 * Test Data - Anthropic Messages API Response Fixtures
 * ========================================================================= */

/* Simple text response */
static const char *RESPONSE_TEXT =
    "{"
    "  \"content\": ["
    "    {\"type\": \"text\", \"text\": \"Hello, world!\"}"
    "  ],"
    "  \"stop_reason\": \"end_turn\","
    "  \"usage\": {\"input_tokens\": 10, \"output_tokens\": 5}"
    "}";

/* Single tool call response */
static const char *RESPONSE_TOOL_CALL =
    "{"
    "  \"content\": ["
    "    {\"type\": \"text\", \"text\": \"Let me check that.\"},"
    "    {\"type\": \"tool_use\", \"id\": \"toolu_abc123\", \"name\": \"get_weather\","
    "     \"input\": {\"city\": \"London\", \"units\": \"celsius\"}}"
    "  ],"
    "  \"stop_reason\": \"tool_use\""
    "}";

/* Multiple tool calls response */
static const char *RESPONSE_MULTI_TOOL =
    "{"
    "  \"content\": ["
    "    {\"type\": \"tool_use\", \"id\": \"toolu_001\", \"name\": \"read_file\","
    "     \"input\": {\"path\": \"/tmp/a.txt\"}},"
    "    {\"type\": \"tool_use\", \"id\": \"toolu_002\", \"name\": \"read_file\","
    "     \"input\": {\"path\": \"/tmp/b.txt\"}}"
    "  ],"
    "  \"stop_reason\": \"tool_use\""
    "}";

/* Text-only response (no tool calls) */
static const char *RESPONSE_NO_TOOLS =
    "{"
    "  \"content\": ["
    "    {\"type\": \"text\", \"text\": \"Just text here.\"}"
    "  ],"
    "  \"stop_reason\": \"end_turn\""
    "}";

/* =========================================================================
 * Tests: Tool Call Parsing
 * ========================================================================= */

TEST(parse_single_tool_call)
{
    R8EToolCall calls[4];
    int n = r8e_agent_parse_tool_calls(RESPONSE_TOOL_CALL,
                                        (int)strlen(RESPONSE_TOOL_CALL),
                                        calls, 4);
    ASSERT_EQ_INT(n, 1);
    ASSERT_STRN_EQ(calls[0].name, calls[0].name_len, "get_weather");
    ASSERT_STRN_EQ(calls[0].id, calls[0].id_len, "toolu_abc123");
    ASSERT_TRUE(calls[0].args_len > 0);
    ASSERT_TRUE(calls[0].args_json != NULL);
}

TEST(parse_multiple_tool_calls)
{
    R8EToolCall calls[4];
    int n = r8e_agent_parse_tool_calls(RESPONSE_MULTI_TOOL,
                                        (int)strlen(RESPONSE_MULTI_TOOL),
                                        calls, 4);
    ASSERT_EQ_INT(n, 2);
    ASSERT_STRN_EQ(calls[0].name, calls[0].name_len, "read_file");
    ASSERT_STRN_EQ(calls[0].id, calls[0].id_len, "toolu_001");
    ASSERT_STRN_EQ(calls[1].name, calls[1].name_len, "read_file");
    ASSERT_STRN_EQ(calls[1].id, calls[1].id_len, "toolu_002");
}

TEST(parse_no_tool_calls)
{
    R8EToolCall calls[4];
    int n = r8e_agent_parse_tool_calls(RESPONSE_TEXT,
                                        (int)strlen(RESPONSE_TEXT),
                                        calls, 4);
    ASSERT_EQ_INT(n, 0);
}

TEST(parse_tool_calls_null_input)
{
    R8EToolCall calls[4];
    ASSERT_EQ_INT(r8e_agent_parse_tool_calls(NULL, 0, calls, 4), -1);
    ASSERT_EQ_INT(r8e_agent_parse_tool_calls("", 0, calls, 4), -1);
    ASSERT_EQ_INT(r8e_agent_parse_tool_calls("{}", 2, NULL, 4), -1);
    ASSERT_EQ_INT(r8e_agent_parse_tool_calls("{}", 2, calls, 0), -1);
}

TEST(parse_tool_calls_max_capacity)
{
    /* Request only 1 slot for a response with 2 tool calls */
    R8EToolCall calls[1];
    int n = r8e_agent_parse_tool_calls(RESPONSE_MULTI_TOOL,
                                        (int)strlen(RESPONSE_MULTI_TOOL),
                                        calls, 1);
    ASSERT_EQ_INT(n, 1);
    ASSERT_STRN_EQ(calls[0].id, calls[0].id_len, "toolu_001");
}

/* =========================================================================
 * Tests: Tool Call Detection
 * ========================================================================= */

TEST(has_tool_calls_true)
{
    ASSERT_TRUE(r8e_agent_has_tool_calls(RESPONSE_TOOL_CALL,
                                          (int)strlen(RESPONSE_TOOL_CALL)));
}

TEST(has_tool_calls_false)
{
    ASSERT_FALSE(r8e_agent_has_tool_calls(RESPONSE_NO_TOOLS,
                                           (int)strlen(RESPONSE_NO_TOOLS)));
}

TEST(has_tool_calls_null)
{
    ASSERT_FALSE(r8e_agent_has_tool_calls(NULL, 0));
    ASSERT_FALSE(r8e_agent_has_tool_calls("", 0));
}

TEST(has_tool_calls_multi)
{
    ASSERT_TRUE(r8e_agent_has_tool_calls(RESPONSE_MULTI_TOOL,
                                          (int)strlen(RESPONSE_MULTI_TOOL)));
}

/* =========================================================================
 * Tests: Tool Result Formatting
 * ========================================================================= */

TEST(format_tool_result_basic)
{
    char buf[512];
    int n = r8e_agent_format_tool_result("toolu_abc123",
                                          "The weather is sunny, 22C",
                                          buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"tool_use_id\":\"toolu_abc123\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"tool_result\"") != NULL);
    ASSERT_TRUE(strstr(buf, "The weather is sunny, 22C") != NULL);
}

TEST(format_tool_result_escaping)
{
    char buf[512];
    int n = r8e_agent_format_tool_result("toolu_001",
                                          "line1\nline2\t\"quoted\"",
                                          buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    /* Should have escaped newline and quotes */
    ASSERT_TRUE(strstr(buf, "\\n") != NULL);
    ASSERT_TRUE(strstr(buf, "\\\"quoted\\\"") != NULL);
}

TEST(format_tool_result_overflow)
{
    char buf[16];  /* too small */
    int n = r8e_agent_format_tool_result("toolu_abc123",
                                          "some result",
                                          buf, sizeof(buf));
    ASSERT_EQ_INT(n, -1);
}

TEST(format_tool_result_null)
{
    char buf[512];
    ASSERT_EQ_INT(r8e_agent_format_tool_result(NULL, "x", buf, 512), -1);
    ASSERT_EQ_INT(r8e_agent_format_tool_result("id", NULL, buf, 512), -1);
    ASSERT_EQ_INT(r8e_agent_format_tool_result("id", "x", NULL, 512), -1);
}

/* =========================================================================
 * Tests: Message Building
 * ========================================================================= */

TEST(build_messages_basic)
{
    char buf[2048];
    const char *msgs[] = {
        "{\"role\":\"user\",\"content\":\"Hello\"}",
        "{\"role\":\"assistant\",\"content\":\"Hi there!\"}"
    };

    int n = r8e_agent_build_messages(buf, sizeof(buf),
                                      "You are a helpful assistant.",
                                      msgs, 2, NULL, 0);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"system\":\"You are a helpful assistant.\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"messages\":[") != NULL);
    ASSERT_TRUE(strstr(buf, "\"role\":\"user\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"role\":\"assistant\"") != NULL);
}

TEST(build_messages_with_tools)
{
    char buf[4096];
    const char *msgs[] = {
        "{\"role\":\"user\",\"content\":\"What is the weather?\"}"
    };

    R8EAgentTool tools[] = {
        {
            .name = "get_weather",
            .description = "Get current weather for a city",
            .input_schema_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}"
        }
    };

    int n = r8e_agent_build_messages(buf, sizeof(buf),
                                      "You are helpful.",
                                      msgs, 1, tools, 1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"tools\":[") != NULL);
    ASSERT_TRUE(strstr(buf, "\"get_weather\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"input_schema\":{") != NULL);
}

TEST(build_messages_no_system)
{
    char buf[1024];
    const char *msgs[] = {
        "{\"role\":\"user\",\"content\":\"Hi\"}"
    };

    int n = r8e_agent_build_messages(buf, sizeof(buf),
                                      NULL, msgs, 1, NULL, 0);
    ASSERT_TRUE(n > 0);
    /* Should not have system field */
    ASSERT_TRUE(strstr(buf, "\"system\"") == NULL);
    ASSERT_TRUE(strstr(buf, "\"messages\":[") != NULL);
}

TEST(build_messages_overflow)
{
    char buf[16];
    const char *msgs[] = {
        "{\"role\":\"user\",\"content\":\"This is a long message that will overflow\"}"
    };

    int n = r8e_agent_build_messages(buf, sizeof(buf),
                                      "System prompt",
                                      msgs, 1, NULL, 0);
    ASSERT_EQ_INT(n, -1);
}

TEST(build_messages_empty)
{
    char buf[1024];
    int n = r8e_agent_build_messages(buf, sizeof(buf),
                                      NULL, NULL, 0, NULL, 0);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_STR(buf, "{\"messages\":[]}");
}

/* =========================================================================
 * Tests: Text Extraction
 * ========================================================================= */

TEST(extract_text_simple)
{
    char buf[256];
    int n = r8e_agent_extract_text(RESPONSE_TEXT,
                                    (int)strlen(RESPONSE_TEXT),
                                    buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_STR(buf, "Hello, world!");
}

TEST(extract_text_with_tool_calls)
{
    /* Should extract only the text block, not the tool_use block */
    char buf[256];
    int n = r8e_agent_extract_text(RESPONSE_TOOL_CALL,
                                    (int)strlen(RESPONSE_TOOL_CALL),
                                    buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_STR(buf, "Let me check that.");
}

TEST(extract_text_no_text_blocks)
{
    /* Response with only tool calls, no text */
    char buf[256];
    int n = r8e_agent_extract_text(RESPONSE_MULTI_TOOL,
                                    (int)strlen(RESPONSE_MULTI_TOOL),
                                    buf, sizeof(buf));
    ASSERT_EQ_INT(n, 0);
}

/* =========================================================================
 * Tests: Stop Reason Extraction
 * ========================================================================= */

TEST(extract_stop_reason_end_turn)
{
    char buf[64];
    int n = r8e_agent_extract_stop_reason(RESPONSE_TEXT,
                                           (int)strlen(RESPONSE_TEXT),
                                           buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_STR(buf, "end_turn");
}

TEST(extract_stop_reason_tool_use)
{
    char buf[64];
    int n = r8e_agent_extract_stop_reason(RESPONSE_TOOL_CALL,
                                           (int)strlen(RESPONSE_TOOL_CALL),
                                           buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_STR(buf, "tool_use");
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_agent_tests(void)
{
    /* Tool call parsing */
    RUN_TEST(parse_single_tool_call);
    RUN_TEST(parse_multiple_tool_calls);
    RUN_TEST(parse_no_tool_calls);
    RUN_TEST(parse_tool_calls_null_input);
    RUN_TEST(parse_tool_calls_max_capacity);

    /* Tool call detection */
    RUN_TEST(has_tool_calls_true);
    RUN_TEST(has_tool_calls_false);
    RUN_TEST(has_tool_calls_null);
    RUN_TEST(has_tool_calls_multi);

    /* Tool result formatting */
    RUN_TEST(format_tool_result_basic);
    RUN_TEST(format_tool_result_escaping);
    RUN_TEST(format_tool_result_overflow);
    RUN_TEST(format_tool_result_null);

    /* Message building */
    RUN_TEST(build_messages_basic);
    RUN_TEST(build_messages_with_tools);
    RUN_TEST(build_messages_no_system);
    RUN_TEST(build_messages_overflow);
    RUN_TEST(build_messages_empty);

    /* Text extraction */
    RUN_TEST(extract_text_simple);
    RUN_TEST(extract_text_with_tool_calls);
    RUN_TEST(extract_text_no_text_blocks);

    /* Stop reason */
    RUN_TEST(extract_stop_reason_end_turn);
    RUN_TEST(extract_stop_reason_tool_use);
}
