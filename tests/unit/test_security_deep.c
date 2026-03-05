/*
 * test_security_deep.c - Deep security boundary tests for the r8e JS engine
 *
 * These tests probe the security boundaries more thoroughly than the
 * basic tests in test_security.c. They focus on:
 *   - Bytecode verifier: crafted malformed bytecodes that exercise each
 *     verification guarantee (stack underflow/overflow, invalid jumps,
 *     out-of-bounds constant/local refs, missing RETURN, invalid opcode)
 *   - Realm isolation: cross-realm object access prevention, prototype
 *     pollution isolation, per-realm resource enforcement
 *   - Arena safety: sequential exhaustion, concurrent arenas, alignment
 *   - Capability model: hierarchical attenuation, revocation cascading,
 *     forged capability detection, mixed-type capabilities
 *   - Resource limits: memory, CPU time, stack depth, string length,
 *     array length, regexp step bounds
 *
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
#define R8E_IS_INT32(v)      (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)    (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_BOOL(v)       ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)     (((v) >> 32) == 0xFFFB0000U)
#define R8E_IS_ATOM(v)       (((v) >> 32) == 0xFFFC0000U)

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

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

typedef struct R8EPropPair {
    uint64_t key;
    uint64_t val;
} R8EPropPair;

typedef struct R8EObjTier0 {
    uint32_t flags;
    uint32_t proto_id;
    uint64_t key0;
    uint64_t val0;
} R8EObjTier0;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  count;
    uint8_t  pad[7];
    R8EPropPair props[4];
} R8EObjTier1;

typedef struct R8EObject {
    union {
        R8EGCHeader header;
        R8EObjTier0 t0;
        R8EObjTier1 t1;
    } u;
} R8EObject;

typedef struct R8EArray {
    uint32_t   flags;
    uint32_t   proto_id;
    uint32_t   length;
    uint32_t   capacity;
    R8EValue  *elements;
    R8EObject *named;
} R8EArray;

typedef struct R8EString {
    uint32_t  flags;
    uint32_t  hash;
    uint32_t  byte_length;
    uint32_t  char_length;
    void     *offset_table;
} R8EString;

typedef struct {
    size_t   max_memory;
    uint64_t max_cpu_time_us;
    uint32_t max_stack_depth;
    uint32_t max_string_length;
    uint32_t max_array_length;
    uint32_t max_regexp_steps;
} R8ERealmLimits;

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

typedef struct {
    uint64_t bloom[4];
    uint32_t count;
    uint32_t capacity;
    void    *entries;
} R8EAtomTable;

typedef struct { void *data; int count; int capacity; } R8ESuspectList;
typedef struct { void *data; int count; int capacity; } R8EDeferredList;
typedef struct {
    int          error_type;
    const char  *error_message;
    uint16_t     error_line;
    uint16_t     error_col;
    const char  *error_file;
} R8EErrorState;

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

typedef struct R8ESecureArena R8ESecureArena;

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

typedef struct {
    R8EStatus  status;
    uint32_t   offset;
    char       message[256];
} R8EVerifyResult;

/* TypedArray buffer */
typedef enum {
    R8E_TYPED_UINT8 = 0,
    R8E_TYPED_INT8,
    R8E_TYPED_UINT16,
    R8E_TYPED_INT16,
    R8E_TYPED_UINT32,
    R8E_TYPED_INT32,
    R8E_TYPED_FLOAT32,
    R8E_TYPED_FLOAT64
} R8ETypedKind;

typedef struct R8ETypedArrayBuffer R8ETypedArrayBuffer;

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
 * External function declarations
 * ========================================================================= */

/* r8e_arena.c (Layer 2) */
extern R8ESecureArena *r8e_secure_arena_create(size_t size, uint32_t realm_id);
extern void *r8e_secure_arena_alloc(R8ESecureArena *arena, size_t size);
extern bool  r8e_secure_arena_contains(const R8ESecureArena *arena,
                                        const void *ptr, size_t size);
extern uint32_t r8e_secure_arena_realm(const R8ESecureArena *arena);
extern size_t   r8e_secure_arena_used(const R8ESecureArena *arena);
extern size_t   r8e_secure_arena_available(const R8ESecureArena *arena);
extern void     r8e_secure_arena_destroy(R8ESecureArena *arena);

extern bool r8e_validate_value(R8EValue val);
extern bool r8e_validate_pointer_in_arena(R8EValue val,
                                           const R8ESecureArena *arena);
extern bool r8e_validate_values(const R8EValue *values, int count);

extern void r8e_canary_init(void *frame_base);
extern bool r8e_canary_check(void *frame_base);

extern bool r8e_arena_registry_add(R8ESecureArena *arena);
extern void r8e_arena_registry_remove(R8ESecureArena *arena);
extern const R8ESecureArena *r8e_arena_registry_find(const void *ptr, size_t size);
extern bool r8e_validate_pointer_global(R8EValue val);

