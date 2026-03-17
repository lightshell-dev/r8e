/*
 * test_security.c - Unit tests for r8e security layers
 *
 * Tests cover:
 *   - Layer 2: Arena allocation with guard pages (r8e_arena.c)
 *   - Layer 2: NaN-box validation at trust boundaries (r8e_arena.c)
 *   - Layer 2: Stack canary check/corrupt detection (r8e_arena.c)
 *   - Layer 3: Bytecode verifier rejects malformed opcodes (r8e_verify.c)
 *   - Layer 3: Bytecode verifier rejects stack underflow (r8e_verify.c)
 *   - Layer 4: Realm creation and destruction (r8e_realm.c)
 *   - Layer 4: Realm resource limits (r8e_realm.c)
 *   - Layer 4: Cross-realm pointer detection (r8e_realm.c)
 *   - Layer 4: Structured clone (r8e_realm.c)
 *   - Layer 5: Capability creation and attenuation (r8e_capability.c)
 *   - Layer 5: Capability revocation (r8e_capability.c)
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Type definitions (must match r8e source layout)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* NaN-boxing type checks */
#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_BOOL(v)       ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)     (((v) >> 48) == 0xFFFBU)
#define R8E_IS_ATOM(v)       (((v) >> 48) == 0xFFFCU)

/* NaN-boxing extraction / encoding */
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}
static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}
static inline R8EValue r8e_from_double(double d) {
    R8EValue v;
    memcpy(&v, &d, sizeof(double));
    return v;
}
static inline R8EValue r8e_from_inline_str(const char *s, int len) {
    if (len < 0 || len > 7) return R8E_UNDEFINED;
    uint64_t v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch > 127) return R8E_UNDEFINED;
        v |= ((uint64_t)ch << (38 - i * 7));
    }
    return v;
}

/* R8EStatus */
typedef enum {
    R8E_OK              =  0,
    R8E_ERROR           = -1,
    R8E_ERROR_LIMIT     = -11,
    R8E_ERROR_VERIFY    = -12
} R8EStatus;

/* GC constants */
#define R8E_GC_TIER_0          0x00000000U
#define R8E_GC_TIER_1          0x00000001U
#define R8E_GC_FROZEN_BIT      0x00000010U
#define R8E_OBJ_IS_ARRAY       0x00000020U
#define R8E_OBJ_IS_FUNCTION    0x00000040U
#define R8E_OBJ_IS_STRING      0x00000060U

/* Prototype IDs */
#define R8E_PROTO_OBJECT       1
#define R8E_PROTO_ARRAY        2
#define R8E_PROTO_COUNT        40

/* Default limits */
#define R8E_DEFAULT_MAX_MEMORY       (16 * 1024 * 1024)
#define R8E_DEFAULT_MAX_CPU_TIME_US  (5000000ULL)
#define R8E_DEFAULT_MAX_STACK_DEPTH  512
#define R8E_DEFAULT_MAX_STRING_LEN   (1024 * 1024)
#define R8E_DEFAULT_MAX_ARRAY_LEN    (1024 * 1024)
#define R8E_DEFAULT_MAX_REGEXP_STEPS 1000000

/* =========================================================================
 * Minimal structure definitions
 * ========================================================================= */

/* GC header helpers */
static inline uint32_t r8e_gc_set_rc(uint32_t flags, uint16_t rc) {
    return (flags & 0x0000FFFFU) | ((uint32_t)rc << 16);
}

/* Object header */
typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* Key-value pair */
typedef struct R8EPropPair {
    uint64_t key;
    uint64_t val;
} R8EPropPair;

/* Tier 0: 0-1 properties, 24 bytes */
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint64_t key0;
    uint64_t val0;
} R8EObjTier0;

/* Tier 1: 2-4 properties */
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  count;
    uint8_t  pad[7];
    R8EPropPair props[4];
} R8EObjTier1;

/* Tier 2 */
typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint8_t     count;
    uint8_t     capacity;
    uint8_t     pad[6];
    R8EPropPair *props;
} R8EObjTier2;

/* Tier 3 */
typedef struct {
    uint32_t    flags;
    uint32_t    proto_id;
    uint16_t    count;
    uint16_t    capacity;
    uint32_t    pad;
    R8EPropPair *buckets;
} R8EObjTier3;

/* Object union */
typedef struct R8EObject {
    union {
        R8EGCHeader header;
        R8EObjTier0 t0;
        R8EObjTier1 t1;
        R8EObjTier2 t2;
        R8EObjTier3 t3;
    } u;
} R8EObject;

/* Array */
typedef struct R8EArray {
    uint32_t   flags;
    uint32_t   proto_id;
    uint32_t   length;
    uint32_t   capacity;
    R8EValue  *elements;
    R8EObject *named;
} R8EArray;

/* String */
typedef struct R8EString {
    uint32_t  flags;
    uint32_t  hash;
    uint32_t  byte_length;
    uint32_t  char_length;
    void     *offset_table;
} R8EString;

/* Realm limits */
typedef struct {
    size_t   max_memory;
    uint64_t max_cpu_time_us;
    uint32_t max_stack_depth;
    uint32_t max_string_length;
    uint32_t max_array_length;
    uint32_t max_regexp_steps;
} R8ERealmLimits;

/* Realm */
typedef struct R8ERealm {
    uint32_t       id;
    uint32_t       flags;
    R8EObject     *global;
    R8EObject    **prototypes;
    uint16_t       proto_count;
    uint16_t       proto_capacity;
    R8ERealmLimits limits;
    void          *arena;
    size_t         arena_size;
    size_t         arena_used;
} R8ERealm;

