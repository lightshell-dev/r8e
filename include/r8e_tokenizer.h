/*
 * r8e_tokenizer.h - BPE (Byte Pair Encoding) Tokenizer
 *
 * Part of the r8e JavaScript engine.
 * Standalone BPE tokenizer for encoding text to token IDs and decoding
 * token IDs back to text. Designed for vocabularies extracted from GGUF
 * model files.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_TOKENIZER_H
#define R8E_TOKENIZER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Token Types
 * ========================================================================= */

typedef enum {
    R8E_TOKEN_NORMAL       = 0,
    R8E_TOKEN_UNKNOWN      = 1,
    R8E_TOKEN_CONTROL      = 2,
    R8E_TOKEN_USER_DEFINED = 3,
    R8E_TOKEN_UNUSED       = 4,
    R8E_TOKEN_BYTE         = 5   /* byte fallback tokens like <0x41> */
} R8ETokenType;

/* =========================================================================
 * Tokenizer Structure
 * ========================================================================= */

typedef struct {
    char    **tokens;          /* token ID -> string (owned, null-terminated) */
    float    *scores;          /* token ID -> merge priority score */
    uint8_t  *token_types;     /* token ID -> R8ETokenType */
    uint32_t  vocab_size;
    uint32_t  bos_id;          /* beginning-of-sequence token */
    uint32_t  eos_id;          /* end-of-sequence token */
    uint32_t  pad_id;          /* padding token (or UINT32_MAX if none) */
    uint32_t  unknown_id;      /* unknown token (or UINT32_MAX if none) */
    /* Internal: hash map for encoding (string -> token ID) */
    uint32_t *hash_keys;       /* FNV-1a hash of token string */
    uint32_t *hash_vals;       /* token ID */
    uint32_t  hash_cap;        /* hash table capacity (power of 2) */
} R8ETokenizer;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * Create a tokenizer from parallel arrays of token strings, scores, and types.
 *
 * @param tokens      Array of vocab_size null-terminated token strings.
 * @param scores      Array of vocab_size merge priority scores.
 * @param types       Array of vocab_size token type tags (R8ETokenType).
 * @param vocab_size  Number of tokens in the vocabulary.
 * @return            Newly allocated tokenizer, or NULL on OOM.
 *                    Must be freed with r8e_tokenizer_free().
 */
R8ETokenizer *r8e_tokenizer_new(const char **tokens, const float *scores,
                                 const uint8_t *types, uint32_t vocab_size);

/**
 * Destroy a tokenizer and free all associated memory.
 *
 * @param tok  Tokenizer to destroy (NULL is safe, does nothing).
 */
void r8e_tokenizer_free(R8ETokenizer *tok);

/**
 * Set special token IDs (BOS, EOS, PAD, unknown).
 * Use UINT32_MAX for any token that is not present.
 */
void r8e_tokenizer_set_special(R8ETokenizer *tok, uint32_t bos, uint32_t eos,
                                uint32_t pad, uint32_t unknown);

/* =========================================================================
 * Encoding (text -> token IDs)
 * ========================================================================= */

/**
 * Encode text into token IDs using BPE.
 *
 * Algorithm:
 *   1. Start with each byte as a separate token (using byte fallback if needed)
 *   2. Find the adjacent pair with the highest merge score
 *   3. Merge that pair into a single token
 *   4. Repeat until no more merges are possible
 *
 * @param tok         Tokenizer instance.
 * @param text        Input text (UTF-8, not necessarily null-terminated).
 * @param text_len    Length of input text in bytes.
 * @param out_tokens  Pre-allocated output array for token IDs.
 * @param max_tokens  Capacity of out_tokens array.
 * @return            Number of tokens written, or -1 on error.
 */
int r8e_tokenize(const R8ETokenizer *tok, const char *text, int text_len,
                 int *out_tokens, int max_tokens);

/* =========================================================================
 * Decoding (token IDs -> text)
 * ========================================================================= */

/**
 * Decode a single token ID to its string representation.
 *
 * For byte fallback tokens (R8E_TOKEN_BYTE), returns the decoded byte
 * as a string. The returned pointer is valid until the tokenizer is freed.
 *
 * @param tok       Tokenizer instance.
 * @param token_id  Token ID to decode.
 * @return          Pointer to the token string, or NULL if token_id is invalid.
 */
const char *r8e_detokenize(const R8ETokenizer *tok, int token_id);

/**
 * Decode an array of token IDs to text.
 *
 * @param tok       Tokenizer instance.
 * @param tokens    Array of token IDs.
 * @param n_tokens  Number of token IDs.
 * @param out_buf   Output buffer for decoded text.
 * @param buf_cap   Capacity of output buffer in bytes.
 * @return          Number of bytes written (excluding null terminator),
 *                  or -1 on error. Output is always null-terminated if
 *                  buf_cap > 0.
 */
int r8e_detokenize_many(const R8ETokenizer *tok, const int *tokens,
                         int n_tokens, char *out_buf, int buf_cap);

/* =========================================================================
 * Token Counting (fast path, no allocation for output)
 * ========================================================================= */

/**
 * Count the number of tokens in text without allocating an output array.
 *
 * @param tok       Tokenizer instance.
 * @param text      Input text (UTF-8).
 * @param text_len  Length of input text in bytes.
 * @return          Number of tokens, or -1 on error.
 */
int r8e_count_tokens(const R8ETokenizer *tok, const char *text, int text_len);

#ifdef __cplusplus
}
#endif

#endif /* R8E_TOKENIZER_H */
