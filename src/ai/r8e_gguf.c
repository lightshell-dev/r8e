/*
 * r8e_gguf.c - GGUF File Format Parser and mmap Loader
 *
 * Part of the r8e JavaScript engine AI subsystem.
 * Implements parsing of GGUF v2/v3 model files with zero-copy mmap tensor
 * data access. Reference: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
 *
 * Design:
 *   - Header + metadata + tensor info parsed into heap structs
 *   - Tensor data region mmap'd read-only with MAP_PRIVATE
 *   - Hash map for O(1) tensor name lookup
 *   - Model config extracted from well-known metadata keys
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_gguf.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef R8E_TESTING
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif


/* =========================================================================
 * Section 1: Read Helpers (cursor-based binary reader)
 * ========================================================================= */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    int            error;    /* set on any read past end of buffer */
} R8EGGUFReader;

static int reader_has(const R8EGGUFReader *r, size_t n) {
    return !r->error && r->pos + n <= r->len;
}

static uint8_t read_u8(R8EGGUFReader *r) {
    if (!reader_has(r, 1)) { r->error = 1; return 0; }
    return r->data[r->pos++];
}

static uint16_t read_u16(R8EGGUFReader *r) {
    if (!reader_has(r, 2)) { r->error = 1; return 0; }
    uint16_t v;
    memcpy(&v, r->data + r->pos, 2);
    r->pos += 2;
    return v;
}

static uint32_t read_u32(R8EGGUFReader *r) {
    if (!reader_has(r, 4)) { r->error = 1; return 0; }
    uint32_t v;
    memcpy(&v, r->data + r->pos, 4);
    r->pos += 4;
    return v;
}

static uint64_t read_u64(R8EGGUFReader *r) {
    if (!reader_has(r, 8)) { r->error = 1; return 0; }
    uint64_t v;
    memcpy(&v, r->data + r->pos, 8);
    r->pos += 8;
    return v;
}

static int8_t read_i8(R8EGGUFReader *r) {
    return (int8_t)read_u8(r);
}

static int16_t read_i16(R8EGGUFReader *r) {
    return (int16_t)read_u16(r);
}

static int32_t read_i32(R8EGGUFReader *r) {
    return (int32_t)read_u32(r);
}

static int64_t read_i64(R8EGGUFReader *r) {
    return (int64_t)read_u64(r);
}

static float read_f32(R8EGGUFReader *r) {
    if (!reader_has(r, 4)) return 0.0f;
    float v;
    memcpy(&v, r->data + r->pos, 4);
    r->pos += 4;
    return v;
}

static double read_f64(R8EGGUFReader *r) {
    if (!reader_has(r, 8)) return 0.0;
    double v;
    memcpy(&v, r->data + r->pos, 8);
    r->pos += 8;
    return v;
}

/**
 * Read a GGUF string: uint64 length followed by that many bytes (no NUL).
 * Returns a heap-allocated NUL-terminated copy. Sets *out_len if non-NULL.
 */
static char *read_gguf_string(R8EGGUFReader *r, uint64_t *out_len) {
    uint64_t len = read_u64(r);
    if (r->error) return NULL;
    if (!reader_has(r, (size_t)len)) { r->error = 1; return NULL; }
    char *s = (char *)malloc((size_t)len + 1);
    if (!s) return NULL;
    memcpy(s, r->data + r->pos, (size_t)len);
    s[len] = '\0';
    r->pos += (size_t)len;
    if (out_len) *out_len = len;
    return s;
}


/* =========================================================================
 * Section 2: Quantization Type Sizes
 * ========================================================================= */

/**
 * Return the size in bytes of a single element for the given type.
 * For block-quantized types, returns the block size.
 * The nbytes calculation uses: ceil(n_elements / block_size) * type_size.
 */
typedef struct {
    size_t type_size;     /* bytes per block */
    size_t block_size;    /* elements per block */
} R8EGGMLTypeInfo;

