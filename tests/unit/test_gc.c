/*
 * test_gc.c - Unit tests for r8e_gc.c (RC + Epoch Cycle Detection)
 *
 * Tests cover:
 *   - Retain/release basic lifecycle
 *   - Multiple retains followed by matching releases
 *   - Release at refcount 1 -> freed
 *   - Deferred release batching
 *   - Deferred deduplication
 *   - Cycle detection (A->B->A cycle)
 *   - No false positives (A->B linear chain)
 *   - Adaptive threshold
 *   - RC elision (BORROWED skips retain/release)
 *   - GC statistics tracking
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
 * Test Harness
 * ========================================================================= */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  %-60s ", #name); \
    name(); \
    printf("[PASS]\n"); \
    tests_passed++; \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        printf("[FAIL]\n    Expected %llu == %llu\n    at %s:%d\n", \
               (unsigned long long)_a, (unsigned long long)_b, \
               __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

/* =========================================================================
 * Types matching r8e_gc.c internal definitions
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_IS_POINTER(v) (((v) >> 48) == 0xFFF9U)

static inline void *r8e_get_pointer_test(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer_test(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* GC header flags layout (must match r8e_gc.c) */
#define R8E_GC_TIER_MASK         0x00000003u
#define R8E_GC_MARK              0x00000004u
#define R8E_GC_ON_SUSPECT        0x00000008u
#define R8E_GC_FROZEN            0x00000010u

#define R8E_GC_KIND_MASK         0x000000E0u
#define R8E_GC_KIND_SHIFT        5
#define R8E_GC_KIND_OBJECT       0u
#define R8E_GC_KIND_STRING       1u
#define R8E_GC_KIND_CLOSURE      2u
#define R8E_GC_KIND_ENVFRAME     3u
#define R8E_GC_KIND_ARRAY        4u

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* Minimal types for GC testing */
typedef struct {
    uint32_t key;
    uint8_t  desc;
    uint8_t  pad[3];
    R8EValue val;
} R8EPropSlot;

/* Tier 0 object */
typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint64_t  key0;
    uint64_t  val0;
} R8EObjTier0;

/* Suspect list */
typedef struct {
    R8EGCHeader **items;
    uint32_t      count;
    uint32_t      capacity;
} R8ESuspectList;

/* External RC table */
typedef struct {
    void    *entries;
    uint32_t count;
    uint32_t capacity;
} R8EExtRCTable;

typedef struct R8EGCState {
    R8ESuspectList  suspects;
    R8EExtRCTable   ext_rc;
    uint32_t        epoch_threshold;
    uint32_t        total_allocated;
    uint32_t        total_freed;
    uint32_t        cycles_collected;
} R8EGCState;

typedef struct R8EContext {
    R8EGCState gc;
} R8EContext;

/* RC elision constants */
#define R8E_RC_OWNED    0
#define R8E_RC_BORROWED 1
#define R8E_RC_TEMP     2

/* Forward declarations of functions from r8e_gc.c */
extern void r8e_gc_init(R8EContext *ctx);
extern void r8e_gc_destroy(R8EContext *ctx);
extern void r8e_retain(R8EContext *ctx, R8EValue val);
extern void r8e_release(R8EContext *ctx, R8EValue val);
extern void r8e_gc_collect(R8EContext *ctx);
extern uint32_t r8e_gc_alive_count(const R8EContext *ctx);
extern uint32_t r8e_gc_suspect_count(const R8EContext *ctx);
extern uint32_t r8e_gc_cycles_collected(const R8EContext *ctx);
extern uint32_t r8e_gc_epoch_threshold(const R8EContext *ctx);
extern void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind,
                                uint32_t proto_id);
extern void r8e_retain_if_owned(R8EContext *ctx, R8EValue val,
                                 uint8_t classification);
extern void r8e_release_if_owned(R8EContext *ctx, R8EValue val,
                                  uint8_t classification);

/* Deferred list */
typedef struct R8EDeferList {
    R8EValue  inline_buf[32];
    R8EValue *overflow;
    uint32_t  inline_count;
    uint32_t  overflow_count;
    uint32_t  overflow_capacity;
} R8EDeferList;

