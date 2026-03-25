/*
 * r8e_sampler.c - LLM Token Sampling Strategies
 *
 * Part of the r8e JavaScript engine.
 * Implements sampling strategies for selecting tokens from logit distributions:
 *   - Greedy (argmax) selection
 *   - Temperature scaling
 *   - Top-k filtering
 *   - Top-p (nucleus) filtering
 *   - Repetition penalty
 *   - Full pipeline combining all strategies
 *
 * Uses xorshift64 for fast, deterministic pseudo-random number generation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_sampler.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* =========================================================================
 * Internal Constants
 * ========================================================================= */

#define MIN_TEMPERATURE  1e-7f   /* clamp temperature to avoid division by zero */
#define NEG_INF         (-INFINITY)

/* =========================================================================
 * Xorshift64 PRNG
 * ========================================================================= */

typedef struct {
    uint64_t state;
} R8ERNG;

static void rng_init(R8ERNG *rng, uint64_t seed) {
    /* Ensure non-zero state */
    if (seed == 0) {
        /* Use a simple hash of the address as fallback entropy */
        seed = (uint64_t)(uintptr_t)rng;
        seed ^= 0x2545F4914F6CDD1DULL;
        if (seed == 0) seed = 1;
    }
    rng->state = seed;
}

static uint64_t rng_next(R8ERNG *rng) {
    uint64_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng->state = x;
    return x;
}

/**
 * Generate a uniform random float in [0, 1).
 */
static float rng_float(R8ERNG *rng) {
    /* Use upper 24 bits for mantissa precision */
    uint64_t r = rng_next(rng);
    return (float)(r >> 40) / (float)(1ULL << 24);
}

/* =========================================================================
 * Greedy Sampling (Argmax)
 * ========================================================================= */

int r8e_sample_greedy(const float *logits, int vocab_size) {
    if (!logits || vocab_size <= 0) return -1;

    int best = 0;
    float best_val = logits[0];

    for (int i = 1; i < vocab_size; i++) {
        if (logits[i] > best_val) {
            best_val = logits[i];
            best = i;
        }
    }

    return best;
}

/* =========================================================================
 * Temperature Scaling
 * ========================================================================= */

void r8e_sample_apply_temperature(float *logits, int vocab_size, float temperature) {
    if (!logits || vocab_size <= 0) return;

    /* Temperature of 0 means greedy -- don't modify logits */
    if (temperature <= 0.0f) return;

    /* Clamp to minimum to avoid numerical issues */
    if (temperature < MIN_TEMPERATURE) {
        temperature = MIN_TEMPERATURE;
    }

    float inv_temp = 1.0f / temperature;
    for (int i = 0; i < vocab_size; i++) {
        logits[i] *= inv_temp;
    }
}

/* =========================================================================
 * Repetition Penalty
 * ========================================================================= */

void r8e_sample_apply_repeat_penalty(float *logits, int vocab_size,
                                      const int *recent, int recent_count,
                                      float penalty) {
    if (!logits || !recent || vocab_size <= 0 || recent_count <= 0) return;
    if (penalty <= 0.0f || penalty == 1.0f) return;

    for (int i = 0; i < recent_count; i++) {
        int tok = recent[i];
        if (tok < 0 || tok >= vocab_size) continue;

        /*
         * If the logit is positive, divide by penalty (makes it less likely).
         * If the logit is negative, multiply by penalty (makes it even less likely).
         */
        if (logits[tok] > 0.0f) {
            logits[tok] /= penalty;
        } else {
            logits[tok] *= penalty;
        }
    }
}

/* =========================================================================
 * Softmax
 * ========================================================================= */