/* Atom table (minimal) */
typedef struct {
    uint64_t bloom[4];
    uint32_t count;
    uint32_t capacity;
    void    *entries;
} R8EAtomTable;

/* Suspect/deferred lists (minimal) */
typedef struct { void *data; int count; int capacity; } R8ESuspectList;
typedef struct { void *data; int count; int capacity; } R8EDeferredList;
typedef struct {
    int          error_type;
    const char  *error_message;
    uint16_t     error_line;
    uint16_t     error_col;
    const char  *error_file;
} R8EErrorState;

/* R8EFunction */
typedef struct R8EFunction {
    uint32_t     flags;
    uint32_t     proto_id;
    uint8_t     *bytecode;
    uint32_t     bytecode_len;
    uint16_t     param_count;
    uint16_t     local_count;
    uint16_t     capture_count;
    uint16_t     stack_size;
    uint32_t     name_atom;
    R8EValue    *constants;
    uint16_t     const_count;
    uint8_t      func_flags;
    uint8_t      rest_param_idx;
    uint16_t     source_line;
    uint16_t     source_col;
    const char  *source_name;
    uint32_t    *default_offsets;
    uint8_t      is_strict;
    uint8_t      is_generator;
    uint8_t      is_async;
} R8EFunction;

/* Minimal R8EContext */
typedef struct R8EContext {
    R8EAtomTable atom_table;

    R8ERealm   *realms[16];
    uint8_t     realm_count;
    uint8_t     current_realm;

    R8ESuspectList  suspect_list;
    R8EDeferredList deferred_list;
    uint32_t        epoch_threshold;

    R8EErrorState error_state;
    R8EValue      last_error;
    bool          has_exception;

    uint32_t    stack_depth;
    uint32_t    max_stack_depth;
} R8EContext;

/* Opaque types for security modules */
typedef struct R8ESecureArena R8ESecureArena;

/* Verification metadata */
typedef struct {
    uint16_t local_count;
    uint16_t param_count;
    uint16_t const_count;
    uint16_t func_count;
    uint16_t max_stack_size;
    uint8_t  is_strict;
    uint8_t  is_generator;
    uint8_t  is_async;
    uint8_t  capture_count;
} R8EVerifyMeta;

/* Verification result */
typedef struct {
    R8EStatus  status;
    uint32_t   offset;
    char       message[256];
} R8EVerifyResult;

/* =========================================================================
 * Test infrastructure (shared globals from test_runner.c)
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n" \
                "      at %s:%d\n", #e, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n" \
                "      got %lld vs %lld\n" \
                "      at %s:%d\n", #a, #b, _a, _b, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "    ASSERT_EQ_STR failed: %s == %s\n" \
                "      got \"%s\" vs \"%s\"\n" \
                "      at %s:%d\n", #a, #b, \
                _a ? _a : "(null)", _b ? _b : "(null)", \
                __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; \
    g_tests_run++; \
    printf("  %-60s ", #fn); \
    test_##fn(); \
    if (g_assert_fail) { g_tests_failed++; printf("FAIL\n"); } \
    else { g_tests_passed++; printf("ok\n"); } \
} while (0)

#define TEST(name) static void test_##name(void)

/* =========================================================================
 * External function declarations: r8e_arena.c (Layer 2)
 * ========================================================================= */

/* Secure arena */
extern R8ESecureArena *r8e_secure_arena_create(size_t size, uint32_t realm_id);
extern void *r8e_secure_arena_alloc(R8ESecureArena *arena, size_t size);
extern bool  r8e_secure_arena_contains(const R8ESecureArena *arena,
                                        const void *ptr, size_t size);
extern uint32_t r8e_secure_arena_realm(const R8ESecureArena *arena);
extern size_t   r8e_secure_arena_used(const R8ESecureArena *arena);
extern size_t   r8e_secure_arena_available(const R8ESecureArena *arena);
extern void     r8e_secure_arena_destroy(R8ESecureArena *arena);

/* NaN-box validation */
extern bool r8e_validate_value(R8EValue val);
extern bool r8e_validate_pointer_in_arena(R8EValue val,
                                           const R8ESecureArena *arena);
extern bool r8e_validate_values(const R8EValue *values, int count);

/* Stack canary */
extern void r8e_canary_init(void *frame_base);
extern bool r8e_canary_check(void *frame_base);

/* Arena registry */
extern bool r8e_arena_registry_add(R8ESecureArena *arena);
extern void r8e_arena_registry_remove(R8ESecureArena *arena);
extern const R8ESecureArena *r8e_arena_registry_find(const void *ptr,
                                                      size_t size);
extern bool r8e_validate_pointer_global(R8EValue val);

/* =========================================================================
 * External function declarations: r8e_verify.c (Layer 3)
 * ========================================================================= */

extern R8EStatus r8e_verify_bytecode(const uint8_t *bytecode, uint32_t bc_len,
                                      const R8EVerifyMeta *meta,
                                      R8EVerifyResult *result);
extern R8EStatus r8e_verify_function(const R8EFunction *func);
extern R8EStatus r8e_verify_bytecode_detailed(const uint8_t *bytecode,
                                               uint32_t bc_len,
                                               const R8EVerifyMeta *meta,
                                               char *out_msg, size_t msg_len);

/* =========================================================================
 * External function declarations: r8e_realm.c (Layer 4)
 * ========================================================================= */