extern void r8e_defer_list_init(R8EDeferList *dl);
extern void r8e_defer_list_destroy(R8EDeferList *dl);
extern void r8e_defer_release(R8EDeferList *dl, R8EValue val);
extern void r8e_flush_deferred(R8EContext *ctx, R8EDeferList *dl);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static R8EContext *make_gc_ctx(void) {
    R8EContext *ctx = (R8EContext *)calloc(1, sizeof(R8EContext));
    r8e_gc_init(ctx);
    return ctx;
}

static void free_gc_ctx(R8EContext *ctx) {
    r8e_gc_destroy(ctx);
    free(ctx);
}

/* Helper to get inline refcount from a GC header */
static uint32_t get_inline_rc(R8EGCHeader *hdr) {
    return (hdr->flags & R8E_GC_RC_INLINE_MASK) >> R8E_GC_RC_INLINE_SHIFT;
}

/* Allocate a simple Tier 0 object with kind = OBJECT, refcount = 1 */
static R8EObjTier0 *alloc_obj(R8EContext *ctx) {
    R8EObjTier0 *obj = (R8EObjTier0 *)calloc(1, sizeof(R8EObjTier0));
    if (!obj) return NULL;
    r8e_gc_header_init((R8EGCHeader *)obj, R8E_GC_KIND_OBJECT, 0);
    ctx->gc.total_allocated++;
    return obj;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

static void test_retain_release_basic(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    ASSERT_NOT_NULL(obj);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 1);

    R8EValue val = r8e_from_pointer_test(obj);

    /* Retain: refcount 1 -> 2 */
    r8e_retain(ctx, val);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 2);

    /* Release: 2 -> 1 */
    r8e_release(ctx, val);
    /* Object still alive */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    /* Release: 1 -> 0 (freed) */
    r8e_release(ctx, val);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_multiple_retains(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    /* Retain 3 times: 1 -> 4 */
    r8e_retain(ctx, val);
    r8e_retain(ctx, val);
    r8e_retain(ctx, val);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 4);

    /* Release 3 times: 4 -> 1 */
    r8e_release(ctx, val);
    r8e_release(ctx, val);
    r8e_release(ctx, val);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    /* Final release -> freed */
    r8e_release(ctx, val);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_release_new_object_freed(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    /* New object starts at refcount 1, single release should free it */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);
    r8e_release(ctx, val);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_deferred_release_basic(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EDeferList dl;
    r8e_defer_list_init(&dl);

    /* Create 10 objects */
    R8EValue vals[10];
    for (int i = 0; i < 10; i++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        vals[i] = r8e_from_pointer_test(obj);
    }
    ASSERT_EQ(r8e_gc_alive_count(ctx), 10);

    /* Defer release of all 10 */
    for (int i = 0; i < 10; i++) {
        r8e_defer_release(&dl, vals[i]);
    }

    /* Objects still alive (deferred) */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 10);

    /* Flush -> all freed */
    r8e_flush_deferred(ctx, &dl);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    r8e_defer_list_destroy(&dl);
    free_gc_ctx(ctx);
}

static void test_deferred_dedup(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EDeferList dl;
    r8e_defer_list_init(&dl);

    /* Create one object, retain it 4 extra times -> refcount = 5 */
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);
    for (int i = 0; i < 4; i++)
        r8e_retain(ctx, val);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 5);

    /* Defer the same value 5 times */
    for (int i = 0; i < 5; i++)
        r8e_defer_release(&dl, val);

    /* Flush: should decrement 5 times total -> refcount 0 -> freed */
    r8e_flush_deferred(ctx, &dl);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    r8e_defer_list_destroy(&dl);
    free_gc_ctx(ctx);
}

