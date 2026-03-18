/*
 * test_alloc.c - Unit tests for r8e_alloc.c
 *
 * Tests arena allocator, slab allocator, memory tracker, stack canaries,
 * and NaN-box validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* NaN-boxing definitions needed for r8e_value_validate */
typedef uint64_t R8EValue;
#define R8E_IS_DOUBLE(v)      ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)       (((v) >> 48) == 0xFFF8U)

static inline double r8e_get_double(R8EValue v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline R8EValue r8e_from_double(double d) {
    R8EValue v; memcpy(&v, &d, 8); return v;
}
static inline R8EValue r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

/* Forward declarations for r8e_alloc.c types and functions */
typedef struct R8EArena R8EArena;
typedef struct R8ESlab R8ESlab;
typedef struct R8EMemTracker R8EMemTracker;

extern R8EArena *r8e_arena_create(size_t size);
extern void     *r8e_arena_alloc(R8EArena *arena, size_t size);
extern void     *r8e_arena_alloc_unzeroed(R8EArena *arena, size_t size);
extern void      r8e_arena_reset(R8EArena *arena);
extern void      r8e_arena_destroy(R8EArena *arena);
extern size_t    r8e_arena_used(const R8EArena *arena);
extern size_t    r8e_arena_available(const R8EArena *arena);
extern size_t    r8e_arena_size(const R8EArena *arena);
extern size_t    r8e_arena_peak(const R8EArena *arena);
extern uint32_t  r8e_arena_alloc_count(const R8EArena *arena);
extern bool      r8e_arena_contains(const R8EArena *arena, const void *ptr);

extern void      r8e_slab_init(R8ESlab *slab, uint32_t obj_size);
extern void     *r8e_slab_alloc(R8ESlab *slab);
extern void      r8e_slab_free(R8ESlab *slab, void *ptr);
extern bool      r8e_slab_contains(const R8ESlab *slab, const void *ptr);
extern void      r8e_slab_destroy(R8ESlab *slab);
extern uint32_t  r8e_slab_current_count(const R8ESlab *slab);
extern uint32_t  r8e_slab_peak_count(const R8ESlab *slab);
extern uint32_t  r8e_slab_obj_size(const R8ESlab *slab);

extern void      r8e_mem_tracker_init(R8EMemTracker *tracker, size_t limit);
extern bool      r8e_mem_tracker_alloc(R8EMemTracker *tracker, size_t size);
extern void      r8e_mem_tracker_free(R8EMemTracker *tracker, size_t size);
extern size_t    r8e_mem_tracker_current(const R8EMemTracker *tracker);
extern size_t    r8e_mem_tracker_peak(const R8EMemTracker *tracker);

extern void      r8e_canary_write(void *ptr);
extern bool      r8e_canary_check(const void *ptr);

extern bool      r8e_value_validate(R8EValue v);

/* Slab struct definition - must match r8e_alloc.c */
typedef struct R8ESlabFreeNode { struct R8ESlabFreeNode *next; } R8ESlabFreeNode;
typedef struct R8ESlabBlock {
    struct R8ESlabBlock *next_block;
    uint8_t *data;
    uint32_t capacity;
    uint32_t obj_size;
} R8ESlabBlock;
struct R8ESlab {
    R8ESlabFreeNode *free_list;
    R8ESlabBlock    *blocks;
    uint32_t obj_size;
    uint32_t obj_size_aligned;
    uint32_t total_allocs;
    uint32_t total_frees;
    uint32_t current_count;
    uint32_t peak_count;
    uint32_t block_count;
};

/* MemTracker struct - must match r8e_alloc.c */
struct R8EMemTracker {
    size_t   current;
    size_t   peak;
    size_t   limit;
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_bytes;
};

/* Test infrastructure */
extern int g_assert_fail;
#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do {                                     \
    g_assert_fail = 0; extern int g_tests_run, g_tests_passed, g_tests_failed; \
    g_tests_run++;                                              \
    printf("  %-60s ", #name);                                  \
    test_##name();                                              \
    if (g_assert_fail) { g_tests_failed++; printf("FAIL\n"); } \
    else { g_tests_passed++; printf("ok\n"); }                  \
} while (0)

#define ASSERT_TRUE(expr)  do { if (!(expr)) { fprintf(stderr, "    ASSERT_TRUE(%s) failed at %s:%d\n", #expr, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_FALSE(expr) do { if (expr) { fprintf(stderr, "    ASSERT_FALSE(%s) failed at %s:%d\n", #expr, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ(a, b)    do { if ((uint64_t)(a) != (uint64_t)(b)) { fprintf(stderr, "    ASSERT_EQ(%s, %s) failed: %llu vs %llu at %s:%d\n", #a, #b, (unsigned long long)(uint64_t)(a), (unsigned long long)(uint64_t)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_NE(a, b)    do { if ((uint64_t)(a) == (uint64_t)(b)) { fprintf(stderr, "    ASSERT_NE(%s, %s) failed at %s:%d\n", #a, #b, __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)
#define ASSERT_EQ_INT(a, b) do { if ((long long)(a) != (long long)(b)) { fprintf(stderr, "    ASSERT_EQ_INT(%s, %s): %lld vs %lld at %s:%d\n", #a, #b, (long long)(a), (long long)(b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)

/* =========================================================================
 * Arena creation and destruction
 * ========================================================================= */

TEST(arena_create_default) {
    R8EArena *arena = r8e_arena_create(0);
    ASSERT_TRUE(arena != NULL);
    /* Default is 128KB, arena size should be >= 128KB after page alignment */
    ASSERT_TRUE(r8e_arena_size(arena) >= 128 * 1024);
    ASSERT_EQ_INT(r8e_arena_used(arena), 0);
    ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 0);
    r8e_arena_destroy(arena);
}