extern R8ERealm *r8e_realm_create(R8EContext *ctx);
extern R8EStatus r8e_realm_destroy(R8EContext *ctx, int realm_id);
extern int       r8e_realm_switch(R8EContext *ctx, int realm_id);
extern R8EStatus r8e_realm_set_limits(R8EContext *ctx, int realm_id,
                                       const R8ERealmLimits *limits);
extern R8EStatus r8e_realm_check_limits(R8EContext *ctx);
extern R8EValue  r8e_realm_tag_pointer(void *ptr, uint32_t realm_id);
extern uint32_t  r8e_realm_id_from_pointer(R8EValue val);
extern void     *r8e_realm_untag_pointer(R8EValue val);
extern bool      r8e_check_cross_realm(R8EValue val, uint32_t expected_realm);
extern R8EValue  r8e_structured_clone(R8EContext *ctx, R8EValue value);
extern R8ERealm *r8e_realm_get_current(R8EContext *ctx);
extern R8ERealm *r8e_realm_get(R8EContext *ctx, uint32_t realm_id);
extern uint32_t  r8e_realm_count(R8EContext *ctx);
extern int       r8e_realm_new(R8EContext *ctx);

/* =========================================================================
 * External function declarations: r8e_capability.c (Layer 5)
 * ========================================================================= */

extern R8EValue  r8e_capability_create_fs(R8EContext *ctx, const char *root,
                                           uint32_t permissions,
                                           size_t quota_bytes);
extern R8EValue  r8e_capability_create_net(R8EContext *ctx,
                                            uint32_t permissions);
extern R8EValue  r8e_capability_attenuate(R8EValue cap_val,
                                           const char *new_root,
                                           uint32_t new_perms,
                                           size_t new_quota);
extern bool      r8e_capability_check_fs(R8EValue cap_val, const char *path,
                                          uint32_t operation, size_t size);
extern void      r8e_capability_revoke(R8EValue cap_val);
extern bool      r8e_capability_is_revoked(R8EValue cap_val);
extern void      r8e_capability_retain(R8EValue cap_val);
extern void      r8e_capability_release(R8EValue cap_val);
extern int       r8e_capability_type(R8EValue cap_val);
extern uint32_t  r8e_capability_realm(R8EValue cap_val);
extern bool      r8e_capability_check(R8EValue cap_val, uint32_t operation);

/* Capability permission flags (match r8e_capability.c) */
#define R8E_PERM_FS_READ     0x0001U
#define R8E_PERM_FS_WRITE    0x0002U
#define R8E_PERM_FS_CREATE   0x0004U
#define R8E_PERM_FS_DELETE   0x0008U
#define R8E_PERM_FS_LIST     0x0010U
#define R8E_PERM_FS_STAT     0x0020U
#define R8E_PERM_FS_ALL      0x003FU

#define R8E_PERM_NET_CONNECT  0x0001U
#define R8E_PERM_NET_LISTEN   0x0002U
#define R8E_PERM_NET_ALL      0x00F7U

/* Capability type enum values */
#define R8E_CAP_FS   0
#define R8E_CAP_NET  1

/* =========================================================================
 * Helper: create a zeroed context for realm tests
 * ========================================================================= */

static R8EContext *make_test_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    if (!ctx) return NULL;
    ctx->realm_count = 0;
    ctx->current_realm = 0;
    ctx->stack_depth = 0;
    ctx->max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    ctx->epoch_threshold = 4096;
    return ctx;
}

static void free_test_ctx(R8EContext *ctx) {
    if (ctx) free(ctx);
}

/* =========================================================================
 * LAYER 2 TESTS: Arena Allocation with Guard Pages
 * ========================================================================= */

TEST(arena_create_default_size) {
    R8ESecureArena *arena = r8e_secure_arena_create(0, 0);
    ASSERT_NOT_NULL(arena);
    /* Default size is 128KB */
    size_t avail = r8e_secure_arena_available(arena);
    ASSERT_TRUE(avail >= 128 * 1024);
    ASSERT_EQ_INT(r8e_secure_arena_used(arena), 0);
    ASSERT_EQ_INT(r8e_secure_arena_realm(arena), 0);
    r8e_secure_arena_destroy(arena);
}

TEST(arena_create_custom_size) {
    R8ESecureArena *arena = r8e_secure_arena_create(8192, 3);
    ASSERT_NOT_NULL(arena);
    ASSERT_EQ_INT(r8e_secure_arena_realm(arena), 3);
    size_t avail = r8e_secure_arena_available(arena);
    /* Requested 8KB but minimum is 4KB and may be aligned up */
    ASSERT_TRUE(avail >= 4096);
    r8e_secure_arena_destroy(arena);
}

TEST(arena_alloc_basic) {
    R8ESecureArena *arena = r8e_secure_arena_create(0, 0);
    ASSERT_NOT_NULL(arena);

    void *p1 = r8e_secure_arena_alloc(arena, 64);
    ASSERT_NOT_NULL(p1);
    ASSERT_TRUE(r8e_secure_arena_used(arena) >= 64);

    void *p2 = r8e_secure_arena_alloc(arena, 128);
    ASSERT_NOT_NULL(p2);
    ASSERT_TRUE(r8e_secure_arena_used(arena) >= 192);

    /* Allocations should not overlap */
    ASSERT_NE(p1, p2);

    r8e_secure_arena_destroy(arena);
}

TEST(arena_alloc_exhaustion) {
    /* Create a small arena */
    R8ESecureArena *arena = r8e_secure_arena_create(4096, 0);
    ASSERT_NOT_NULL(arena);

    /* Allocate until exhausted */
    void *p = r8e_secure_arena_alloc(arena, 4096);
    /* Might succeed or fail depending on alignment, but the next should fail */
    if (p) {
        void *p2 = r8e_secure_arena_alloc(arena, 4096);
        ASSERT_NULL(p2);
    }

    r8e_secure_arena_destroy(arena);
}