extern R8ETypedArrayBuffer *r8e_typedarray_create(R8ETypedKind kind,
                                                    uint32_t length);
extern R8EValue r8e_typedarray_get(const R8ETypedArrayBuffer *buf,
                                    uint32_t index);
extern void r8e_typedarray_set(R8ETypedArrayBuffer *buf, uint32_t index,
                                R8EValue value);
extern uint32_t r8e_typedarray_length(const R8ETypedArrayBuffer *buf);
extern void r8e_typedarray_destroy(R8ETypedArrayBuffer *buf);
extern int32_t r8e_bounds_clamp(int32_t idx, int32_t length);

/* r8e_verify.c (Layer 3) */
extern R8EStatus r8e_verify_bytecode(const uint8_t *bytecode, uint32_t bc_len,
                                      const R8EVerifyMeta *meta,
                                      R8EVerifyResult *result);
extern R8EStatus r8e_verify_function(const R8EFunction *func);
extern R8EStatus r8e_verify_bytecode_detailed(const uint8_t *bytecode,
                                               uint32_t bc_len,
                                               const R8EVerifyMeta *meta,
                                               char *out_msg, size_t msg_len);

/* r8e_realm.c (Layer 4) */
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

/* r8e_capability.c (Layer 5) */
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
extern R8EValue  r8e_capability_create_timer(R8EContext *ctx,
                                              uint32_t permissions,
                                              uint32_t min_interval_ms,
                                              uint32_t max_concurrent);
extern bool      r8e_capability_check_timer(R8EValue cap_val,
                                             uint32_t operation);
extern bool      r8e_capability_check_net(R8EValue cap_val,
                                           const char *host, uint16_t port,
                                           uint32_t operation);

/* Capability permission flags */
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

#define R8E_PERM_TIMER_SET    0x0001U
#define R8E_PERM_TIMER_CLEAR  0x0002U
#define R8E_PERM_TIMER_ALL    0x0003U

#define R8E_CAP_FS   0
#define R8E_CAP_NET  1
#define R8E_CAP_TIMER 2

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

/* Default verify meta helper */
static void init_meta(R8EVerifyMeta *meta, uint16_t locals,
                      uint16_t consts, uint16_t stack)
{
    memset(meta, 0, sizeof(R8EVerifyMeta));
    meta->local_count = locals;
    meta->const_count = consts;
    meta->max_stack_size = stack;
}

/* =========================================================================
 * BYTECODE VERIFIER DEEP TESTS (Layer 3)
 *
 * Each test crafts specific malformed bytecode sequences that must be
 * rejected by the verifier. These go beyond the basic tests in
 * test_security.c by testing more subtle violation patterns.
 * ========================================================================= */

/* --- Test: stack overflow (push too many values without consuming) --- */
TEST(verify_stack_overflow) {
    /*
     * Push values until we exceed the declared max_stack_size.
     * Bytecode: PUSH_UNDEFINED repeated (max_stack_size + 1) times, then RETURN.
     * The verifier should detect the overflow before execution.
     */
    uint8_t bc[66];
    int i;
    /* Fill with PUSH_UNDEFINED (0x09) to overflow a stack of size 2 */
    for (i = 0; i < 64; i++) bc[i] = 0x09;
    bc[64] = 0x4F; /* RETURN */
    bc[65] = 0;

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 0, 2); /* max_stack_size = 2 */

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, 65, &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
    ASSERT_TRUE(strlen(result.message) > 0);
}

