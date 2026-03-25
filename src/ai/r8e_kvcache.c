/*
 * r8e_kvcache.c - Key-Value Cache for Transformer Attention
 *
 * Part of the r8e JavaScript engine.
 * Stores key and value vectors for each transformer layer in float16 format
 * for memory-efficient autoregressive inference. Supports sliding window
 * eviction, truncation, and reset operations.
 *
 * Float16 conversion is done via portable bit manipulation -- no hardware
 * half-precision intrinsics are used, ensuring compatibility across all
 * C11 targets.
 *
 * Memory layout per cache (K or V):
 *   [layer_0][layer_1]...[layer_N-1]
 *   where each layer is: [pos_0][pos_1]...[pos_max-1]
 *   where each position is: [head_0][head_1]...[head_H-1]
 *   where each head is: [d_0][d_1]...[d_D-1] (as float16)
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_kvcache.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Float16 <-> Float32 Conversion (Portable Bit Manipulation)
 * ========================================================================= */

/*
 * IEEE 754 float32 layout (32 bits):
 *   [1 sign][8 exponent][23 mantissa]
 *   exponent bias = 127
 *
 * IEEE 754 float16 layout (16 bits):
 *   [1 sign][5 exponent][10 mantissa]
 *   exponent bias = 15
 */

uint16_t r8e_f32_to_f16(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));

    uint32_t sign     = (bits >> 31) & 0x1;
    uint32_t exponent = (bits >> 23) & 0xFF;
    uint32_t mantissa = bits & 0x7FFFFF;

    uint16_t h_sign = (uint16_t)(sign << 15);

    if (exponent == 0xFF) {
        /* Inf or NaN */
        if (mantissa == 0) {
            /* Infinity */
            return h_sign | 0x7C00;
        } else {
            /* NaN: preserve some mantissa bits */
            return h_sign | 0x7C00 | (uint16_t)(mantissa >> 13);
        }
    }

    /* Re-bias exponent from float32 bias (127) to float16 bias (15) */
    int32_t new_exp = (int32_t)exponent - 127 + 15;

    if (new_exp >= 31) {
        /* Overflow -> infinity */
        return h_sign | 0x7C00;
    }

    if (new_exp <= 0) {
        /* Denormalized or zero in float16 */
        if (new_exp < -10) {
            /* Too small, round to zero */
            return h_sign;
        }

        /* Denormalized: shift mantissa with implicit leading 1 */
        uint32_t full_mantissa = mantissa | 0x800000; /* add implicit 1 */
        int shift = 14 - new_exp; /* shift amount: 1 - new_exp + 13 */
        uint16_t h_mantissa = (uint16_t)(full_mantissa >> shift);

        /* Round to nearest even */
        uint32_t round_bit = full_mantissa >> (shift - 1);
        if ((round_bit & 1) && ((h_mantissa & 1) || (full_mantissa & ((1U << (shift - 1)) - 1)))) {
            h_mantissa++;
        }

        return h_sign | h_mantissa;
    }

    /* Normal case */
    uint16_t h_exp      = (uint16_t)(new_exp << 10);
    uint16_t h_mantissa = (uint16_t)(mantissa >> 13);

    /* Round to nearest even */
    uint32_t round_bit = (mantissa >> 12) & 1;
    uint32_t sticky    = mantissa & 0xFFF;
    if (round_bit && (sticky || (h_mantissa & 1))) {
        h_mantissa++;
        if (h_mantissa >= 0x400) {
            /* Mantissa overflow: increment exponent */
            h_mantissa = 0;
            h_exp += 0x0400;
            if (h_exp >= 0x7C00) {
                /* Exponent overflow -> infinity */
                return h_sign | 0x7C00;
            }
        }
    }

    return h_sign | h_exp | h_mantissa;
}

