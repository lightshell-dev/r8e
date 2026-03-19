/*
 * test_sse_parser.c - Unit tests for r8e_sse_parser.c
 *
 * Tests cover:
 *   - Complete Anthropic text streaming response
 *   - Complete OpenAI text streaming response
 *   - Tool call events from both providers
 *   - Chunked delivery (split events across multiple feed() calls)
 *   - data: [DONE] termination
 *   - Usage/token count events
 *   - Error events
 *   - Edge cases: empty data, comments, no trailing newline, \r\n
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../../include/r8e_sse_parser.h"

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

#define ASSERT_STRN_EQ(a, a_len, b) do {                            \
    const char *_a = (a), *_b = (b);                                \
    int _len = (a_len);                                             \
    if (_a == NULL || _b == NULL ||                                  \
        _len != (int)strlen(_b) ||                                  \
        memcmp(_a, _b, (size_t)_len) != 0) {                       \
        fprintf(stderr, "    ASSERT_STRN_EQ failed: %s == %s\n"     \
                "      got \"%.*s\" (len=%d) vs \"%s\"\n"            \
                "      at %s:%d\n",                                 \
                #a, #b,                                              \
                _a ? _len : 6, _a ? _a : "(null)", _len,             \
                _b ? _b : "(null)",                                  \
                __FILE__, __LINE__);                                 \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* =========================================================================
 * Test Helpers: Event Collector
 *
 * Collects events into a simple array for verification after feed().
 * ========================================================================= */

#define MAX_COLLECTED_EVENTS 32
#define MAX_TEXT_BUF         4096

typedef struct {
    R8EAIEventType type;
    char  text[512];
    int   text_len;
    char  tool_name[128];
    int   tool_name_len;
    char  tool_id[128];
    int   tool_id_len;
    int   input_tokens;
    int   output_tokens;
    char  error_msg[256];
    int   error_msg_len;
} CollectedEvent;

typedef struct {
    CollectedEvent events[MAX_COLLECTED_EVENTS];
    int            count;
} EventCollector;

static void collect_event(R8EAIEvent *ev, void *ud)
{
    EventCollector *c = (EventCollector *)ud;
    if (c->count >= MAX_COLLECTED_EVENTS) return;

    CollectedEvent *ce = &c->events[c->count++];
    memset(ce, 0, sizeof(*ce));
    ce->type = ev->type;

    if (ev->text && ev->text_len > 0) {
        int copy = ev->text_len < 511 ? ev->text_len : 511;
        memcpy(ce->text, ev->text, (size_t)copy);
        ce->text[copy] = '\0';
        ce->text_len = ev->text_len;
    }
    if (ev->tool_name && ev->tool_name_len > 0) {
        int copy = ev->tool_name_len < 127 ? ev->tool_name_len : 127;
        memcpy(ce->tool_name, ev->tool_name, (size_t)copy);
        ce->tool_name[copy] = '\0';
        ce->tool_name_len = ev->tool_name_len;
    }
    if (ev->tool_id && ev->tool_id_len > 0) {
        int copy = ev->tool_id_len < 127 ? ev->tool_id_len : 127;
        memcpy(ce->tool_id, ev->tool_id, (size_t)copy);
        ce->tool_id[copy] = '\0';
        ce->tool_id_len = ev->tool_id_len;
    }
    ce->input_tokens = ev->input_tokens;
    ce->output_tokens = ev->output_tokens;
    if (ev->error_msg && ev->error_msg_len > 0) {
        int copy = ev->error_msg_len < 255 ? ev->error_msg_len : 255;
        memcpy(ce->error_msg, ev->error_msg, (size_t)copy);
        ce->error_msg[copy] = '\0';
        ce->error_msg_len = ev->error_msg_len;
    }
}

/* =========================================================================
 * Tests: Anthropic Provider
 * ========================================================================= */

TEST(anthropic_text_streaming)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":25}}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\" world\"}}\n"
        "\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":12}}\n"
        "\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 5);

    /* message_start -> usage event with input_tokens */
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_USAGE);
    ASSERT_EQ_INT(c.events[0].input_tokens, 25);

    /* First text delta */
    ASSERT_EQ_INT(c.events[1].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[1].text, "Hello");

    /* Second text delta */
    ASSERT_EQ_INT(c.events[2].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[2].text, " world");

    /* message_delta -> usage with output_tokens */
    ASSERT_EQ_INT(c.events[3].type, R8E_AI_EVENT_USAGE);
    ASSERT_EQ_INT(c.events[3].output_tokens, 12);

    /* message_stop -> done */
    ASSERT_EQ_INT(c.events[4].type, R8E_AI_EVENT_DONE);

    r8e_sse_parser_free(p);
}

