/*
 * test_sampler.c - Unit tests for r8e LLM token sampler
 *
 * Tests cover:
 *   - Greedy (argmax) sampling
 *   - Temperature scaling
 *   - Softmax computation
 *   - Top-k filtering
 *   - Top-p (nucleus) filtering
 *   - Repetition penalty
 *   - Full sampling pipeline
 *   - Edge cases (empty input, single token, etc.)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "r8e_sampler.h"

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

/* =========================================================================
 * Greedy Sampling Tests
 * ========================================================================= */

TEST(greedy_basic) {
    float logits[] = { 1.0f, 3.0f, 2.0f, 0.5f };
    int idx = r8e_sample_greedy(logits, 4);
    ASSERT_EQ_INT(idx, 1);
}

TEST(greedy_first_element) {
    float logits[] = { 10.0f, 3.0f, 2.0f };
    int idx = r8e_sample_greedy(logits, 3);
    ASSERT_EQ_INT(idx, 0);
}

TEST(greedy_last_element) {
    float logits[] = { 1.0f, 2.0f, 3.0f, 100.0f };
    int idx = r8e_sample_greedy(logits, 4);
    ASSERT_EQ_INT(idx, 3);
}

TEST(greedy_single_element) {
    float logits[] = { 42.0f };
    int idx = r8e_sample_greedy(logits, 1);
    ASSERT_EQ_INT(idx, 0);
}

TEST(greedy_negative_logits) {
    float logits[] = { -5.0f, -1.0f, -3.0f, -2.0f };
    int idx = r8e_sample_greedy(logits, 4);
    ASSERT_EQ_INT(idx, 1);
}

TEST(greedy_null_returns_neg1) {
    int idx = r8e_sample_greedy(NULL, 5);
    ASSERT_EQ_INT(idx, -1);
}

TEST(greedy_zero_size_returns_neg1) {
    float logits[] = { 1.0f };
    int idx = r8e_sample_greedy(logits, 0);
    ASSERT_EQ_INT(idx, -1);
}

/* =========================================================================
 * Temperature Tests
 * ========================================================================= */

TEST(temperature_scaling) {
    float logits[] = { 2.0f, 4.0f, 6.0f };
    r8e_sample_apply_temperature(logits, 3, 2.0f);
    ASSERT_EQ_DBL(logits[0], 1.0f, 1e-5);
    ASSERT_EQ_DBL(logits[1], 2.0f, 1e-5);
    ASSERT_EQ_DBL(logits[2], 3.0f, 1e-5);
}

TEST(temperature_one_no_change) {
    float logits[] = { 1.0f, 2.0f, 3.0f };
    float orig[] =   { 1.0f, 2.0f, 3.0f };
    r8e_sample_apply_temperature(logits, 3, 1.0f);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_DBL(logits[i], orig[i], 1e-5);
    }
}

TEST(temperature_zero_greedy) {
    /* Temperature 0 should not modify logits (greedy passthrough) */
    float logits[] = { 1.0f, 5.0f, 3.0f };
    float orig[] =   { 1.0f, 5.0f, 3.0f };
    r8e_sample_apply_temperature(logits, 3, 0.0f);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_DBL(logits[i], orig[i], 1e-5);
    }
}

TEST(temperature_low_sharpens) {
    /* Low temperature should amplify differences */
    float logits[] = { 1.0f, 2.0f };
    r8e_sample_apply_temperature(logits, 2, 0.1f);
    ASSERT_EQ_DBL(logits[0], 10.0f, 1e-4);
    ASSERT_EQ_DBL(logits[1], 20.0f, 1e-4);
}

/* =========================================================================
 * Softmax Tests
 * ========================================================================= */

TEST(softmax_sums_to_one) {
    float logits[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    r8e_softmax(logits, 4);
    float sum = 0.0f;
    for (int i = 0; i < 4; i++) sum += logits[i];
    ASSERT_EQ_DBL(sum, 1.0f, 1e-5);
}

TEST(softmax_monotonic) {
    float logits[] = { 1.0f, 2.0f, 3.0f };
    r8e_softmax(logits, 3);
    ASSERT_TRUE(logits[0] < logits[1]);
    ASSERT_TRUE(logits[1] < logits[2]);
}

TEST(softmax_all_same) {
    float logits[] = { 5.0f, 5.0f, 5.0f, 5.0f };
    r8e_softmax(logits, 4);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_DBL(logits[i], 0.25f, 1e-5);
    }
}

TEST(softmax_single) {
    float logits[] = { 42.0f };
    r8e_softmax(logits, 1);
    ASSERT_EQ_DBL(logits[0], 1.0f, 1e-5);
}

TEST(softmax_large_values_stable) {
    /* Should not overflow even with very large logits */
    float logits[] = { 1000.0f, 1001.0f, 1002.0f };
    r8e_softmax(logits, 3);
    float sum = 0.0f;
    for (int i = 0; i < 3; i++) sum += logits[i];
    ASSERT_EQ_DBL(sum, 1.0f, 1e-5);
    ASSERT_TRUE(logits[2] > logits[1]);
    ASSERT_TRUE(logits[1] > logits[0]);
}