float r8e_f16_to_f32(uint16_t h) {
    uint32_t sign     = (uint32_t)(h >> 15) & 0x1;
    uint32_t exponent = (uint32_t)(h >> 10) & 0x1F;
    uint32_t mantissa = (uint32_t)(h & 0x3FF);

    uint32_t f_sign = sign << 31;

    if (exponent == 0x1F) {
        /* Inf or NaN */
        if (mantissa == 0) {
            /* Infinity */
            uint32_t bits = f_sign | 0x7F800000;
            float result;
            memcpy(&result, &bits, sizeof(result));
            return result;
        } else {
            /* NaN */
            uint32_t bits = f_sign | 0x7FC00000 | (mantissa << 13);
            float result;
            memcpy(&result, &bits, sizeof(result));
            return result;
        }
    }

    if (exponent == 0) {
        if (mantissa == 0) {
            /* Signed zero */
            float result;
            uint32_t bits = f_sign;
            memcpy(&result, &bits, sizeof(result));
            return result;
        }

        /* Denormalized float16 -> normalized float32 */
        /* Find the leading 1 bit in mantissa */
        uint32_t m = mantissa;
        int shift = 0;
        while ((m & 0x400) == 0) {
            m <<= 1;
            shift++;
        }
        /* Remove the leading 1 */
        m &= 0x3FF;

        uint32_t f_exp = (uint32_t)(127 - 15 + 1 - shift) << 23;
        uint32_t f_man = m << 13;
        uint32_t bits = f_sign | f_exp | f_man;
        float result;
        memcpy(&result, &bits, sizeof(result));
        return result;
    }

    /* Normal case: re-bias exponent from 15 to 127 */
    uint32_t f_exp = (uint32_t)(exponent - 15 + 127) << 23;
    uint32_t f_man = mantissa << 13;
    uint32_t bits = f_sign | f_exp | f_man;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/**
 * Compute the number of float16 elements per layer:
 *   max_seq * n_kv_heads * d_head
 */
static inline size_t layer_size(const R8EKVCache *cache) {
    return (size_t)cache->max_seq * (size_t)cache->n_kv_heads * (size_t)cache->d_head;
}

/**
 * Compute the number of float16 elements per position:
 *   n_kv_heads * d_head
 */
static inline size_t pos_size(const R8EKVCache *cache) {
    return (size_t)cache->n_kv_heads * (size_t)cache->d_head;
}

/**
 * Get pointer to the start of a layer's data in a cache buffer.
 */
static inline uint16_t *cache_layer_ptr(uint16_t *buf, const R8EKVCache *cache,
                                         uint32_t layer) {
    return buf + (size_t)layer * layer_size(cache);
}

/**
 * Get pointer to a specific position within a layer.
 */
static inline uint16_t *cache_pos_ptr(uint16_t *buf, const R8EKVCache *cache,
                                       uint32_t layer, uint32_t pos) {
    return cache_layer_ptr(buf, cache, layer) + (size_t)pos * pos_size(cache);
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

R8EKVCache *r8e_kv_cache_new(uint32_t n_layers, uint32_t max_seq,
                              uint32_t n_kv_heads, uint32_t d_head) {
    if (n_layers == 0 || max_seq == 0 || n_kv_heads == 0 || d_head == 0) {
        return NULL;
    }

    R8EKVCache *cache = (R8EKVCache *)calloc(1, sizeof(R8EKVCache));
    if (!cache) return NULL;

    cache->n_layers  = n_layers;
    cache->max_seq   = max_seq;
    cache->n_kv_heads = n_kv_heads;
    cache->d_head    = d_head;
    cache->seq_len   = 0;

    /* Total elements per cache: n_layers * max_seq * n_kv_heads * d_head */
    size_t total = (size_t)n_layers * (size_t)max_seq *
                   (size_t)n_kv_heads * (size_t)d_head;

    /* Check for overflow */
    if (total / n_layers / max_seq / n_kv_heads != d_head) {
        free(cache);
        return NULL;
    }

    size_t alloc_bytes = total * sizeof(uint16_t);

    cache->k_cache = (uint16_t *)calloc(total, sizeof(uint16_t));
    cache->v_cache = (uint16_t *)calloc(total, sizeof(uint16_t));

    if (!cache->k_cache || !cache->v_cache) {
        free(cache->k_cache);
        free(cache->v_cache);
        free(cache);
        return NULL;
    }

    (void)alloc_bytes;
    return cache;
}

void r8e_kv_cache_free(R8EKVCache *cache) {
    if (!cache) return;
    free(cache->k_cache);
    free(cache->v_cache);
    free(cache);
}

/* =========================================================================
 * Read / Write Operations
 * ========================================================================= */

void r8e_kv_cache_write(R8EKVCache *cache, uint32_t layer, uint32_t pos,
                         const float *k, const float *v) {
    if (!cache || !k || !v) return;
    if (layer >= cache->n_layers) return;
    if (pos >= cache->max_seq) return;

    size_t n = pos_size(cache);
    uint16_t *k_dst = cache_pos_ptr(cache->k_cache, cache, layer, pos);
    uint16_t *v_dst = cache_pos_ptr(cache->v_cache, cache, layer, pos);

    for (size_t i = 0; i < n; i++) {
        k_dst[i] = r8e_f32_to_f16(k[i]);
        v_dst[i] = r8e_f32_to_f16(v[i]);
    }

    /* Update seq_len if we're extending */
    if (pos + 1 > cache->seq_len) {
        cache->seq_len = pos + 1;
    }
}

const uint16_t *r8e_kv_cache_k(const R8EKVCache *cache, uint32_t layer) {
    if (!cache || layer >= cache->n_layers) return NULL;
    return cache->k_cache + (size_t)layer * layer_size(cache);
}

const uint16_t *r8e_kv_cache_v(const R8EKVCache *cache, uint32_t layer) {
    if (!cache || layer >= cache->n_layers) return NULL;
    return cache->v_cache + (size_t)layer * layer_size(cache);
}

/* =========================================================================
 * Cache Management
 * ========================================================================= */

void r8e_kv_cache_shift(R8EKVCache *cache, uint32_t shift_amount) {
    if (!cache || shift_amount == 0) return;

    if (shift_amount >= cache->seq_len) {
        /* Shift out everything */
        r8e_kv_cache_reset(cache);
        return;
    }

    uint32_t remaining = cache->seq_len - shift_amount;
    size_t psize = pos_size(cache);
    size_t shift_bytes = (size_t)shift_amount * psize * sizeof(uint16_t);
    size_t remain_bytes = (size_t)remaining * psize * sizeof(uint16_t);

    for (uint32_t l = 0; l < cache->n_layers; l++) {
        uint16_t *k_base = cache_layer_ptr(cache->k_cache, cache, l);
        uint16_t *v_base = cache_layer_ptr(cache->v_cache, cache, l);

        /* Move remaining data to the front */
        memmove(k_base, k_base + shift_amount * psize, remain_bytes);
        memmove(v_base, v_base + shift_amount * psize, remain_bytes);

        /* Zero out the freed region at the end */
        memset(k_base + remaining * psize, 0,
               (size_t)(cache->max_seq - remaining) * psize * sizeof(uint16_t));
        memset(v_base + remaining * psize, 0,
               (size_t)(cache->max_seq - remaining) * psize * sizeof(uint16_t));
    }

    cache->seq_len = remaining;
    (void)shift_bytes;
}

void r8e_kv_cache_truncate(R8EKVCache *cache, uint32_t pos) {
    if (!cache) return;

    if (pos >= cache->seq_len) return; /* nothing to truncate */

    size_t psize = pos_size(cache);

    for (uint32_t l = 0; l < cache->n_layers; l++) {
        uint16_t *k_base = cache_layer_ptr(cache->k_cache, cache, l);
        uint16_t *v_base = cache_layer_ptr(cache->v_cache, cache, l);

        /* Zero out from pos to end */
        size_t clear_bytes = (size_t)(cache->seq_len - pos) * psize * sizeof(uint16_t);
        memset(k_base + pos * psize, 0, clear_bytes);
        memset(v_base + pos * psize, 0, clear_bytes);
    }

    cache->seq_len = pos;
}

void r8e_kv_cache_reset(R8EKVCache *cache) {
    if (!cache) return;

    size_t total = (size_t)cache->n_layers * layer_size(cache);
    memset(cache->k_cache, 0, total * sizeof(uint16_t));
    memset(cache->v_cache, 0, total * sizeof(uint16_t));
    cache->seq_len = 0;
}