TEST(arena_create_custom_size) {
    R8EArena *arena = r8e_arena_create(8192);
    ASSERT_TRUE(arena != NULL);
    ASSERT_TRUE(r8e_arena_size(arena) >= 8192);
    r8e_arena_destroy(arena);
}

TEST(arena_create_min_size) {
    /* Should clamp to minimum 4KB */
    R8EArena *arena = r8e_arena_create(100);
    ASSERT_TRUE(arena != NULL);
    ASSERT_TRUE(r8e_arena_size(arena) >= 4096);
    r8e_arena_destroy(arena);
}

TEST(arena_destroy_null) {
    /* Should not crash */
    r8e_arena_destroy(NULL);
}

/* =========================================================================
 * Arena allocation
 * ========================================================================= */

TEST(arena_alloc_basic) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    void *p1 = r8e_arena_alloc(arena, 100);
    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(r8e_arena_used(arena) >= 100);
    ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 1);

    void *p2 = r8e_arena_alloc(arena, 200);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p2 != p1);  /* different pointers */
    ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 2);

    r8e_arena_destroy(arena);
}

TEST(arena_alloc_alignment) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    /* Odd-sized alloc should still return 8-byte aligned pointer */
    void *p1 = r8e_arena_alloc(arena, 3);
    ASSERT_TRUE(p1 != NULL);
    ASSERT_EQ((uintptr_t)p1 % 8, 0);

    void *p2 = r8e_arena_alloc(arena, 5);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_EQ((uintptr_t)p2 % 8, 0);

    void *p3 = r8e_arena_alloc(arena, 13);
    ASSERT_TRUE(p3 != NULL);
    ASSERT_EQ((uintptr_t)p3 % 8, 0);

    r8e_arena_destroy(arena);
}

TEST(arena_alloc_zeroed) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    uint8_t *p = (uint8_t *)r8e_arena_alloc(arena, 64);
    ASSERT_TRUE(p != NULL);

    /* Verify memory is zeroed */
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ_INT(p[i], 0);
    }

    r8e_arena_destroy(arena);
}

TEST(arena_alloc_zero_size) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    void *p = r8e_arena_alloc(arena, 0);
    ASSERT_TRUE(p == NULL);

    r8e_arena_destroy(arena);
}

TEST(arena_contains) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    void *p = r8e_arena_alloc(arena, 32);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(r8e_arena_contains(arena, p));

    int stack_var;
    ASSERT_FALSE(r8e_arena_contains(arena, &stack_var));
    ASSERT_FALSE(r8e_arena_contains(arena, NULL));

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena reset
 * ========================================================================= */

TEST(arena_reset) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    r8e_arena_alloc(arena, 100);
    r8e_arena_alloc(arena, 200);
    ASSERT_TRUE(r8e_arena_used(arena) > 0);
    size_t peak = r8e_arena_peak(arena);

    r8e_arena_reset(arena);
    ASSERT_EQ_INT(r8e_arena_used(arena), 0);
    ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 0);
    /* Peak should be preserved (not reset) - actually reset does reset alloc_count */
    ASSERT_TRUE(r8e_arena_peak(arena) == peak);

    /* Can allocate again after reset */
    void *p = r8e_arena_alloc(arena, 50);
    ASSERT_TRUE(p != NULL);

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena overflow
 * ========================================================================= */

TEST(arena_overflow) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    /* Fill the arena */
    size_t avail = r8e_arena_available(arena);
    void *p = r8e_arena_alloc(arena, avail);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ_INT(r8e_arena_available(arena), 0);

    /* Further allocation should return NULL */
    void *p2 = r8e_arena_alloc(arena, 8);
    ASSERT_TRUE(p2 == NULL);

    r8e_arena_destroy(arena);
}

TEST(arena_overflow_large) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    /* Request more than the arena capacity */
    void *p = r8e_arena_alloc(arena, r8e_arena_size(arena) + 1);
    ASSERT_TRUE(p == NULL);

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Slab allocator
 * ========================================================================= */

TEST(slab_init) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);
    ASSERT_EQ_INT(r8e_slab_obj_size(&slab), 24);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 0);
    ASSERT_EQ_INT(r8e_slab_peak_count(&slab), 0);
    r8e_slab_destroy(&slab);
}

TEST(slab_alloc_free) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    void *p1 = r8e_slab_alloc(&slab);
    ASSERT_TRUE(p1 != NULL);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 1);

    void *p2 = r8e_slab_alloc(&slab);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p2 != p1);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 2);

    r8e_slab_free(&slab, p1);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 1);

    r8e_slab_free(&slab, p2);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 0);

    r8e_slab_destroy(&slab);
}

TEST(slab_reuse) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    void *p1 = r8e_slab_alloc(&slab);
    r8e_slab_free(&slab, p1);

    /* Freed block should be reused */
    void *p2 = r8e_slab_alloc(&slab);
    ASSERT_TRUE(p2 == p1);

    r8e_slab_destroy(&slab);
}

TEST(slab_peak_tracking) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = r8e_slab_alloc(&slab);
        ASSERT_TRUE(ptrs[i] != NULL);
    }
    ASSERT_EQ_INT(r8e_slab_peak_count(&slab), 10);

    for (int i = 0; i < 10; i++) {
        r8e_slab_free(&slab, ptrs[i]);
    }
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 0);
    ASSERT_EQ_INT(r8e_slab_peak_count(&slab), 10);

    r8e_slab_destroy(&slab);
}

TEST(slab_contains) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    void *p = r8e_slab_alloc(&slab);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(r8e_slab_contains(&slab, p));

    int stack_var;
    ASSERT_FALSE(r8e_slab_contains(&slab, &stack_var));

    r8e_slab_destroy(&slab);
}