TEST(arena_alloc_zero_size) {
    R8ESecureArena *arena = r8e_secure_arena_create(0, 0);
    ASSERT_NOT_NULL(arena);

    /* Allocating 0 bytes should return NULL */
    void *p = r8e_secure_arena_alloc(arena, 0);
    ASSERT_NULL(p);

    r8e_secure_arena_destroy(arena);
}

TEST(arena_contains) {
    R8ESecureArena *arena = r8e_secure_arena_create(0, 0);
    ASSERT_NOT_NULL(arena);

    void *p = r8e_secure_arena_alloc(arena, 64);
    ASSERT_NOT_NULL(p);

    /* Pointer within arena should be contained */
    ASSERT_TRUE(r8e_secure_arena_contains(arena, p, 64));

    /* Stack pointer should NOT be contained */
    int stack_var = 42;
    ASSERT_FALSE(r8e_secure_arena_contains(arena, &stack_var, sizeof(int)));

    /* NULL should not be contained */
    ASSERT_FALSE(r8e_secure_arena_contains(arena, NULL, 1));

    r8e_secure_arena_destroy(arena);
}

TEST(arena_destroy_null_safe) {
    /* Destroying NULL should not crash */
    r8e_secure_arena_destroy(NULL);
    ASSERT_TRUE(1); /* If we get here, it did not crash */
}

TEST(arena_alloc_null_arena) {
    /* Allocating from NULL should return NULL */
    void *p = r8e_secure_arena_alloc(NULL, 64);
    ASSERT_NULL(p);
}

/* =========================================================================
 * LAYER 2 TESTS: NaN-Box Validation
 * ========================================================================= */

TEST(validate_double) {
    /* Regular double values should be valid */
    R8EValue dval = r8e_from_double(3.14);
    ASSERT_TRUE(r8e_validate_value(dval));

    /* Zero is valid */
    R8EValue zero = r8e_from_double(0.0);
    ASSERT_TRUE(r8e_validate_value(zero));

    /* Negative number */
    R8EValue neg = r8e_from_double(-42.5);
    ASSERT_TRUE(r8e_validate_value(neg));
}

TEST(validate_int32) {
    R8EValue ival = r8e_from_int32(42);
    ASSERT_TRUE(r8e_validate_value(ival));

    R8EValue neg = r8e_from_int32(-1);
    ASSERT_TRUE(r8e_validate_value(neg));

    R8EValue zero = r8e_from_int32(0);
    ASSERT_TRUE(r8e_validate_value(zero));
}

TEST(validate_special_values) {
    ASSERT_TRUE(r8e_validate_value(R8E_UNDEFINED));
    ASSERT_TRUE(r8e_validate_value(R8E_NULL));
    ASSERT_TRUE(r8e_validate_value(R8E_TRUE));
    ASSERT_TRUE(r8e_validate_value(R8E_FALSE));
}

TEST(validate_inline_string) {
    R8EValue hello = r8e_from_inline_str("hello", 5);
    ASSERT_TRUE(r8e_validate_value(hello));

    R8EValue empty = r8e_from_inline_str("", 0);
    ASSERT_TRUE(r8e_validate_value(empty));
}

TEST(validate_pointer_valid) {
    /* A valid heap pointer (aligned, non-zero, >= 0x1000) */
    int *heap = (int *)malloc(sizeof(int));
    ASSERT_NOT_NULL(heap);
    R8EValue pval = r8e_from_pointer(heap);
    ASSERT_TRUE(r8e_validate_value(pval));
    free(heap);
}

TEST(validate_pointer_null_rejected) {
    /* Pointer to address 0 should be rejected */
    R8EValue pval = 0xFFF9000000000000ULL; /* pointer with 0 address */
    ASSERT_FALSE(r8e_validate_value(pval));
}

TEST(validate_pointer_misaligned_rejected) {
    /* Odd address should be rejected (must be at least 4-byte aligned) */
    R8EValue pval = 0xFFF9000000000001ULL; /* address 1 = misaligned */
    ASSERT_FALSE(r8e_validate_value(pval));
}

TEST(validate_invalid_tag_rejected) {
    /* Tags 0xFFFE and 0xFFFF are invalid */
    R8EValue bad1 = 0xFFFE000000000000ULL;
    ASSERT_FALSE(r8e_validate_value(bad1));

    R8EValue bad2 = 0xFFFF000000000000ULL;
    ASSERT_FALSE(r8e_validate_value(bad2));
}

TEST(validate_special_invalid_payload) {
    /* Special tag with payload > 3 should be invalid */
    R8EValue bad = 0xFFFA000000000004ULL;
    ASSERT_FALSE(r8e_validate_value(bad));

    R8EValue bad2 = 0xFFFA000000000FFULL;
    ASSERT_FALSE(r8e_validate_value(bad2));
}

TEST(validate_values_array) {
    R8EValue vals[4] = {
        r8e_from_int32(1),
        r8e_from_double(2.0),
        R8E_TRUE,
        R8E_NULL
    };
    ASSERT_TRUE(r8e_validate_values(vals, 4));
}

TEST(validate_values_with_bad_entry) {
    R8EValue vals[3] = {
        r8e_from_int32(1),
        0xFFFE000000000000ULL, /* invalid tag */
        R8E_NULL
    };
    ASSERT_FALSE(r8e_validate_values(vals, 3));
}