/* =========================================================================
 * Top-K Tests
 * ========================================================================= */

TEST(topk_basic) {
    float logits[] = { 1.0f, 5.0f, 3.0f, 2.0f, 4.0f };
    r8e_sample_top_k(logits, 5, 2);
    /* Top 2 are indices 1 (5.0) and 4 (4.0) */
    ASSERT_TRUE(logits[1] > -1e30f);  /* kept */
    ASSERT_TRUE(logits[4] > -1e30f);  /* kept */
    ASSERT_TRUE(logits[0] < -1e30f);  /* zeroed */
    ASSERT_TRUE(logits[3] < -1e30f);  /* zeroed */
}

TEST(topk_k_equals_vocab) {
    float logits[] = { 1.0f, 2.0f, 3.0f };
    float orig[] =   { 1.0f, 2.0f, 3.0f };
    r8e_sample_top_k(logits, 3, 3);
    /* No filtering when k >= vocab_size */
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_DBL(logits[i], orig[i], 1e-5);
    }
}

TEST(topk_k_zero_no_filter) {
    float logits[] = { 1.0f, 2.0f };
    float orig[] =   { 1.0f, 2.0f };
    r8e_sample_top_k(logits, 2, 0);
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ_DBL(logits[i], orig[i], 1e-5);
    }
}

TEST(topk_k_one) {
    float logits[] = { 1.0f, 5.0f, 3.0f };
    r8e_sample_top_k(logits, 3, 1);
    ASSERT_TRUE(logits[1] > -1e30f);  /* kept (max) */
    ASSERT_TRUE(logits[0] < -1e30f);  /* zeroed */
    ASSERT_TRUE(logits[2] < -1e30f);  /* zeroed */
}

/* =========================================================================
 * Top-P Tests
 * ========================================================================= */

TEST(topp_basic) {
    /* Already probabilities (after softmax) */
    float probs[] = { 0.5f, 0.3f, 0.1f, 0.05f, 0.05f };
    r8e_sample_top_p(probs, 5, 0.8f);
    /* Top-p=0.8: keep 0.5 + 0.3 = 0.8, then the rest should be zeroed */
    ASSERT_TRUE(probs[0] > 0.0f);
    ASSERT_TRUE(probs[1] > 0.0f);
    /* The 0.1 entry (index 2) might also be included to exceed 0.8 */
}

TEST(topp_one_no_filter) {
    float probs[] = { 0.5f, 0.3f, 0.2f };
    float orig[] =  { 0.5f, 0.3f, 0.2f };
    r8e_sample_top_p(probs, 3, 1.0f);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_DBL(probs[i], orig[i], 1e-5);
    }
}

TEST(topp_keeps_at_least_one) {
    float probs[] = { 0.01f, 0.01f, 0.98f };
    r8e_sample_top_p(probs, 3, 0.0001f);
    /* Should keep at least one (the highest) */
    ASSERT_TRUE(probs[2] > 0.0f);
}

/* =========================================================================
 * Repetition Penalty Tests
 * ========================================================================= */

TEST(repeat_penalty_basic) {
    float logits[] = { 2.0f, 4.0f, 6.0f, 8.0f };
    int recent[] = { 1, 3 };
    r8e_sample_apply_repeat_penalty(logits, 4, recent, 2, 2.0f);
    /* Token 1: 4.0 / 2.0 = 2.0 */
    ASSERT_EQ_DBL(logits[1], 2.0f, 1e-5);
    /* Token 3: 8.0 / 2.0 = 4.0 */
    ASSERT_EQ_DBL(logits[3], 4.0f, 1e-5);
    /* Tokens 0 and 2 unchanged */
    ASSERT_EQ_DBL(logits[0], 2.0f, 1e-5);
    ASSERT_EQ_DBL(logits[2], 6.0f, 1e-5);
}

TEST(repeat_penalty_negative_logits) {
    float logits[] = { -2.0f, 4.0f };
    int recent[] = { 0 };
    r8e_sample_apply_repeat_penalty(logits, 2, recent, 1, 2.0f);
    /* Negative logit: multiplied by penalty (makes more negative) */
    ASSERT_EQ_DBL(logits[0], -4.0f, 1e-5);
    ASSERT_EQ_DBL(logits[1], 4.0f, 1e-5);
}

TEST(repeat_penalty_one_no_change) {
    float logits[] = { 2.0f, 4.0f };
    int recent[] = { 0, 1 };
    r8e_sample_apply_repeat_penalty(logits, 2, recent, 2, 1.0f);
    ASSERT_EQ_DBL(logits[0], 2.0f, 1e-5);
    ASSERT_EQ_DBL(logits[1], 4.0f, 1e-5);
}