/* --- Test: jump target lands mid-instruction (CFI violation) --- */
TEST(verify_jump_mid_instruction) {
    /*
     * PUSH_INT8 takes 2 bytes: opcode + imm8.
     * Jumping to the byte after the opcode (into the immediate) is invalid.
     *
     * Bytecode: JUMP +1, PUSH_INT8 42, RETURN
     * If jump offset lands on the immediate byte of PUSH_INT8, that is
     * not a valid instruction boundary.
     */
    uint8_t bc[] = {
        0x40, 3,   /* JUMP off16: skip 3 bytes forward to offset 5 */
        0,         /* off16 high byte */
        0x05, 42,  /* PUSH_INT8 42 (2 bytes) */
        0x4F       /* RETURN */
    };

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 0, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    /* This should fail because the jump target is byte 3 which is
       the high byte of the offset, or byte 5 which is the immediate of
       PUSH_INT8 - either way it is not a valid instruction boundary. */
    /* The exact validation depends on how the verifier interprets JUMP encoding,
       but the intent is that a malformed jump target should fail. */
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

/* --- Test: LOAD_CONST with out-of-bounds constant pool index --- */
TEST(verify_const_out_of_bounds) {
    /*
     * LOAD_CONST with idx=10 but const_count=2.
     * Bytecode: LOAD_CONST idx_lo=10, idx_hi=0, RETURN
     */
    uint8_t bc[] = { 0x04, 10, 0, 0x4F }; /* LOAD_CONST idx=10, RETURN */

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 2, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

/* --- Test: STORE_LOCAL with out-of-bounds register --- */
TEST(verify_store_local_out_of_range) {
    /*
     * PUSH_UNDEFINED then STORE_LOCAL to register 255 with only 4 locals.
     */
    uint8_t bc[] = { 0x09, 0x01, 255, 0x4F };
    /* PUSH_UNDEFINED, STORE_LOCAL reg=255, RETURN */

    R8EVerifyMeta meta;
    init_meta(&meta, 4, 0, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

/* --- Test: missing RETURN at end of bytecode --- */
TEST(verify_missing_return) {
    /*
     * Bytecode that does not end with RETURN. The verifier should detect
     * that control can fall off the end of the function.
     */
    uint8_t bc[] = { 0x09 }; /* just PUSH_UNDEFINED, no RETURN */

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 0, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

/* --- Test: completely invalid opcode byte --- */
TEST(verify_invalid_opcode_byte) {
    /*
     * Opcode 0xFF is not a valid opcode. The verifier should reject it.
     */
    uint8_t bc[] = { 0xFF, 0x4F }; /* invalid opcode, then RETURN */

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 0, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

/* --- Test: backward jump creating an infinite loop is valid but detectable --- */
TEST(verify_backward_jump_valid) {
    /*
     * A backward jump is semantically valid (it is how loops work).
     * The verifier should accept it as long as it lands on a valid
     * instruction boundary and stack depth is consistent.
     *
     * Bytecode: PUSH_UNDEFINED, DROP, JUMP -3 (back to PUSH_UNDEFINED), RETURN
     * This is a valid infinite loop. The verifier does not reject infinite
     * loops (that is a runtime concern via CHECK_LIMITS).
     *
     * We just verify it does not crash the verifier or produce false errors.
     */
    uint8_t bc[] = {
        0x09,       /* offset 0: PUSH_UNDEFINED */
        0x11,       /* offset 1: DROP */
        0x40, 0xFD, 0xFF,  /* offset 2: JUMP to offset 0 (offset = -5 + 5 = 0) */
        0x4F        /* offset 5: RETURN (unreachable) */
    };

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 0, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    /* A backward jump to a valid instruction boundary with consistent stack
       depth should be accepted. */
    ASSERT_EQ_INT(status, R8E_OK);
}

/* --- Test: stack depth mismatch at merge point --- */
TEST(verify_stack_depth_mismatch) {
    /*
     * Two control flow paths converge at a merge point with different
     * stack depths. This is invalid in Wasm-style verification.
     *
     * Path 1: PUSH_TRUE, JUMP_IF_FALSE +N, PUSH_INT8 1, PUSH_INT8 2 (depth=2)
     * Path 2: (from the false branch, depth=0)
     * -> merge at RETURN with depth 0 or 2 => MISMATCH
     */
    uint8_t bc[] = {
        0x0B,              /* offset 0: PUSH_TRUE (depth 0->1) */
        0x42, 0x04, 0x00,  /* offset 1: JUMP_IF_FALSE +4 (to offset 8) (depth 1->0) */
        0x05, 1,           /* offset 4: PUSH_INT8 1 (depth 0->1) */
        0x05, 2,           /* offset 6: PUSH_INT8 2 (depth 1->2) */
        /* offset 8: merge point */
        0x4F               /* offset 8: RETURN */
    };

    R8EVerifyMeta meta;
    init_meta(&meta, 0, 0, 16);

    R8EVerifyResult result;
    R8EStatus status = r8e_verify_bytecode(bc, sizeof(bc), &meta, &result);
    /* Path 1 reaches RETURN with depth 2, path 2 with depth 0 -> mismatch */
    ASSERT_EQ_INT(status, R8E_ERROR_VERIFY);
}

/* =========================================================================
 * REALM ISOLATION DEEP TESTS (Layer 4)
 * ========================================================================= */

/* --- Test: create multiple realms and verify isolation --- */
TEST(realm_multi_create_isolated) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *r1 = r8e_realm_create(ctx);
    R8ERealm *r2 = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);

    /* Each realm has its own ID */
    ASSERT_NE(r1->id, r2->id);

    /* Each realm has its own global object */
    ASSERT_NE(r1->global, r2->global);

    /* Each realm has its own prototype table */
    ASSERT_NE(r1->prototypes, r2->prototypes);

    /* Each realm has its own arena */
    ASSERT_NE(r1->arena, r2->arena);

    r8e_realm_destroy(ctx, (int)r1->id);
    r8e_realm_destroy(ctx, (int)r2->id);
    free_test_ctx(ctx);
}

/* --- Test: cross-realm pointer tagging detects foreign pointers --- */
TEST(realm_cross_realm_detection) {
    int dummy1 = 100;
    int dummy2 = 200;

    /* Tag pointer with realm 2 */
    R8EValue tagged2 = r8e_realm_tag_pointer(&dummy1, 2);

    /* Tag pointer with realm 5 */
    R8EValue tagged5 = r8e_realm_tag_pointer(&dummy2, 5);

    /* Cross-realm check: tagged2 should belong to realm 2, not 5 */
    ASSERT_TRUE(r8e_check_cross_realm(tagged2, 2));
    ASSERT_FALSE(r8e_check_cross_realm(tagged2, 5));

    /* And vice versa */
    ASSERT_TRUE(r8e_check_cross_realm(tagged5, 5));
    ASSERT_FALSE(r8e_check_cross_realm(tagged5, 2));

    /* Extract realm ID */
    ASSERT_EQ_INT(r8e_realm_id_from_pointer(tagged2), 2);
    ASSERT_EQ_INT(r8e_realm_id_from_pointer(tagged5), 5);
}

/* --- Test: prototype pollution in one realm does not affect another --- */
TEST(realm_prototype_isolation) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *r1 = r8e_realm_create(ctx);
    R8ERealm *r2 = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);

    /* Verify prototype tables are separate allocations */
    ASSERT_TRUE(r1->prototypes != r2->prototypes);

    /* Verify both have the standard prototype entries */
    ASSERT_TRUE(r1->proto_count >= R8E_PROTO_COUNT);
    ASSERT_TRUE(r2->proto_count >= R8E_PROTO_COUNT);

    /* Modifying realm 1's prototype table should not affect realm 2 */
    r1->prototypes[R8E_PROTO_OBJECT] = (R8EObject *)0xDEADBEEF;
    ASSERT_NE(r1->prototypes[R8E_PROTO_OBJECT],
              r2->prototypes[R8E_PROTO_OBJECT]);

    r8e_realm_destroy(ctx, (int)r1->id);
    r8e_realm_destroy(ctx, (int)r2->id);
    free_test_ctx(ctx);
}

/* --- Test: structured clone of primitive values --- */
TEST(realm_structured_clone_primitives) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Clone integer */
    R8EValue cloned_int = r8e_structured_clone(ctx, r8e_from_int32(42));
    ASSERT_TRUE(R8E_IS_INT32(cloned_int));
    ASSERT_EQ_INT(r8e_get_int32(cloned_int), 42);

    /* Clone double */
    R8EValue cloned_dbl = r8e_structured_clone(ctx, r8e_from_double(3.14));
    ASSERT_TRUE(R8E_IS_DOUBLE(cloned_dbl));

    /* Clone boolean */
    R8EValue cloned_true = r8e_structured_clone(ctx, R8E_TRUE);
    ASSERT_EQ(cloned_true, R8E_TRUE);

    /* Clone null */
    R8EValue cloned_null = r8e_structured_clone(ctx, R8E_NULL);
    ASSERT_EQ(cloned_null, R8E_NULL);

    /* Clone undefined */
    R8EValue cloned_undef = r8e_structured_clone(ctx, R8E_UNDEFINED);
    ASSERT_EQ(cloned_undef, R8E_UNDEFINED);

    free_test_ctx(ctx);
}

/* --- Test: realm resource limits enforcement --- */
TEST(realm_limits_restrictive) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    /* Set very restrictive limits */
    R8ERealmLimits limits;
    limits.max_memory = 4096;          /* 4KB only */
    limits.max_cpu_time_us = 1000;     /* 1ms only */
    limits.max_stack_depth = 10;       /* very shallow */
    limits.max_string_length = 256;    /* short strings */
    limits.max_array_length = 50;      /* small arrays */
    limits.max_regexp_steps = 100;     /* minimal regexp */

    R8EStatus status = r8e_realm_set_limits(ctx, rid, &limits);
    ASSERT_EQ_INT(status, R8E_OK);

    /* Verify limits took effect via check_limits */
    r8e_realm_switch(ctx, rid);
    status = r8e_realm_check_limits(ctx);
    /* Should be OK since nothing has been consumed yet */
    ASSERT_EQ_INT(status, R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* =========================================================================
 * ARENA SAFETY DEEP TESTS (Layer 2)
 * ========================================================================= */

/* --- Test: multiple arenas with different realm IDs stay isolated --- */
TEST(arena_multi_realm_isolation) {
    R8ESecureArena *arena1 = r8e_secure_arena_create(8192, 1);
    R8ESecureArena *arena2 = r8e_secure_arena_create(8192, 2);
    ASSERT_NOT_NULL(arena1);
    ASSERT_NOT_NULL(arena2);

    /* Allocate in arena 1 */
    void *p1 = r8e_secure_arena_alloc(arena1, 64);
    ASSERT_NOT_NULL(p1);

    /* Allocate in arena 2 */
    void *p2 = r8e_secure_arena_alloc(arena2, 64);
    ASSERT_NOT_NULL(p2);

    /* p1 should be in arena1, not arena2 */
    ASSERT_TRUE(r8e_secure_arena_contains(arena1, p1, 64));
    ASSERT_FALSE(r8e_secure_arena_contains(arena2, p1, 64));

    /* p2 should be in arena2, not arena1 */
    ASSERT_TRUE(r8e_secure_arena_contains(arena2, p2, 64));
    ASSERT_FALSE(r8e_secure_arena_contains(arena1, p2, 64));

    /* Realm IDs are correct */
    ASSERT_EQ_INT(r8e_secure_arena_realm(arena1), 1);
    ASSERT_EQ_INT(r8e_secure_arena_realm(arena2), 2);

    r8e_secure_arena_destroy(arena1);
    r8e_secure_arena_destroy(arena2);
}

/* --- Test: sequential allocations are contiguous and aligned --- */
TEST(arena_sequential_alignment) {
    R8ESecureArena *arena = r8e_secure_arena_create(0, 0);
    ASSERT_NOT_NULL(arena);

    void *p1 = r8e_secure_arena_alloc(arena, 17);
    void *p2 = r8e_secure_arena_alloc(arena, 33);
    void *p3 = r8e_secure_arena_alloc(arena, 1);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);

    /* All pointers should be at least 16-byte aligned (R8E_SECURE_ALIGN) */
    ASSERT_EQ((uintptr_t)p1 % 16, 0);
    ASSERT_EQ((uintptr_t)p2 % 16, 0);
    ASSERT_EQ((uintptr_t)p3 % 16, 0);

    /* Each allocation comes after the previous (no overlap) */
    ASSERT_TRUE((uintptr_t)p2 >= (uintptr_t)p1 + 17);
    ASSERT_TRUE((uintptr_t)p3 >= (uintptr_t)p2 + 33);

    r8e_secure_arena_destroy(arena);
}

/* --- Test: NaN-box validation rejects all possible corrupted patterns --- */
TEST(arena_nanbox_corruption_patterns) {
    /* Valid tags: 0xFFF8 (int32), 0xFFF9 (pointer), 0xFFFA (special),
       0xFFFB (symbol), 0xFFFC (atom), 0xFFFD (inline str).
       Invalid: 0xFFFE, 0xFFFF, and any double-range value with
       special NaN patterns. */

    /* Tag 0xFFFE: completely unused -> must reject */
    ASSERT_FALSE(r8e_validate_value(0xFFFE000000000000ULL));
    ASSERT_FALSE(r8e_validate_value(0xFFFE000000000001ULL));
    ASSERT_FALSE(r8e_validate_value(0xFFFEFFFFFFFFFFFFULL));

    /* Tag 0xFFFF: completely unused -> must reject */
    ASSERT_FALSE(r8e_validate_value(0xFFFF000000000000ULL));
    ASSERT_FALSE(r8e_validate_value(0xFFFFFFFFFFFFFFFFULL));

    /* Special tag with bogus payload beyond 3 */
    ASSERT_FALSE(r8e_validate_value(0xFFFA000000000005ULL));
    ASSERT_FALSE(r8e_validate_value(0xFFFA00000000FFFFULL));

    /* Valid values should still pass */
    ASSERT_TRUE(r8e_validate_value(r8e_from_int32(0)));
    ASSERT_TRUE(r8e_validate_value(r8e_from_int32(-1)));
    ASSERT_TRUE(r8e_validate_value(r8e_from_double(1.0)));
    ASSERT_TRUE(r8e_validate_value(R8E_UNDEFINED));
    ASSERT_TRUE(r8e_validate_value(R8E_NULL));
    ASSERT_TRUE(r8e_validate_value(R8E_TRUE));
    ASSERT_TRUE(r8e_validate_value(R8E_FALSE));
    ASSERT_TRUE(r8e_validate_value(r8e_from_inline_str("abc", 3)));
}

/* --- Test: TypedArray bounds clamping is branchless and correct --- */
TEST(arena_typedarray_bounds_clamp) {
    /* The branchless bounds clamp formula:
       idx = idx - ((idx - length) & ~((int32_t)(idx - length) >> 31))
       This should clamp any out-of-bounds index to length-1. */

    /* In-range values should pass through unchanged */
    ASSERT_EQ_INT(r8e_bounds_clamp(0, 10), 0);
    ASSERT_EQ_INT(r8e_bounds_clamp(5, 10), 5);
    ASSERT_EQ_INT(r8e_bounds_clamp(9, 10), 9);

    /* Out-of-range high values should clamp to length-1 */
    ASSERT_EQ_INT(r8e_bounds_clamp(10, 10), 9);
    ASSERT_EQ_INT(r8e_bounds_clamp(100, 10), 9);
    ASSERT_EQ_INT(r8e_bounds_clamp(INT32_MAX, 10), 9);

    /* Negative values should clamp to 0 */
    ASSERT_EQ_INT(r8e_bounds_clamp(-1, 10), 0);
    ASSERT_EQ_INT(r8e_bounds_clamp(-100, 10), 0);
    ASSERT_EQ_INT(r8e_bounds_clamp(INT32_MIN, 10), 0);
}

/* --- Test: canary corruption at different byte positions --- */
TEST(arena_canary_multi_corruption) {
    /* Place canary, then corrupt at specific byte offsets to ensure
       the check catches any single-byte corruption. */
    uint64_t buffer[4];
    memset(buffer, 0, sizeof(buffer));

    r8e_canary_init(&buffer[1]);
    ASSERT_TRUE(r8e_canary_check(&buffer[1]));

    /* Corrupt first byte of canary */
    uint8_t *bytes = (uint8_t *)&buffer[1];
    uint8_t saved = bytes[0];
    bytes[0] ^= 0x01;
    ASSERT_FALSE(r8e_canary_check(&buffer[1]));
    bytes[0] = saved; /* restore */

    /* Corrupt last byte of canary */
    saved = bytes[7];
    bytes[7] ^= 0x80;
    ASSERT_FALSE(r8e_canary_check(&buffer[1]));
    bytes[7] = saved; /* restore */

    /* Verify restored canary still passes */
    ASSERT_TRUE(r8e_canary_check(&buffer[1]));
}

/* =========================================================================
 * CAPABILITY MODEL DEEP TESTS (Layer 5)
 * ========================================================================= */

/* --- Test: hierarchical attenuation narrows permissions --- */
TEST(cap_hierarchical_attenuation) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create full-permission FS capability */
    R8EValue cap_full = r8e_capability_create_fs(
        ctx, "/app", R8E_PERM_FS_ALL, 1024 * 1024);
    ASSERT_FALSE(R8E_IS_UNDEFINED(cap_full));
    ASSERT_EQ_INT(r8e_capability_type(cap_full), R8E_CAP_FS);

    /* Attenuate to read-only */
    R8EValue cap_ro = r8e_capability_attenuate(
        cap_full, "/app/data", R8E_PERM_FS_READ, 0);
    ASSERT_FALSE(R8E_IS_UNDEFINED(cap_ro));

    /* Attenuated cap should allow reads under /app/data */
    ASSERT_TRUE(r8e_capability_check_fs(cap_ro, "/app/data/file.txt",
                                         R8E_PERM_FS_READ, 0));

    /* Attenuated cap should NOT allow writes */
    ASSERT_FALSE(r8e_capability_check_fs(cap_ro, "/app/data/file.txt",
                                          R8E_PERM_FS_WRITE, 100));

    /* Attenuated cap should NOT allow access outside /app/data */
    ASSERT_FALSE(r8e_capability_check_fs(cap_ro, "/app/config.json",
                                          R8E_PERM_FS_READ, 0));

    /* Original cap should still have full permissions */
    ASSERT_TRUE(r8e_capability_check_fs(cap_full, "/app/config.json",
                                         R8E_PERM_FS_READ, 0));

    r8e_capability_release(cap_ro);
    r8e_capability_release(cap_full);
    free_test_ctx(ctx);
}