TEST(slab_many_allocs) {
    R8ESlab slab;
    r8e_slab_init(&slab, 72);

    /* Allocate more than one block (block_count = 64) */
    void *ptrs[128];
    for (int i = 0; i < 128; i++) {
        ptrs[i] = r8e_slab_alloc(&slab);
        ASSERT_TRUE(ptrs[i] != NULL);
    }
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 128);

    for (int i = 0; i < 128; i++) {
        r8e_slab_free(&slab, ptrs[i]);
    }
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 0);

    r8e_slab_destroy(&slab);
}

/* =========================================================================
 * Memory tracker
 * ========================================================================= */

TEST(mem_tracker_basic) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);  /* unlimited */

    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 0);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 0);

    ASSERT_TRUE(r8e_mem_tracker_alloc(&tracker, 100));
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 100);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 100);

    ASSERT_TRUE(r8e_mem_tracker_alloc(&tracker, 50));
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 150);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 150);

    r8e_mem_tracker_free(&tracker, 100);
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 50);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 150);  /* peak unchanged */
}

TEST(mem_tracker_limit) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 200);

    ASSERT_TRUE(r8e_mem_tracker_alloc(&tracker, 100));
    ASSERT_TRUE(r8e_mem_tracker_alloc(&tracker, 50));

    /* This should fail: 100 + 50 + 100 = 250 > 200 */
    ASSERT_FALSE(r8e_mem_tracker_alloc(&tracker, 100));
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 150);

    /* Free some, then the alloc should succeed */
    r8e_mem_tracker_free(&tracker, 100);
    ASSERT_TRUE(r8e_mem_tracker_alloc(&tracker, 100));
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 150);
}

TEST(mem_tracker_underflow_clamp) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);

    r8e_mem_tracker_alloc(&tracker, 50);
    /* Free more than allocated: should clamp to 0 */
    r8e_mem_tracker_free(&tracker, 100);
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 0);
}

/* =========================================================================
 * Stack canaries
 * ========================================================================= */

TEST(canary_write_check) {
    uint64_t buf[2] = {0, 0};
    r8e_canary_write(&buf[0]);
    ASSERT_TRUE(r8e_canary_check(&buf[0]));

    /* Corrupted canary should fail */
    buf[0] = 0x1234;
    ASSERT_FALSE(r8e_canary_check(&buf[0]));

    /* NULL should fail */
    ASSERT_FALSE(r8e_canary_check(NULL));
}

/* =========================================================================
 * NaN-box validation
 * ========================================================================= */

TEST(value_validate) {
    /* Valid doubles */
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(0.0)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(1.5)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(-1e100)));
    /* Canonical NaN is a valid double encoding */
    ASSERT_TRUE(r8e_value_validate(0x7FF8000000000000ULL));

    /* Valid int32 */
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(0)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(42)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(-1)));

    /* Valid specials */
    ASSERT_TRUE(r8e_value_validate(0xFFFA000000000000ULL)); /* undefined */
    ASSERT_TRUE(r8e_value_validate(0xFFFA000000000001ULL)); /* null */
    ASSERT_TRUE(r8e_value_validate(0xFFFA000000000002ULL)); /* true */
    ASSERT_TRUE(r8e_value_validate(0xFFFA000000000003ULL)); /* false */

    /* Invalid special payload */
    ASSERT_FALSE(r8e_value_validate(0xFFFA000000000004ULL));

    /* Invalid tag: 0xFFFE and 0xFFFF are reserved */
    ASSERT_FALSE(r8e_value_validate(0xFFFE000000000000ULL));
    ASSERT_FALSE(r8e_value_validate(0xFFFF000000000000ULL));
}

/* =========================================================================
 * Arena: unzeroed allocation
 * ========================================================================= */

TEST(arena_alloc_unzeroed_basic) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    void *p = r8e_arena_alloc_unzeroed(arena, 64);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(r8e_arena_used(arena) >= 64);
    ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 1);

    /* Writing should be fine */
    memset(p, 0xAA, 64);

    r8e_arena_destroy(arena);
}

TEST(arena_alloc_unzeroed_null_arena) {
    void *p = r8e_arena_alloc_unzeroed(NULL, 64);
    ASSERT_TRUE(p == NULL);
}

TEST(arena_alloc_unzeroed_zero_size) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    void *p = r8e_arena_alloc_unzeroed(arena, 0);
    ASSERT_TRUE(p == NULL);

    r8e_arena_destroy(arena);
}

TEST(arena_alloc_unzeroed_overflow) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    /* Exhaust the arena */
    size_t avail = r8e_arena_available(arena);
    void *p1 = r8e_arena_alloc_unzeroed(arena, avail);
    ASSERT_TRUE(p1 != NULL);

    /* Further unzeroed allocation should return NULL */
    void *p2 = r8e_arena_alloc_unzeroed(arena, 8);
    ASSERT_TRUE(p2 == NULL);

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena: fill-to-capacity
 * ========================================================================= */

TEST(arena_fill_to_capacity) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    /* Allocate in small chunks until we fill the arena */
    int alloc_count = 0;
    while (r8e_arena_available(arena) >= 8) {
        void *p = r8e_arena_alloc(arena, 8);
        ASSERT_TRUE(p != NULL);
        alloc_count++;
    }
    ASSERT_TRUE(alloc_count > 0);
    ASSERT_TRUE(r8e_arena_available(arena) < 8);

    /* Arena is full; next allocation must fail */
    if (r8e_arena_available(arena) == 0) {
        void *p = r8e_arena_alloc(arena, 1);
        ASSERT_TRUE(p == NULL);
    }

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena: peak tracking
 * ========================================================================= */