TEST(validate_values_null_array) {
    ASSERT_FALSE(r8e_validate_values(NULL, 5));
    ASSERT_TRUE(r8e_validate_values(NULL, 0));
}

TEST(validate_pointer_in_arena) {
    R8ESecureArena *arena = r8e_secure_arena_create(0, 0);
    ASSERT_NOT_NULL(arena);

    void *p = r8e_secure_arena_alloc(arena, 64);
    ASSERT_NOT_NULL(p);
    R8EValue pval = r8e_from_pointer(p);

    ASSERT_TRUE(r8e_validate_pointer_in_arena(pval, arena));

    /* Stack pointer should not be in the arena */
    int stack_var;
    R8EValue stack_pval = r8e_from_pointer(&stack_var);
    ASSERT_FALSE(r8e_validate_pointer_in_arena(stack_pval, arena));

    /* Non-pointer value should fail */
    ASSERT_FALSE(r8e_validate_pointer_in_arena(r8e_from_int32(42), arena));

    r8e_secure_arena_destroy(arena);
}

/* =========================================================================
 * LAYER 2 TESTS: Stack Canary
 * ========================================================================= */

TEST(canary_init_and_check) {
    uint64_t buffer[4];
    memset(buffer, 0, sizeof(buffer));

    r8e_canary_init(&buffer[1]);
    ASSERT_TRUE(r8e_canary_check(&buffer[1]));
}

TEST(canary_corrupt_detected) {
    uint64_t buffer[4];
    memset(buffer, 0, sizeof(buffer));

    r8e_canary_init(&buffer[1]);
    ASSERT_TRUE(r8e_canary_check(&buffer[1]));

    /* Corrupt the canary */
    buffer[1] = 0x0000000000000000ULL;
    ASSERT_FALSE(r8e_canary_check(&buffer[1]));
}

TEST(canary_null_check) {
    /* NULL frame_base should return false */
    ASSERT_FALSE(r8e_canary_check(NULL));
}

TEST(canary_init_null_safe) {
    /* Initializing NULL should not crash */
    r8e_canary_init(NULL);
    ASSERT_TRUE(1);
}

/* =========================================================================
 * LAYER 3 TESTS: Bytecode Verifier
 * ========================================================================= */

TEST(verify_null_bytecode) {
    R8EVerifyMeta meta;
    memset(&meta, 0, sizeof(meta));
    R8EVerifyResult result;

    R8EStatus status = r8e_verify_bytecode(NULL, 0, &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

TEST(verify_null_meta) {
    uint8_t bc[] = { 0x09, 0x4F }; /* PUSH_UNDEFINED, RETURN */
    R8EVerifyResult result;

    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), NULL, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

TEST(verify_valid_simple) {
    /* PUSH_UNDEFINED (0x09), RETURN (0x4F) */
    uint8_t bc[] = { 0x09, 0x4F };
    R8EVerifyMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.local_count = 0;
    meta.max_stack_size = 16;
    R8EVerifyResult result;

    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_OK);
}

TEST(verify_stack_underflow) {
    /* ADD (0x20) with empty stack: requires 2 values, stack is empty */
    uint8_t bc[] = { 0x20, 0x4F }; /* ADD, RETURN */
    R8EVerifyMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.local_count = 0;
    meta.max_stack_size = 16;
    R8EVerifyResult result;

    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
    /* Verify error message mentions stack underflow */
    ASSERT_NOT_NULL(result.message);
    ASSERT_TRUE(strlen(result.message) > 0);
}

TEST(verify_local_out_of_range) {
    /* LOAD_LOCAL(0x00) with register 10, but local_count = 2 */
    uint8_t bc[] = { 0x00, 10, 0x4F }; /* LOAD_LOCAL reg=10, RETURN */
    R8EVerifyMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.local_count = 2;
    meta.max_stack_size = 16;
    R8EVerifyResult result;

    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

TEST(verify_valid_load_local) {
    /* LOAD_LOCAL(0x00) reg=0, RETURN(0x4F) */
    uint8_t bc[] = { 0x00, 0, 0x4F };
    R8EVerifyMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.local_count = 4;
    meta.max_stack_size = 16;
    R8EVerifyResult result;

    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_OK);
}

TEST(verify_detailed_error_msg) {
    /* Empty bytecode */
    R8EVerifyMeta meta;
    memset(&meta, 0, sizeof(meta));
    char msg[256];

    R8EStatus status = r8e_verify_bytecode_detailed(NULL, 0, &meta, msg, 256);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
    ASSERT_TRUE(strlen(msg) > 0);
}

TEST(verify_function_null) {
    R8EStatus status = r8e_verify_function(NULL);
    ASSERT_EQ_INT(status, R8E_ERROR);
}

/* =========================================================================
 * LAYER 4 TESTS: Realm Creation and Destruction
 * ========================================================================= */

TEST(realm_create) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    ASSERT_NOT_NULL(realm->global);
    ASSERT_NOT_NULL(realm->prototypes);
    ASSERT_TRUE(realm->proto_count >= R8E_PROTO_COUNT);
    ASSERT_NOT_NULL(realm->arena);
    ASSERT_TRUE(realm->arena_size > 0);
    ASSERT_EQ_INT(realm->arena_used, 0);

    r8e_realm_destroy(ctx, (int)realm->id);
    free_test_ctx(ctx);
}

TEST(realm_create_null_ctx) {
    R8ERealm *realm = r8e_realm_create(NULL);
    ASSERT_NULL(realm);
}