TEST(anthropic_tool_call)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01A\",\"name\":\"get_weather\"}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"location\\\"\"}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\":\\\"NYC\\\"}\"}}\n"
        "\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\"}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 4);

    /* Tool call start */
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TOOL_CALL_START);
    ASSERT_EQ_STR(c.events[0].tool_name, "get_weather");
    ASSERT_EQ_STR(c.events[0].tool_id, "toolu_01A");

    /* Tool call deltas */
    ASSERT_EQ_INT(c.events[1].type, R8E_AI_EVENT_TOOL_CALL_DELTA);
    ASSERT_EQ_STR(c.events[1].text, "{\"location\"");

    ASSERT_EQ_INT(c.events[2].type, R8E_AI_EVENT_TOOL_CALL_DELTA);
    ASSERT_EQ_STR(c.events[2].text, ":\"NYC\"}");

    /* Tool call end */
    ASSERT_EQ_INT(c.events[3].type, R8E_AI_EVENT_TOOL_CALL_END);

    r8e_sse_parser_free(p);
}

TEST(anthropic_done_sentinel)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "data: [DONE]\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_DONE);

    r8e_sse_parser_free(p);
}

TEST(anthropic_error_event)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"message\":\"rate limit exceeded\"}}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_ERROR);
    ASSERT_EQ_STR(c.events[0].error_msg, "rate limit exceeded");

    r8e_sse_parser_free(p);
}

/* =========================================================================
 * Tests: OpenAI Provider
 * ========================================================================= */

TEST(openai_text_streaming)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n"
        "\n"
        "data: [DONE]\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 3);

    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "Hello");

    ASSERT_EQ_INT(c.events[1].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[1].text, " world");

    /* finish_reason "stop" is silently consumed; [DONE] -> DONE */
    ASSERT_EQ_INT(c.events[2].type, R8E_AI_EVENT_DONE);

    r8e_sse_parser_free(p);
}

TEST(openai_tool_call)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_abc\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":{\"arguments\":\"{\\\"loc\"}}]}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":{\"arguments\":\"ation\\\":\\\"NYC\\\"}\"}}]}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n"
        "\n"
        "data: [DONE]\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 5);

    /* Tool call start (name + id in first chunk) */
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TOOL_CALL_START);
    ASSERT_EQ_STR(c.events[0].tool_name, "get_weather");
    ASSERT_EQ_STR(c.events[0].tool_id, "call_abc");

    /* Tool call argument deltas */
    ASSERT_EQ_INT(c.events[1].type, R8E_AI_EVENT_TOOL_CALL_DELTA);
    ASSERT_EQ_STR(c.events[1].text, "{\"loc");

    ASSERT_EQ_INT(c.events[2].type, R8E_AI_EVENT_TOOL_CALL_DELTA);
    ASSERT_EQ_STR(c.events[2].text, "ation\":\"NYC\"}");

    /* finish_reason "tool_calls" -> TOOL_CALL_END */
    ASSERT_EQ_INT(c.events[3].type, R8E_AI_EVENT_TOOL_CALL_END);

    /* [DONE] */
    ASSERT_EQ_INT(c.events[4].type, R8E_AI_EVENT_DONE);

    r8e_sse_parser_free(p);
}

TEST(openai_usage_event)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "data: {\"usage\":{\"prompt_tokens\":15,\"completion_tokens\":42}}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_USAGE);
    ASSERT_EQ_INT(c.events[0].input_tokens, 15);
    ASSERT_EQ_INT(c.events[0].output_tokens, 42);

    r8e_sse_parser_free(p);
}

/* =========================================================================
 * Tests: Chunked Delivery (Partial Lines)
 * ========================================================================= */

TEST(chunked_across_boundaries)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Split a complete event across three feed() calls */
    const char *chunk1 = "event: content_block";
    const char *chunk2 = "_delta\ndata: {\"type\":\"content_block_delta\",\"del";
    const char *chunk3 = "ta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n";

    r8e_sse_parser_feed(p, chunk1, (int)strlen(chunk1), collect_event, &c);
    ASSERT_EQ_INT(c.count, 0); /* no complete event yet */

    r8e_sse_parser_feed(p, chunk2, (int)strlen(chunk2), collect_event, &c);
    ASSERT_EQ_INT(c.count, 0); /* still incomplete */

    r8e_sse_parser_feed(p, chunk3, (int)strlen(chunk3), collect_event, &c);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "Hi");

    r8e_sse_parser_free(p);
}

TEST(chunked_split_at_newline)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Split right at the \n\n boundary */
    const char *chunk1 = "data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n";
    const char *chunk2 = "\n";

    r8e_sse_parser_feed(p, chunk1, (int)strlen(chunk1), collect_event, &c);
    ASSERT_EQ_INT(c.count, 0); /* data line parsed but no empty line yet */

    r8e_sse_parser_feed(p, chunk2, (int)strlen(chunk2), collect_event, &c);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "OK");

    r8e_sse_parser_free(p);
}

/* =========================================================================
 * Tests: Edge Cases
 * ========================================================================= */