TEST(arena_peak_tracking) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    r8e_arena_alloc(arena, 100);
    ASSERT_TRUE(r8e_arena_peak(arena) >= 100);

    r8e_arena_alloc(arena, 200);
    size_t peak_after_two = r8e_arena_peak(arena);
    ASSERT_TRUE(peak_after_two >= 300);

    /* Reset - peak should be preserved */
    r8e_arena_reset(arena);
    ASSERT_EQ_INT(r8e_arena_used(arena), 0);
    ASSERT_TRUE(r8e_arena_peak(arena) == peak_after_two);

    /* Allocating less than peak should not change peak */
    r8e_arena_alloc(arena, 50);
    ASSERT_TRUE(r8e_arena_peak(arena) == peak_after_two);

    /* Allocating past previous peak should raise it */
    r8e_arena_alloc(arena, peak_after_two);
    ASSERT_TRUE(r8e_arena_peak(arena) > peak_after_two);

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena: multiple resets
 * ========================================================================= */

TEST(arena_multiple_resets) {
    R8EArena *arena = r8e_arena_create(4096);
    ASSERT_TRUE(arena != NULL);

    for (int round = 0; round < 5; round++) {
        void *p1 = r8e_arena_alloc(arena, 64);
        ASSERT_TRUE(p1 != NULL);
        void *p2 = r8e_arena_alloc(arena, 128);
        ASSERT_TRUE(p2 != NULL);
        ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 2);

        r8e_arena_reset(arena);
        ASSERT_EQ_INT(r8e_arena_used(arena), 0);
        ASSERT_EQ_INT(r8e_arena_alloc_count(arena), 0);
    }

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena: null parameter handling
 * ========================================================================= */

TEST(arena_null_arena_alloc) {
    void *p = r8e_arena_alloc(NULL, 64);
    ASSERT_TRUE(p == NULL);
}

TEST(arena_null_arena_reset) {
    /* Should not crash */
    r8e_arena_reset(NULL);
}

TEST(arena_null_arena_stats) {
    ASSERT_EQ_INT(r8e_arena_used(NULL), 0);
    ASSERT_EQ_INT(r8e_arena_available(NULL), 0);
    ASSERT_EQ_INT(r8e_arena_size(NULL), 0);
    ASSERT_EQ_INT(r8e_arena_peak(NULL), 0);
    ASSERT_EQ_INT(r8e_arena_alloc_count(NULL), 0);
    ASSERT_FALSE(r8e_arena_contains(NULL, NULL));
}

/* =========================================================================
 * Arena: alignment for various sizes
 * ========================================================================= */

TEST(arena_alignment_various_sizes) {
    R8EArena *arena = r8e_arena_create(8192);
    ASSERT_TRUE(arena != NULL);

    /* Allocate with sizes 1-32 and check 8-byte alignment */
    for (size_t sz = 1; sz <= 32; sz++) {
        void *p = r8e_arena_alloc(arena, sz);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ((uintptr_t)p % 8, 0);
    }

    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Arena: maximum size clamping
 * ========================================================================= */

TEST(arena_max_size_clamp) {
    /* Request larger than R8E_ARENA_MAX_SIZE (16MB) */
    R8EArena *arena = r8e_arena_create(32 * 1024 * 1024);
    ASSERT_TRUE(arena != NULL);
    /* Should be clamped to at most 16MB + page alignment */
    ASSERT_TRUE(r8e_arena_size(arena) <= 16 * 1024 * 1024 + 65536);
    r8e_arena_destroy(arena);
}

/* =========================================================================
 * Slab: alignment verification
 * ========================================================================= */

TEST(slab_alignment_check) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    void *ptrs[32];
    for (int i = 0; i < 32; i++) {
        ptrs[i] = r8e_slab_alloc(&slab);
        ASSERT_TRUE(ptrs[i] != NULL);
        /* Every slab allocation must be 8-byte aligned */
        ASSERT_EQ((uintptr_t)ptrs[i] % 8, 0);
    }

    for (int i = 0; i < 32; i++) {
        r8e_slab_free(&slab, ptrs[i]);
    }
    r8e_slab_destroy(&slab);
}

/* =========================================================================
 * Slab: minimum size clamped to free-list node
 * ========================================================================= */

TEST(slab_min_size_clamped) {
    R8ESlab slab;
    /* Request size smaller than sizeof(R8ESlabFreeNode) */
    r8e_slab_init(&slab, 1);
    /* obj_size should be clamped up to at least sizeof(pointer) */
    ASSERT_TRUE(r8e_slab_obj_size(&slab) >= sizeof(void *));

    /* Should still be able to alloc and free */
    void *p = r8e_slab_alloc(&slab);
    ASSERT_TRUE(p != NULL);
    r8e_slab_free(&slab, p);

    r8e_slab_destroy(&slab);
}

/* =========================================================================
 * Slab: null parameter handling
 * ========================================================================= */

TEST(slab_null_alloc) {
    void *p = r8e_slab_alloc(NULL);
    ASSERT_TRUE(p == NULL);
}

TEST(slab_null_free) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);
    /* Should not crash */
    r8e_slab_free(&slab, NULL);
    r8e_slab_free(NULL, NULL);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 0);
    r8e_slab_destroy(&slab);
}

TEST(slab_null_contains) {
    ASSERT_FALSE(r8e_slab_contains(NULL, NULL));
    R8ESlab slab;
    r8e_slab_init(&slab, 24);
    ASSERT_FALSE(r8e_slab_contains(&slab, NULL));
    r8e_slab_destroy(&slab);
}