static R8EGGMLTypeInfo ggml_type_info(R8EGGMLType type) {
    switch (type) {
    case R8E_GGML_TYPE_F32:   return (R8EGGMLTypeInfo){ 4, 1 };
    case R8E_GGML_TYPE_F16:   return (R8EGGMLTypeInfo){ 2, 1 };
    case R8E_GGML_TYPE_Q4_0:  return (R8EGGMLTypeInfo){ 18, 32 };  /* 2 + 16 bytes per 32 elements */
    case R8E_GGML_TYPE_Q4_1:  return (R8EGGMLTypeInfo){ 20, 32 };
    case R8E_GGML_TYPE_Q5_0:  return (R8EGGMLTypeInfo){ 22, 32 };
    case R8E_GGML_TYPE_Q5_1:  return (R8EGGMLTypeInfo){ 24, 32 };
    case R8E_GGML_TYPE_Q8_0:  return (R8EGGMLTypeInfo){ 34, 32 };  /* 2 + 32 bytes per 32 elements */
    case R8E_GGML_TYPE_Q8_1:  return (R8EGGMLTypeInfo){ 36, 32 };
    case R8E_GGML_TYPE_Q2_K:  return (R8EGGMLTypeInfo){ 84, 256 };
    case R8E_GGML_TYPE_Q3_K:  return (R8EGGMLTypeInfo){ 110, 256 };
    case R8E_GGML_TYPE_Q4_K:  return (R8EGGMLTypeInfo){ 144, 256 };  /* Q4_K_M is same */
    case R8E_GGML_TYPE_Q5_K:  return (R8EGGMLTypeInfo){ 176, 256 };  /* Q5_K_M is same */
    case R8E_GGML_TYPE_Q6_K:  return (R8EGGMLTypeInfo){ 210, 256 };
    default:                  return (R8EGGMLTypeInfo){ 0, 0 };
    }
}

static uint64_t compute_tensor_nbytes(R8EGGMLType type, const uint64_t *dims,
                                       uint32_t n_dims) {
    if (n_dims == 0) return 0;
    uint64_t n_elements = 1;
    for (uint32_t i = 0; i < n_dims; i++) {
        n_elements *= dims[i];
    }
    R8EGGMLTypeInfo info = ggml_type_info(type);
    if (info.block_size == 0) return 0;
    uint64_t n_blocks = (n_elements + info.block_size - 1) / info.block_size;
    return n_blocks * info.type_size;
}


/* =========================================================================
 * Section 3: Tensor Hash Map
 * ========================================================================= */

static uint32_t gguf_hash_string(const char *s) {
    uint32_t h = 5381;
    while (*s) {
        h = ((h << 5) + h) ^ (uint8_t)*s++;
    }
    return h;
}

static int tensor_map_init(R8EGGUFTensorMap *map, uint32_t capacity) {
    if (capacity < 16) capacity = 16;
    /* Round up to power of 2 */
    uint32_t cap = 16;
    while (cap < capacity) cap <<= 1;
    map->buckets = (R8EGGUFTensorMapEntry **)calloc(cap, sizeof(R8EGGUFTensorMapEntry *));
    if (!map->buckets) return -1;
    map->capacity = cap;
    map->count = 0;
    return 0;
}

static void tensor_map_free(R8EGGUFTensorMap *map) {
    if (!map->buckets) return;
    for (uint32_t i = 0; i < map->capacity; i++) {
        R8EGGUFTensorMapEntry *e = map->buckets[i];
        while (e) {
            R8EGGUFTensorMapEntry *next = e->next;
            /* name is shared with tensor info, don't free here */
            free(e);
            e = next;
        }
    }
    free(map->buckets);
    map->buckets = NULL;
    map->capacity = 0;
    map->count = 0;
}

static int tensor_map_insert(R8EGGUFTensorMap *map, R8EGGUFTensorInfo *info,
                              uint64_t data_offset) {
    uint32_t hash = gguf_hash_string(info->name);
    uint32_t idx = hash & (map->capacity - 1);
    R8EGGUFTensorMapEntry *e = (R8EGGUFTensorMapEntry *)malloc(sizeof(*e));
    if (!e) return -1;
    e->name = info->name;
    e->hash = hash;
    e->data_offset = data_offset;
    e->info = info;
    e->next = map->buckets[idx];
    map->buckets[idx] = e;
    map->count++;
    return 0;
}