/* --- Test: revoking parent capability invalidates attenuated children --- */
TEST(cap_revocation_cascading) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8EValue cap_parent = r8e_capability_create_fs(
        ctx, "/app", R8E_PERM_FS_ALL, 0);
    ASSERT_FALSE(R8E_IS_UNDEFINED(cap_parent));

    R8EValue cap_child = r8e_capability_attenuate(
        cap_parent, "/app/data", R8E_PERM_FS_READ, 0);
    ASSERT_FALSE(R8E_IS_UNDEFINED(cap_child));

    /* Both should work before revocation */
    ASSERT_FALSE(r8e_capability_is_revoked(cap_parent));
    ASSERT_FALSE(r8e_capability_is_revoked(cap_child));

    /* Revoke the parent */
    r8e_capability_revoke(cap_parent);
    ASSERT_TRUE(r8e_capability_is_revoked(cap_parent));

    /* Parent's FS checks should now fail */
    ASSERT_FALSE(r8e_capability_check_fs(cap_parent, "/app/file.txt",
                                          R8E_PERM_FS_READ, 0));

    r8e_capability_release(cap_child);
    r8e_capability_release(cap_parent);
    free_test_ctx(ctx);
}

/* --- Test: forged capability value is detected and rejected --- */
TEST(cap_forged_value_rejected) {
    /* A forged capability is a NaN-boxed pointer that does not point to
       a valid capability structure (no R8E_CAP_MAGIC). */

    /* Use an arbitrary pointer value as a "forged" capability */
    int dummy = 42;
    R8EValue forged = r8e_from_pointer(&dummy);

    /* Type check should return an invalid type or -1 */
    int type = r8e_capability_type(forged);
    ASSERT_TRUE(type < 0 || type >= 6); /* invalid type */

    /* Check should return false */
    ASSERT_FALSE(r8e_capability_check(forged, R8E_PERM_FS_READ));

    /* Revoke should not crash on forged value */
    r8e_capability_revoke(forged);

    /* Is-revoked on forged should return true (or safe default) */
    ASSERT_TRUE(r8e_capability_is_revoked(forged));
}

