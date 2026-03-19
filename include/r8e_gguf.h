/*
 * r8e_gguf.h - GGUF File Format Parser and mmap Loader
 *
 * Part of the r8e JavaScript engine AI subsystem.
 * Parses GGUF v2/v3 model files (https://github.com/ggerganov/ggml/blob/master/docs/gguf.md)
 * with zero-copy mmap tensor data access.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_GGUF_H
#define R8E_GGUF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Section 1: GGUF Constants
 * ========================================================================= */

#define R8E_GGUF_MAGIC          0x46475547U  /* "GGUF" in little-endian */
#define R8E_GGUF_VERSION_2      2
#define R8E_GGUF_VERSION_3      3
#define R8E_GGUF_MAX_DIMS       4
#define R8E_GGUF_TENSOR_MAP_CAP 4096

/* =========================================================================
 * Section 2: GGUF Metadata Value Types
 * ========================================================================= */

typedef enum {
    R8E_GGUF_TYPE_UINT8   = 0,
    R8E_GGUF_TYPE_INT8    = 1,
    R8E_GGUF_TYPE_UINT16  = 2,
    R8E_GGUF_TYPE_INT16   = 3,
    R8E_GGUF_TYPE_UINT32  = 4,
    R8E_GGUF_TYPE_INT32   = 5,
    R8E_GGUF_TYPE_FLOAT32 = 6,
    R8E_GGUF_TYPE_BOOL    = 7,
    R8E_GGUF_TYPE_STRING  = 8,
    R8E_GGUF_TYPE_ARRAY   = 9,
    R8E_GGUF_TYPE_UINT64  = 10,
    R8E_GGUF_TYPE_INT64   = 11,
    R8E_GGUF_TYPE_FLOAT64 = 12,
} R8EGGUFValueType;

/* =========================================================================
 * Section 3: Quantization Types
 * ========================================================================= */

typedef enum {
    R8E_GGML_TYPE_F32    = 0,
    R8E_GGML_TYPE_F16    = 1,
    R8E_GGML_TYPE_Q4_0   = 2,
    R8E_GGML_TYPE_Q4_1   = 3,
    R8E_GGML_TYPE_Q5_0   = 6,
    R8E_GGML_TYPE_Q5_1   = 7,
    R8E_GGML_TYPE_Q8_0   = 8,
    R8E_GGML_TYPE_Q8_1   = 9,
    R8E_GGML_TYPE_Q2_K   = 10,
    R8E_GGML_TYPE_Q3_K   = 11,
    R8E_GGML_TYPE_Q4_K   = 12,
    R8E_GGML_TYPE_Q5_K   = 13,
    R8E_GGML_TYPE_Q6_K   = 14,
    R8E_GGML_TYPE_Q4_K_M = 12,  /* alias: Q4_K medium = Q4_K */
    R8E_GGML_TYPE_Q5_K_M = 13,  /* alias: Q5_K medium = Q5_K */
} R8EGGMLType;

/* =========================================================================
 * Section 4: Metadata KV Entry
 * ========================================================================= */

typedef struct {
    char            *key;        /* heap-allocated key string */
    R8EGGUFValueType type;       /* value type */
    union {
        uint8_t      u8;
        int8_t       i8;
        uint16_t     u16;
        int16_t      i16;
        uint32_t     u32;
        int32_t      i32;
        float        f32;
        uint8_t      b;          /* bool */
        struct {
            char    *data;
            uint64_t len;
        } str;
        struct {
            R8EGGUFValueType elem_type;
            uint64_t         count;
            void            *data;    /* raw array data (parsed elements) */
        } arr;
        uint64_t     u64;
        int64_t      i64;
        double       f64;
    } value;
} R8EGGUFMetaKV;

/* =========================================================================
 * Section 5: Tensor Info
 * ========================================================================= */

typedef struct {
    char        *name;
    uint32_t     n_dims;
    uint64_t     dims[R8E_GGUF_MAX_DIMS];
    R8EGGMLType  type;
    uint64_t     offset;         /* offset from start of tensor data region */
    uint64_t     nbytes;         /* computed byte size */
} R8EGGUFTensorInfo;

/* =========================================================================
 * Section 6: Model Config (extracted from metadata)
 * ========================================================================= */

typedef struct {
    uint32_t n_layers;           /* general.block_count */
    uint32_t d_model;            /* *.embedding_length */
    uint32_t d_ff;               /* *.feed_forward_length */
    uint32_t n_heads;            /* *.attention.head_count */
    uint32_t n_kv_heads;         /* *.attention.head_count_kv */
    uint32_t vocab_size;         /* tokenizer.ggml.tokens length */
    uint32_t max_seq_len;        /* *.context_length */
    float    rope_freq_base;     /* *.rope.freq_base */
    float    rope_freq_scale;    /* *.rope.freq_scale */
    uint8_t  norm_type;          /* 0=RMSNorm, 1=LayerNorm */
    uint8_t  act_type;           /* 0=SiLU, 1=GELU */
    uint8_t  has_parallel_ffn;   /* Gemma-style */
} R8EModelConfig;