static void test_cycle_detection(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Create two Tier 0 objects forming a cycle: A -> B -> A */
    R8EObjTier0 *a = alloc_obj(ctx);
    R8EObjTier0 *b = alloc_obj(ctx);

    R8EValue va = r8e_from_pointer_test(a);
    R8EValue vb = r8e_from_pointer_test(b);

    /* A's val0 points to B */
    a->key0 = 1;
    a->val0 = vb;
    r8e_retain(ctx, vb); /* B refcount: 1 -> 2 */

    /* B's val0 points to A */
    b->key0 = 1;
    b->val0 = va;
    r8e_retain(ctx, va); /* A refcount: 1 -> 2 */

    /* Now drop the external references (A and B each go from 2 -> 1) */
    r8e_release(ctx, va); /* A: 2 -> 1, goes to suspect list */
    r8e_release(ctx, vb); /* B: 2 -> 1, goes to suspect list */

    /* Both still alive (cycle keeps them alive) */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 2);

    /* Trigger cycle scan */
    r8e_gc_collect(ctx);

    /* Cycle should have been collected */
    ASSERT_EQ(r8e_gc_cycles_collected(ctx), 2);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_no_false_positives(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Create A -> B (no cycle). Release A; B should survive. */
    R8EObjTier0 *a = alloc_obj(ctx);
    R8EObjTier0 *b = alloc_obj(ctx);

    R8EValue va = r8e_from_pointer_test(a);
    R8EValue vb = r8e_from_pointer_test(b);

    /* A points to B */
    a->key0 = 1;
    a->val0 = vb;
    r8e_retain(ctx, vb); /* B refcount: 1 -> 2 */

    /* Keep an external ref to B */
    r8e_retain(ctx, vb); /* B refcount: 2 -> 3 */

    /* Release A: refcount 1 -> 0, A freed, B's refcount drops to 2 */
    r8e_release(ctx, va);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1); /* Only B alive */

    /* Trigger cycle scan just to be sure no false positive */
    r8e_gc_collect(ctx);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1); /* B still alive */

    /* Release remaining refs to B */
    r8e_release(ctx, vb);
    r8e_release(ctx, vb);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_adaptive_threshold_decrease(void) {
    R8EContext *ctx = make_gc_ctx();

    uint32_t initial_threshold = r8e_gc_epoch_threshold(ctx);
    ASSERT_EQ(initial_threshold, 4096);

    /* Create a cycle and collect it to trigger threshold decrease */
    R8EObjTier0 *a = alloc_obj(ctx);
    R8EObjTier0 *b = alloc_obj(ctx);
    R8EValue va = r8e_from_pointer_test(a);
    R8EValue vb = r8e_from_pointer_test(b);

    a->key0 = 1; a->val0 = vb; r8e_retain(ctx, vb);
    b->key0 = 1; b->val0 = va; r8e_retain(ctx, va);

    r8e_release(ctx, va);
    r8e_release(ctx, vb);

    r8e_gc_collect(ctx);

    /* Threshold should have decreased (halved) */
    uint32_t new_threshold = r8e_gc_epoch_threshold(ctx);
    ASSERT(new_threshold < initial_threshold);
    ASSERT_EQ(new_threshold, initial_threshold / 2);

    free_gc_ctx(ctx);
}

static void test_rc_elision_borrowed(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    uint32_t initial_rc = get_inline_rc((R8EGCHeader *)obj);

    /* BORROWED classification should skip retain */
    r8e_retain_if_owned(ctx, val, R8E_RC_BORROWED);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc);

    /* BORROWED classification should skip release */
    r8e_release_if_owned(ctx, val, R8E_RC_BORROWED);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc);

    /* TEMP classification should also skip */
    r8e_retain_if_owned(ctx, val, R8E_RC_TEMP);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc);

    /* OWNED should actually do RC */
    r8e_retain_if_owned(ctx, val, R8E_RC_OWNED);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc + 1);

    r8e_release_if_owned(ctx, val, R8E_RC_OWNED);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc);

    /* Cleanup */
    r8e_release(ctx, val);
    free_gc_ctx(ctx);
}

