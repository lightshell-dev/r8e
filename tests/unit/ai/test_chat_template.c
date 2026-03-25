/*
 * test_chat_template.c - Unit tests for r8e chat template engine
 *
 * Tests cover:
 *   - Template creation and destruction
 *   - Format auto-detection (ChatML, LLaMA 3, Phi-3)
 *   - ChatML prompt formatting
 *   - LLaMA 3 prompt formatting
 *   - Phi-3 prompt formatting
 *   - Generation prompt insertion
 *   - Multi-turn conversations
 *   - Buffer overflow handling
 *   - Edge cases (NULL inputs, empty messages)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "r8e_chat_template.h"

/* =========================================================================
 * Test Infrastructure (matches test_runner.c)
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

#define ASSERT_NE(a, b) do {                                        \
    uint64_t _a = (uint64_t)(uintptr_t)(a), _b = (uint64_t)(uintptr_t)(b); \
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

/* =========================================================================
 * Creation / Detection Tests
 * ========================================================================= */

TEST(create_chatml_from_template) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "{% for message in messages %}<|im_start|>{{ message.role }}\n"
        "{{ message.content }}<|im_end|>\n{% endfor %}",
        NULL, NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 1); /* R8E_CHAT_FMT_CHATML */
    r8e_chat_template_free(t);
}

TEST(create_llama3_from_template) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|start_header_id|>{{ message.role }}<|end_header_id|>",
        NULL, NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 2); /* R8E_CHAT_FMT_LLAMA3 */
    r8e_chat_template_free(t);
}

TEST(create_phi3_from_template) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|user|>\n{{ message.content }}<|end|>",
        NULL, NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 3); /* R8E_CHAT_FMT_PHI3 */
    r8e_chat_template_free(t);
}

TEST(detect_llama3_from_model_name) {
    R8EChatTemplate *t = r8e_chat_template_new(NULL, "Meta-Llama-3-8B", NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 2); /* R8E_CHAT_FMT_LLAMA3 */
    r8e_chat_template_free(t);
}

TEST(detect_phi3_from_model_name) {
    R8EChatTemplate *t = r8e_chat_template_new(NULL, "microsoft/Phi-3-mini", NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 3); /* R8E_CHAT_FMT_PHI3 */
    r8e_chat_template_free(t);
}

TEST(default_to_chatml) {
    R8EChatTemplate *t = r8e_chat_template_new(NULL, NULL, NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 1); /* R8E_CHAT_FMT_CHATML */
    r8e_chat_template_free(t);
}

TEST(null_free_safe) {
    r8e_chat_template_free(NULL); /* should not crash */
}

/* =========================================================================
 * ChatML Format Tests
 * ========================================================================= */

TEST(chatml_single_user) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|im_start|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    R8EChatMessage msgs[] = {
        { "user", "Hello!" }
    };

    char buf[512];
    int n = r8e_chat_template_apply(t, msgs, 1, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_STR(buf,
        "<|im_start|>user\nHello!<|im_end|>\n"
        "<|im_start|>assistant\n");

    r8e_chat_template_free(t);
}

TEST(chatml_system_and_user) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|im_start|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    R8EChatMessage msgs[] = {
        { "system", "You are helpful." },
        { "user", "Hi!" }
    };

    char buf[1024];
    int n = r8e_chat_template_apply(t, msgs, 2, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "<|im_start|>system\nYou are helpful.<|im_end|>") != NULL);
    ASSERT_TRUE(strstr(buf, "<|im_start|>user\nHi!<|im_end|>") != NULL);
    ASSERT_TRUE(strstr(buf, "<|im_start|>assistant\n") != NULL);

    r8e_chat_template_free(t);
}

TEST(chatml_no_gen_prompt_when_assistant_last) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|im_start|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    R8EChatMessage msgs[] = {
        { "user", "Hi!" },
        { "assistant", "Hello!" }
    };

    char buf[1024];
    int n = r8e_chat_template_apply(t, msgs, 2, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    /* Should end with the assistant's message, no extra gen prompt */
    const char *expected_end = "<|im_end|>\n";
    int elen = (int)strlen(expected_end);
    ASSERT_TRUE(n >= elen);
    ASSERT_EQ_STR(buf + n - elen, expected_end);

    r8e_chat_template_free(t);
}

/* =========================================================================
 * LLaMA 3 Format Tests
 * ========================================================================= */