TEST(slab_null_stats) {
    ASSERT_EQ_INT(r8e_slab_current_count(NULL), 0);
    ASSERT_EQ_INT(r8e_slab_peak_count(NULL), 0);
    ASSERT_EQ_INT(r8e_slab_obj_size(NULL), 0);
}

/* =========================================================================
 * Slab: alloc-free-realloc pattern
 * ========================================================================= */

TEST(slab_alloc_free_realloc_pattern) {
    R8ESlab slab;
    r8e_slab_init(&slab, 40);

    /* Allocate some, free some, allocate more */
    void *p1 = r8e_slab_alloc(&slab);
    void *p2 = r8e_slab_alloc(&slab);
    void *p3 = r8e_slab_alloc(&slab);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 3);

    r8e_slab_free(&slab, p2);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 2);

    /* p2 should be reused */
    void *p4 = r8e_slab_alloc(&slab);
    ASSERT_TRUE(p4 == p2);
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 3);
    ASSERT_EQ_INT(r8e_slab_peak_count(&slab), 3);

    r8e_slab_free(&slab, p1);
    r8e_slab_free(&slab, p3);
    r8e_slab_free(&slab, p4);
    r8e_slab_destroy(&slab);
}

/* =========================================================================
 * Slab: different object sizes
 * ========================================================================= */

TEST(slab_different_sizes) {
    /* Test slab with Tier0 (24), Tier1 small (40), Tier1 large (72) */
    uint32_t sizes[] = {24, 40, 72};
    for (int s = 0; s < 3; s++) {
        R8ESlab slab;
        r8e_slab_init(&slab, sizes[s]);
        ASSERT_EQ_INT(r8e_slab_obj_size(&slab), sizes[s]);

        void *p = r8e_slab_alloc(&slab);
        ASSERT_TRUE(p != NULL);
        /* Write obj_size bytes to verify no corruption */
        memset(p, 0xAA, sizes[s]);
        r8e_slab_free(&slab, p);
        r8e_slab_destroy(&slab);
    }
}

/* =========================================================================
 * Slab: destroy with allocated objects
 * ========================================================================= */

TEST(slab_destroy_with_allocated) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    void *p1 = r8e_slab_alloc(&slab);
    void *p2 = r8e_slab_alloc(&slab);
    (void)p1;
    (void)p2;
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 2);

    /* Destroy without freeing individual objects - should not leak */
    r8e_slab_destroy(&slab);
    /* After destroy, slab is zeroed */
    ASSERT_EQ_INT(slab.current_count, 0);
    ASSERT_TRUE(slab.blocks == NULL);
}

/* =========================================================================
 * Slab: multiple blocks
 * ========================================================================= */

TEST(slab_multiple_blocks) {
    R8ESlab slab;
    r8e_slab_init(&slab, 24);

    /* SLAB_BLOCK_COUNT is 64 per block, so 200 allocs needs 4 blocks */
    void *ptrs[200];
    for (int i = 0; i < 200; i++) {
        ptrs[i] = r8e_slab_alloc(&slab);
        ASSERT_TRUE(ptrs[i] != NULL);
    }
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 200);
    ASSERT_EQ_INT(r8e_slab_peak_count(&slab), 200);

    /* All allocated pointers should be contained in the slab */
    for (int i = 0; i < 200; i++) {
        ASSERT_TRUE(r8e_slab_contains(&slab, ptrs[i]));
    }

    for (int i = 0; i < 200; i++) {
        r8e_slab_free(&slab, ptrs[i]);
    }
    ASSERT_EQ_INT(r8e_slab_current_count(&slab), 0);
    r8e_slab_destroy(&slab);
}

/* =========================================================================
 * Slab Pool
 * ========================================================================= */

/* Forward declarations for slab pool */
typedef struct R8ESlabPool R8ESlabPool;
extern void  r8e_slab_pool_init(R8ESlabPool *pool);
extern void *r8e_slab_pool_alloc(R8ESlabPool *pool, uint32_t size);
extern void  r8e_slab_pool_free(R8ESlabPool *pool, void *ptr, uint32_t size);
extern void  r8e_slab_pool_destroy(R8ESlabPool *pool);

/* Must match r8e_alloc.c */
#define R8E_SLAB_NUM_CLASSES 3
struct R8ESlabPool {
    R8ESlab slabs[R8E_SLAB_NUM_CLASSES];
    uint32_t sizes[R8E_SLAB_NUM_CLASSES];
};

TEST(slab_pool_basic) {
    R8ESlabPool pool;
    r8e_slab_pool_init(&pool);

    /* Allocate size <= 24 (Tier0) */
    void *p1 = r8e_slab_pool_alloc(&pool, 24);
    ASSERT_TRUE(p1 != NULL);

    /* Allocate size <= 40 (Tier1 small) */
    void *p2 = r8e_slab_pool_alloc(&pool, 32);
    ASSERT_TRUE(p2 != NULL);

    /* Allocate size <= 72 (Tier1 large) */
    void *p3 = r8e_slab_pool_alloc(&pool, 72);
    ASSERT_TRUE(p3 != NULL);

    r8e_slab_pool_free(&pool, p1, 24);
    r8e_slab_pool_free(&pool, p2, 32);
    r8e_slab_pool_free(&pool, p3, 72);

    r8e_slab_pool_destroy(&pool);
}

TEST(slab_pool_oversized) {
    R8ESlabPool pool;
    r8e_slab_pool_init(&pool);

    /* Size > 72 should return NULL (exceeds all slab classes) */
    void *p = r8e_slab_pool_alloc(&pool, 100);
    ASSERT_TRUE(p == NULL);

    r8e_slab_pool_destroy(&pool);
}