static void test_gc_stats_alive_count(void) {
    R8EContext *ctx = make_gc_ctx();

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    R8EObjTier0 *a = alloc_obj(ctx);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    R8EObjTier0 *b = alloc_obj(ctx);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 2);

    /* Release a */
    r8e_release(ctx, r8e_from_pointer_test(a));
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    /* Release b */
    r8e_release(ctx, r8e_from_pointer_test(b));
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_retain_release_non_pointer(void) {
    /* Retain/release on non-pointer values should be no-ops */
    R8EContext *ctx = make_gc_ctx();

    R8EValue int_val = 0xFFF8000000000042ULL; /* int32 */
    R8EValue undef_val = R8E_UNDEFINED;

    /* These should not crash or change anything */
    r8e_retain(ctx, int_val);
    r8e_release(ctx, int_val);
    r8e_retain(ctx, undef_val);
    r8e_release(ctx, undef_val);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_adaptive_threshold_increase(void) {
    R8EContext *ctx = make_gc_ctx();

    uint32_t initial = r8e_gc_epoch_threshold(ctx);

    /* Trigger a scan with no cycles -> threshold should increase */
    /* We need at least one suspect for the scan to run */
    R8EObjTier0 *a = alloc_obj(ctx);
    R8EObjTier0 *b = alloc_obj(ctx);
    R8EValue va = r8e_from_pointer_test(a);
    R8EValue vb = r8e_from_pointer_test(b);

    /* Give them extra refs so they are not freed on release */
    r8e_retain(ctx, va); /* 1 -> 2 */
    r8e_retain(ctx, vb); /* 1 -> 2 */

    /* Release once: 2 -> 1, goes to suspect */
    r8e_release(ctx, va);
    r8e_release(ctx, vb);

    /* Force scan */
    r8e_gc_collect(ctx);

    /* No cycles found -> threshold should have doubled */
    uint32_t after = r8e_gc_epoch_threshold(ctx);
    ASSERT_EQ(after, initial * 2);

    /* Cleanup */
    r8e_release(ctx, va);
    r8e_release(ctx, vb);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: RC increment/decrement correctness
 * ========================================================================= */

static void test_rc_increment_decrement_precise(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 1);

    /* Increment to 10 */
    for (int i = 0; i < 9; i++) {
        r8e_retain(ctx, val);
    }
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 10);

    /* Decrement back to 1 */
    for (int i = 0; i < 9; i++) {
        r8e_release(ctx, val);
    }
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    /* Final release frees it */
    r8e_release(ctx, val);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

static void test_rc_refcount_at_exact_boundaries(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    /* Test at refcount = 1 (just created) */
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 1);

    /* Increment to 2 and back to 1 */
    r8e_retain(ctx, val);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), 2);
    r8e_release(ctx, val);
    /* Object survives because rc went to 1, not 0 */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    r8e_release(ctx, val);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Cycle detection - A->B->C->A 3-node cycle
 * ========================================================================= */

static void test_cycle_detection_3_nodes(void) {
    R8EContext *ctx = make_gc_ctx();

    R8EObjTier0 *a = alloc_obj(ctx);
    R8EObjTier0 *b = alloc_obj(ctx);
    R8EObjTier0 *c = alloc_obj(ctx);

    R8EValue va = r8e_from_pointer_test(a);
    R8EValue vb = r8e_from_pointer_test(b);
    R8EValue vc = r8e_from_pointer_test(c);

    /* A -> B */
    a->key0 = 1; a->val0 = vb;
    r8e_retain(ctx, vb);

    /* B -> C */
    b->key0 = 1; b->val0 = vc;
    r8e_retain(ctx, vc);

    /* C -> A */
    c->key0 = 1; c->val0 = va;
    r8e_retain(ctx, va);

    /* Drop external refs (each goes from 2 -> 1) */
    r8e_release(ctx, va);
    r8e_release(ctx, vb);
    r8e_release(ctx, vc);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 3);

    /* Cycle scan should collect all 3 */
    r8e_gc_collect(ctx);

    ASSERT_EQ(r8e_gc_cycles_collected(ctx), 3);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Self-referencing object
 * ========================================================================= */

static void test_self_referencing_object(void) {
    R8EContext *ctx = make_gc_ctx();

    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    /* Object points to itself */
    obj->key0 = 1;
    obj->val0 = val;
    r8e_retain(ctx, val); /* refcount: 1 -> 2 */

    /* Drop external reference: 2 -> 1 */
    r8e_release(ctx, val);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    /* Cycle scan should collect the self-referencing object */
    r8e_gc_collect(ctx);

    ASSERT_EQ(r8e_gc_cycles_collected(ctx), 1);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Suspect list behavior
 * ========================================================================= */

static void test_suspect_list_add_on_decrement(void) {
    R8EContext *ctx = make_gc_ctx();

    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    /* Give it an extra ref: 1 -> 2 */
    r8e_retain(ctx, val);

    /* Release: 2 -> 1, should go on suspect list */
    r8e_release(ctx, val);

    ASSERT(r8e_gc_suspect_count(ctx) > 0);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1);

    /* Cleanup */
    r8e_release(ctx, val);

    free_gc_ctx(ctx);
}

