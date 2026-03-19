/*
 * r8e_tokenizer.c - BPE (Byte Pair Encoding) Tokenizer Implementation
 *
 * Part of the r8e JavaScript engine.
 * Standalone BPE tokenizer: encode text to token IDs, decode back to text.
 * Works with vocabularies extracted from GGUF model files.
 *
 * Design:
 *   - FNV-1a hash map for O(1) token string -> ID lookup
 *   - Greedy BPE merge loop: find best-scoring adjacent pair, merge, repeat
 *   - Byte fallback: <0xNN> tokens for bytes not in the vocabulary
 *   - UTF-8 aware: handles multi-byte sequences correctly
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_tokenizer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

/* =========================================================================
 * Internal Constants
 * ========================================================================= */

#define HASH_LOAD_FACTOR  2     /* hash table capacity = vocab_size * factor */
#define FNV_OFFSET_BASIS  2166136261U
#define FNV_PRIME         16777619U
#define INVALID_TOKEN     UINT32_MAX

/* Maximum tokens from a single encode call (safety limit) */
#define MAX_WORK_TOKENS   (1024 * 1024)

/* =========================================================================
 * FNV-1a Hashing
 * ========================================================================= */

static uint32_t
fnv1a(const char *s, int len)
{
    uint32_t h = FNV_OFFSET_BASIS;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint32_t
fnv1a_str(const char *s)
{
    uint32_t h = FNV_OFFSET_BASIS;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= FNV_PRIME;
    }
    return h;
}

/* =========================================================================
 * Hash Map Operations
 * ========================================================================= */

static uint32_t
next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v < 16 ? 16 : v;
}

/*
 * Insert a token into the hash map.
 * Uses open addressing with linear probing.
 * hash_keys stores the FNV-1a hash (0 = empty slot).
 * To distinguish hash value 0 from empty, we set bit 31 on all stored hashes.
 */