TEST(slab_pool_null) {
    void *p = r8e_slab_pool_alloc(NULL, 24);
    ASSERT_TRUE(p == NULL);
    /* Should not crash */
    r8e_slab_pool_free(NULL, NULL, 24);
    r8e_slab_pool_destroy(NULL);
}

/* =========================================================================
 * Tracked alloc / realloc / free
 * ========================================================================= */

extern void *r8e_tracked_alloc(R8EMemTracker *tracker, size_t size);
extern void  r8e_tracked_free(R8EMemTracker *tracker, void *ptr);
extern void *r8e_tracked_realloc(R8EMemTracker *tracker, void *ptr, size_t new_size);

TEST(tracked_alloc_basic) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0); /* unlimited */

    void *p = r8e_tracked_alloc(&tracker, 100);
    ASSERT_TRUE(p != NULL);

    /* Verify memory is zeroed */
    uint8_t *bytes = (uint8_t *)p;
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ_INT(bytes[i], 0);
    }

    /* Tracker should show allocation */
    ASSERT_TRUE(r8e_mem_tracker_current(&tracker) >= 100);

    r8e_tracked_free(&tracker, p);
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 0);
}

TEST(tracked_alloc_zero_size) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);

    void *p = r8e_tracked_alloc(&tracker, 0);
    ASSERT_TRUE(p == NULL);
}

TEST(tracked_alloc_with_limit) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 200);

    void *p1 = r8e_tracked_alloc(&tracker, 100);
    ASSERT_TRUE(p1 != NULL);

    /* Second alloc should fail: 100+8 + 100+8 = 216 > 200 */
    void *p2 = r8e_tracked_alloc(&tracker, 100);
    ASSERT_TRUE(p2 == NULL);

    r8e_tracked_free(&tracker, p1);
}

TEST(tracked_free_null) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);
    /* Should not crash */
    r8e_tracked_free(&tracker, NULL);
    r8e_tracked_free(NULL, NULL);
}

TEST(tracked_alloc_null_tracker) {
    /* Should work with NULL tracker (no tracking) */
    void *p = r8e_tracked_alloc(NULL, 64);
    ASSERT_TRUE(p != NULL);
    r8e_tracked_free(NULL, p);
}

TEST(tracked_realloc_grow) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0); /* unlimited */

    void *p = r8e_tracked_alloc(&tracker, 50);
    ASSERT_TRUE(p != NULL);
    /* Write pattern to verify data is preserved after realloc */
    memset(p, 0xAB, 50);

    void *p2 = r8e_tracked_realloc(&tracker, p, 200);
    ASSERT_TRUE(p2 != NULL);
    /* Old data should be preserved */
    uint8_t *bytes = (uint8_t *)p2;
    for (int i = 0; i < 50; i++) {
        ASSERT_EQ_INT(bytes[i], 0xAB);
    }
    /* New portion should be zeroed */
    for (int i = 50; i < 200; i++) {
        ASSERT_EQ_INT(bytes[i], 0);
    }

    r8e_tracked_free(&tracker, p2);
}

TEST(tracked_realloc_shrink) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);

    void *p = r8e_tracked_alloc(&tracker, 200);
    ASSERT_TRUE(p != NULL);
    size_t before = r8e_mem_tracker_current(&tracker);

    void *p2 = r8e_tracked_realloc(&tracker, p, 50);
    ASSERT_TRUE(p2 != NULL);
    /* Current usage should have decreased */
    ASSERT_TRUE(r8e_mem_tracker_current(&tracker) < before);

    r8e_tracked_free(&tracker, p2);
}

TEST(tracked_realloc_null_ptr) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);

    /* realloc(NULL, size) should behave like alloc */
    void *p = r8e_tracked_realloc(&tracker, NULL, 100);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(r8e_mem_tracker_current(&tracker) >= 100);

    r8e_tracked_free(&tracker, p);
}

TEST(tracked_realloc_zero_size) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);

    void *p = r8e_tracked_alloc(&tracker, 100);
    ASSERT_TRUE(p != NULL);

    /* realloc(ptr, 0) should behave like free */
    void *p2 = r8e_tracked_realloc(&tracker, p, 0);
    ASSERT_TRUE(p2 == NULL);
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 0);
}

TEST(tracked_realloc_with_limit) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 500);

    void *p = r8e_tracked_alloc(&tracker, 100);
    ASSERT_TRUE(p != NULL);

    /* Growing past limit should fail and preserve original allocation */
    void *p2 = r8e_tracked_realloc(&tracker, p, 1000);
    ASSERT_TRUE(p2 == NULL);

    /* Original allocation should still be valid and tracker unchanged */
    r8e_tracked_free(&tracker, p);
}

/* =========================================================================
 * Memory tracker: extended tests
 * ========================================================================= */

TEST(mem_tracker_null_handling) {
    /* All tracker functions should handle NULL gracefully */
    ASSERT_TRUE(r8e_mem_tracker_alloc(NULL, 100));  /* returns true for NULL */
    r8e_mem_tracker_free(NULL, 100); /* no crash */
    ASSERT_EQ_INT(r8e_mem_tracker_current(NULL), 0);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(NULL), 0);
}

TEST(mem_tracker_peak_after_free) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 0);

    r8e_mem_tracker_alloc(&tracker, 100);
    r8e_mem_tracker_alloc(&tracker, 200);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 300);

    r8e_mem_tracker_free(&tracker, 300);
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 0);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 300);

    /* New allocation lower than peak should not change peak */
    r8e_mem_tracker_alloc(&tracker, 50);
    ASSERT_EQ_INT(r8e_mem_tracker_peak(&tracker), 300);
}