static void test_suspect_not_added_for_immediate_free(void) {
    R8EContext *ctx = make_gc_ctx();

    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    uint32_t suspects_before = r8e_gc_suspect_count(ctx);

    /* Single release from refcount 1 -> 0: freed immediately, no suspect */
    r8e_release(ctx, val);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);
    /* Suspect count should not have increased for immediately freed objects */
    ASSERT_EQ(r8e_gc_suspect_count(ctx), suspects_before);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Deferred decrement - overflow to heap buffer
 * ========================================================================= */

static void test_deferred_overflow_to_heap(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EDeferList dl;
    r8e_defer_list_init(&dl);

    /* Create more than 32 objects (inline capacity) to test overflow */
    R8EValue vals[50];
    for (int i = 0; i < 50; i++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        vals[i] = r8e_from_pointer_test(obj);
    }
    ASSERT_EQ(r8e_gc_alive_count(ctx), 50);

    /* Defer all 50 */
    for (int i = 0; i < 50; i++) {
        r8e_defer_release(&dl, vals[i]);
    }

    /* 32 go in inline buffer, 18 overflow to heap */
    ASSERT_EQ(dl.inline_count, 32);
    ASSERT_EQ(dl.overflow_count, 18);

    /* Flush should free all 50 */
    r8e_flush_deferred(ctx, &dl);
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    r8e_defer_list_destroy(&dl);
    free_gc_ctx(ctx);
}