static void
hash_insert(R8ETokenizer *tok, const char *token_str, uint32_t token_id)
{
    uint32_t h = fnv1a_str(token_str) | 0x80000000U;
    uint32_t mask = tok->hash_cap - 1;
    uint32_t idx = h & mask;

    for (;;) {
        if (tok->hash_keys[idx] == 0) {
            tok->hash_keys[idx] = h;
            tok->hash_vals[idx] = token_id;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

/*
 * Look up a token string in the hash map.
 * Returns the token ID, or INVALID_TOKEN if not found.
 */
static uint32_t
hash_lookup(const R8ETokenizer *tok, const char *s, int len)
{
    uint32_t h = fnv1a(s, len) | 0x80000000U;
    uint32_t mask = tok->hash_cap - 1;
    uint32_t idx = h & mask;

    for (;;) {
        uint32_t stored = tok->hash_keys[idx];
        if (stored == 0) {
            return INVALID_TOKEN;
        }
        if (stored == h) {
            /* Hash match - verify the actual string */
            uint32_t tid = tok->hash_vals[idx];
            const char *candidate = tok->tokens[tid];
            int clen = (int)strlen(candidate);
            if (clen == len && memcmp(candidate, s, (size_t)len) == 0) {
                return tid;
            }
        }
        idx = (idx + 1) & mask;
    }
}

/* =========================================================================
 * Byte Fallback Helpers
 * ========================================================================= */

/*
 * Parse a byte fallback token like "<0x41>" and return its byte value.
 * Returns -1 if the string is not a valid byte token.
 */
static int
parse_byte_token(const char *s)
{
    if (s[0] != '<' || s[1] != '0' || s[2] != 'x') return -1;

    int val = 0;
    int i = 3;
    for (; s[i] && s[i] != '>'; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9')      d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f')  d = s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F')  d = s[i] - 'A' + 10;
        else return -1;
        val = val * 16 + d;
    }

    if (s[i] != '>' || val > 255) return -1;
    return val;
}

/*
 * Format a byte fallback token name into buf (must be >= 7 bytes).
 * Returns the length written (always 6): "<0xNN>"
 */
static int
format_byte_token(char *buf, uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '<';
    buf[1] = '0';
    buf[2] = 'x';
    buf[3] = hex[(byte >> 4) & 0xF];
    buf[4] = hex[byte & 0xF];
    buf[5] = '>';
    buf[6] = '\0';
    return 6;
}

/* =========================================================================
 * Tokenizer Lifecycle
 * ========================================================================= */

R8ETokenizer *
r8e_tokenizer_new(const char **tokens, const float *scores,
                   const uint8_t *types, uint32_t vocab_size)
{
    if (!tokens || vocab_size == 0) return NULL;

    R8ETokenizer *tok = calloc(1, sizeof(R8ETokenizer));
    if (!tok) return NULL;

    tok->vocab_size = vocab_size;
    tok->bos_id     = INVALID_TOKEN;
    tok->eos_id     = INVALID_TOKEN;
    tok->pad_id     = INVALID_TOKEN;
    tok->unknown_id = INVALID_TOKEN;

    /* Allocate token string array */
    tok->tokens = calloc(vocab_size, sizeof(char *));
    if (!tok->tokens) goto fail;

    /* Allocate scores array */
    tok->scores = calloc(vocab_size, sizeof(float));
    if (!tok->scores) goto fail;

    /* Allocate types array */
    tok->token_types = calloc(vocab_size, sizeof(uint8_t));
    if (!tok->token_types) goto fail;

    /* Copy token data */
    for (uint32_t i = 0; i < vocab_size; i++) {
        if (tokens[i]) {
            size_t len = strlen(tokens[i]);
            tok->tokens[i] = malloc(len + 1);
            if (!tok->tokens[i]) goto fail;
            memcpy(tok->tokens[i], tokens[i], len + 1);
        } else {
            tok->tokens[i] = calloc(1, 1);  /* empty string */
            if (!tok->tokens[i]) goto fail;
        }
        tok->scores[i]      = scores ? scores[i] : -(float)i;
        tok->token_types[i]  = types  ? types[i]  : R8E_TOKEN_NORMAL;
    }

    /* Build hash map */
    tok->hash_cap = next_pow2(vocab_size * HASH_LOAD_FACTOR);
    tok->hash_keys = calloc(tok->hash_cap, sizeof(uint32_t));
    tok->hash_vals = calloc(tok->hash_cap, sizeof(uint32_t));
    if (!tok->hash_keys || !tok->hash_vals) goto fail;

    for (uint32_t i = 0; i < vocab_size; i++) {
        if (tok->tokens[i][0] != '\0') {
            hash_insert(tok, tok->tokens[i], i);
        }
    }

    return tok;

fail:
    r8e_tokenizer_free(tok);
    return NULL;
}

void
r8e_tokenizer_free(R8ETokenizer *tok)
{
    if (!tok) return;

    if (tok->tokens) {
        for (uint32_t i = 0; i < tok->vocab_size; i++) {
            free(tok->tokens[i]);
        }
        free(tok->tokens);
    }
    free(tok->scores);
    free(tok->token_types);
    free(tok->hash_keys);
    free(tok->hash_vals);
    free(tok);
}

void
r8e_tokenizer_set_special(R8ETokenizer *tok, uint32_t bos, uint32_t eos,
                            uint32_t pad, uint32_t unknown)
{
    if (!tok) return;
    tok->bos_id     = bos;
    tok->eos_id     = eos;
    tok->pad_id     = pad;
    tok->unknown_id = unknown;
}

/* =========================================================================
 * BPE Encoding
 *
 * We use a linked-list-style approach over an array of token IDs:
 *   1. Initialize each byte/character as its own token
 *   2. Repeatedly find the highest-scoring adjacent merge pair
 *   3. Merge them into a single token
 *   4. Stop when no more merges are possible
 *
 * For efficiency we concatenate the strings of adjacent tokens and look up
 * the result in the hash map. If found, the merge score is the score of
 * the merged token.
 * ========================================================================= */

/* Work item for the merge loop */
typedef struct {
    int      token_id;   /* current token ID */
    int      str_off;    /* offset into the original text */
    int      str_len;    /* length in the original text */
    int      next;       /* index of next item (-1 = end) */
    int      prev;       /* index of prev item (-1 = start) */
} BPEItem;

/*
 * Try to find a merged token for two adjacent items.
 * Concatenates their strings and looks up in the hash map.
 * Returns the merged token ID or -1 if not found.
 */
static int
try_merge(const R8ETokenizer *tok, const char *text,
          const BPEItem *a, const BPEItem *b)
{
    int total_len = a->str_len + b->str_len;
    if (total_len > 256) return -1;  /* sanity limit on merged token length */

    /* If both items are contiguous in the original text, we can avoid copying */
    if (a->str_off + a->str_len == b->str_off) {
        uint32_t id = hash_lookup(tok, text + a->str_off, total_len);
        return id == INVALID_TOKEN ? -1 : (int)id;
    }

    /* Non-contiguous (shouldn't happen in normal flow, but handle it) */
    char buf[257];
    /* Use the token strings instead */
    const char *sa = tok->tokens[a->token_id];
    const char *sb = tok->tokens[b->token_id];
    int la = (int)strlen(sa);
    int lb = (int)strlen(sb);
    if (la + lb > 256) return -1;
    memcpy(buf, sa, (size_t)la);
    memcpy(buf + la, sb, (size_t)lb);
    uint32_t id = hash_lookup(tok, buf, la + lb);
    return id == INVALID_TOKEN ? -1 : (int)id;
}

/*
 * Core BPE encoding. Writes token IDs into out_tokens.
 * If out_tokens is NULL, only counts tokens.
 * Returns the number of tokens, or -1 on error.
 */
static int
bpe_encode(const R8ETokenizer *tok, const char *text, int text_len,
           int *out_tokens, int max_tokens)
{
    if (text_len == 0) return 0;
    if (text_len > MAX_WORK_TOKENS) return -1;

    /* Step 1: Initialize each byte as a token */
    int n = text_len;
    BPEItem *items = malloc((size_t)n * sizeof(BPEItem));
    if (!items) return -1;

    for (int i = 0; i < n; i++) {
        /* Try single-byte token first */
        uint32_t tid = hash_lookup(tok, text + i, 1);
        if (tid == INVALID_TOKEN) {
            /* Try byte fallback token <0xNN> */
            char byte_tok[8];
            format_byte_token(byte_tok, (uint8_t)text[i]);
            tid = hash_lookup(tok, byte_tok, 6);
        }
        if (tid == INVALID_TOKEN) {
            /* Last resort: unknown token */
            tid = tok->unknown_id;
            if (tid == INVALID_TOKEN) {
                /* No unknown token available; skip this byte */
                items[i].token_id = -1;
                items[i].str_off  = i;
                items[i].str_len  = 1;
                items[i].prev     = i - 1;
                items[i].next     = (i + 1 < n) ? i + 1 : -1;
                continue;
            }
        }
        items[i].token_id = (int)tid;
        items[i].str_off  = i;
        items[i].str_len  = 1;
        items[i].prev     = i - 1;
        items[i].next     = (i + 1 < n) ? i + 1 : -1;
    }

    /* Remove items with token_id == -1 (unmappable bytes) */
    int head = 0;
    while (head < n && items[head].token_id == -1) head++;
    if (head >= n) {
        free(items);
        return 0;
    }
    /* Fix up the linked list to skip removed items */
    int prev_valid = -1;
    for (int i = head; i != -1; ) {
        int next = items[i].next;
        if (items[i].token_id == -1) {
            /* Remove from list */
            if (prev_valid >= 0) items[prev_valid].next = next;
            if (next >= 0) items[next].prev = prev_valid;
        } else {
            items[i].prev = prev_valid;
            prev_valid = i;
        }
        i = next;
    }

    /* Step 2: BPE merge loop */
    for (;;) {
        /* Find the best merge (highest score) */
        float best_score = -FLT_MAX;
        int best_i = -1;
        int best_merged_id = -1;

        for (int i = head; i != -1; ) {
            int j = items[i].next;
            if (j == -1) break;

            int merged_id = try_merge(tok, text, &items[i], &items[j]);
            if (merged_id >= 0) {
                float score = tok->scores[merged_id];
                if (score > best_score) {
                    best_score = score;
                    best_i = i;
                    best_merged_id = merged_id;
                }
            }
            i = j;
        }

        if (best_i < 0) break;  /* no more merges possible */

        /* Apply the best merge: combine items[best_i] and items[next] */
        int j = items[best_i].next;
        items[best_i].token_id = best_merged_id;
        items[best_i].str_len  = items[best_i].str_len + items[j].str_len;
        items[best_i].next     = items[j].next;
        if (items[j].next >= 0) {
            items[items[j].next].prev = best_i;
        }
    }

    /* Step 3: Collect results */
    int count = 0;
    for (int i = head; i != -1; i = items[i].next) {
        if (out_tokens) {
            if (count >= max_tokens) break;
            out_tokens[count] = items[i].token_id;
        }
        count++;
    }

    free(items);
    return count;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int
r8e_tokenize(const R8ETokenizer *tok, const char *text, int text_len,
              int *out_tokens, int max_tokens)
{
    if (!tok || !text || text_len < 0 || !out_tokens || max_tokens <= 0) {
        if (text_len == 0) return 0;
        return -1;
    }
    return bpe_encode(tok, text, text_len, out_tokens, max_tokens);
}

const char *
r8e_detokenize(const R8ETokenizer *tok, int token_id)
{
    if (!tok || token_id < 0 || (uint32_t)token_id >= tok->vocab_size) {
        return NULL;
    }
    return tok->tokens[token_id];
}

int
r8e_detokenize_many(const R8ETokenizer *tok, const int *tokens, int n_tokens,
                     char *out_buf, int buf_cap)
{
    if (!tok || !tokens || !out_buf || buf_cap <= 0) return -1;

    int written = 0;
    for (int i = 0; i < n_tokens; i++) {
        int tid = tokens[i];
        if (tid < 0 || (uint32_t)tid >= tok->vocab_size) continue;

        const char *s = tok->tokens[tid];
        uint8_t type = tok->token_types[tid];

        if (type == R8E_TOKEN_BYTE) {
            /* Decode byte fallback token <0xNN> to actual byte */
            int byte_val = parse_byte_token(s);
            if (byte_val >= 0) {
                if (written + 1 < buf_cap) {
                    out_buf[written] = (char)byte_val;
                    written++;
                } else {
                    break;
                }
                continue;
            }
        }

        if (type == R8E_TOKEN_CONTROL) {
            /* Skip control tokens (BOS, EOS, etc.) in decoded output */
            continue;
        }

        int slen = (int)strlen(s);
        if (written + slen >= buf_cap) {
            /* Partial write: copy what fits */
            int avail = buf_cap - 1 - written;
            if (avail > 0) {
                memcpy(out_buf + written, s, (size_t)avail);
                written += avail;
            }
            break;
        }
        memcpy(out_buf + written, s, (size_t)slen);
        written += slen;
    }

    out_buf[written] = '\0';
    return written;
}

int
r8e_count_tokens(const R8ETokenizer *tok, const char *text, int text_len)
{
    if (!tok || !text || text_len < 0) {
        if (text_len == 0) return 0;
        return -1;
    }
    return bpe_encode(tok, text, text_len, NULL, 0);
}
