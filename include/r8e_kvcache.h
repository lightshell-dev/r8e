/*
 * r8e_kvcache.h - Key-Value Cache for Transformer Attention
 *
 * Part of the r8e JavaScript engine.
 * Provides a KV cache for storing key and value vectors across transformer
 * layers during autoregressive inference. Vectors are stored in float16
 * format for memory efficiency.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_KVCACHE_H
#define R8E_KVCACHE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * KV Cache Structure
 * ========================================================================= */

typedef struct {
    uint16_t *k_cache;    /* [n_layers x max_seq x n_kv_heads x d_head] as float16 */
    uint16_t *v_cache;    /* same dimensions */
    uint32_t  seq_len;    /* current sequence length */
    uint32_t  max_seq;    /* maximum context window */
    uint32_t  n_layers;
    uint32_t  n_kv_heads;
    uint32_t  d_head;
} R8EKVCache;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * Allocate a new KV cache.
 *
 * @param n_layers   Number of transformer layers.
 * @param max_seq    Maximum sequence length (context window).
 * @param n_kv_heads Number of key-value heads per layer.
 * @param d_head     Dimension of each head.
 * @return           Newly allocated cache, or NULL on OOM.
 *                   Must be freed with r8e_kv_cache_free().
 */
R8EKVCache *r8e_kv_cache_new(uint32_t n_layers, uint32_t max_seq,
                              uint32_t n_kv_heads, uint32_t d_head);

/**
 * Free a KV cache and all associated memory.
 *
 * @param cache  Cache to free (NULL is safe, does nothing).
 */
void r8e_kv_cache_free(R8EKVCache *cache);

/* =========================================================================
 * Read / Write Operations
 * ========================================================================= */

/**
 * Write K and V vectors for one position in one layer.
 *
 * Converts from float32 to float16 and stores into the cache.
 * The k and v arrays must each have (n_kv_heads * d_head) elements.
 *
 * @param cache  KV cache instance.
 * @param layer  Layer index (0-based).
 * @param pos    Sequence position to write at.
 * @param k      Key vector in float32 [n_kv_heads * d_head].
 * @param v      Value vector in float32 [n_kv_heads * d_head].
 */
void r8e_kv_cache_write(R8EKVCache *cache, uint32_t layer, uint32_t pos,
                         const float *k, const float *v);

/**
 * Get pointer to the K cache for a given layer.
 *
 * Returns a pointer into the cache's float16 storage. The returned data
 * spans [max_seq x n_kv_heads x d_head] elements.
 *
 * @param cache  KV cache instance.
 * @param layer  Layer index (0-based).
 * @return       Pointer to float16 K data for the layer.
 */
const uint16_t *r8e_kv_cache_k(const R8EKVCache *cache, uint32_t layer);

/**
 * Get pointer to the V cache for a given layer.
 *
 * @param cache  KV cache instance.
 * @param layer  Layer index (0-based).
 * @return       Pointer to float16 V data for the layer.
 */
const uint16_t *r8e_kv_cache_v(const R8EKVCache *cache, uint32_t layer);

/* =========================================================================
 * Cache Management
 * ========================================================================= */

/**
 * Sliding window shift: discard the oldest shift_amount positions and
 * move remaining entries to the start of the cache.
 *
 * @param cache         KV cache instance.
 * @param shift_amount  Number of positions to shift out.
 */
void r8e_kv_cache_shift(R8EKVCache *cache, uint32_t shift_amount);

/**
 * Truncate: discard all entries from position pos onwards.
 *
 * @param cache  KV cache instance.
 * @param pos    First position to discard (new seq_len becomes pos).
 */
void r8e_kv_cache_truncate(R8EKVCache *cache, uint32_t pos);

/**
 * Reset the cache to empty (seq_len = 0). Does not free memory.
 *
 * @param cache  KV cache instance.
 */
void r8e_kv_cache_reset(R8EKVCache *cache);

/* =========================================================================
 * Float16 Conversion Helpers
 * ========================================================================= */

/**
 * Convert a float32 value to float16 (IEEE 754 half-precision).
 *
 * Uses portable bit manipulation (no hardware intrinsics).
 * Handles denormals, infinity, and NaN.
 *
 * @param f  Float32 value.
 * @return   Float16 value as uint16_t.
 */
uint16_t r8e_f32_to_f16(float f);

/**
 * Convert a float16 value to float32.
 *
 * @param h  Float16 value as uint16_t.
 * @return   Float32 value.
 */
float r8e_f16_to_f32(uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* R8E_KVCACHE_H */