TEST(realm_destroy_invalid) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Cannot destroy realm 0 (default) */
    ASSERT_EQ_INT(r8e_realm_destroy(ctx, 0), R8E_ERROR);

    /* Cannot destroy negative ID */
    ASSERT_EQ_INT(r8e_realm_destroy(ctx, -1), R8E_ERROR);

    /* Cannot destroy out-of-range ID */
    ASSERT_EQ_INT(r8e_realm_destroy(ctx, 100), R8E_ERROR);

    free_test_ctx(ctx);
}

TEST(realm_switch) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    /* Switch to the new realm, get back previous realm ID */
    int prev = r8e_realm_switch(ctx, rid);
    ASSERT_TRUE(prev >= 0);
    ASSERT_EQ_INT(ctx->current_realm, rid);

    /* Switch back */
    r8e_realm_switch(ctx, prev);
    ASSERT_EQ_INT(ctx->current_realm, prev);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

TEST(realm_switch_invalid) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    int prev = r8e_realm_switch(ctx, -1);
    ASSERT_EQ_INT(prev, -1);

    prev = r8e_realm_switch(ctx, 100);
    ASSERT_EQ_INT(prev, -1);

    prev = r8e_realm_switch(NULL, 0);
    ASSERT_EQ_INT(prev, -1);

    free_test_ctx(ctx);
}

/* =========================================================================
 * LAYER 4 TESTS: Resource Limits
 * ========================================================================= */

TEST(realm_set_limits) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = 1024 * 1024; /* 1MB */
    limits.max_cpu_time_us = 1000000; /* 1 second */
    limits.max_stack_depth = 100;
    limits.max_string_length = 65536;
    limits.max_array_length = 10000;
    limits.max_regexp_steps = 500000;

    R8EStatus status = r8e_realm_set_limits(ctx, rid, &limits);
    ASSERT_EQ_INT(status, R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

TEST(realm_set_limits_null) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    /* NULL limits should fail */
    R8EStatus status = r8e_realm_set_limits(ctx, rid, NULL);
    ASSERT_EQ_INT(status, R8E_ERROR);

    /* NULL ctx should fail */
    R8ERealmLimits limits;
    memset(&limits, 0, sizeof(limits));
    status = r8e_realm_set_limits(NULL, rid, &limits);
    ASSERT_EQ_INT(status, R8E_ERROR);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* =========================================================================
 * LAYER 4 TESTS: Cross-Realm Pointer Detection
 * ========================================================================= */

TEST(realm_tag_pointer) {
    /* Tag a pointer with realm ID 5 */
    int dummy = 42;
    void *ptr = &dummy;
    R8EValue tagged = r8e_realm_tag_pointer(ptr, 5);

    /* Should be a valid pointer value */
    ASSERT_TRUE(R8E_IS_POINTER(tagged));

    /* Should extract realm ID 5 */
    uint32_t rid = r8e_realm_id_from_pointer(tagged);
    ASSERT_EQ_INT(rid, 5);
}

TEST(realm_tag_null_pointer) {
    R8EValue tagged = r8e_realm_tag_pointer(NULL, 0);
    ASSERT_EQ(tagged, R8E_NULL);
}

TEST(realm_tag_invalid_realm) {
    int dummy = 42;
    R8EValue tagged = r8e_realm_tag_pointer(&dummy, 16); /* max is 15 */
    ASSERT_EQ(tagged, R8E_NULL);
}

TEST(realm_id_from_non_pointer) {
    /* Non-pointer values should return UINT32_MAX */
    uint32_t rid = r8e_realm_id_from_pointer(r8e_from_int32(42));
    ASSERT_EQ(rid, UINT32_MAX);

    rid = r8e_realm_id_from_pointer(R8E_UNDEFINED);
    ASSERT_EQ(rid, UINT32_MAX);
}

TEST(cross_realm_check) {
    int dummy = 42;
    R8EValue tagged_r3 = r8e_realm_tag_pointer(&dummy, 3);

    /* Same realm should pass */
    ASSERT_TRUE(r8e_check_cross_realm(tagged_r3, 3));

    /* Different realm should fail */
    ASSERT_FALSE(r8e_check_cross_realm(tagged_r3, 5));
}

TEST(cross_realm_non_pointer) {
    /* Non-pointer values are realm-independent */
    ASSERT_TRUE(r8e_check_cross_realm(r8e_from_int32(42), 0));
    ASSERT_TRUE(r8e_check_cross_realm(R8E_TRUE, 5));
    ASSERT_TRUE(r8e_check_cross_realm(R8E_NULL, 10));
    ASSERT_TRUE(r8e_check_cross_realm(r8e_from_double(3.14), 15));
}

/* =========================================================================
 * LAYER 4 TESTS: Structured Clone
 * ========================================================================= */

TEST(structured_clone_primitive_int) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_from_int32(42);
    R8EValue cloned = r8e_structured_clone(ctx, val);
    ASSERT_EQ(cloned, val);

    free_test_ctx(ctx);
}

TEST(structured_clone_primitive_double) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_from_double(3.14);
    R8EValue cloned = r8e_structured_clone(ctx, val);
    ASSERT_EQ(cloned, val);

    free_test_ctx(ctx);
}

TEST(structured_clone_special) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    ASSERT_EQ(r8e_structured_clone(ctx, R8E_UNDEFINED), R8E_UNDEFINED);
    ASSERT_EQ(r8e_structured_clone(ctx, R8E_NULL), R8E_NULL);
    ASSERT_EQ(r8e_structured_clone(ctx, R8E_TRUE), R8E_TRUE);
    ASSERT_EQ(r8e_structured_clone(ctx, R8E_FALSE), R8E_FALSE);

    free_test_ctx(ctx);
}