TEST(repeat_penalty_out_of_range) {
    /* Recent tokens with out-of-range indices should be ignored */
    float logits[] = { 2.0f, 4.0f };
    int recent[] = { -1, 5, 1000 };
    r8e_sample_apply_repeat_penalty(logits, 2, recent, 3, 2.0f);
    ASSERT_EQ_DBL(logits[0], 2.0f, 1e-5);
    ASSERT_EQ_DBL(logits[1], 4.0f, 1e-5);
}

/* =========================================================================
 * Full Pipeline Tests
 * ========================================================================= */

TEST(sample_greedy_via_config) {
    float logits[] = { 1.0f, 5.0f, 3.0f, 2.0f };
    R8ESamplerConfig config = {
        .temperature = 0.0f,  /* greedy */
        .top_p = 0.9f,
        .top_k = 40,
        .repeat_penalty = 1.0f,
        .repeat_window = 0,
        .seed = 42
    };
    int idx = r8e_sample(logits, 4, &config, NULL, 0);
    ASSERT_EQ_INT(idx, 1);
}

TEST(sample_deterministic_with_seed) {
    /* Same seed should produce same result */
    float logits1[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float logits2[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R8ESamplerConfig config = {
        .temperature = 1.0f,
        .top_p = 1.0f,
        .top_k = 0,
        .repeat_penalty = 1.0f,
        .repeat_window = 0,
        .seed = 12345
    };
    int idx1 = r8e_sample(logits1, 4, &config, NULL, 0);
    int idx2 = r8e_sample(logits2, 4, &config, NULL, 0);
    ASSERT_EQ_INT(idx1, idx2);
}

TEST(sample_null_config_returns_neg1) {
    float logits[] = { 1.0f };
    int idx = r8e_sample(logits, 1, NULL, NULL, 0);
    ASSERT_EQ_INT(idx, -1);
}

TEST(sample_with_repeat_penalty) {
    /* With strong repeat penalty, token 1 should become less likely */
    float logits[] = { 1.0f, 10.0f, 1.0f };
    int recent[] = { 1 };
    R8ESamplerConfig config = {
        .temperature = 0.0001f,  /* near-greedy */
        .top_p = 1.0f,
        .top_k = 0,
        .repeat_penalty = 100.0f,  /* extreme penalty */
        .repeat_window = 1,
        .seed = 42
    };
    int idx = r8e_sample(logits, 3, &config, recent, 1);
    /* Token 1 gets heavily penalized, so another token should be selected */
    ASSERT_TRUE(idx == 0 || idx == 2);
}

TEST(sample_returns_valid_index) {
    float logits[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f };
    R8ESamplerConfig config = {
        .temperature = 0.8f,
        .top_p = 0.9f,
        .top_k = 3,
        .repeat_penalty = 1.0f,
        .repeat_window = 0,
        .seed = 999
    };
    int idx = r8e_sample(logits, 5, &config, NULL, 0);
    ASSERT_TRUE(idx >= 0 && idx < 5);
}

/* =========================================================================
 * Public Test Suite Entry Point
 * ========================================================================= */

void run_sampler_tests(void) {
    /* Greedy */
    RUN_TEST(greedy_basic);
    RUN_TEST(greedy_first_element);
    RUN_TEST(greedy_last_element);
    RUN_TEST(greedy_single_element);
    RUN_TEST(greedy_negative_logits);
    RUN_TEST(greedy_null_returns_neg1);
    RUN_TEST(greedy_zero_size_returns_neg1);

    /* Temperature */
    RUN_TEST(temperature_scaling);
    RUN_TEST(temperature_one_no_change);
    RUN_TEST(temperature_zero_greedy);
    RUN_TEST(temperature_low_sharpens);

    /* Softmax */
    RUN_TEST(softmax_sums_to_one);
    RUN_TEST(softmax_monotonic);
    RUN_TEST(softmax_all_same);
    RUN_TEST(softmax_single);
    RUN_TEST(softmax_large_values_stable);

    /* Top-K */
    RUN_TEST(topk_basic);
    RUN_TEST(topk_k_equals_vocab);
    RUN_TEST(topk_k_zero_no_filter);
    RUN_TEST(topk_k_one);

    /* Top-P */
    RUN_TEST(topp_basic);
    RUN_TEST(topp_one_no_filter);
    RUN_TEST(topp_keeps_at_least_one);

    /* Repetition Penalty */
    RUN_TEST(repeat_penalty_basic);
    RUN_TEST(repeat_penalty_negative_logits);
    RUN_TEST(repeat_penalty_one_no_change);
    RUN_TEST(repeat_penalty_out_of_range);

    /* Full Pipeline */
    RUN_TEST(sample_greedy_via_config);
    RUN_TEST(sample_deterministic_with_seed);
    RUN_TEST(sample_null_config_returns_neg1);
    RUN_TEST(sample_with_repeat_penalty);
    RUN_TEST(sample_returns_valid_index);
}