TEST(llama3_single_user) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|start_header_id|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 2);

    R8EChatMessage msgs[] = {
        { "user", "Hello!" }
    };

    char buf[1024];
    int n = r8e_chat_template_apply(t, msgs, 1, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "<|begin_of_text|>") != NULL);
    ASSERT_TRUE(strstr(buf, "<|start_header_id|>user<|end_header_id|>") != NULL);
    ASSERT_TRUE(strstr(buf, "\n\nHello!<|eot_id|>") != NULL);
    /* Should end with assistant gen prompt */
    ASSERT_TRUE(strstr(buf, "<|start_header_id|>assistant<|end_header_id|>\n\n") != NULL);

    r8e_chat_template_free(t);
}

TEST(llama3_multi_turn) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|start_header_id|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    R8EChatMessage msgs[] = {
        { "system", "Be concise." },
        { "user", "What is 2+2?" },
        { "assistant", "4." },
        { "user", "And 3+3?" }
    };

    char buf[2048];
    int n = r8e_chat_template_apply(t, msgs, 4, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "system") != NULL);
    ASSERT_TRUE(strstr(buf, "4.<|eot_id|>") != NULL);

    r8e_chat_template_free(t);
}

/* =========================================================================
 * Phi-3 Format Tests
 * ========================================================================= */

TEST(phi3_single_user) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|user|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);
    ASSERT_EQ_INT(t->format, 3);

    R8EChatMessage msgs[] = {
        { "user", "Hello!" }
    };

    char buf[512];
    int n = r8e_chat_template_apply(t, msgs, 1, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "<|user|>\nHello!<|end|>\n") != NULL);
    ASSERT_TRUE(strstr(buf, "<|assistant|>\n") != NULL);

    r8e_chat_template_free(t);
}

TEST(phi3_with_system) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|user|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    R8EChatMessage msgs[] = {
        { "system", "You are helpful." },
        { "user", "Hi!" }
    };

    char buf[1024];
    int n = r8e_chat_template_apply(t, msgs, 2, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "<|system|>\nYou are helpful.<|end|>") != NULL);

    r8e_chat_template_free(t);
}

/* =========================================================================
 * Buffer Handling Tests
 * ========================================================================= */

TEST(buffer_too_small_returns_required_size) {
    R8EChatTemplate *t = r8e_chat_template_new(
        "<|im_start|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    R8EChatMessage msgs[] = {
        { "user", "Hello world! This is a somewhat long message." }
    };

    /* Buffer too small */
    char buf[10];
    int required = r8e_chat_template_apply(t, msgs, 1, buf, sizeof(buf));
    ASSERT_TRUE(required > 10); /* Should return required size */

    /* Now allocate enough */
    char *big_buf = (char *)malloc((size_t)required + 1);
    ASSERT_NE(big_buf, NULL);
    int n = r8e_chat_template_apply(t, msgs, 1, big_buf, required + 1);
    ASSERT_EQ_INT(n, required);
    free(big_buf);

    r8e_chat_template_free(t);
}

TEST(null_template_returns_neg1) {
    R8EChatMessage msgs[] = { { "user", "Hello" } };
    int n = r8e_chat_template_apply(NULL, msgs, 1, NULL, 0);
    ASSERT_EQ_INT(n, -1);
}

TEST(zero_messages_returns_neg1) {
    R8EChatTemplate *t = r8e_chat_template_new("<|im_start|>", NULL, NULL, NULL);
    ASSERT_NE(t, NULL);

    char buf[256];
    int n = r8e_chat_template_apply(t, NULL, 0, buf, sizeof(buf));
    ASSERT_EQ_INT(n, -1);

    r8e_chat_template_free(t);
}

/* =========================================================================
 * Public Test Suite Entry Point
 * ========================================================================= */

void run_chat_template_tests(void) {
    /* Creation / Detection */
    RUN_TEST(create_chatml_from_template);
    RUN_TEST(create_llama3_from_template);
    RUN_TEST(create_phi3_from_template);
    RUN_TEST(detect_llama3_from_model_name);
    RUN_TEST(detect_phi3_from_model_name);
    RUN_TEST(default_to_chatml);
    RUN_TEST(null_free_safe);

    /* ChatML */
    RUN_TEST(chatml_single_user);
    RUN_TEST(chatml_system_and_user);
    RUN_TEST(chatml_no_gen_prompt_when_assistant_last);

    /* LLaMA 3 */
    RUN_TEST(llama3_single_user);
    RUN_TEST(llama3_multi_turn);

    /* Phi-3 */
    RUN_TEST(phi3_single_user);
    RUN_TEST(phi3_with_system);

    /* Buffer handling */
    RUN_TEST(buffer_too_small_returns_required_size);
    RUN_TEST(null_template_returns_neg1);
    RUN_TEST(zero_messages_returns_neg1);
}
