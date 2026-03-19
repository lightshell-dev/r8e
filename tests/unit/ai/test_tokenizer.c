/*
 * test_tokenizer.c - Unit tests for r8e BPE tokenizer
 *
 * Tests cover:
 *   - Tokenizer creation and destruction
 *   - Hash map lookup for all vocab entries
 *   - BPE encoding of simple ASCII text
 *   - Decoding token IDs back to text (round-trip)
 *   - Token counting matches encode length
 *   - Byte fallback for unknown characters
 *   - Empty input produces 0 tokens
 *   - Single character encoding
 *   - Multi-byte UTF-8 character handling via byte fallback
 *   - BOS/EOS special token IDs
 *   - Truncation with max_tokens limit
 *   - Large text encoding (1000+ characters)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "r8e_tokenizer.h"

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

/* =========================================================================
 * Test Vocabulary
 *
 * A small BPE vocabulary simulating a real model:
 *   0: "<unk>"    (unknown)
 *   1: "<s>"      (BOS, control)
 *   2: "</s>"     (EOS, control)
 *   3: " "
 *   4: "e"
 *   5: "t"
 *   6: "h"
 *   7: "th"       (merge of t+h)
 *   8: "he"       (merge of h+e)
 *   9: "the"      (merge of th+e)
 *  10: " the"     (merge of " "+the)
 *  11: "a"
 *  12: "n"
 *  13: "an"       (merge of a+n)
 *  14: "i"
 *  15: "in"       (merge of i+n)
 *  16: "ing"      (merge of in+g)
 *  17: " a"       (merge of " "+a)
 *  18: "r"
 *  19: "re"       (merge of r+e)
 *  20: "l"
 *  21: "o"
 *  22: "g"
 *  23: "d"
 *  24: "H"
 *  25: "w"
 *  26: "<0xC3>"   (byte fallback)
 *  27: "<0xA9>"   (byte fallback, forms U+00E9 'e' with <0xC3>)
 * ========================================================================= */

#define VOCAB_SIZE 28

static const char *test_vocab_strings[VOCAB_SIZE] = {
    "<unk>",  "<s>",   "</s>",  " ",     "e",     "t",     "h",
    "th",     "he",    "the",   " the",  "a",     "n",     "an",
    "i",      "in",    "ing",   " a",    "r",     "re",    "l",
    "o",      "g",     "d",     "H",     "w",     "<0xC3>", "<0xA9>"
};

/* Higher score = higher merge priority.
 * Scores decrease so that longer (later) merges have higher priority. */
static const float test_vocab_scores[VOCAB_SIZE] = {
    0.0f,     0.0f,    0.0f,   -3.0f,  -4.0f,  -5.0f,  -6.0f,
    -2.5f,   -2.4f,   -1.5f,  -0.5f,  -7.0f,  -8.0f,  -3.5f,
    -9.0f,   -3.0f,   -1.0f,  -2.0f,  -10.0f, -3.2f,  -11.0f,
    -12.0f,  -13.0f,  -14.0f, -15.0f, -16.0f, -20.0f,  -20.0f
};

static const uint8_t test_vocab_types[VOCAB_SIZE] = {
    R8E_TOKEN_UNKNOWN,  /* 0: <unk> */
    R8E_TOKEN_CONTROL,  /* 1: <s> */
    R8E_TOKEN_CONTROL,  /* 2: </s> */
    R8E_TOKEN_NORMAL,   /* 3: " " */
    R8E_TOKEN_NORMAL,   /* 4: e */
    R8E_TOKEN_NORMAL,   /* 5: t */
    R8E_TOKEN_NORMAL,   /* 6: h */
    R8E_TOKEN_NORMAL,   /* 7: th */
    R8E_TOKEN_NORMAL,   /* 8: he */
    R8E_TOKEN_NORMAL,   /* 9: the */
    R8E_TOKEN_NORMAL,   /* 10: " the" */
    R8E_TOKEN_NORMAL,   /* 11: a */
    R8E_TOKEN_NORMAL,   /* 12: n */
    R8E_TOKEN_NORMAL,   /* 13: an */
    R8E_TOKEN_NORMAL,   /* 14: i */
    R8E_TOKEN_NORMAL,   /* 15: in */
    R8E_TOKEN_NORMAL,   /* 16: ing */
    R8E_TOKEN_NORMAL,   /* 17: " a" */
    R8E_TOKEN_NORMAL,   /* 18: r */
    R8E_TOKEN_NORMAL,   /* 19: re */
    R8E_TOKEN_NORMAL,   /* 20: l */
    R8E_TOKEN_NORMAL,   /* 21: o */
    R8E_TOKEN_NORMAL,   /* 22: g */
    R8E_TOKEN_NORMAL,   /* 23: d */
    R8E_TOKEN_NORMAL,   /* 24: H */
    R8E_TOKEN_NORMAL,   /* 25: w */
    R8E_TOKEN_BYTE,     /* 26: <0xC3> */
    R8E_TOKEN_BYTE,     /* 27: <0xA9> */
};