TEST(comments_ignored)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        ": this is a comment\n"
        "event: content_block_delta\n"
        ": another comment\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"A\"}}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "A");

    r8e_sse_parser_free(p);
}

TEST(crlf_line_endings)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"CR\"}}]}\r\n"
        "\r\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "CR");

    r8e_sse_parser_free(p);
}

TEST(empty_data_field)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Empty data: line followed by empty line should not crash */
    const char *stream =
        "data:\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    /* Empty data = no meaningful event dispatched (data_len is 0) */
    ASSERT_EQ_INT(c.count, 0);

    r8e_sse_parser_free(p);
}

TEST(multiple_data_lines)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Per SSE spec, multiple data: lines join with \n.
       However our provider dispatch expects JSON so this is unusual.
       Just verify it doesn't crash and data gets concatenated. */
    const char *stream =
        "event: message_stop\n"
        "data: {\"type\":\n"
        "data: \"message_stop\"}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    /* message_stop dispatches as DONE regardless of data content */
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_DONE);

    r8e_sse_parser_free(p);
}

TEST(parser_reset)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Feed a partial event */
    const char *partial = "event: content_block_delta\ndata: {\"type\":\"con";
    r8e_sse_parser_feed(p, partial, (int)strlen(partial), collect_event, &c);
    ASSERT_EQ_INT(c.count, 0);

    /* Reset the parser */
    r8e_sse_parser_reset(p);

    /* Feed a complete new event - should work clean */
    const char *stream =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"X\"}}\n"
        "\n";
    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "X");

    r8e_sse_parser_free(p);
}

TEST(null_safety)
{
    /* NULL parser should not crash */
    r8e_sse_parser_feed(NULL, "data: x\n\n", 9, collect_event, NULL);
    r8e_sse_parser_free(NULL);
    r8e_sse_parser_reset(NULL);

    /* NULL chunk should not crash */
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);
    r8e_sse_parser_feed(p, NULL, 0, collect_event, NULL);
    r8e_sse_parser_feed(p, "data: x\n", 0, collect_event, NULL);
    r8e_sse_parser_free(p);
}

TEST(no_trailing_newline)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Data without trailing \n\n - should buffer, then flush on next empty */
    const char *chunk1 = "data: {\"choices\":[{\"delta\":{\"content\":\"buf\"}}]}";
    r8e_sse_parser_feed(p, chunk1, (int)strlen(chunk1), collect_event, &c);
    ASSERT_EQ_INT(c.count, 0);

    /* Now send the terminating newlines */
    const char *chunk2 = "\n\n";
    r8e_sse_parser_feed(p, chunk2, (int)strlen(chunk2), collect_event, &c);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "buf");

    r8e_sse_parser_free(p);
}

TEST(escaped_text_in_delta)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    /* Text containing escaped characters */
    const char *stream =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"line1\\nline2\\t\\\"quoted\\\"\"}}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_TEXT_DELTA);
    ASSERT_EQ_STR(c.events[0].text, "line1\nline2\t\"quoted\"");

    r8e_sse_parser_free(p);
}

TEST(openai_done_sentinel)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_OPENAI);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "data: [DONE]\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_DONE);

    r8e_sse_parser_free(p);
}

TEST(anthropic_usage_input_tokens)
{
    R8ESSEParser *p = r8e_sse_parser_new(R8E_PROVIDER_ANTHROPIC);
    ASSERT_TRUE(p != NULL);

    EventCollector c = {0};

    const char *stream =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":100}}}\n"
        "\n";

    r8e_sse_parser_feed(p, stream, (int)strlen(stream), collect_event, &c);

    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_INT(c.events[0].type, R8E_AI_EVENT_USAGE);
    ASSERT_EQ_INT(c.events[0].input_tokens, 100);
    ASSERT_EQ_INT(c.events[0].output_tokens, 0);

    r8e_sse_parser_free(p);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_sse_parser_tests(void)
{
    /* Anthropic provider tests */
    RUN_TEST(anthropic_text_streaming);
    RUN_TEST(anthropic_tool_call);
    RUN_TEST(anthropic_done_sentinel);
    RUN_TEST(anthropic_error_event);
    RUN_TEST(anthropic_usage_input_tokens);

    /* OpenAI provider tests */
    RUN_TEST(openai_text_streaming);
    RUN_TEST(openai_tool_call);
    RUN_TEST(openai_usage_event);
    RUN_TEST(openai_done_sentinel);

    /* Chunked delivery */
    RUN_TEST(chunked_across_boundaries);
    RUN_TEST(chunked_split_at_newline);

    /* Edge cases */
    RUN_TEST(comments_ignored);
    RUN_TEST(crlf_line_endings);
    RUN_TEST(empty_data_field);
    RUN_TEST(multiple_data_lines);
    RUN_TEST(parser_reset);
    RUN_TEST(null_safety);
    RUN_TEST(no_trailing_newline);
    RUN_TEST(escaped_text_in_delta);
}