static R8EGGUFTensorMapEntry *tensor_map_find(const R8EGGUFTensorMap *map,
                                                const char *name) {
    if (!map->buckets) return NULL;
    uint32_t hash = gguf_hash_string(name);
    uint32_t idx = hash & (map->capacity - 1);
    R8EGGUFTensorMapEntry *e = map->buckets[idx];
    while (e) {
        if (e->hash == hash && strcmp(e->name, name) == 0) return e;
        e = e->next;
    }
    return NULL;
}


/* =========================================================================
 * Section 4: Metadata KV Parsing
 * ========================================================================= */

static void free_meta_kv(R8EGGUFMetaKV *kv) {
    free(kv->key);
    if (kv->type == R8E_GGUF_TYPE_STRING) {
        free(kv->value.str.data);
    } else if (kv->type == R8E_GGUF_TYPE_ARRAY) {
        /* For string arrays, each element is a heap string */
        if (kv->value.arr.elem_type == R8E_GGUF_TYPE_STRING && kv->value.arr.data) {
            char **strs = (char **)kv->value.arr.data;
            for (uint64_t i = 0; i < kv->value.arr.count; i++) {
                free(strs[i]);
            }
        }
        free(kv->value.arr.data);
    }
}

/**
 * Parse a single metadata KV pair. Returns 0 on success, -1 on error.
 */