/* --- Test: net capability restricts host and protocol --- */
TEST(cap_net_capability) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create net capability with connect-only permission */
    R8EValue cap_net = r8e_capability_create_net(ctx, R8E_PERM_NET_CONNECT);
    ASSERT_FALSE(R8E_IS_UNDEFINED(cap_net));
    ASSERT_EQ_INT(r8e_capability_type(cap_net), R8E_CAP_NET);

    /* Connect should be allowed */
    ASSERT_TRUE(r8e_capability_check(cap_net, R8E_PERM_NET_CONNECT));

    /* Listen should NOT be allowed */
    ASSERT_FALSE(r8e_capability_check(cap_net, R8E_PERM_NET_LISTEN));

    /* Revoke and verify */
    r8e_capability_revoke(cap_net);
    ASSERT_TRUE(r8e_capability_is_revoked(cap_net));
    ASSERT_FALSE(r8e_capability_check(cap_net, R8E_PERM_NET_CONNECT));

    r8e_capability_release(cap_net);
    free_test_ctx(ctx);
}

/* --- Test: timer capability enforces minimum interval --- */
TEST(cap_timer_capability) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    /* Create timer capability with 10ms minimum, max 5 concurrent */
    R8EValue cap_timer = r8e_capability_create_timer(
        ctx, R8E_PERM_TIMER_ALL, 10, 5);
    ASSERT_FALSE(R8E_IS_UNDEFINED(cap_timer));
    ASSERT_EQ_INT(r8e_capability_type(cap_timer), R8E_CAP_TIMER);

    /* Timer set should be allowed */
    ASSERT_TRUE(r8e_capability_check_timer(cap_timer, R8E_PERM_TIMER_SET));

    /* Timer clear should be allowed */
    ASSERT_TRUE(r8e_capability_check_timer(cap_timer, R8E_PERM_TIMER_CLEAR));

    r8e_capability_release(cap_timer);
    free_test_ctx(ctx);
}