void r8e_softmax(float *logits, int n) {
    if (!logits || n <= 0) return;

    /* Find max for numerical stability */
    float max_val = logits[0];
    for (int i = 1; i < n; i++) {
        if (logits[i] > max_val) {
            max_val = logits[i];
        }
    }

    /* Compute exp(x - max) and sum */
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float val = logits[i] - max_val;
        /* Clamp to avoid underflow */
        if (val < -88.0f) val = -88.0f;
        logits[i] = expf(val);
        sum += logits[i];
    }

    /* Normalize */
    if (sum > 0.0f) {
        float inv_sum = 1.0f / sum;
        for (int i = 0; i < n; i++) {
            logits[i] *= inv_sum;
        }
    }
}

/* =========================================================================
 * Top-K Filtering
 * ========================================================================= */

/**
 * Find the k-th largest value in an array using partial selection.
 *
 * This uses a simple O(n*k) approach which is efficient for small k values
 * typical in LLM sampling (k=20..100). For very large k, a more
 * sophisticated algorithm would be warranted.
 */
static float find_kth_largest(const float *arr, int n, int k) {
    if (k <= 0 || k > n) return NEG_INF;

    /* Use a small min-heap of size k to track the top-k values.
     * For simplicity, use a sorted array approach for small k. */
    float *topk = (float *)malloc((size_t)k * sizeof(float));
    if (!topk) return NEG_INF;

    /* Initialize with -inf */
    for (int i = 0; i < k; i++) {
        topk[i] = NEG_INF;
    }

    for (int i = 0; i < n; i++) {
        float val = arr[i];
        /* Check if val is greater than the smallest in our top-k */
        if (val > topk[0]) {
            topk[0] = val;
            /* Bubble up to maintain sorted order (ascending) */
            for (int j = 0; j < k - 1; j++) {
                if (topk[j] > topk[j + 1]) {
                    float tmp = topk[j];
                    topk[j] = topk[j + 1];
                    topk[j + 1] = tmp;
                } else {
                    break;
                }
            }
        }
    }

    float result = topk[0]; /* k-th largest = smallest in top-k set */
    free(topk);
    return result;
}

void r8e_sample_top_k(float *logits, int vocab_size, int k) {
    if (!logits || vocab_size <= 0) return;
    if (k <= 0 || k >= vocab_size) return; /* no filtering needed */

    float threshold = find_kth_largest(logits, vocab_size, k);

    /* Zero out everything below the threshold.
     * Count how many are at or above the threshold to handle ties. */
    int count = 0;
    for (int i = 0; i < vocab_size; i++) {
        if (logits[i] >= threshold) {
            count++;
        }
    }

    /* If there are more than k values at the threshold (ties),
     * we still keep all of them to avoid arbitrary elimination. */
    if (count <= k) {
        for (int i = 0; i < vocab_size; i++) {
            if (logits[i] < threshold) {
                logits[i] = NEG_INF;
            }
        }
    } else {
        /* Ties at threshold: keep exactly k by eliminating excess */
        int excess = count - k;
        for (int i = 0; i < vocab_size && excess > 0; i++) {
            if (logits[i] == threshold) {
                logits[i] = NEG_INF;
                excess--;
            }
        }
    }
}

/* =========================================================================
 * Top-P (Nucleus) Filtering
 * ========================================================================= */

/**
 * Comparison function for sorting indices by probability (descending).
 */
typedef struct {
    float prob;
    int   index;
} ProbIndex;

static int prob_index_cmp_desc(const void *a, const void *b) {
    const ProbIndex *pa = (const ProbIndex *)a;
    const ProbIndex *pb = (const ProbIndex *)b;
    if (pa->prob > pb->prob) return -1;
    if (pa->prob < pb->prob) return  1;
    return 0;
}