TEST(mem_tracker_exact_limit) {
    R8EMemTracker tracker;
    r8e_mem_tracker_init(&tracker, 100);

    /* Allocating exactly at the limit should succeed */
    ASSERT_TRUE(r8e_mem_tracker_alloc(&tracker, 100));
    ASSERT_EQ_INT(r8e_mem_tracker_current(&tracker), 100);

    /* One more byte should fail */
    ASSERT_FALSE(r8e_mem_tracker_alloc(&tracker, 1));

    r8e_mem_tracker_free(&tracker, 100);
}

/* =========================================================================
 * Stack canary: extended
 * ========================================================================= */

TEST(canary_write_null) {
    /* Should not crash */
    r8e_canary_write(NULL);
}

TEST(canary_multiple_locations) {
    uint64_t bufs[4] = {0};
    for (int i = 0; i < 4; i++) {
        r8e_canary_write(&bufs[i]);
        ASSERT_TRUE(r8e_canary_check(&bufs[i]));
    }
    /* Corrupt one canary */
    bufs[2] = 0;
    ASSERT_FALSE(r8e_canary_check(&bufs[2]));
    /* Others should still pass */
    ASSERT_TRUE(r8e_canary_check(&bufs[0]));
    ASSERT_TRUE(r8e_canary_check(&bufs[1]));
    ASSERT_TRUE(r8e_canary_check(&bufs[3]));
}

/* =========================================================================
 * NaN-box validation: extended
 * ========================================================================= */

TEST(value_validate_inline_strings) {
    /* Valid inline string: length 0-6 */
    /* Tag 0xFFFD, length in bits [47:45] */
    ASSERT_TRUE(r8e_value_validate(0xFFFD000000000000ULL));  /* len=0 */
    ASSERT_TRUE(r8e_value_validate(0xFFFD200000000000ULL));  /* len=1 */
    ASSERT_TRUE(r8e_value_validate(0xFFFDC00000000000ULL));  /* len=6 */
    ASSERT_FALSE(r8e_value_validate(0xFFFDE00000000000ULL)); /* len=7 invalid */
}

TEST(value_validate_symbols) {
    /* Symbol tag 0xFFFB */
    ASSERT_TRUE(r8e_value_validate(0xFFFB000000000000ULL));
    ASSERT_TRUE(r8e_value_validate(0xFFFB000000000001ULL));
    ASSERT_TRUE(r8e_value_validate(0xFFFB00000000FFFFULL));
}

TEST(value_validate_atoms) {
    /* Atom tag 0xFFFC */
    ASSERT_TRUE(r8e_value_validate(0xFFFC000000000000ULL));
    ASSERT_TRUE(r8e_value_validate(0xFFFC000000000001ULL));
}

TEST(value_validate_negative_doubles) {
    /* Negative doubles should be valid */
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(-1.0)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(-1e308)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(-0.0)));
}

TEST(value_validate_special_doubles) {
    /* Infinity */
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(1.0 / 0.0)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_double(-1.0 / 0.0)));
}

TEST(value_validate_int32_extremes) {
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(0)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(INT32_MAX)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(INT32_MIN)));
    ASSERT_TRUE(r8e_value_validate(r8e_from_int32(-1)));
}

/* =========================================================================
 * Arena Pool
 * ========================================================================= */

/* Forward declarations */
typedef struct R8EArenaPool R8EArenaPool;
extern void     r8e_arena_pool_init(R8EArenaPool *pool, size_t total_limit);
extern int      r8e_arena_pool_add(R8EArenaPool *pool, size_t size);
extern R8EArena *r8e_arena_pool_get(R8EArenaPool *pool, int index);
extern void     r8e_arena_pool_destroy(R8EArenaPool *pool);

#define R8E_MAX_ARENAS 16
struct R8EArenaPool {
    R8EArena       *arenas[R8E_MAX_ARENAS];
    uint8_t         count;
    R8EMemTracker   tracker;
};

TEST(arena_pool_basic) {
    R8EArenaPool pool;
    r8e_arena_pool_init(&pool, 0); /* unlimited */

    int idx = r8e_arena_pool_add(&pool, 4096);
    ASSERT_TRUE(idx >= 0);

    R8EArena *arena = r8e_arena_pool_get(&pool, idx);
    ASSERT_TRUE(arena != NULL);

    /* Can allocate from the pooled arena */
    void *p = r8e_arena_alloc(arena, 32);
    ASSERT_TRUE(p != NULL);

    r8e_arena_pool_destroy(&pool);
}

TEST(arena_pool_multiple) {
    R8EArenaPool pool;
    r8e_arena_pool_init(&pool, 0);

    int idx0 = r8e_arena_pool_add(&pool, 4096);
    int idx1 = r8e_arena_pool_add(&pool, 4096);
    ASSERT_TRUE(idx0 >= 0);
    ASSERT_TRUE(idx1 >= 0);
    ASSERT_NE(idx0, idx1);

    R8EArena *a0 = r8e_arena_pool_get(&pool, idx0);
    R8EArena *a1 = r8e_arena_pool_get(&pool, idx1);
    ASSERT_TRUE(a0 != NULL);
    ASSERT_TRUE(a1 != NULL);
    ASSERT_TRUE(a0 != a1);

    r8e_arena_pool_destroy(&pool);
}

TEST(arena_pool_invalid_index) {
    R8EArenaPool pool;
    r8e_arena_pool_init(&pool, 0);

    ASSERT_TRUE(r8e_arena_pool_get(&pool, -1) == NULL);
    ASSERT_TRUE(r8e_arena_pool_get(&pool, 0) == NULL);
    ASSERT_TRUE(r8e_arena_pool_get(&pool, 100) == NULL);

    r8e_arena_pool_destroy(&pool);
}