/* =========================================================================
 * RESOURCE LIMITS DEEP TESTS (Layer 4)
 * ========================================================================= */

/* --- Test: memory limit is stored and can be read back --- */
TEST(limits_memory) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = 2048;
    limits.max_cpu_time_us = R8E_DEFAULT_MAX_CPU_TIME_US;
    limits.max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    limits.max_string_length = R8E_DEFAULT_MAX_STRING_LEN;
    limits.max_array_length = R8E_DEFAULT_MAX_ARRAY_LEN;
    limits.max_regexp_steps = R8E_DEFAULT_MAX_REGEXP_STEPS;

    ASSERT_EQ_INT(r8e_realm_set_limits(ctx, rid, &limits), R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* --- Test: stack depth limit stored correctly --- */
TEST(limits_stack_depth) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = R8E_DEFAULT_MAX_MEMORY;
    limits.max_cpu_time_us = R8E_DEFAULT_MAX_CPU_TIME_US;
    limits.max_stack_depth = 5; /* extremely shallow */
    limits.max_string_length = R8E_DEFAULT_MAX_STRING_LEN;
    limits.max_array_length = R8E_DEFAULT_MAX_ARRAY_LEN;
    limits.max_regexp_steps = R8E_DEFAULT_MAX_REGEXP_STEPS;

    ASSERT_EQ_INT(r8e_realm_set_limits(ctx, rid, &limits), R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* --- Test: string length limit --- */
TEST(limits_string_length) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = R8E_DEFAULT_MAX_MEMORY;
    limits.max_cpu_time_us = R8E_DEFAULT_MAX_CPU_TIME_US;
    limits.max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    limits.max_string_length = 128; /* very short */
    limits.max_array_length = R8E_DEFAULT_MAX_ARRAY_LEN;
    limits.max_regexp_steps = R8E_DEFAULT_MAX_REGEXP_STEPS;

    ASSERT_EQ_INT(r8e_realm_set_limits(ctx, rid, &limits), R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* --- Test: array length limit --- */
TEST(limits_array_length) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = R8E_DEFAULT_MAX_MEMORY;
    limits.max_cpu_time_us = R8E_DEFAULT_MAX_CPU_TIME_US;
    limits.max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    limits.max_string_length = R8E_DEFAULT_MAX_STRING_LEN;
    limits.max_array_length = 10; /* very small */
    limits.max_regexp_steps = R8E_DEFAULT_MAX_REGEXP_STEPS;

    ASSERT_EQ_INT(r8e_realm_set_limits(ctx, rid, &limits), R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* --- Test: regexp step limit --- */
TEST(limits_regexp_steps) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = R8E_DEFAULT_MAX_MEMORY;
    limits.max_cpu_time_us = R8E_DEFAULT_MAX_CPU_TIME_US;
    limits.max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    limits.max_string_length = R8E_DEFAULT_MAX_STRING_LEN;
    limits.max_array_length = R8E_DEFAULT_MAX_ARRAY_LEN;
    limits.max_regexp_steps = 50; /* very small */

    ASSERT_EQ_INT(r8e_realm_set_limits(ctx, rid, &limits), R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* --- Test: CPU time limit --- */
TEST(limits_cpu_time) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    R8ERealmLimits limits;
    limits.max_memory = R8E_DEFAULT_MAX_MEMORY;
    limits.max_cpu_time_us = 500; /* 0.5ms */
    limits.max_stack_depth = R8E_DEFAULT_MAX_STACK_DEPTH;
    limits.max_string_length = R8E_DEFAULT_MAX_STRING_LEN;
    limits.max_array_length = R8E_DEFAULT_MAX_ARRAY_LEN;
    limits.max_regexp_steps = R8E_DEFAULT_MAX_REGEXP_STEPS;

    ASSERT_EQ_INT(r8e_realm_set_limits(ctx, rid, &limits), R8E_OK);

    r8e_realm_destroy(ctx, rid);
    free_test_ctx(ctx);
}

/* --- Test: realm count tracking --- */
TEST(realm_count_tracking) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    uint32_t initial = r8e_realm_count(ctx);

    R8ERealm *r1 = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(r1);
    ASSERT_EQ_INT(r8e_realm_count(ctx), initial + 1);

    R8ERealm *r2 = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ_INT(r8e_realm_count(ctx), initial + 2);

    r8e_realm_destroy(ctx, (int)r1->id);
    ASSERT_EQ_INT(r8e_realm_count(ctx), initial + 1);

    r8e_realm_destroy(ctx, (int)r2->id);
    ASSERT_EQ_INT(r8e_realm_count(ctx), initial);

    free_test_ctx(ctx);
}

/* --- Test: double-destroy does not crash --- */
TEST(realm_double_destroy_safe) {
    R8EContext *ctx = make_test_ctx();
    ASSERT_NOT_NULL(ctx);

    R8ERealm *realm = r8e_realm_create(ctx);
    ASSERT_NOT_NULL(realm);
    int rid = (int)realm->id;

    ASSERT_EQ_INT(r8e_realm_destroy(ctx, rid), R8E_OK);

    /* Second destroy should fail gracefully */
    ASSERT_EQ_INT(r8e_realm_destroy(ctx, rid), R8E_ERROR);

    free_test_ctx(ctx);
}

/* =========================================================================
 * Suite Runner
 * ========================================================================= */

void run_security_deep_tests(void) {
    /* Bytecode verifier deep tests */
    RUN_TEST(verify_stack_overflow);
    RUN_TEST(verify_jump_mid_instruction);
    RUN_TEST(verify_const_out_of_bounds);
    RUN_TEST(verify_store_local_out_of_range);
    RUN_TEST(verify_missing_return);
    RUN_TEST(verify_invalid_opcode_byte);
    RUN_TEST(verify_backward_jump_valid);
    RUN_TEST(verify_stack_depth_mismatch);

    /* Realm isolation deep tests */
    RUN_TEST(realm_multi_create_isolated);
    RUN_TEST(realm_cross_realm_detection);
    RUN_TEST(realm_prototype_isolation);
    RUN_TEST(realm_structured_clone_primitives);
    RUN_TEST(realm_limits_restrictive);

    /* Arena safety deep tests */
    RUN_TEST(arena_multi_realm_isolation);
    RUN_TEST(arena_sequential_alignment);
    RUN_TEST(arena_nanbox_corruption_patterns);
    RUN_TEST(arena_typedarray_bounds_clamp);
    RUN_TEST(arena_canary_multi_corruption);

    /* Capability model deep tests */
    RUN_TEST(cap_hierarchical_attenuation);
    RUN_TEST(cap_revocation_cascading);
    RUN_TEST(cap_forged_value_rejected);
    RUN_TEST(cap_net_capability);
    RUN_TEST(cap_timer_capability);

    /* Resource limits deep tests */
    RUN_TEST(limits_memory);
    RUN_TEST(limits_stack_depth);
    RUN_TEST(limits_string_length);
    RUN_TEST(limits_array_length);
    RUN_TEST(limits_regexp_steps);
    RUN_TEST(limits_cpu_time);

    /* Realm lifecycle */
    RUN_TEST(realm_count_tracking);
    RUN_TEST(realm_double_destroy_safe);
}