static void test_deferred_ignores_non_pointers(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EDeferList dl;
    r8e_defer_list_init(&dl);

    /* Defer non-pointer values */
    r8e_defer_release(&dl, R8E_UNDEFINED);
    r8e_defer_release(&dl, 0xFFF8000000000042ULL); /* int32 */

    /* Non-pointers should be ignored */
    ASSERT_EQ(dl.inline_count, 0);

    r8e_defer_list_destroy(&dl);
    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Adaptive threshold details
 * ========================================================================= */

static void test_adaptive_threshold_minimum_256(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Force many cycles to halve the threshold repeatedly */
    for (int round = 0; round < 10; round++) {
        R8EObjTier0 *a = alloc_obj(ctx);
        R8EObjTier0 *b = alloc_obj(ctx);
        R8EValue va = r8e_from_pointer_test(a);
        R8EValue vb = r8e_from_pointer_test(b);

        a->key0 = 1; a->val0 = vb; r8e_retain(ctx, vb);
        b->key0 = 1; b->val0 = va; r8e_retain(ctx, va);

        r8e_release(ctx, va);
        r8e_release(ctx, vb);

        r8e_gc_collect(ctx);
    }

    /* Threshold should not go below 256 */
    uint32_t threshold = r8e_gc_epoch_threshold(ctx);
    ASSERT(threshold >= 256);

    free_gc_ctx(ctx);
}

static void test_adaptive_threshold_maximum_65536(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Repeatedly scan with no cycles to double the threshold */
    for (int round = 0; round < 20; round++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        R8EValue val = r8e_from_pointer_test(obj);
        r8e_retain(ctx, val); /* 1 -> 2 */
        r8e_release(ctx, val); /* 2 -> 1, goes to suspect */

        r8e_gc_collect(ctx);

        r8e_release(ctx, val); /* cleanup */
    }

    /* Threshold should not exceed 65536 */
    uint32_t threshold = r8e_gc_epoch_threshold(ctx);
    ASSERT(threshold <= 65536);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Large object graph
 * ========================================================================= */

static void test_large_graph_alloc_free(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Allocate 1000 independent objects */
    R8EValue vals[1000];
    for (int i = 0; i < 1000; i++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        ASSERT_NOT_NULL(obj);
        vals[i] = r8e_from_pointer_test(obj);
    }
    ASSERT_EQ(r8e_gc_alive_count(ctx), 1000);

    /* Free all 1000 */
    for (int i = 0; i < 1000; i++) {
        r8e_release(ctx, vals[i]);
    }
    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Stress rapid alloc/free cycles
 * ========================================================================= */

static void test_stress_rapid_alloc_free(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Rapid alloc/free: 500 rounds */
    for (int i = 0; i < 500; i++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        R8EValue val = r8e_from_pointer_test(obj);
        r8e_release(ctx, val);
    }

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: GC header init correctness
 * ========================================================================= */

static void test_gc_header_init_correctness(void) {
    R8EContext *ctx = make_gc_ctx();

    R8EGCHeader hdr;
    r8e_gc_header_init(&hdr, R8E_GC_KIND_OBJECT, 5);

    /* Refcount should be 1 */
    ASSERT_EQ(get_inline_rc(&hdr), 1);

    /* Kind should be OBJECT */
    uint32_t kind = (hdr.flags & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT;
    ASSERT_EQ(kind, R8E_GC_KIND_OBJECT);

    /* Proto_id should be 5 */
    ASSERT_EQ(hdr.proto_id, 5);

    /* No mark, no suspect, no frozen */
    ASSERT(!(hdr.flags & R8E_GC_MARK));
    ASSERT(!(hdr.flags & R8E_GC_ON_SUSPECT));
    ASSERT(!(hdr.flags & R8E_GC_FROZEN));

    free_gc_ctx(ctx);
}

static void test_gc_header_init_various_kinds(void) {
    R8EGCHeader hdr;

    r8e_gc_header_init(&hdr, R8E_GC_KIND_STRING, 0);
    uint32_t k = (hdr.flags & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT;
    ASSERT_EQ(k, R8E_GC_KIND_STRING);

    r8e_gc_header_init(&hdr, R8E_GC_KIND_CLOSURE, 2);
    k = (hdr.flags & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT;
    ASSERT_EQ(k, R8E_GC_KIND_CLOSURE);

    r8e_gc_header_init(&hdr, R8E_GC_KIND_ENVFRAME, 0);
    k = (hdr.flags & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT;
    ASSERT_EQ(k, R8E_GC_KIND_ENVFRAME);
}

/* =========================================================================
 * NEW TESTS: Deferred list fast path (<=4 entries)
 * ========================================================================= */

static void test_deferred_fast_path_few_entries(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EDeferList dl;
    r8e_defer_list_init(&dl);

    /* Create 3 objects and defer (hits fast path: total <= 4) */
    R8EValue vals[3];
    for (int i = 0; i < 3; i++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        vals[i] = r8e_from_pointer_test(obj);
    }

    for (int i = 0; i < 3; i++)
        r8e_defer_release(&dl, vals[i]);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 3);

    r8e_flush_deferred(ctx, &dl);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    r8e_defer_list_destroy(&dl);
    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: Cycle with mixed no-cycle external refs
 * ========================================================================= */

static void test_cycle_with_external_ref_survives(void) {
    R8EContext *ctx = make_gc_ctx();

    /* A <-> B cycle, but C has external ref and points to A */
    R8EObjTier0 *a = alloc_obj(ctx);
    R8EObjTier0 *b = alloc_obj(ctx);
    R8EObjTier0 *c = alloc_obj(ctx);

    R8EValue va = r8e_from_pointer_test(a);
    R8EValue vb = r8e_from_pointer_test(b);
    R8EValue vc = r8e_from_pointer_test(c);

    /* A -> B */
    a->key0 = 1; a->val0 = vb;
    r8e_retain(ctx, vb);

    /* B -> A */
    b->key0 = 1; b->val0 = va;
    r8e_retain(ctx, va);

    /* C -> A (C has external ref via vc, so A and B are reachable) */
    c->key0 = 1; c->val0 = va;
    r8e_retain(ctx, va);

    /* Drop external refs to A and B */
    r8e_release(ctx, va); /* A: 3 -> 2 */
    r8e_release(ctx, vb); /* B: 2 -> 1 */

    /* C still holds external ref */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 3);

    /* Cycle scan: A and B are reachable through C, should NOT be collected */
    r8e_gc_collect(ctx);

    /* All 3 should survive */
    ASSERT_EQ(r8e_gc_alive_count(ctx), 3);
    ASSERT_EQ(r8e_gc_cycles_collected(ctx), 0);

    /* Now release C: the A<->B cycle becomes unreachable */
    r8e_release(ctx, vc);

    /* C freed immediately (rc -> 0). A and B still in cycle. */
    /* But A's ref from C was decremented, so need to trigger scan */
    r8e_gc_collect(ctx);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: RC elision TEMP classification
 * ========================================================================= */

static void test_rc_elision_temp(void) {
    R8EContext *ctx = make_gc_ctx();
    R8EObjTier0 *obj = alloc_obj(ctx);
    R8EValue val = r8e_from_pointer_test(obj);

    uint32_t initial_rc = get_inline_rc((R8EGCHeader *)obj);

    /* TEMP classification should skip both retain and release */
    r8e_retain_if_owned(ctx, val, R8E_RC_TEMP);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc);

    r8e_release_if_owned(ctx, val, R8E_RC_TEMP);
    ASSERT_EQ(get_inline_rc((R8EGCHeader *)obj), initial_rc);

    /* Cleanup */
    r8e_release(ctx, val);
    free_gc_ctx(ctx);
}

/* =========================================================================
 * NEW TESTS: GC statistics tracking
 * ========================================================================= */

static void test_gc_stats_total_allocated_freed(void) {
    R8EContext *ctx = make_gc_ctx();

    /* Allocate 5 objects */
    R8EValue vals[5];
    for (int i = 0; i < 5; i++) {
        R8EObjTier0 *obj = alloc_obj(ctx);
        vals[i] = r8e_from_pointer_test(obj);
    }

    ASSERT_EQ(r8e_gc_alive_count(ctx), 5);

    /* Free 3 of them */
    for (int i = 0; i < 3; i++) {
        r8e_release(ctx, vals[i]);
    }

    ASSERT_EQ(r8e_gc_alive_count(ctx), 2);

    /* Free remaining 2 */
    r8e_release(ctx, vals[3]);
    r8e_release(ctx, vals[4]);

    ASSERT_EQ(r8e_gc_alive_count(ctx), 0);

    free_gc_ctx(ctx);
}

/* =========================================================================
 * Test Runner
 * ========================================================================= */

void run_gc_tests(void) {
    printf("\n=== GC Tests ===\n");

    /* Original tests */
    RUN_TEST(test_retain_release_basic);
    RUN_TEST(test_multiple_retains);
    RUN_TEST(test_release_new_object_freed);
    RUN_TEST(test_deferred_release_basic);
    RUN_TEST(test_deferred_dedup);
    RUN_TEST(test_cycle_detection);
    RUN_TEST(test_no_false_positives);
    RUN_TEST(test_adaptive_threshold_decrease);
    RUN_TEST(test_adaptive_threshold_increase);
    RUN_TEST(test_rc_elision_borrowed);
    RUN_TEST(test_gc_stats_alive_count);
    RUN_TEST(test_retain_release_non_pointer);

    /* NEW: RC correctness */
    RUN_TEST(test_rc_increment_decrement_precise);
    RUN_TEST(test_rc_refcount_at_exact_boundaries);

    /* NEW: Cycle detection variations */
    RUN_TEST(test_cycle_detection_3_nodes);
    RUN_TEST(test_self_referencing_object);
    RUN_TEST(test_cycle_with_external_ref_survives);

    /* NEW: Suspect list */
    RUN_TEST(test_suspect_list_add_on_decrement);
    RUN_TEST(test_suspect_not_added_for_immediate_free);

    /* NEW: Deferred decrement */
    RUN_TEST(test_deferred_overflow_to_heap);
    RUN_TEST(test_deferred_ignores_non_pointers);
    RUN_TEST(test_deferred_fast_path_few_entries);

    /* NEW: Adaptive threshold */
    RUN_TEST(test_adaptive_threshold_minimum_256);
    RUN_TEST(test_adaptive_threshold_maximum_65536);

    /* NEW: Large graphs and stress */
    RUN_TEST(test_large_graph_alloc_free);
    RUN_TEST(test_stress_rapid_alloc_free);

    /* NEW: GC header */
    RUN_TEST(test_gc_header_init_correctness);
    RUN_TEST(test_gc_header_init_various_kinds);

    /* NEW: RC elision TEMP */
    RUN_TEST(test_rc_elision_temp);

    /* NEW: Stats */
    RUN_TEST(test_gc_stats_total_allocated_freed);

    printf("\n  GC Tests: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf("\n");
}

#ifdef TEST_GC_MAIN
int main(void) {
    run_gc_tests();
    return tests_failed > 0 ? 1 : 0;
}
#endif