static int parse_meta_kv(R8EGGUFReader *r, R8EGGUFMetaKV *kv) {
    memset(kv, 0, sizeof(*kv));

    kv->key = read_gguf_string(r, NULL);
    if (!kv->key || r->error) { free(kv->key); return -1; }

    uint32_t type_raw = read_u32(r);
    if (r->error) { free(kv->key); return -1; }
    kv->type = (R8EGGUFValueType)type_raw;

    switch (kv->type) {
    case R8E_GGUF_TYPE_UINT8:   kv->value.u8  = read_u8(r);  break;
    case R8E_GGUF_TYPE_INT8:    kv->value.i8  = read_i8(r);  break;
    case R8E_GGUF_TYPE_UINT16:  kv->value.u16 = read_u16(r); break;
    case R8E_GGUF_TYPE_INT16:   kv->value.i16 = read_i16(r); break;
    case R8E_GGUF_TYPE_UINT32:  kv->value.u32 = read_u32(r); break;
    case R8E_GGUF_TYPE_INT32:   kv->value.i32 = read_i32(r); break;
    case R8E_GGUF_TYPE_FLOAT32: kv->value.f32 = read_f32(r); break;
    case R8E_GGUF_TYPE_BOOL:    kv->value.b   = read_u8(r);  break;
    case R8E_GGUF_TYPE_STRING:
        kv->value.str.data = read_gguf_string(r, &kv->value.str.len);
        if (!kv->value.str.data) { free(kv->key); return -1; }
        break;
    case R8E_GGUF_TYPE_UINT64:  kv->value.u64 = read_u64(r); break;
    case R8E_GGUF_TYPE_INT64:   kv->value.i64 = read_i64(r); break;
    case R8E_GGUF_TYPE_FLOAT64: kv->value.f64 = read_f64(r); break;
    case R8E_GGUF_TYPE_ARRAY: {
        uint32_t elem_type = read_u32(r);
        uint64_t count = read_u64(r);
        kv->value.arr.elem_type = (R8EGGUFValueType)elem_type;
        kv->value.arr.count = count;
        kv->value.arr.data = NULL;

        if (count == 0) break;

        /* Parse array elements based on element type */
        switch ((R8EGGUFValueType)elem_type) {
        case R8E_GGUF_TYPE_STRING: {
            char **strs = (char **)calloc((size_t)count, sizeof(char *));
            if (!strs) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) {
                strs[i] = read_gguf_string(r, NULL);
                if (!strs[i]) {
                    for (uint64_t j = 0; j < i; j++) free(strs[j]);
                    free(strs);
                    free(kv->key);
                    return -1;
                }
            }
            kv->value.arr.data = strs;
            break;
        }
        case R8E_GGUF_TYPE_UINT32: {
            uint32_t *vals = (uint32_t *)calloc((size_t)count, sizeof(uint32_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_u32(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_INT32: {
            int32_t *vals = (int32_t *)calloc((size_t)count, sizeof(int32_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_i32(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_FLOAT32: {
            float *vals = (float *)calloc((size_t)count, sizeof(float));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_f32(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_UINT8: {
            uint8_t *vals = (uint8_t *)calloc((size_t)count, sizeof(uint8_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_u8(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_INT8: {
            int8_t *vals = (int8_t *)calloc((size_t)count, sizeof(int8_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_i8(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_UINT16: {
            uint16_t *vals = (uint16_t *)calloc((size_t)count, sizeof(uint16_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_u16(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_INT16: {
            int16_t *vals = (int16_t *)calloc((size_t)count, sizeof(int16_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_i16(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_UINT64: {
            uint64_t *vals = (uint64_t *)calloc((size_t)count, sizeof(uint64_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_u64(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_INT64: {
            int64_t *vals = (int64_t *)calloc((size_t)count, sizeof(int64_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_i64(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_FLOAT64: {
            double *vals = (double *)calloc((size_t)count, sizeof(double));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_f64(r);
            kv->value.arr.data = vals;
            break;
        }
        case R8E_GGUF_TYPE_BOOL: {
            uint8_t *vals = (uint8_t *)calloc((size_t)count, sizeof(uint8_t));
            if (!vals) { free(kv->key); return -1; }
            for (uint64_t i = 0; i < count; i++) vals[i] = read_u8(r);
            kv->value.arr.data = vals;
            break;
        }
        default:
            /* Unknown array element type: skip by reading raw bytes is not safe.
             * Treat as parse error. */
            free(kv->key);
            return -1;
        }
        break;
    }
    default:
        /* Unknown metadata type */
        free(kv->key);
        return -1;
    }

    return 0;
}


/* =========================================================================
 * Section 5: Tensor Info Parsing
 * ========================================================================= */

static int parse_tensor_info(R8EGGUFReader *r, R8EGGUFTensorInfo *ti) {
    memset(ti, 0, sizeof(*ti));

    ti->name = read_gguf_string(r, NULL);
    if (!ti->name) return -1;

    ti->n_dims = read_u32(r);
    if (ti->n_dims > R8E_GGUF_MAX_DIMS) {
        free(ti->name);
        return -1;
    }

    for (uint32_t i = 0; i < ti->n_dims; i++) {
        ti->dims[i] = read_u64(r);
    }

    ti->type = (R8EGGMLType)read_u32(r);
    ti->offset = read_u64(r);
    ti->nbytes = compute_tensor_nbytes(ti->type, ti->dims, ti->n_dims);

    return 0;
}


/* =========================================================================
 * Section 6: Core Parse Routine
 *
 * Shared by r8e_gguf_open (file-based) and r8e_gguf_parse_buffer (testing).
 * Parses header, metadata, tensor info from a contiguous byte buffer.
 * ========================================================================= */

static R8EGGUFFile *parse_gguf_data(const uint8_t *data, size_t len) {
    R8EGGUFReader r = { data, len, 0, 0 };

    /* --- Header --- */
    if (!reader_has(&r, 4 + 4 + 8 + 8)) return NULL;

    uint32_t magic = read_u32(&r);
    if (magic != R8E_GGUF_MAGIC) return NULL;

    uint32_t version = read_u32(&r);
    if (version != R8E_GGUF_VERSION_2 && version != R8E_GGUF_VERSION_3) return NULL;

    uint64_t tensor_count = read_u64(&r);
    uint64_t metadata_kv_count = read_u64(&r);

    /* Sanity: prevent absurdly large counts from causing huge allocations */
    if (tensor_count > 1000000 || metadata_kv_count > 1000000) return NULL;

    /* --- Allocate file handle --- */
    R8EGGUFFile *file = (R8EGGUFFile *)calloc(1, sizeof(R8EGGUFFile));
    if (!file) return NULL;

    file->version = version;
    file->tensor_count = tensor_count;
    file->metadata_kv_count = metadata_kv_count;
    file->fd = -1;

    /* --- Parse metadata --- */
    if (metadata_kv_count > 0) {
        file->metadata = (R8EGGUFMetaKV *)calloc((size_t)metadata_kv_count,
                                                    sizeof(R8EGGUFMetaKV));
        if (!file->metadata) { free(file); return NULL; }

        for (uint64_t i = 0; i < metadata_kv_count; i++) {
            if (parse_meta_kv(&r, &file->metadata[i]) != 0) {
                /* Cleanup already-parsed entries */
                for (uint64_t j = 0; j < i; j++) {
                    free_meta_kv(&file->metadata[j]);
                }
                free(file->metadata);
                free(file);
                return NULL;
            }
        }
    }

    /* --- Parse tensor info --- */
    if (tensor_count > 0) {
        file->tensors = (R8EGGUFTensorInfo *)calloc((size_t)tensor_count,
                                                       sizeof(R8EGGUFTensorInfo));
        if (!file->tensors) {
            for (uint64_t i = 0; i < metadata_kv_count; i++)
                free_meta_kv(&file->metadata[i]);
            free(file->metadata);
            free(file);
            return NULL;
        }

        for (uint64_t i = 0; i < tensor_count; i++) {
            if (parse_tensor_info(&r, &file->tensors[i]) != 0) {
                for (uint64_t j = 0; j < i; j++) free(file->tensors[j].name);
                free(file->tensors);
                for (uint64_t j = 0; j < metadata_kv_count; j++)
                    free_meta_kv(&file->metadata[j]);
                free(file->metadata);
                free(file);
                return NULL;
            }
        }
    }

    /* Tensor data starts at alignment boundary after header+meta+tensor_info.
     * GGUF spec: tensor data is aligned to 32 bytes from start of file. */
    uint64_t header_end = r.pos;
    file->tensor_data_off = (header_end + 31) & ~(uint64_t)31;

    /* --- Build tensor hash map --- */
    if (tensor_count > 0) {
        uint32_t map_cap = (uint32_t)tensor_count * 2;
        if (map_cap < 16) map_cap = 16;
        if (tensor_map_init(&file->tensor_map, map_cap) != 0) {
            r8e_gguf_close(file);
            return NULL;
        }
        for (uint64_t i = 0; i < tensor_count; i++) {
            uint64_t data_off = file->tensor_data_off + file->tensors[i].offset;
            if (tensor_map_insert(&file->tensor_map, &file->tensors[i], data_off) != 0) {
                r8e_gguf_close(file);
                return NULL;
            }
        }
    }

    return file;
}


/* =========================================================================
 * Section 7: Metadata Lookup Helpers
 * ========================================================================= */

static const R8EGGUFMetaKV *find_meta_kv(const R8EGGUFFile *file, const char *key) {
    if (!file || !file->metadata || !key) return NULL;
    for (uint64_t i = 0; i < file->metadata_kv_count; i++) {
        if (file->metadata[i].key && strcmp(file->metadata[i].key, key) == 0) {
            return &file->metadata[i];
        }
    }
    return NULL;
}

/**
 * Find an integer metadata value, trying key with various architecture prefixes.
 * GGUF keys use patterns like "llama.embedding_length", "general.block_count".
 */
static int64_t find_meta_int(const R8EGGUFFile *file, const char *suffix) {
    /* Try exact key first */
    const R8EGGUFMetaKV *kv = find_meta_kv(file, suffix);
    if (kv) return r8e_gguf_get_int(file, suffix);

    /* Try with architecture prefix from general.architecture */
    const char *arch = r8e_gguf_get_string(file, "general.architecture");
    if (arch) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "%s.%s", arch, suffix);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            int64_t v = r8e_gguf_get_int(file, buf);
            if (v != 0) return v;
        }
    }
    return 0;
}

static float find_meta_float(const R8EGGUFFile *file, const char *suffix) {
    const char *arch = r8e_gguf_get_string(file, "general.architecture");
    if (arch) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "%s.%s", arch, suffix);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            float v = r8e_gguf_get_float(file, buf);
            if (v != 0.0f) return v;
        }
    }
    return r8e_gguf_get_float(file, suffix);
}


/* =========================================================================
 * Section 8: Model Config Extraction
 * ========================================================================= */

static void extract_model_config(R8EGGUFFile *file) {
    R8EModelConfig *cfg = &file->config;
    memset(cfg, 0, sizeof(*cfg));

    cfg->n_layers    = (uint32_t)find_meta_int(file, "block_count");
    if (cfg->n_layers == 0)
        cfg->n_layers = (uint32_t)r8e_gguf_get_int(file, "general.block_count");

    cfg->d_model     = (uint32_t)find_meta_int(file, "embedding_length");
    cfg->d_ff        = (uint32_t)find_meta_int(file, "feed_forward_length");
    cfg->n_heads     = (uint32_t)find_meta_int(file, "attention.head_count");
    cfg->n_kv_heads  = (uint32_t)find_meta_int(file, "attention.head_count_kv");
    cfg->max_seq_len = (uint32_t)find_meta_int(file, "context_length");

    cfg->rope_freq_base  = find_meta_float(file, "rope.freq_base");
    cfg->rope_freq_scale = find_meta_float(file, "rope.freq_scale");

    /* Vocab size: length of tokenizer.ggml.tokens array */
    const R8EGGUFMetaKV *tokens_kv = find_meta_kv(file, "tokenizer.ggml.tokens");
    if (tokens_kv && tokens_kv->type == R8E_GGUF_TYPE_ARRAY) {
        cfg->vocab_size = (uint32_t)tokens_kv->value.arr.count;
    }

    /* Norm type: default RMSNorm (0) unless metadata says otherwise */
    cfg->norm_type = 0;

    /* Activation type: default SiLU (0) */
    cfg->act_type = 0;

    /* Parallel FFN: check for Gemma architecture */
    const char *arch = r8e_gguf_get_string(file, "general.architecture");
    if (arch && strcmp(arch, "gemma") == 0) {
        cfg->has_parallel_ffn = 1;
    }

    file->config_valid = 1;
}


/* =========================================================================
 * Section 9: File-based Open (mmap)
 * ========================================================================= */

#ifndef R8E_TESTING

R8EGGUFFile *r8e_gguf_open(const char *path) {
    if (!path) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    if (file_size < 24) {  /* minimum: magic + version + tensor_count + kv_count */
        close(fd);
        return NULL;
    }

    /* mmap the entire file for parsing header/metadata/tensor_info */
    void *mapped = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    /* Parse from the mapped buffer */
    R8EGGUFFile *file = parse_gguf_data((const uint8_t *)mapped, file_size);
    if (!file) {
        munmap(mapped, file_size);
        close(fd);
        return NULL;
    }

    /* Store mmap info on the file handle */
    file->mmap_addr = mapped;
    file->mmap_len = file_size;
    file->fd = fd;

    /* Set tensor data base pointer */
    if (file->tensor_data_off < file_size) {
        file->tensor_data_base = (uint8_t *)mapped + file->tensor_data_off;
    }

    /* Extract model config */
    extract_model_config(file);

    return file;
}

#else /* R8E_TESTING */

/* In testing mode, r8e_gguf_open is not available (no file I/O) */
R8EGGUFFile *r8e_gguf_open(const char *path) {
    (void)path;
    return NULL;
}

#endif /* R8E_TESTING */


/* =========================================================================
 * Section 10: Buffer-based Parse (testing)
 * ========================================================================= */

R8EGGUFFile *r8e_gguf_parse_buffer(const uint8_t *data, size_t len) {
    R8EGGUFFile *file = parse_gguf_data(data, len);
    if (!file) return NULL;

    /* For buffer-based parsing, store the buffer pointer as tensor data base
     * so tensor_data lookups work against the buffer. */
    if (file->tensor_data_off <= len) {
        file->tensor_data_base = (void *)(data + file->tensor_data_off);
    }

    /* Extract model config */
    extract_model_config(file);

    return file;
}


/* =========================================================================
 * Section 11: Close / Cleanup
 * ========================================================================= */

void r8e_gguf_close(R8EGGUFFile *file) {
    if (!file) return;

    /* Free tensor hash map */
    tensor_map_free(&file->tensor_map);

    /* Free tensor info */
    if (file->tensors) {
        for (uint64_t i = 0; i < file->tensor_count; i++) {
            free(file->tensors[i].name);
        }
        free(file->tensors);
    }

    /* Free metadata */
    if (file->metadata) {
        for (uint64_t i = 0; i < file->metadata_kv_count; i++) {
            free_meta_kv(&file->metadata[i]);
        }
        free(file->metadata);
    }

#ifndef R8E_TESTING
    /* Unmap and close fd */
    if (file->mmap_addr) {
        munmap(file->mmap_addr, file->mmap_len);
    }
    if (file->fd >= 0) {
        close(file->fd);
    }
#endif

    free(file);
}


/* =========================================================================
 * Section 12: Public Query API
 * ========================================================================= */

const R8EModelConfig *r8e_gguf_get_config(const R8EGGUFFile *file) {
    if (!file || !file->config_valid) return NULL;
    return &file->config;
}

void *r8e_gguf_tensor_data(const R8EGGUFFile *file, const char *name,
                           R8ETensorInfo *info) {
    if (!file || !name) return NULL;

    R8EGGUFTensorMapEntry *entry = tensor_map_find(&file->tensor_map, name);
    if (!entry) return NULL;

    if (info) {
        info->n_dims = entry->info->n_dims;
        for (uint32_t i = 0; i < entry->info->n_dims; i++) {
            info->dims[i] = entry->info->dims[i];
        }
        info->type = entry->info->type;
        info->nbytes = entry->info->nbytes;
    }

    if (!file->tensor_data_base) return NULL;

    /* For mmap'd files, tensor_data_base points to start of tensor data region.
     * entry->data_offset is absolute offset from file start.
     * The tensor's relative offset within the data region = entry->info->offset. */
    return (uint8_t *)file->tensor_data_base + entry->info->offset;
}

const char *r8e_gguf_get_string(const R8EGGUFFile *file, const char *key) {
    const R8EGGUFMetaKV *kv = find_meta_kv(file, key);
    if (!kv || kv->type != R8E_GGUF_TYPE_STRING) return NULL;
    return kv->value.str.data;
}

int64_t r8e_gguf_get_int(const R8EGGUFFile *file, const char *key) {
    const R8EGGUFMetaKV *kv = find_meta_kv(file, key);
    if (!kv) return 0;

    switch (kv->type) {
    case R8E_GGUF_TYPE_UINT8:   return (int64_t)kv->value.u8;
    case R8E_GGUF_TYPE_INT8:    return (int64_t)kv->value.i8;
    case R8E_GGUF_TYPE_UINT16:  return (int64_t)kv->value.u16;
    case R8E_GGUF_TYPE_INT16:   return (int64_t)kv->value.i16;
    case R8E_GGUF_TYPE_UINT32:  return (int64_t)kv->value.u32;
    case R8E_GGUF_TYPE_INT32:   return (int64_t)kv->value.i32;
    case R8E_GGUF_TYPE_UINT64:  return (int64_t)kv->value.u64;
    case R8E_GGUF_TYPE_INT64:   return kv->value.i64;
    default: return 0;
    }
}

float r8e_gguf_get_float(const R8EGGUFFile *file, const char *key) {
    const R8EGGUFMetaKV *kv = find_meta_kv(file, key);
    if (!kv) return 0.0f;

    switch (kv->type) {
    case R8E_GGUF_TYPE_FLOAT32: return kv->value.f32;
    case R8E_GGUF_TYPE_FLOAT64: return (float)kv->value.f64;
    default: return 0.0f;
    }
}
