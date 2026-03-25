/*
 * test_kvcache.c - Unit tests for r8e KV cache
 *
 * Tests cover:
 *   - KV cache creation and destruction
 *   - Float16 <-> Float32 conversion accuracy
 *   - Writing and reading K,V vectors
 *   - Sliding window shift
 *   - Truncation
 *   - Reset
 *   - Edge cases (NULL inputs, boundary positions)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "r8e_kvcache.h"

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
 * Float16 Conversion Tests
 * ========================================================================= */

TEST(f16_zero) {
    uint16_t h = r8e_f32_to_f16(0.0f);
    float f = r8e_f16_to_f32(h);
    ASSERT_EQ_DBL(f, 0.0f, 1e-10);
}

TEST(f16_one) {
    uint16_t h = r8e_f32_to_f16(1.0f);
    float f = r8e_f16_to_f32(h);
    ASSERT_EQ_DBL(f, 1.0f, 1e-5);
}

TEST(f16_negative) {
    uint16_t h = r8e_f32_to_f16(-3.5f);
    float f = r8e_f16_to_f32(h);
    ASSERT_EQ_DBL(f, -3.5f, 1e-3);
}

TEST(f16_small_value) {
    /* Test a small but representable value */
    uint16_t h = r8e_f32_to_f16(0.001f);
    float f = r8e_f16_to_f32(h);
    ASSERT_EQ_DBL(f, 0.001f, 1e-4);
}

TEST(f16_roundtrip_various) {
    float test_values[] = { 0.5f, 1.5f, 2.0f, -0.25f, 100.0f, -100.0f, 0.1f };
    int n = sizeof(test_values) / sizeof(test_values[0]);
    for (int i = 0; i < n; i++) {
        uint16_t h = r8e_f32_to_f16(test_values[i]);
        float f = r8e_f16_to_f32(h);
        /* Float16 has limited precision, allow ~0.1% relative error */
        float abs_err = fabsf(f - test_values[i]);
        float rel_err = abs_err / (fabsf(test_values[i]) + 1e-10f);
        ASSERT_TRUE(rel_err < 0.01f);
    }
}

TEST(f16_infinity) {
    uint16_t h = r8e_f32_to_f16(INFINITY);
    float f = r8e_f16_to_f32(h);
    ASSERT_TRUE(isinf(f) && f > 0);
}

TEST(f16_neg_infinity) {
    uint16_t h = r8e_f32_to_f16(-INFINITY);
    float f = r8e_f16_to_f32(h);
    ASSERT_TRUE(isinf(f) && f < 0);
}

TEST(f16_nan) {
    uint16_t h = r8e_f32_to_f16(NAN);
    float f = r8e_f16_to_f32(h);
    ASSERT_TRUE(isnan(f));
}

/* =========================================================================
 * KV Cache Lifecycle Tests
 * ========================================================================= */

TEST(kvcache_create_destroy) {
    R8EKVCache *cache = r8e_kv_cache_new(2, 16, 4, 8);
    ASSERT_NE(cache, NULL);
    ASSERT_EQ_INT(cache->n_layers, 2);
    ASSERT_EQ_INT(cache->max_seq, 16);
    ASSERT_EQ_INT(cache->n_kv_heads, 4);
    ASSERT_EQ_INT(cache->d_head, 8);
    ASSERT_EQ_INT(cache->seq_len, 0);
    r8e_kv_cache_free(cache);
}

TEST(kvcache_null_free_safe) {
    r8e_kv_cache_free(NULL); /* should not crash */
}

TEST(kvcache_zero_params_returns_null) {
    ASSERT_TRUE(r8e_kv_cache_new(0, 16, 4, 8) == NULL);
    ASSERT_TRUE(r8e_kv_cache_new(2, 0, 4, 8) == NULL);
    ASSERT_TRUE(r8e_kv_cache_new(2, 16, 0, 8) == NULL);
    ASSERT_TRUE(r8e_kv_cache_new(2, 16, 4, 0) == NULL);
}

/* =========================================================================
 * Read / Write Tests
 * ========================================================================= */

TEST(kvcache_write_read) {
    /* 1 layer, 8 positions, 1 head, 4 dims */
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 4);
    ASSERT_NE(cache, NULL);

    float k[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float v[] = { 5.0f, 6.0f, 7.0f, 8.0f };

    r8e_kv_cache_write(cache, 0, 0, k, v);
    ASSERT_EQ_INT(cache->seq_len, 1);

    /* Read back and verify (through float16 conversion) */
    const uint16_t *k_data = r8e_kv_cache_k(cache, 0);
    const uint16_t *v_data = r8e_kv_cache_v(cache, 0);
    ASSERT_NE(k_data, NULL);
    ASSERT_NE(v_data, NULL);

    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[i]), k[i], 0.01f);
        ASSERT_EQ_DBL(r8e_f16_to_f32(v_data[i]), v[i], 0.01f);
    }

    r8e_kv_cache_free(cache);
}

TEST(kvcache_write_multiple_positions) {
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    for (uint32_t pos = 0; pos < 4; pos++) {
        float k[] = { (float)pos, (float)(pos + 10) };
        float v[] = { (float)(pos + 20), (float)(pos + 30) };
        r8e_kv_cache_write(cache, 0, pos, k, v);
    }

    ASSERT_EQ_INT(cache->seq_len, 4);

    const uint16_t *k_data = r8e_kv_cache_k(cache, 0);
    /* Check position 2 */
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[2 * 2 + 0]), 2.0f, 0.01f);
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[2 * 2 + 1]), 12.0f, 0.01f);

    r8e_kv_cache_free(cache);
}