void r8e_sample_top_p(float *logits, int vocab_size, float p) {
    if (!logits || vocab_size <= 0) return;
    if (p <= 0.0f || p >= 1.0f) return; /* no filtering needed */

    /* Build sorted index array */
    ProbIndex *sorted = (ProbIndex *)malloc((size_t)vocab_size * sizeof(ProbIndex));
    if (!sorted) return;

    for (int i = 0; i < vocab_size; i++) {
        sorted[i].prob  = logits[i];
        sorted[i].index = i;
    }

    qsort(sorted, (size_t)vocab_size, sizeof(ProbIndex), prob_index_cmp_desc);

    /* Accumulate probabilities until we exceed p */
    float cumsum = 0.0f;
    int cutoff = vocab_size; /* index in sorted array where we cut */

    for (int i = 0; i < vocab_size; i++) {
        cumsum += sorted[i].prob;
        if (cumsum > p) {
            cutoff = i + 1; /* keep this one too */
            break;
        }
    }

    /* Always keep at least one token */
    if (cutoff < 1) cutoff = 1;

    /* Zero out tokens below the cutoff */
    for (int i = cutoff; i < vocab_size; i++) {
        logits[sorted[i].index] = 0.0f;
    }

    free(sorted);
}

/* =========================================================================
 * Categorical Sampling from Probability Distribution
 * ========================================================================= */

/**
 * Sample an index from a probability distribution.
 *
 * @param probs       Array of probabilities (must sum to ~1.0).
 * @param n           Number of elements.
 * @param rng         Random number generator.
 * @return            Sampled index.
 */
static int sample_categorical(const float *probs, int n, R8ERNG *rng) {
    float r = rng_float(rng);

    /* Compute sum for re-normalization (probs may not sum to 1.0 after filtering) */
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += probs[i];
    }

    if (sum <= 0.0f) {
        /* Fallback: uniform random selection among non-zero entries */
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (probs[i] > 0.0f) count++;
        }
        if (count == 0) return 0; /* shouldn't happen, but safety */

        int target = (int)(r * (float)count);
        if (target >= count) target = count - 1;
        int seen = 0;
        for (int i = 0; i < n; i++) {
            if (probs[i] > 0.0f) {
                if (seen == target) return i;
                seen++;
            }
        }
        return 0;
    }

    /* Scale random value by sum for proper normalization */
    float target = r * sum;
    float cumsum = 0.0f;

    for (int i = 0; i < n; i++) {
        cumsum += probs[i];
        if (cumsum > target) {
            return i;
        }
    }

    /* Rounding: return last non-zero entry */
    for (int i = n - 1; i >= 0; i--) {
        if (probs[i] > 0.0f) return i;
    }

    return n - 1;
}

/* =========================================================================
 * Full Sampling Pipeline
 * ========================================================================= */

int r8e_sample(float *logits, int vocab_size, const R8ESamplerConfig *config,
               const int *recent_tokens, int recent_count) {
    if (!logits || vocab_size <= 0 || !config) return -1;

    /* Step 0: Handle greedy case */
    if (config->temperature <= 0.0f) {
        return r8e_sample_greedy(logits, vocab_size);
    }

    /* Step 1: Apply repetition penalty */
    if (config->repeat_penalty > 1.0f && recent_tokens && recent_count > 0) {
        int window = recent_count;
        if (config->repeat_window > 0 && config->repeat_window < window) {
            window = config->repeat_window;
        }
        /* Apply penalty only to the last 'window' tokens */
        const int *recent_start = recent_tokens + (recent_count - window);
        r8e_sample_apply_repeat_penalty(logits, vocab_size,
                                         recent_start, window,
                                         config->repeat_penalty);
    }

    /* Step 2: Apply temperature */
    r8e_sample_apply_temperature(logits, vocab_size, config->temperature);

    /* Step 3: Top-k filtering */
    if (config->top_k > 0 && config->top_k < vocab_size) {
        r8e_sample_top_k(logits, vocab_size, config->top_k);
    }

    /* Step 4: Softmax to convert logits to probabilities */
    r8e_softmax(logits, vocab_size);

    /* Step 5: Top-p (nucleus) filtering */
    if (config->top_p > 0.0f && config->top_p < 1.0f) {
        r8e_sample_top_p(logits, vocab_size, config->top_p);
    }

    /* Step 6: Sample from the distribution */
    R8ERNG rng;
    rng_init(&rng, config->seed);

    return sample_categorical(logits, vocab_size, &rng);
}