static R8ETokenizer *
make_test_tokenizer(void)
{
    R8ETokenizer *tok = r8e_tokenizer_new(
        test_vocab_strings, test_vocab_scores,
        test_vocab_types, VOCAB_SIZE
    );
    if (tok) {
        r8e_tokenizer_set_special(tok, 1, 2, UINT32_MAX, 0);
    }
    return tok;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

TEST(tokenizer_create_destroy)
{
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    ASSERT_EQ_INT(tok->vocab_size, VOCAB_SIZE);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_null_safe)
{
    /* NULL free should not crash */
    r8e_tokenizer_free(NULL);
    /* NULL/invalid args */
    ASSERT_EQ_INT(r8e_tokenize(NULL, "hi", 2, NULL, 0), -1);
    ASSERT_TRUE(r8e_detokenize(NULL, 0) == NULL);
}

TEST(tokenizer_special_ids)
{
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    ASSERT_EQ_INT(tok->bos_id, 1);
    ASSERT_EQ_INT(tok->eos_id, 2);
    ASSERT_EQ_INT(tok->pad_id, UINT32_MAX);
    ASSERT_EQ_INT(tok->unknown_id, 0);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_hash_lookup_all)
{
    /* Every non-empty token in vocab should be found by hash lookup */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    for (int i = 0; i < VOCAB_SIZE; i++) {
        const char *s = r8e_detokenize(tok, i);
        ASSERT_TRUE(s != NULL);
        ASSERT_EQ_STR(s, test_vocab_strings[i]);
    }
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_detokenize_invalid)
{
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    ASSERT_TRUE(r8e_detokenize(tok, -1) == NULL);
    ASSERT_TRUE(r8e_detokenize(tok, VOCAB_SIZE) == NULL);
    ASSERT_TRUE(r8e_detokenize(tok, 9999) == NULL);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_empty_input)
{
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[16];
    int n = r8e_tokenize(tok, "", 0, out, 16);
    ASSERT_EQ_INT(n, 0);
    n = r8e_count_tokens(tok, "", 0);
    ASSERT_EQ_INT(n, 0);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_single_char)
{
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[4];
    int n = r8e_tokenize(tok, "e", 1, out, 4);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0], 4);  /* "e" is token 4 */
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_encode_the)
{
    /* "the" should merge: t(5)+h(6)->th(7), th(7)+e(4)->the(9) */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[16];
    int n = r8e_tokenize(tok, "the", 3, out, 16);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0], 9);  /* "the" */
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_encode_space_the)
{
    /* " the" should merge to token 10 */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[16];
    int n = r8e_tokenize(tok, " the", 4, out, 16);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0], 10);  /* " the" */
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_encode_ing)
{
    /* "ing" should merge: i(14)+n(12)->in(15), in(15)+g(22)->ing(16) */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[16];
    int n = r8e_tokenize(tok, "ing", 3, out, 16);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0], 16);  /* "ing" */
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_roundtrip)
{
    /* Encode " the" then decode and verify text matches */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[16];
    int n = r8e_tokenize(tok, " the", 4, out, 16);
    ASSERT_EQ_INT(n, 1);

    char buf[64];
    int written = r8e_detokenize_many(tok, out, n, buf, 64);
    ASSERT_EQ_INT(written, 4);
    ASSERT_EQ_STR(buf, " the");
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_roundtrip_multi)
{
    /* "the the" -> [9, 10] -> "the the" */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    const char *text = "the the";
    int out[16];
    int n = r8e_tokenize(tok, text, 7, out, 16);
    ASSERT_TRUE(n >= 1);

    char buf[64];
    int written = r8e_detokenize_many(tok, out, n, buf, 64);
    ASSERT_EQ_STR(buf, text);
    ASSERT_EQ_INT(written, 7);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_count_matches_encode)
{
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    const char *text = "the the";
    int text_len = 7;
    int out[32];
    int n_encode = r8e_tokenize(tok, text, text_len, out, 32);
    int n_count  = r8e_count_tokens(tok, text, text_len);
    ASSERT_EQ_INT(n_encode, n_count);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_decode_control_skipped)
{
    /* Control tokens (BOS, EOS) should be skipped in decoded output */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int tokens[] = { 1, 9, 2 };  /* <s> the </s> */
    char buf[64];
    int written = r8e_detokenize_many(tok, tokens, 3, buf, 64);
    ASSERT_EQ_STR(buf, "the");
    ASSERT_EQ_INT(written, 3);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_byte_fallback_utf8)
{
    /* UTF-8 'e' (U+00E9) = bytes 0xC3 0xA9 -> byte tokens 26, 27 */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    const char text[] = "\xC3\xA9";  /* UTF-8 for U+00E9 */
    int out[16];
    int n = r8e_tokenize(tok, text, 2, out, 16);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(out[0], 26);  /* <0xC3> */
    ASSERT_EQ_INT(out[1], 27);  /* <0xA9> */

    /* Decode byte fallback tokens back to raw bytes */
    char buf[16];
    int written = r8e_detokenize_many(tok, out, n, buf, 16);
    ASSERT_EQ_INT(written, 2);
    ASSERT_EQ_INT((uint8_t)buf[0], 0xC3);
    ASSERT_EQ_INT((uint8_t)buf[1], 0xA9);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_max_tokens_truncation)
{
    /* Encode text but with a very small max_tokens to test truncation */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    const char *text = "the the the";
    int out[1];
    int n = r8e_tokenize(tok, text, 11, out, 1);
    /* Should return at most 1 token due to max_tokens limit */
    ASSERT_EQ_INT(n, 1);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_large_text)
{
    /* Encode 1000+ characters of repeated "the " */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);

    int text_len = 1024;
    char *text = malloc((size_t)text_len + 1);
    ASSERT_TRUE(text != NULL);
    for (int i = 0; i < text_len; i += 4) {
        int remaining = text_len - i;
        int chunk = remaining < 4 ? remaining : 4;
        memcpy(text + i, "the ", (size_t)chunk);
    }
    text[text_len] = '\0';

    int *out = malloc(sizeof(int) * (size_t)text_len);
    ASSERT_TRUE(out != NULL);

    int n = r8e_tokenize(tok, text, text_len, out, text_len);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(n < text_len);  /* BPE should compress */

    /* Round-trip check */
    char *buf = malloc((size_t)text_len + 1);
    ASSERT_TRUE(buf != NULL);
    int written = r8e_detokenize_many(tok, out, n, buf, text_len + 1);
    ASSERT_EQ_INT(written, text_len);
    ASSERT_TRUE(memcmp(buf, text, (size_t)text_len) == 0);

    free(buf);
    free(out);
    free(text);
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_decode_many_buffer_limit)
{
    /* Ensure decode respects buffer capacity */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int tokens[] = { 9, 3, 9 };  /* "the" + " " + "the" = "the the" */
    char buf[5];  /* Only 4 chars + null */
    int written = r8e_detokenize_many(tok, tokens, 3, buf, 5);
    /* Should write at most 4 bytes */
    ASSERT_TRUE(written <= 4);
    ASSERT_TRUE(buf[written] == '\0');
    r8e_tokenizer_free(tok);
}