TEST(arena_pool_max_arenas) {
    R8EArenaPool pool;
    r8e_arena_pool_init(&pool, 0);

    /* Fill to max */
    for (int i = 0; i < R8E_MAX_ARENAS; i++) {
        int idx = r8e_arena_pool_add(&pool, 4096);
        ASSERT_TRUE(idx >= 0);
    }

    /* Next add should fail */
    int idx = r8e_arena_pool_add(&pool, 4096);
    ASSERT_EQ_INT(idx, -1);

    r8e_arena_pool_destroy(&pool);
}

TEST(arena_pool_null_handling) {
    /* Should not crash */
    r8e_arena_pool_init(NULL, 0);
    ASSERT_TRUE(r8e_arena_pool_get(NULL, 0) == NULL);
    ASSERT_EQ_INT(r8e_arena_pool_add(NULL, 4096), -1);
    r8e_arena_pool_destroy(NULL);
}

/* =========================================================================
 * Test suite entry point
 * ========================================================================= */

void test_suite_alloc(void) {
    /* Arena creation/destruction */
    RUN_TEST(arena_create_default);
    RUN_TEST(arena_create_custom_size);
    RUN_TEST(arena_create_min_size);
    RUN_TEST(arena_destroy_null);

    /* Arena allocation */
    RUN_TEST(arena_alloc_basic);
    RUN_TEST(arena_alloc_alignment);
    RUN_TEST(arena_alloc_zeroed);
    RUN_TEST(arena_alloc_zero_size);
    RUN_TEST(arena_contains);

    /* Arena reset */
    RUN_TEST(arena_reset);

    /* Arena overflow */
    RUN_TEST(arena_overflow);
    RUN_TEST(arena_overflow_large);

    /* Slab allocator */
    RUN_TEST(slab_init);
    RUN_TEST(slab_alloc_free);
    RUN_TEST(slab_reuse);
    RUN_TEST(slab_peak_tracking);
    RUN_TEST(slab_contains);
    RUN_TEST(slab_many_allocs);

    /* Memory tracker */
    RUN_TEST(mem_tracker_basic);
    RUN_TEST(mem_tracker_limit);
    RUN_TEST(mem_tracker_underflow_clamp);

    /* Stack canaries */
    RUN_TEST(canary_write_check);

    /* NaN-box validation */
    RUN_TEST(value_validate);

    /* Arena: unzeroed allocation */
    RUN_TEST(arena_alloc_unzeroed_basic);
    RUN_TEST(arena_alloc_unzeroed_null_arena);
    RUN_TEST(arena_alloc_unzeroed_zero_size);
    RUN_TEST(arena_alloc_unzeroed_overflow);

    /* Arena: fill-to-capacity */
    RUN_TEST(arena_fill_to_capacity);

    /* Arena: peak tracking */
    RUN_TEST(arena_peak_tracking);

    /* Arena: multiple resets */
    RUN_TEST(arena_multiple_resets);

    /* Arena: null parameter handling */
    RUN_TEST(arena_null_arena_alloc);
    RUN_TEST(arena_null_arena_reset);
    RUN_TEST(arena_null_arena_stats);

    /* Arena: alignment for various sizes */
    RUN_TEST(arena_alignment_various_sizes);

    /* Arena: maximum size clamping */
    RUN_TEST(arena_max_size_clamp);

    /* Slab: alignment */
    RUN_TEST(slab_alignment_check);

    /* Slab: minimum size clamped */
    RUN_TEST(slab_min_size_clamped);

    /* Slab: null handling */
    RUN_TEST(slab_null_alloc);
    RUN_TEST(slab_null_free);
    RUN_TEST(slab_null_contains);
    RUN_TEST(slab_null_stats);

    /* Slab: alloc-free-realloc pattern */
    RUN_TEST(slab_alloc_free_realloc_pattern);

    /* Slab: different sizes */
    RUN_TEST(slab_different_sizes);

    /* Slab: destroy with allocated objects */
    RUN_TEST(slab_destroy_with_allocated);

    /* Slab: multiple blocks */
    RUN_TEST(slab_multiple_blocks);

    /* Slab pool */
    RUN_TEST(slab_pool_basic);
    RUN_TEST(slab_pool_oversized);
    RUN_TEST(slab_pool_null);

    /* Tracked alloc/realloc/free */
    RUN_TEST(tracked_alloc_basic);
    RUN_TEST(tracked_alloc_zero_size);
    RUN_TEST(tracked_alloc_with_limit);
    RUN_TEST(tracked_free_null);
    RUN_TEST(tracked_alloc_null_tracker);
    RUN_TEST(tracked_realloc_grow);
    RUN_TEST(tracked_realloc_shrink);
    RUN_TEST(tracked_realloc_null_ptr);
    RUN_TEST(tracked_realloc_zero_size);
    RUN_TEST(tracked_realloc_with_limit);

    /* Memory tracker: extended */
    RUN_TEST(mem_tracker_null_handling);
    RUN_TEST(mem_tracker_peak_after_free);
    RUN_TEST(mem_tracker_exact_limit);

    /* Stack canary: extended */
    RUN_TEST(canary_write_null);
    RUN_TEST(canary_multiple_locations);

    /* NaN-box validation: extended */
    RUN_TEST(value_validate_inline_strings);
    RUN_TEST(value_validate_symbols);
    RUN_TEST(value_validate_atoms);
    RUN_TEST(value_validate_negative_doubles);
    RUN_TEST(value_validate_special_doubles);
    RUN_TEST(value_validate_int32_extremes);

    /* Arena pool */
    RUN_TEST(arena_pool_basic);
    RUN_TEST(arena_pool_multiple);
    RUN_TEST(arena_pool_invalid_index);
    RUN_TEST(arena_pool_max_arenas);
    RUN_TEST(arena_pool_null_handling);
}