TEST(structured_clone_inline_string) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue val = r8e_from_inline_str("hello", 5);
    R8EValue cloned = r8e_structured_clone(ctx, val);
    ASSERT_EQ(cloned, val);

    free_test_ctx(ctx);
}

TEST(structured_clone_null_ctx) {
    R8EValue result = r8e_structured_clone(NULL, r8e_from_int32(1));
    ASSERT_EQ(result, R8E_UNDEFINED);
}

/* =========================================================================
 * LAYER 5 TESTS: Capability Creation
 * ========================================================================= */

TEST(capability_create_fs) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cap = r8e_capability_create_fs(
        ctx, "/app/data",
        R8E_PERM_FS_READ | R8E_PERM_FS_LIST,
        1024 * 1024 /* 1MB quota */
    );
    ASSERT_TRUE(R8E_IS_POINTER(cap));
    ASSERT_EQ_INT(r8e_capability_type(cap), R8E_CAP_FS);
    ASSERT_FALSE(r8e_capability_is_revoked(cap));

    r8e_capability_release(cap);
    free_test_ctx(ctx);
}

TEST(capability_create_fs_null_ctx) {
    R8EValue cap = r8e_capability_create_fs(NULL, "/data", R8E_PERM_FS_READ, 0);
    ASSERT_EQ(cap, R8E_UNDEFINED);
}

TEST(capability_create_net) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cap = r8e_capability_create_net(ctx, R8E_PERM_NET_CONNECT);
    ASSERT_TRUE(R8E_IS_POINTER(cap));
    ASSERT_EQ_INT(r8e_capability_type(cap), R8E_CAP_NET);

    r8e_capability_release(cap);
    free_test_ctx(ctx);
}

TEST(capability_check_fs_read_allowed) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cap = r8e_capability_create_fs(
        ctx, "/app/data",
        R8E_PERM_FS_READ | R8E_PERM_FS_LIST,
        0
    );

    /* Reading within root should be allowed */
    ASSERT_TRUE(r8e_capability_check_fs(cap, "/app/data/file.txt",
                                         R8E_PERM_FS_READ, 0));

    /* Reading outside root should be denied */
    ASSERT_FALSE(r8e_capability_check_fs(cap, "/etc/passwd",
                                          R8E_PERM_FS_READ, 0));

    /* Writing should be denied (no write permission) */
    ASSERT_FALSE(r8e_capability_check_fs(cap, "/app/data/file.txt",
                                          R8E_PERM_FS_WRITE, 100));

    r8e_capability_release(cap);
    free_test_ctx(ctx);
}

/* =========================================================================
 * LAYER 5 TESTS: Capability Attenuation
 * ========================================================================= */

TEST(capability_attenuate_fs) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create parent with full FS permissions */
    R8EValue parent = r8e_capability_create_fs(
        ctx, "/app",
        R8E_PERM_FS_ALL,
        1024 * 1024
    );
    ASSERT_TRUE(R8E_IS_POINTER(parent));

    /* Attenuate to read-only on /app/data */
    R8EValue child = r8e_capability_attenuate(
        parent, "/app/data",
        R8E_PERM_FS_READ,
        512 * 1024
    );
    ASSERT_TRUE(R8E_IS_POINTER(child));

    /* Child can read within /app/data */
    ASSERT_TRUE(r8e_capability_check_fs(child, "/app/data/file.txt",
                                         R8E_PERM_FS_READ, 0));

    /* Child cannot write (attenuated away) */
    ASSERT_FALSE(r8e_capability_check_fs(child, "/app/data/file.txt",
                                          R8E_PERM_FS_WRITE, 0));

    /* Child cannot access outside /app/data */
    ASSERT_FALSE(r8e_capability_check_fs(child, "/app/other/file.txt",
                                          R8E_PERM_FS_READ, 0));

    r8e_capability_release(child);
    r8e_capability_release(parent);
    free_test_ctx(ctx);
}

TEST(capability_attenuate_outside_root) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue parent = r8e_capability_create_fs(
        ctx, "/app/data",
        R8E_PERM_FS_ALL,
        0
    );

    /* Attempting to attenuate to a path outside the parent root should fail */
    R8EValue child = r8e_capability_attenuate(
        parent, "/etc",
        R8E_PERM_FS_READ,
        0
    );
    ASSERT_EQ(child, R8E_UNDEFINED);

    r8e_capability_release(parent);
    free_test_ctx(ctx);
}

/* =========================================================================
 * LAYER 5 TESTS: Capability Revocation
 * ========================================================================= */

TEST(capability_revoke) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cap = r8e_capability_create_fs(
        ctx, "/app/data",
        R8E_PERM_FS_READ,
        0
    );
    ASSERT_FALSE(r8e_capability_is_revoked(cap));

    /* Access should work before revocation */
    ASSERT_TRUE(r8e_capability_check_fs(cap, "/app/data/file.txt",
                                         R8E_PERM_FS_READ, 0));

    /* Revoke */
    r8e_capability_revoke(cap);
    ASSERT_TRUE(r8e_capability_is_revoked(cap));

    /* Access should fail after revocation */
    ASSERT_FALSE(r8e_capability_check_fs(cap, "/app/data/file.txt",
                                          R8E_PERM_FS_READ, 0));

    r8e_capability_release(cap);
    free_test_ctx(ctx);
}

