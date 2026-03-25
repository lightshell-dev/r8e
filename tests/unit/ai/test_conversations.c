/*
 * test_conversations.c - Unit tests for r8e_conversations.c
 *
 * Tests cover:
 *   - Create and free conversations
 *   - Add messages with various roles
 *   - Retrieve all messages
 *   - Token-budget truncation (preserves system + most recent)
 *   - Clear conversation
 *   - Total token counting
 *   - JSON serialization
 *   - Edge cases: empty conversation, OOM simulation, large message counts
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../../include/r8e_conversations.h"

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

/* =========================================================================
 * Tests: Conversation Lifecycle
 * ========================================================================= */

TEST(conversation_new_and_free)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);
    ASSERT_TRUE(conv->id[0] != '\0');
    ASSERT_EQ_INT(conv->n_messages, 0);
    ASSERT_TRUE(conv->created_at > 0);
    r8e_conversation_free(conv);
}

TEST(conversation_free_null)
{
    /* Should not crash */
    r8e_conversation_free(NULL);
}

/* =========================================================================
 * Tests: Adding Messages
 * ========================================================================= */

TEST(add_user_message)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    r8e_conversation_add_message(conv, "user", "Hello!", NULL, NULL);
    ASSERT_EQ_INT(conv->n_messages, 1);
    ASSERT_EQ_STR(conv->messages[0].role, "user");
    ASSERT_EQ_STR(conv->messages[0].content, "Hello!");
    ASSERT_TRUE(conv->messages[0].tool_call_id == NULL);
    ASSERT_TRUE(conv->messages[0].tool_calls_json == NULL);
    ASSERT_TRUE(conv->messages[0].tokens > 0);

    r8e_conversation_free(conv);
}

TEST(add_assistant_message)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    r8e_conversation_add_message(conv, "assistant", "Hi there!",
                                  NULL, NULL);
    ASSERT_EQ_INT(conv->n_messages, 1);
    ASSERT_EQ_STR(conv->messages[0].role, "assistant");
    ASSERT_EQ_STR(conv->messages[0].content, "Hi there!");

    r8e_conversation_free(conv);
}

TEST(add_tool_message)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    r8e_conversation_add_message(conv, "tool", "weather: sunny, 22C",
                                  "toolu_abc123", NULL);
    ASSERT_EQ_INT(conv->n_messages, 1);
    ASSERT_EQ_STR(conv->messages[0].role, "tool");
    ASSERT_EQ_STR(conv->messages[0].content, "weather: sunny, 22C");
    ASSERT_EQ_STR(conv->messages[0].tool_call_id, "toolu_abc123");

    r8e_conversation_free(conv);
}

TEST(add_assistant_with_tool_calls)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    const char *tc_json = "[{\"id\":\"toolu_001\",\"name\":\"search\"}]";
    r8e_conversation_add_message(conv, "assistant", "Let me search.",
                                  NULL, tc_json);
    ASSERT_EQ_INT(conv->n_messages, 1);
    ASSERT_EQ_STR(conv->messages[0].tool_calls_json, tc_json);

    r8e_conversation_free(conv);
}

TEST(add_system_message)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    r8e_conversation_add_message(conv, "system",
                                  "You are a helpful assistant.",
                                  NULL, NULL);
    ASSERT_EQ_INT(conv->n_messages, 1);
    ASSERT_EQ_STR(conv->messages[0].role, "system");

    r8e_conversation_free(conv);
}

TEST(add_multiple_messages)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    r8e_conversation_add_message(conv, "system", "Be helpful.", NULL, NULL);
    r8e_conversation_add_message(conv, "user", "Hello", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "Hi!", NULL, NULL);
    r8e_conversation_add_message(conv, "user", "How are you?", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "I'm doing well!", NULL, NULL);

    ASSERT_EQ_INT(conv->n_messages, 5);
    ASSERT_EQ_STR(conv->messages[0].role, "system");
    ASSERT_EQ_STR(conv->messages[1].role, "user");
    ASSERT_EQ_STR(conv->messages[2].role, "assistant");
    ASSERT_EQ_STR(conv->messages[3].role, "user");
    ASSERT_EQ_STR(conv->messages[4].role, "assistant");

    r8e_conversation_free(conv);
}