TEST(kvcache_write_multiple_layers) {
    R8EKVCache *cache = r8e_kv_cache_new(3, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    for (uint32_t layer = 0; layer < 3; layer++) {
        float k[] = { (float)(layer * 100), (float)(layer * 100 + 1) };
        float v[] = { (float)(layer * 200), (float)(layer * 200 + 1) };
        r8e_kv_cache_write(cache, layer, 0, k, v);
    }

    /* Verify each layer's data is independent */
    for (uint32_t layer = 0; layer < 3; layer++) {
        const uint16_t *k_data = r8e_kv_cache_k(cache, layer);
        ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[0]), (float)(layer * 100), 1.0f);
    }

    r8e_kv_cache_free(cache);
}

TEST(kvcache_read_invalid_layer) {
    R8EKVCache *cache = r8e_kv_cache_new(2, 8, 1, 4);
    ASSERT_NE(cache, NULL);
    ASSERT_TRUE(r8e_kv_cache_k(cache, 5) == NULL);
    ASSERT_TRUE(r8e_kv_cache_v(cache, 5) == NULL);
    r8e_kv_cache_free(cache);
}

/* =========================================================================
 * Cache Management Tests
 * ========================================================================= */

TEST(kvcache_reset) {
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    float k[] = { 1.0f, 2.0f };
    float v[] = { 3.0f, 4.0f };
    r8e_kv_cache_write(cache, 0, 0, k, v);
    ASSERT_EQ_INT(cache->seq_len, 1);

    r8e_kv_cache_reset(cache);
    ASSERT_EQ_INT(cache->seq_len, 0);

    /* Data should be zeroed */
    const uint16_t *k_data = r8e_kv_cache_k(cache, 0);
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[0]), 0.0f, 1e-10);

    r8e_kv_cache_free(cache);
}

TEST(kvcache_truncate) {
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    /* Write 4 positions */
    for (uint32_t i = 0; i < 4; i++) {
        float k[] = { (float)(i + 1), (float)(i + 10) };
        float v[] = { (float)(i + 20), (float)(i + 30) };
        r8e_kv_cache_write(cache, 0, i, k, v);
    }
    ASSERT_EQ_INT(cache->seq_len, 4);

    /* Truncate to 2 */
    r8e_kv_cache_truncate(cache, 2);
    ASSERT_EQ_INT(cache->seq_len, 2);

    /* Position 0 and 1 should still be valid */
    const uint16_t *k_data = r8e_kv_cache_k(cache, 0);
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[0]), 1.0f, 0.01f);
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[2]), 2.0f, 0.01f);

    /* Position 2 should be zeroed */
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[4]), 0.0f, 1e-10);

    r8e_kv_cache_free(cache);
}

TEST(kvcache_truncate_at_seq_len_noop) {
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    float k[] = { 1.0f, 2.0f };
    float v[] = { 3.0f, 4.0f };
    r8e_kv_cache_write(cache, 0, 0, k, v);

    r8e_kv_cache_truncate(cache, 5); /* beyond seq_len -> no-op */
    ASSERT_EQ_INT(cache->seq_len, 1);

    r8e_kv_cache_free(cache);
}

TEST(kvcache_shift) {
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    /* Write positions 0-3 */
    for (uint32_t i = 0; i < 4; i++) {
        float k[] = { (float)(i + 1) * 10.0f, (float)(i + 1) * 20.0f };
        float v[] = { (float)(i + 1) * 30.0f, (float)(i + 1) * 40.0f };
        r8e_kv_cache_write(cache, 0, i, k, v);
    }
    ASSERT_EQ_INT(cache->seq_len, 4);

    /* Shift by 2: positions 2,3 become 0,1 */
    r8e_kv_cache_shift(cache, 2);
    ASSERT_EQ_INT(cache->seq_len, 2);

    /* What was position 2 (k=[30,60]) is now at position 0 */
    const uint16_t *k_data = r8e_kv_cache_k(cache, 0);
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[0]), 30.0f, 0.5f);
    ASSERT_EQ_DBL(r8e_f16_to_f32(k_data[1]), 60.0f, 0.5f);

    r8e_kv_cache_free(cache);
}

TEST(kvcache_shift_all) {
    R8EKVCache *cache = r8e_kv_cache_new(1, 8, 1, 2);
    ASSERT_NE(cache, NULL);

    float k[] = { 1.0f, 2.0f };
    float v[] = { 3.0f, 4.0f };
    r8e_kv_cache_write(cache, 0, 0, k, v);

    r8e_kv_cache_shift(cache, 10); /* shift more than seq_len */
    ASSERT_EQ_INT(cache->seq_len, 0);

    r8e_kv_cache_free(cache);
}

/* =========================================================================
 * Public Test Suite Entry Point
 * ========================================================================= */

void run_kvcache_tests(void) {
    /* Float16 conversion */
    RUN_TEST(f16_zero);
    RUN_TEST(f16_one);
    RUN_TEST(f16_negative);
    RUN_TEST(f16_small_value);
    RUN_TEST(f16_roundtrip_various);
    RUN_TEST(f16_infinity);
    RUN_TEST(f16_neg_infinity);
    RUN_TEST(f16_nan);

    /* Lifecycle */
    RUN_TEST(kvcache_create_destroy);
    RUN_TEST(kvcache_null_free_safe);
    RUN_TEST(kvcache_zero_params_returns_null);

    /* Read / Write */
    RUN_TEST(kvcache_write_read);
    RUN_TEST(kvcache_write_multiple_positions);
    RUN_TEST(kvcache_write_multiple_layers);
    RUN_TEST(kvcache_read_invalid_layer);

    /* Cache Management */
    RUN_TEST(kvcache_reset);
    RUN_TEST(kvcache_truncate);
    RUN_TEST(kvcache_truncate_at_seq_len_noop);
    RUN_TEST(kvcache_shift);
    RUN_TEST(kvcache_shift_all);
}