TEST(capability_revoke_propagates) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create parent and attenuated child */
    R8EValue parent = r8e_capability_create_fs(
        ctx, "/app",
        R8E_PERM_FS_ALL,
        0
    );
    R8EValue child = r8e_capability_attenuate(
        parent, "/app/data",
        R8E_PERM_FS_READ,
        0
    );
    ASSERT_TRUE(R8E_IS_POINTER(child));

    /* Child should work before parent revocation */
    ASSERT_FALSE(r8e_capability_is_revoked(child));
    ASSERT_TRUE(r8e_capability_check_fs(child, "/app/data/file.txt",
                                         R8E_PERM_FS_READ, 0));

    /* Revoke the parent */
    r8e_capability_revoke(parent);

    /* Child should now be effectively revoked too */
    ASSERT_TRUE(r8e_capability_is_revoked(child));
    ASSERT_FALSE(r8e_capability_check_fs(child, "/app/data/file.txt",
                                          R8E_PERM_FS_READ, 0));

    r8e_capability_release(child);
    r8e_capability_release(parent);
    free_test_ctx(ctx);
}

TEST(capability_revoke_non_pointer) {
    /* Revoking a non-pointer value should not crash */
    r8e_capability_revoke(r8e_from_int32(42));
    ASSERT_TRUE(1);

    /* Non-pointer should report as revoked */
    ASSERT_TRUE(r8e_capability_is_revoked(r8e_from_int32(42)));
}

TEST(capability_realm_id) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);
    ctx->current_realm = 7;

    R8EValue cap = r8e_capability_create_fs(
        ctx, "/data",
        R8E_PERM_FS_READ,
        0
    );
    ASSERT_TRUE(R8E_IS_POINTER(cap));
    ASSERT_EQ_INT(r8e_capability_realm(cap), 7);

    r8e_capability_release(cap);
    free_test_ctx(ctx);
}

/* =========================================================================
 * Main entry point for test runner
 * ========================================================================= */

void run_security_tests(void) {
    /* Layer 2: Secure Arena */
    RUN_TEST(arena_create_default_size);
    RUN_TEST(arena_create_custom_size);
    RUN_TEST(arena_alloc_basic);
    RUN_TEST(arena_alloc_exhaustion);
    RUN_TEST(arena_alloc_zero_size);
    RUN_TEST(arena_contains);
    RUN_TEST(arena_destroy_null_safe);
    RUN_TEST(arena_alloc_null_arena);

    /* Layer 2: NaN-Box Validation */
    RUN_TEST(validate_double);
    RUN_TEST(validate_int32);
    RUN_TEST(validate_special_values);
    RUN_TEST(validate_inline_string);
    RUN_TEST(validate_pointer_valid);
    RUN_TEST(validate_pointer_null_rejected);
    RUN_TEST(validate_pointer_misaligned_rejected);
    RUN_TEST(validate_invalid_tag_rejected);
    RUN_TEST(validate_special_invalid_payload);
    RUN_TEST(validate_values_array);
    RUN_TEST(validate_values_with_bad_entry);
    RUN_TEST(validate_values_null_array);
    RUN_TEST(validate_pointer_in_arena);

    /* Layer 2: Stack Canary */
    RUN_TEST(canary_init_and_check);
    RUN_TEST(canary_corrupt_detected);
    RUN_TEST(canary_null_check);
    RUN_TEST(canary_init_null_safe);

    /* Layer 3: Bytecode Verifier */
    RUN_TEST(verify_null_bytecode);
    RUN_TEST(verify_null_meta);
    RUN_TEST(verify_valid_simple);
    RUN_TEST(verify_stack_underflow);
    RUN_TEST(verify_local_out_of_range);
    RUN_TEST(verify_valid_load_local);
    RUN_TEST(verify_detailed_error_msg);
    RUN_TEST(verify_function_null);

    /* Layer 4: Realm */
    RUN_TEST(realm_create);
    RUN_TEST(realm_create_null_ctx);
    RUN_TEST(realm_destroy_invalid);
    RUN_TEST(realm_switch);
    RUN_TEST(realm_switch_invalid);
    RUN_TEST(realm_set_limits);
    RUN_TEST(realm_set_limits_null);

    /* Layer 4: Cross-Realm Pointers */
    RUN_TEST(realm_tag_pointer);
    RUN_TEST(realm_tag_null_pointer);
    RUN_TEST(realm_tag_invalid_realm);
    RUN_TEST(realm_id_from_non_pointer);
    RUN_TEST(cross_realm_check);
    RUN_TEST(cross_realm_non_pointer);

    /* Layer 4: Structured Clone */
    RUN_TEST(structured_clone_primitive_int);
    RUN_TEST(structured_clone_primitive_double);
    RUN_TEST(structured_clone_special);
    RUN_TEST(structured_clone_inline_string);
    RUN_TEST(structured_clone_null_ctx);

    /* Layer 5: Capabilities */
    RUN_TEST(capability_create_fs);
    RUN_TEST(capability_create_fs_null_ctx);
    RUN_TEST(capability_create_net);
    RUN_TEST(capability_check_fs_read_allowed);
    RUN_TEST(capability_attenuate_fs);
    RUN_TEST(capability_attenuate_outside_root);
    RUN_TEST(capability_revoke);
    RUN_TEST(capability_revoke_propagates);
    RUN_TEST(capability_revoke_non_pointer);
    RUN_TEST(capability_realm_id);
}

/* =========================================================================
 * Standalone main (for isolated test runs)
 * ========================================================================= */

#ifdef TEST_SECURITY_MAIN

int g_tests_run    = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_assert_fail  = 0;

int main(void) {
    printf("=== r8e security tests ===\n\n");
    run_security_tests();
    printf("\n=== Summary: %d passed, %d failed out of %d tests ===\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}

#endif /* TEST_SECURITY_MAIN */