TEST(add_beyond_initial_capacity)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_TRUE(conv != NULL);

    /* Add more messages than the initial capacity (16) */
    for (int i = 0; i < 20; i++) {
        char content[32];
        snprintf(content, sizeof(content), "Message %d", i);
        r8e_conversation_add_message(conv, "user", content, NULL, NULL);
    }
    ASSERT_EQ_INT(conv->n_messages, 20);

    /* Verify last message */
    ASSERT_EQ_STR(conv->messages[19].content, "Message 19");

    r8e_conversation_free(conv);
}

/* =========================================================================
 * Tests: Retrieving Messages
 * ========================================================================= */

TEST(get_messages_basic)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hello", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "Hi", NULL, NULL);

    R8EMessage *msgs = NULL;
    int n = r8e_conversation_get_messages(conv, &msgs);
    ASSERT_EQ_INT(n, 2);
    ASSERT_TRUE(msgs != NULL);
    ASSERT_EQ_STR(msgs[0].content, "Hello");
    ASSERT_EQ_STR(msgs[1].content, "Hi");

    r8e_conversation_free(conv);
}

TEST(get_messages_empty)
{
    R8EConversation *conv = r8e_conversation_new();

    R8EMessage *msgs = NULL;
    int n = r8e_conversation_get_messages(conv, &msgs);
    ASSERT_EQ_INT(n, 0);

    r8e_conversation_free(conv);
}

/* =========================================================================
 * Tests: Token-Budget Truncation
 * ========================================================================= */

TEST(truncate_all_fit)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hi", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "Hello", NULL, NULL);

    R8EMessage *msgs = NULL;
    int n = r8e_conversation_get_messages_truncated(conv, 10000, &msgs);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_STR(msgs[0].role, "user");
    ASSERT_EQ_STR(msgs[1].role, "assistant");

    free(msgs);
    r8e_conversation_free(conv);
}

TEST(truncate_preserves_system)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "system", "Be helpful.", NULL, NULL);

    /* Add many messages to exceed a small budget */
    for (int i = 0; i < 10; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "This is a somewhat long message number %d with some text", i);
        r8e_conversation_add_message(conv,
                                      (i % 2 == 0) ? "user" : "assistant",
                                      buf, NULL, NULL);
    }

    /* Use a small token budget - system + only a few recent messages should fit */
    R8EMessage *msgs = NULL;
    int n = r8e_conversation_get_messages_truncated(conv, 30, &msgs);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(n < conv->n_messages);  /* Should have truncated something */
    ASSERT_EQ_STR(msgs[0].role, "system");  /* System message preserved */

    /* Last message should be from the end of the conversation */
    ASSERT_TRUE(strstr(msgs[n - 1].content, "9") != NULL ||
                strstr(msgs[n - 1].content, "8") != NULL);

    free(msgs);
    r8e_conversation_free(conv);
}

TEST(truncate_zero_budget)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hello", NULL, NULL);

    R8EMessage *msgs = NULL;
    int n = r8e_conversation_get_messages_truncated(conv, 0, &msgs);
    ASSERT_EQ_INT(n, 0);

    r8e_conversation_free(conv);
}

TEST(truncate_empty_conversation)
{
    R8EConversation *conv = r8e_conversation_new();

    R8EMessage *msgs = NULL;
    int n = r8e_conversation_get_messages_truncated(conv, 1000, &msgs);
    ASSERT_EQ_INT(n, 0);

    r8e_conversation_free(conv);
}

/* =========================================================================
 * Tests: Clear
 * ========================================================================= */

TEST(clear_conversation)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hello", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "Hi", NULL, NULL);
    ASSERT_EQ_INT(conv->n_messages, 2);

    r8e_conversation_clear(conv);
    ASSERT_EQ_INT(conv->n_messages, 0);

    /* Should be able to add messages again after clear */
    r8e_conversation_add_message(conv, "user", "New message", NULL, NULL);
    ASSERT_EQ_INT(conv->n_messages, 1);
    ASSERT_EQ_STR(conv->messages[0].content, "New message");

    r8e_conversation_free(conv);
}

TEST(clear_empty_conversation)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_clear(conv);  /* Should not crash */
    ASSERT_EQ_INT(conv->n_messages, 0);
    r8e_conversation_free(conv);
}

/* =========================================================================
 * Tests: Total Token Count
 * ========================================================================= */