/* =========================================================================
 * Section 7: Tensor Lookup Result
 * ========================================================================= */

typedef struct {
    uint32_t    n_dims;
    uint64_t    dims[R8E_GGUF_MAX_DIMS];
    R8EGGMLType type;
    uint64_t    nbytes;
} R8ETensorInfo;

/* =========================================================================
 * Section 8: Tensor Hash Map Entry (name -> tensor info + data pointer)
 * ========================================================================= */

typedef struct R8EGGUFTensorMapEntry {
    char                        *name;
    uint32_t                     hash;
    uint64_t                     data_offset;  /* offset into mmap */
    R8EGGUFTensorInfo           *info;
    struct R8EGGUFTensorMapEntry *next;
} R8EGGUFTensorMapEntry;

typedef struct {
    R8EGGUFTensorMapEntry **buckets;
    uint32_t                capacity;
    uint32_t                count;
} R8EGGUFTensorMap;

/* =========================================================================
 * Section 9: Top-Level GGUF File Handle
 * ========================================================================= */

typedef struct {
    /* Header */
    uint32_t            version;
    uint64_t            tensor_count;
    uint64_t            metadata_kv_count;

    /* Parsed metadata */
    R8EGGUFMetaKV      *metadata;

    /* Parsed tensor descriptors */
    R8EGGUFTensorInfo   *tensors;

    /* mmap state */
    void               *mmap_addr;        /* base of mmap region */
    size_t              mmap_len;          /* total mmap length */
    void               *tensor_data_base; /* pointer to start of tensor data */
    uint64_t            tensor_data_off;   /* file offset of tensor data start */

    /* File descriptor */
    int                 fd;

    /* Tensor lookup hash map */
    R8EGGUFTensorMap    tensor_map;

    /* Extracted model config */
    R8EModelConfig      config;
    int                 config_valid;
} R8EGGUFFile;

/* =========================================================================
 * Section 10: Public API
 * ========================================================================= */

/**
 * Open and parse a GGUF file. mmap's tensor data for zero-copy access.
 *
 * @param path  Path to GGUF file.
 * @return      Parsed file handle, or NULL on error.
 */
R8EGGUFFile *r8e_gguf_open(const char *path);

/**
 * Close a GGUF file handle and release all resources.
 *
 * @param file  File handle (NULL is safe).
 */
void r8e_gguf_close(R8EGGUFFile *file);

/**
 * Get the extracted model configuration.
 *
 * @param file  File handle.
 * @return      Pointer to config, or NULL if extraction failed.
 */
const R8EModelConfig *r8e_gguf_get_config(const R8EGGUFFile *file);

/**
 * Get a pointer to tensor data by name. Fills info with shape/type metadata.
 *
 * @param file  File handle.
 * @param name  Tensor name (e.g. "blk.0.attn_q.weight").
 * @param info  Output: tensor metadata (may be NULL if not needed).
 * @return      Pointer to mmap'd tensor data, or NULL if not found.
 */
void *r8e_gguf_tensor_data(const R8EGGUFFile *file, const char *name,
                           R8ETensorInfo *info);

/**
 * Look up a string metadata value by key.
 *
 * @param file  File handle.
 * @param key   Metadata key.
 * @return      String value, or NULL if not found or not a string.
 */
const char *r8e_gguf_get_string(const R8EGGUFFile *file, const char *key);

/**
 * Look up an integer metadata value by key.
 * Works for UINT8, INT8, UINT16, INT16, UINT32, INT32, UINT64, INT64.
 *
 * @param file  File handle.
 * @param key   Metadata key.
 * @return      Integer value, or 0 if not found.
 */
int64_t r8e_gguf_get_int(const R8EGGUFFile *file, const char *key);

/**
 * Look up a float metadata value by key.
 * Works for FLOAT32 and FLOAT64.
 *
 * @param file  File handle.
 * @param key   Metadata key.
 * @return      Float value, or 0.0f if not found.
 */
float r8e_gguf_get_float(const R8EGGUFFile *file, const char *key);

/* =========================================================================
 * Section 11: Internal Helpers (exposed for testing)
 * ========================================================================= */

#ifdef R8E_TESTING

/**
 * Parse a GGUF file from an in-memory buffer (no mmap, no file I/O).
 * Used by unit tests to construct synthetic GGUF data.
 *
 * @param data  Pointer to GGUF data.
 * @param len   Length of data in bytes.
 * @return      Parsed file handle, or NULL on error.
 */
R8EGGUFFile *r8e_gguf_parse_buffer(const uint8_t *data, size_t len);

#endif /* R8E_TESTING */

#ifdef __cplusplus
}
#endif

#endif /* R8E_GGUF_H */