TEST(tokenizer_encode_an)
{
    /* "an" should merge: a(11)+n(12)->an(13) */
    R8ETokenizer *tok = make_test_tokenizer();
    ASSERT_TRUE(tok != NULL);
    int out[4];
    int n = r8e_tokenize(tok, "an", 2, out, 4);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0], 13);  /* "an" */
    r8e_tokenizer_free(tok);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void
run_tokenizer_tests(void)
{
    RUN_TEST(tokenizer_create_destroy);
    RUN_TEST(tokenizer_null_safe);
    RUN_TEST(tokenizer_special_ids);
    RUN_TEST(tokenizer_hash_lookup_all);
    RUN_TEST(tokenizer_detokenize_invalid);
    RUN_TEST(tokenizer_empty_input);
    RUN_TEST(tokenizer_single_char);
    RUN_TEST(tokenizer_encode_the);
    RUN_TEST(tokenizer_encode_space_the);
    RUN_TEST(tokenizer_encode_ing);
    RUN_TEST(tokenizer_encode_an);
    RUN_TEST(tokenizer_roundtrip);
    RUN_TEST(tokenizer_roundtrip_multi);
    RUN_TEST(tokenizer_count_matches_encode);
    RUN_TEST(tokenizer_decode_control_skipped);
    RUN_TEST(tokenizer_byte_fallback_utf8);
    RUN_TEST(tokenizer_max_tokens_truncation);
    RUN_TEST(tokenizer_large_text);
    RUN_TEST(tokenizer_decode_many_buffer_limit);
}