TEST(total_tokens_basic)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hello", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "Hi there!", NULL, NULL);

    int total = r8e_conversation_total_tokens(conv);
    ASSERT_TRUE(total > 0);

    /* "Hello" = 5 chars -> ~2 tokens, "Hi there!" = 9 chars -> ~3 tokens */
    ASSERT_TRUE(total >= 2);

    r8e_conversation_free(conv);
}

TEST(total_tokens_empty)
{
    R8EConversation *conv = r8e_conversation_new();
    ASSERT_EQ_INT(r8e_conversation_total_tokens(conv), 0);
    r8e_conversation_free(conv);
}

TEST(total_tokens_null)
{
    ASSERT_EQ_INT(r8e_conversation_total_tokens(NULL), 0);
}

/* =========================================================================
 * Tests: JSON Serialization
 * ========================================================================= */

TEST(to_json_basic)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hello", NULL, NULL);
    r8e_conversation_add_message(conv, "assistant", "Hi", NULL, NULL);

    char buf[2048];
    int n = r8e_conversation_to_json(conv, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"id\":\"conv_") != NULL);
    ASSERT_TRUE(strstr(buf, "\"messages\":[") != NULL);
    ASSERT_TRUE(strstr(buf, "\"role\":\"user\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"content\":\"Hello\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"role\":\"assistant\"") != NULL);

    r8e_conversation_free(conv);
}

TEST(to_json_with_tool_call)
{
    R8EConversation *conv = r8e_conversation_new();
    const char *tc = "[{\"id\":\"t1\",\"name\":\"search\"}]";
    r8e_conversation_add_message(conv, "assistant", "Searching...",
                                  NULL, tc);
    r8e_conversation_add_message(conv, "tool", "Found: 42",
                                  "t1", NULL);

    char buf[2048];
    int n = r8e_conversation_to_json(conv, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"tool_calls\":[") != NULL);
    ASSERT_TRUE(strstr(buf, "\"tool_call_id\":\"t1\"") != NULL);

    r8e_conversation_free(conv);
}

TEST(to_json_overflow)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user", "Hello world", NULL, NULL);

    char buf[16];  /* too small */
    int n = r8e_conversation_to_json(conv, buf, sizeof(buf));
    ASSERT_EQ_INT(n, -1);

    r8e_conversation_free(conv);
}

TEST(to_json_empty)
{
    R8EConversation *conv = r8e_conversation_new();

    char buf[1024];
    int n = r8e_conversation_to_json(conv, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"messages\":[]") != NULL);

    r8e_conversation_free(conv);
}

TEST(to_json_escaping)
{
    R8EConversation *conv = r8e_conversation_new();
    r8e_conversation_add_message(conv, "user",
                                  "line1\nline2\t\"quoted\"",
                                  NULL, NULL);

    char buf[2048];
    int n = r8e_conversation_to_json(conv, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\\n") != NULL);
    ASSERT_TRUE(strstr(buf, "\\t") != NULL);
    ASSERT_TRUE(strstr(buf, "\\\"quoted\\\"") != NULL);

    r8e_conversation_free(conv);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_conversations_tests(void)
{
    /* Lifecycle */
    RUN_TEST(conversation_new_and_free);
    RUN_TEST(conversation_free_null);

    /* Adding messages */
    RUN_TEST(add_user_message);
    RUN_TEST(add_assistant_message);
    RUN_TEST(add_tool_message);
    RUN_TEST(add_assistant_with_tool_calls);
    RUN_TEST(add_system_message);
    RUN_TEST(add_multiple_messages);
    RUN_TEST(add_beyond_initial_capacity);

    /* Retrieving messages */
    RUN_TEST(get_messages_basic);
    RUN_TEST(get_messages_empty);

    /* Truncation */
    RUN_TEST(truncate_all_fit);
    RUN_TEST(truncate_preserves_system);
    RUN_TEST(truncate_zero_budget);
    RUN_TEST(truncate_empty_conversation);

    /* Clear */
    RUN_TEST(clear_conversation);
    RUN_TEST(clear_empty_conversation);

    /* Token counting */
    RUN_TEST(total_tokens_basic);
    RUN_TEST(total_tokens_empty);
    RUN_TEST(total_tokens_null);

    /* JSON serialization */
    RUN_TEST(to_json_basic);
    RUN_TEST(to_json_with_tool_call);
    RUN_TEST(to_json_overflow);
    RUN_TEST(to_json_empty);
    RUN_TEST(to_json_escaping);
}
