/*
 * r8e_sampler.h - LLM Token Sampling Strategies
 *
 * Part of the r8e JavaScript engine.
 * Sampling strategies for selecting tokens from logit distributions
 * produced by language models. Supports temperature scaling, top-k,
 * top-p (nucleus), repetition penalty, and greedy (argmax) selection.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_SAMPLER_H
#define R8E_SAMPLER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Sampler Configuration
 * ========================================================================= */

typedef struct {
    float    temperature;     /* 0.0=greedy, 1.0=neutral */
    float    top_p;           /* nucleus sampling (0.9 typical) */
    int32_t  top_k;           /* top-k filtering (40 typical) */
    float    repeat_penalty;  /* penalize recent tokens (1.1 typical) */
    int32_t  repeat_window;   /* how many recent tokens to check */
    uint64_t seed;            /* RNG seed (0 = random) */
} R8ESamplerConfig;

/* =========================================================================
 * Sampling Functions
 * ========================================================================= */

/**
 * Sample a token from a logits array using the full sampling pipeline.
 *
 * Pipeline order:
 *   1. Apply repetition penalty
 *   2. Apply temperature scaling
 *   3. Apply top-k filtering
 *   4. Compute softmax probabilities
 *   5. Apply top-p (nucleus) filtering
 *   6. Sample from the resulting distribution
 *
 * @param logits        Array of raw logits (length = vocab_size). Modified in-place.
 * @param vocab_size    Number of tokens in vocabulary.
 * @param config        Sampling configuration parameters.
 * @param recent_tokens Array of recently generated token IDs (for repeat penalty).
 * @param recent_count  Number of recent tokens.
 * @return              Selected token index, or -1 on error.
 */
int r8e_sample(float *logits, int vocab_size, const R8ESamplerConfig *config,
               const int *recent_tokens, int recent_count);

/**
 * Greedy (argmax) sampling -- always pick the highest logit.
 *
 * @param logits      Array of logits.
 * @param vocab_size  Number of tokens.
 * @return            Index of the highest logit, or -1 if vocab_size <= 0.
 */
int r8e_sample_greedy(const float *logits, int vocab_size);

/**
 * Apply temperature scaling to logits in-place.
 *
 * Each logit is divided by temperature. Temperature of 0.0 is treated
 * as greedy (no modification). Temperature < 0.0 is clamped to a small
 * positive value.
 *
 * @param logits      Array of logits (modified in-place).
 * @param vocab_size  Number of tokens.
 * @param temperature Temperature value (> 0.0).
 */
void r8e_sample_apply_temperature(float *logits, int vocab_size, float temperature);

/**
 * Apply repetition penalty to logits in-place.
 *
 * For each token that appears in the recent_tokens array, its logit is
 * divided by penalty if positive, or multiplied by penalty if negative.
 *
 * @param logits        Array of logits (modified in-place).
 * @param vocab_size    Number of tokens.
 * @param recent        Array of recently generated token IDs.
 * @param recent_count  Number of recent tokens.
 * @param penalty       Penalty multiplier (1.0 = no penalty, > 1.0 = penalize).
 */
void r8e_sample_apply_repeat_penalty(float *logits, int vocab_size,
                                      const int *recent, int recent_count,
                                      float penalty);

/**
 * Compute softmax over logits in-place.
 *
 * After this call, logits[i] contains the probability for token i.
 * Uses the max-subtraction trick for numerical stability.
 *
 * @param logits  Array of logits/probabilities (modified in-place).
 * @param n       Number of elements.
 */
void r8e_softmax(float *logits, int n);

/**
 * Top-k filtering: zero out all but the top-k logits.
 *
 * Finds the k-th largest logit value and sets all logits below that
 * threshold to -INFINITY.
 *
 * @param logits      Array of logits (modified in-place).
 * @param vocab_size  Number of tokens.
 * @param k           Number of top tokens to keep (k <= 0 means no filtering).
 */
void r8e_sample_top_k(float *logits, int vocab_size, int k);

/**
 * Top-p (nucleus) filtering: zero out logits below the nucleus threshold.
 *
 * After softmax, tokens are sorted by probability. The smallest set of
 * tokens whose cumulative probability exceeds p is kept; all others are
 * set to 0.0.
 *
 * @param logits      Array of probabilities after softmax (modified in-place).
 * @param vocab_size  Number of tokens.
 * @param p           Nucleus threshold (0.0 < p <= 1.0).
 */
void r8e_sample_top_p(float *logits, int vocab_size, float p);

#ifdef __cplusplus
}
#endif

#endif /* R8E_SAMPLER_H */
