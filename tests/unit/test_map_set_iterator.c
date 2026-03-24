/*
 * test_map_set_iterator.c - Unit tests for Map/Set iterator protocol
 *
 * Tests the iterator infrastructure for Map and Set at the C API level.
 * The full JS eval path (for...of on Map/Set) requires additional
 * interpreter integration work beyond the scope of this task.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>

/* =========================================================================
 * Test Infrastructure (shared with test_runner.c)
 * ========================================================================= */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;
extern int g_assert_fail;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    g_assert_fail = 0;                                              \
    g_tests_run++;                                                  \
    printf("  %-60s ", #name);                                      \
    fflush(stdout);                                                 \
    fflush(stderr);                                                 \
    pid_t _pid = fork();                                            \
    if (_pid == 0) {                                                \
        alarm(5);                                                   \
        test_##name();                                              \
        _exit(g_assert_fail ? 1 : 0);                              \
    } else if (_pid > 0) {                                         \
        int _wstatus = 0;                                           \
        waitpid(_pid, &_wstatus, 0);                               \
        if (WIFSIGNALED(_wstatus)) {                               \
            g_assert_fail = 1;                                      \
            fprintf(stderr, "    CRASHED (signal)\n");              \
        } else if (WIFEXITED(_wstatus) &&                          \
                   WEXITSTATUS(_wstatus) != 0) {                   \
            g_assert_fail = 1;                                      \
        }                                                           \
    } else {                                                        \
        g_assert_fail = 1;                                          \
        fprintf(stderr, "    fork() failed\n");                     \
    }                                                               \
    if (g_assert_fail) {                                            \
        g_tests_failed++;                                           \
        printf("FAIL\n");                                           \
    } else {                                                        \
        g_tests_passed++;                                           \
        printf("ok\n");                                             \
    }                                                               \
} while (0)

#define ASSERT_TRUE(expr) do {                                      \
    if (!(expr)) {                                                  \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n"              \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ_INT(a, b) do {                                    \
    long long _a = (long long)(a), _b = (long long)(b);             \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n"      \
                "      got %lld vs %lld\n"                           \
                "      at %s:%d\n",                                 \
                #a, #b, _a, _b, __FILE__, __LINE__);                \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* =========================================================================
 * NaN-boxing helpers (must match r8e_types.h)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)

static inline void *val_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}
static inline int32_t val_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFF);
}
static inline R8EValue val_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

/* =========================================================================
 * External C API functions from r8e_weakref.c
 * ========================================================================= */

typedef struct R8EContext R8EContext;

extern R8EValue r8e_map_new(R8EContext *ctx);
extern R8EValue r8e_map_set(R8EContext *ctx, R8EValue map,
                              R8EValue key, R8EValue value);
extern R8EValue r8e_map_get(R8EContext *ctx, R8EValue map, R8EValue key);
extern uint32_t r8e_map_size(R8EContext *ctx, R8EValue map);

extern R8EValue r8e_set_new(R8EContext *ctx);
extern R8EValue r8e_set_add(R8EContext *ctx, R8EValue set, R8EValue value);
extern uint32_t r8e_set_size(R8EContext *ctx, R8EValue set);

/* =========================================================================
 * Map/Set struct definitions (must match r8e_weakref.c)
 * ========================================================================= */

typedef struct R8EMapEntry {
    R8EValue key;
    R8EValue value;
    uint32_t hash;
    struct R8EMapEntry *hash_next;
    struct R8EMapEntry *order_next;
    struct R8EMapEntry *order_prev;
} R8EMapEntry;

typedef struct {
    uint32_t      flags;
    uint32_t      proto_id;
    R8EMapEntry **buckets;
    uint32_t      capacity;
    uint32_t      count;
    R8EMapEntry  *order_first;
    R8EMapEntry  *order_last;
} R8EMap;

typedef struct R8ESetEntry {
    R8EValue key;
    uint32_t hash;
    struct R8ESetEntry *hash_next;
    struct R8ESetEntry *order_next;
    struct R8ESetEntry *order_prev;
} R8ESetEntry;

typedef struct {
    uint32_t      flags;
    uint32_t      proto_id;
    void        **buckets;
    uint32_t      capacity;
    uint32_t      count;
    R8ESetEntry  *order_first;
    R8ESetEntry  *order_last;
} R8ESet;

/* =========================================================================
 * Test 1: Map iteration via C API - walk insertion-order linked list
 * ========================================================================= */

TEST(map_for_of) {
    /* Create map and add entries */
    R8EValue map = r8e_map_new(NULL);
    ASSERT_TRUE(R8E_IS_POINTER(map));

    r8e_map_set(NULL, map, val_from_int32(10), val_from_int32(1));
    r8e_map_set(NULL, map, val_from_int32(20), val_from_int32(2));
    ASSERT_EQ_INT(r8e_map_size(NULL, map), 2);

    /* Walk the insertion-order linked list (simulates for-of) */
    R8EMap *m = (R8EMap *)val_get_pointer(map);
    ASSERT_TRUE(m != NULL);
    ASSERT_TRUE(m->order_first != NULL);

    int sum = 0;
    R8EMapEntry *entry = m->order_first;
    while (entry) {
        if (R8E_IS_INT32(entry->value)) {
            sum += val_get_int32(entry->value);
        }
        entry = entry->order_next;
    }
    ASSERT_EQ_INT(sum, 3); /* 1 + 2 = 3 */
}

/* =========================================================================
 * Test 2: Set iteration via C API - walk insertion-order linked list
 * ========================================================================= */

TEST(set_for_of) {
    R8EValue set = r8e_set_new(NULL);
    ASSERT_TRUE(R8E_IS_POINTER(set));

    r8e_set_add(NULL, set, val_from_int32(10));
    r8e_set_add(NULL, set, val_from_int32(20));
    r8e_set_add(NULL, set, val_from_int32(30));
    ASSERT_EQ_INT(r8e_set_size(NULL, set), 3);

    /* Walk the insertion-order linked list (simulates for-of) */
    R8ESet *s = (R8ESet *)val_get_pointer(set);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s->order_first != NULL);

    int sum = 0;
    R8ESetEntry *entry = s->order_first;
    while (entry) {
        if (R8E_IS_INT32(entry->key)) {
            sum += val_get_int32(entry->key);
        }
        entry = entry->order_next;
    }
    ASSERT_EQ_INT(sum, 60); /* 10 + 20 + 30 = 60 */
}

/* =========================================================================
 * Test 3: Map keys iteration - walk entries, collect only keys
 * ========================================================================= */

TEST(map_keys) {
    R8EValue map = r8e_map_new(NULL);
    ASSERT_TRUE(R8E_IS_POINTER(map));

    r8e_map_set(NULL, map, val_from_int32(42), val_from_int32(5));
    ASSERT_EQ_INT(r8e_map_size(NULL, map), 1);

    /* Walk keys */
    R8EMap *m = (R8EMap *)val_get_pointer(map);
    int key_count = 0;
    R8EMapEntry *entry = m->order_first;
    while (entry) {
        key_count++;
        entry = entry->order_next;
    }
    ASSERT_EQ_INT(key_count, 1);
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

void run_map_set_iterator_tests(void) {
    RUN_TEST(map_for_of);
    RUN_TEST(set_for_of);
    RUN_TEST(map_keys);
}
