/*
 * test_atom.c - Unit tests for r8e_atom.c
 *
 * Tests atom table initialization, interning, deduplication, lookup,
 * Bloom filter, and pre-defined atoms.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Type definitions matching r8e_atom.c
 * ------------------------------------------------------------------------- */
typedef uint64_t R8EValue;

typedef struct R8EContext R8EContext;
struct R8EContext {
    void *arena;
};

/* Extern declarations for r8e_atom.c functions */
extern bool     r8e_atom_table_init(R8EContext *ctx);
extern void     r8e_atom_table_destroy(R8EContext *ctx);
extern uint32_t r8e_atom_intern(R8EContext *ctx, const char *str, uint32_t len);
extern uint32_t r8e_atom_intern_cstr(R8EContext *ctx, const char *cstr);
extern const char *r8e_atom_get(R8EContext *ctx, uint32_t atom_id);
extern uint32_t r8e_atom_lookup(R8EContext *ctx, const char *str, uint32_t len);
extern uint32_t r8e_atom_lookup_cstr(R8EContext *ctx, const char *cstr);
extern uint32_t r8e_atom_length(R8EContext *ctx, uint32_t atom_id);
extern uint32_t r8e_atom_hash(R8EContext *ctx, uint32_t atom_id);
extern bool     r8e_atom_equal(uint32_t a, uint32_t b);
extern uint32_t r8e_atom_count(R8EContext *ctx);

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
#define ASSERT_EQ_STR(a, b) do { if (strcmp((a),(b)) != 0) { fprintf(stderr, "    ASSERT_EQ_STR(%s, %s): \"%s\" vs \"%s\" at %s:%d\n", #a, #b, (a), (b), __FILE__, __LINE__); g_assert_fail = 1; return; } } while(0)

/* =========================================================================
 * Helper: ensure atom table is initialized, run test, then destroy
 * ========================================================================= */

static R8EContext g_ctx = {0};

static void atom_setup(void) {
    r8e_atom_table_destroy(&g_ctx);
    r8e_atom_table_init(&g_ctx);
}

static void atom_teardown(void) {
    r8e_atom_table_destroy(&g_ctx);
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

TEST(atom_table_init) {
    atom_setup();
    /* After init, count should be > 0 (pre-populated atoms) */
    uint32_t count = r8e_atom_count(&g_ctx);
    ASSERT_TRUE(count > 100);  /* We have ~256 builtin atoms (some dedup) */
    atom_teardown();
}

TEST(atom_table_double_init) {
    atom_setup();
    uint32_t count1 = r8e_atom_count(&g_ctx);
    /* Calling init again should be idempotent (returns true, no change) */
    ASSERT_TRUE(r8e_atom_table_init(&g_ctx));
    ASSERT_EQ_INT(r8e_atom_count(&g_ctx), count1);
    atom_teardown();
}

/* =========================================================================
 * Interning and deduplication
 * ========================================================================= */

TEST(atom_intern_new) {
    atom_setup();
    uint32_t count_before = r8e_atom_count(&g_ctx);

    /* Intern a new string not in the builtins */
    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "myCustomVar");
    ASSERT_NE(id, 0);
    ASSERT_EQ_INT(r8e_atom_count(&g_ctx), count_before + 1);

    atom_teardown();
}

TEST(atom_intern_dedup) {
    atom_setup();

    uint32_t id1 = r8e_atom_intern_cstr(&g_ctx, "fooBarBaz");
    ASSERT_NE(id1, 0);
    uint32_t count_after_first = r8e_atom_count(&g_ctx);

    /* Re-interning the same string should return the same ID */
    uint32_t id2 = r8e_atom_intern_cstr(&g_ctx, "fooBarBaz");
    ASSERT_EQ(id1, id2);
    /* Count should not increase */
    ASSERT_EQ_INT(r8e_atom_count(&g_ctx), count_after_first);

    atom_teardown();
}

TEST(atom_intern_different) {
    atom_setup();

    uint32_t id1 = r8e_atom_intern_cstr(&g_ctx, "alpha");
    uint32_t id2 = r8e_atom_intern_cstr(&g_ctx, "beta");
    ASSERT_NE(id1, 0);
    ASSERT_NE(id2, 0);
    ASSERT_NE(id1, id2);

    atom_teardown();
}

TEST(atom_intern_with_length) {
    atom_setup();

    /* Intern using explicit length (not null-terminated) */
    uint32_t id1 = r8e_atom_intern(&g_ctx, "hello world", 5);  /* "hello" */
    ASSERT_NE(id1, 0);

    const char *str = r8e_atom_get(&g_ctx, id1);
    ASSERT_TRUE(str != NULL);
    ASSERT_EQ_STR(str, "hello");

    atom_teardown();
}

/* =========================================================================
 * Lookup
 * ========================================================================= */

TEST(atom_lookup_found) {
    atom_setup();

    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "testLookup");
    uint32_t found = r8e_atom_lookup_cstr(&g_ctx, "testLookup");
    ASSERT_EQ(id, found);

    atom_teardown();
}

TEST(atom_lookup_not_found) {
    atom_setup();

    /* This string has never been interned */
    uint32_t found = r8e_atom_lookup_cstr(&g_ctx, "nonExistentVar12345");
    ASSERT_EQ_INT(found, 0);

    atom_teardown();
}

TEST(atom_get_string) {
    atom_setup();

    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "myVar");
    const char *str = r8e_atom_get(&g_ctx, id);
    ASSERT_TRUE(str != NULL);
    ASSERT_EQ_STR(str, "myVar");

    atom_teardown();
}

TEST(atom_get_invalid_id) {
    atom_setup();

    ASSERT_TRUE(r8e_atom_get(&g_ctx, 0) == NULL);
    ASSERT_TRUE(r8e_atom_get(&g_ctx, 999999) == NULL);

    atom_teardown();
}

TEST(atom_length) {
    atom_setup();

    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "test");
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, id), 4);

    uint32_t id2 = r8e_atom_intern_cstr(&g_ctx, "");
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, id2), 0);

    atom_teardown();
}

TEST(atom_hash_nonzero) {
    atom_setup();

    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "test");
    uint32_t h = r8e_atom_hash(&g_ctx, id);
    /* Hash should be non-zero for a non-empty string */
    ASSERT_NE(h, 0);

    atom_teardown();
}

/* =========================================================================
 * Bloom filter
 * ========================================================================= */

TEST(bloom_interned_passes) {
    atom_setup();

    /* Interned strings should always be found by lookup (Bloom says "maybe") */
    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "bloomTest");
    uint32_t found = r8e_atom_lookup_cstr(&g_ctx, "bloomTest");
    ASSERT_EQ(id, found);

    atom_teardown();
}

TEST(bloom_random_mostly_rejected) {
    atom_setup();

    /*
     * Random strings that were never interned should mostly be rejected
     * by the Bloom filter (returned as 0 by r8e_atom_lookup_cstr).
     * We test several random-looking strings.
     */
    int rejected = 0;
    const char *random_strings[] = {
        "xyzzy_9287", "qwfp_1234", "zxcv_7890", "mnbv_4567",
        "asdf_3456", "poiu_8765", "lkjh_2345", "wert_6789",
        "rtyuiop_001", "zzzqqq_999"
    };
    for (int i = 0; i < 10; i++) {
        uint32_t found = r8e_atom_lookup_cstr(&g_ctx, random_strings[i]);
        if (found == 0) rejected++;
    }
    /* We expect most (at least 5 of 10) to be rejected */
    ASSERT_TRUE(rejected >= 5);

    atom_teardown();
}

/* =========================================================================
 * Pre-defined atoms
 * ========================================================================= */

TEST(predefined_length) {
    atom_setup();

    /* "length" is the first builtin atom (ID 1) */
    uint32_t id = r8e_atom_lookup_cstr(&g_ctx, "length");
    ASSERT_NE(id, 0);
    ASSERT_EQ_INT(id, 1);
    const char *str = r8e_atom_get(&g_ctx, id);
    ASSERT_TRUE(str != NULL);
    ASSERT_EQ_STR(str, "length");

    atom_teardown();
}

TEST(predefined_prototype) {
    atom_setup();

    uint32_t id = r8e_atom_lookup_cstr(&g_ctx, "prototype");
    ASSERT_NE(id, 0);
    ASSERT_EQ_INT(id, 2);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), "prototype");

    atom_teardown();
}

TEST(predefined_constructor) {
    atom_setup();

    uint32_t id = r8e_atom_lookup_cstr(&g_ctx, "constructor");
    ASSERT_NE(id, 0);
    ASSERT_EQ_INT(id, 3);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), "constructor");

    atom_teardown();
}

TEST(predefined_toString) {
    atom_setup();

    uint32_t id = r8e_atom_lookup_cstr(&g_ctx, "toString");
    ASSERT_NE(id, 0);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), "toString");

    atom_teardown();
}

TEST(predefined_valueOf) {
    atom_setup();

    uint32_t id = r8e_atom_lookup_cstr(&g_ctx, "valueOf");
    ASSERT_NE(id, 0);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), "valueOf");

    atom_teardown();
}

TEST(predefined_undefined) {
    atom_setup();

    uint32_t id = r8e_atom_lookup_cstr(&g_ctx, "undefined");
    ASSERT_NE(id, 0);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), "undefined");

    atom_teardown();
}

TEST(predefined_keywords) {
    atom_setup();

    /* Verify several keywords are pre-interned */
    const char *keywords[] = {
        "var", "let", "const", "if", "else", "for", "while", "do",
        "return", "function", "class", "this", "new", "delete"
    };
    for (int i = 0; i < (int)(sizeof(keywords)/sizeof(keywords[0])); i++) {
        uint32_t id = r8e_atom_lookup_cstr(&g_ctx, keywords[i]);
        ASSERT_NE(id, 0);
        ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), keywords[i]);
    }

    atom_teardown();
}

/* =========================================================================
 * Atom equality
 * ========================================================================= */

TEST(atom_equality) {
    ASSERT_TRUE(r8e_atom_equal(1, 1));
    ASSERT_FALSE(r8e_atom_equal(1, 2));
    ASSERT_TRUE(r8e_atom_equal(0, 0));
}

/* =========================================================================
 * Stress test: many internings
 * ========================================================================= */

TEST(atom_intern_many) {
    atom_setup();

    uint32_t base_count = r8e_atom_count(&g_ctx);

    /* Intern 500 unique strings */
    uint32_t ids[500];
    char buf[32];
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "stress_%d", i);
        ids[i] = r8e_atom_intern_cstr(&g_ctx, buf);
        ASSERT_NE(ids[i], 0);
    }

    ASSERT_EQ_INT(r8e_atom_count(&g_ctx), base_count + 500);

    /* Verify all can be looked up */
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "stress_%d", i);
        uint32_t found = r8e_atom_lookup_cstr(&g_ctx, buf);
        ASSERT_EQ(found, ids[i]);
    }

    /* Verify all have correct strings */
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "stress_%d", i);
        const char *str = r8e_atom_get(&g_ctx, ids[i]);
        ASSERT_TRUE(str != NULL);
        ASSERT_EQ_STR(str, buf);
    }

    atom_teardown();
}

/* =========================================================================
 * Bloom filter extended testing
 * ========================================================================= */

TEST(bloom_false_positive_rate) {
    atom_setup();

    /*
     * Test Bloom filter false positive rate with a large number of
     * random-looking strings. All should return 0 from lookup (not interned).
     * The Bloom filter might cause a false positive, but the rate should
     * be low (we accept up to 30% false positives).
     */
    int rejected = 0;
    int total = 100;
    char buf[32];
    for (int i = 0; i < total; i++) {
        snprintf(buf, sizeof(buf), "randomXYZ_%d_%d", i * 7919, i ^ 0x5A5A);
        uint32_t found = r8e_atom_lookup_cstr(&g_ctx, buf);
        if (found == 0) rejected++;
    }
    /* We expect at least 70% to be rejected (Bloom FP rate < 30%) */
    ASSERT_TRUE(rejected >= 70);

    atom_teardown();
}

TEST(bloom_all_interned_found) {
    atom_setup();

    /* Every interned string must always be found by lookup */
    char buf[32];
    uint32_t ids[50];
    for (int i = 0; i < 50; i++) {
        snprintf(buf, sizeof(buf), "bloom_test_%d", i);
        ids[i] = r8e_atom_intern_cstr(&g_ctx, buf);
        ASSERT_NE(ids[i], 0);
    }

    /* Every single one must be found */
    for (int i = 0; i < 50; i++) {
        snprintf(buf, sizeof(buf), "bloom_test_%d", i);
        uint32_t found = r8e_atom_lookup_cstr(&g_ctx, buf);
        ASSERT_EQ(found, ids[i]);
    }

    atom_teardown();
}

/* =========================================================================
 * Hash collision handling
 * ========================================================================= */

TEST(atom_hash_distinct) {
    atom_setup();

    /* Intern several strings and verify they have different hashes */
    const char *strs[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    uint32_t hashes[5];
    for (int i = 0; i < 5; i++) {
        uint32_t id = r8e_atom_intern_cstr(&g_ctx, strs[i]);
        hashes[i] = r8e_atom_hash(&g_ctx, id);
    }

    /* At least 4 of 5 should be unique (very unlikely to have collision) */
    int unique = 0;
    for (int i = 0; i < 5; i++) {
        bool is_unique = true;
        for (int j = 0; j < i; j++) {
            if (hashes[i] == hashes[j]) { is_unique = false; break; }
        }
        if (is_unique) unique++;
    }
    ASSERT_TRUE(unique >= 4);

    atom_teardown();
}

TEST(atom_collision_still_works) {
    atom_setup();

    /*
     * Even if two strings hash to the same bucket, they should both
     * be internable and retrievable. We intern many strings to force
     * at least some collisions in the hash table.
     */
    uint32_t ids[200];
    char buf[32];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "col_%d", i);
        ids[i] = r8e_atom_intern_cstr(&g_ctx, buf);
        ASSERT_NE(ids[i], 0);
    }

    /* All must be retrievable with correct string */
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "col_%d", i);
        const char *str = r8e_atom_get(&g_ctx, ids[i]);
        ASSERT_TRUE(str != NULL);
        ASSERT_EQ_STR(str, buf);
    }

    atom_teardown();
}

/* =========================================================================
 * Atom table growth / rehash
 * ========================================================================= */

TEST(atom_table_growth) {
    atom_setup();

    uint32_t initial_count = r8e_atom_count(&g_ctx);

    /* Intern 1000 unique strings to force table growth */
    char buf[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "growth_%d", i);
        uint32_t id = r8e_atom_intern_cstr(&g_ctx, buf);
        ASSERT_NE(id, 0);
    }

    ASSERT_EQ_INT(r8e_atom_count(&g_ctx), initial_count + 1000);

    /* All should still be retrievable after rehashing */
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "growth_%d", i);
        uint32_t found = r8e_atom_lookup_cstr(&g_ctx, buf);
        ASSERT_NE(found, 0);
        ASSERT_EQ_STR(r8e_atom_get(&g_ctx, found), buf);
    }

    atom_teardown();
}

/* =========================================================================
 * Extended pre-interned atoms
 * ========================================================================= */

TEST(predefined_object_builtins) {
    atom_setup();

    const char *builtins[] = {
        "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable",
        "toLocaleString", "apply", "call", "bind"
    };
    for (int i = 0; i < (int)(sizeof(builtins)/sizeof(builtins[0])); i++) {
        uint32_t id = r8e_atom_lookup_cstr(&g_ctx, builtins[i]);
        ASSERT_NE(id, 0);
        ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), builtins[i]);
    }

    atom_teardown();
}

TEST(predefined_array_methods) {
    atom_setup();

    const char *methods[] = {
        "push", "pop", "shift", "unshift", "splice", "slice",
        "concat", "join", "indexOf", "map", "filter", "reduce",
        "forEach", "find", "includes", "sort"
    };
    for (int i = 0; i < (int)(sizeof(methods)/sizeof(methods[0])); i++) {
        uint32_t id = r8e_atom_lookup_cstr(&g_ctx, methods[i]);
        ASSERT_NE(id, 0);
        ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), methods[i]);
    }

    atom_teardown();
}

TEST(predefined_type_names) {
    atom_setup();

    const char *types[] = {
        "Object", "Array", "String", "Number", "Boolean",
        "Function", "Symbol", "Error", "RegExp", "Date"
    };
    for (int i = 0; i < (int)(sizeof(types)/sizeof(types[0])); i++) {
        uint32_t id = r8e_atom_lookup_cstr(&g_ctx, types[i]);
        ASSERT_NE(id, 0);
        ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), types[i]);
    }

    atom_teardown();
}

/* =========================================================================
 * Deduplication verification
 * ========================================================================= */

TEST(atom_dedup_after_growth) {
    atom_setup();

    /* Intern many strings, then verify dedup still works after rehash */
    char buf[32];
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "dedup_%d", i);
        r8e_atom_intern_cstr(&g_ctx, buf);
    }

    uint32_t count_before = r8e_atom_count(&g_ctx);

    /* Re-intern all 500 strings: count should not change */
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "dedup_%d", i);
        r8e_atom_intern_cstr(&g_ctx, buf);
    }

    ASSERT_EQ_INT(r8e_atom_count(&g_ctx), count_before);

    atom_teardown();
}

/* =========================================================================
 * Edge cases
 * ========================================================================= */

TEST(atom_intern_empty_string) {
    atom_setup();

    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "");
    ASSERT_NE(id, 0);
    const char *str = r8e_atom_get(&g_ctx, id);
    ASSERT_TRUE(str != NULL);
    ASSERT_EQ_STR(str, "");
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, id), 0);

    /* Dedup */
    uint32_t id2 = r8e_atom_intern_cstr(&g_ctx, "");
    ASSERT_EQ(id, id2);

    atom_teardown();
}

TEST(atom_intern_single_char) {
    atom_setup();

    uint32_t id = r8e_atom_intern_cstr(&g_ctx, "x");
    ASSERT_NE(id, 0);
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, id), 1);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), "x");

    atom_teardown();
}

TEST(atom_intern_long_string) {
    atom_setup();

    /* Test with a longer string */
    const char *long_str = "this_is_a_fairly_long_identifier_name_for_testing";
    uint32_t id = r8e_atom_intern_cstr(&g_ctx, long_str);
    ASSERT_NE(id, 0);
    ASSERT_EQ_STR(r8e_atom_get(&g_ctx, id), long_str);
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, id), (uint32_t)strlen(long_str));

    /* Dedup */
    uint32_t id2 = r8e_atom_intern_cstr(&g_ctx, long_str);
    ASSERT_EQ(id, id2);

    atom_teardown();
}

TEST(atom_lookup_with_length) {
    atom_setup();

    uint32_t id = r8e_atom_intern(&g_ctx, "helloXXX", 5);
    uint32_t found = r8e_atom_lookup(&g_ctx, "hello world", 5);
    ASSERT_EQ(id, found);

    atom_teardown();
}

TEST(atom_length_invalid_id) {
    atom_setup();
    /* Invalid ID should return 0 length */
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, 0), 0);
    ASSERT_EQ_INT(r8e_atom_length(&g_ctx, 999999), 0);
    atom_teardown();
}

TEST(atom_hash_invalid_id) {
    atom_setup();
    /* Hash of invalid ID should return 0 */
    ASSERT_EQ_INT(r8e_atom_hash(&g_ctx, 0), 0);
    ASSERT_EQ_INT(r8e_atom_hash(&g_ctx, 999999), 0);
    atom_teardown();
}

/* =========================================================================
 * Test suite entry point
 * ========================================================================= */

void test_suite_atom(void) {
    /* Initialization */
    RUN_TEST(atom_table_init);
    RUN_TEST(atom_table_double_init);

    /* Interning and deduplication */
    RUN_TEST(atom_intern_new);
    RUN_TEST(atom_intern_dedup);
    RUN_TEST(atom_intern_different);
    RUN_TEST(atom_intern_with_length);

    /* Lookup */
    RUN_TEST(atom_lookup_found);
    RUN_TEST(atom_lookup_not_found);
    RUN_TEST(atom_get_string);
    RUN_TEST(atom_get_invalid_id);
    RUN_TEST(atom_length);
    RUN_TEST(atom_hash_nonzero);

    /* Bloom filter */
    RUN_TEST(bloom_interned_passes);
    RUN_TEST(bloom_random_mostly_rejected);

    /* Pre-defined atoms */
    RUN_TEST(predefined_length);
    RUN_TEST(predefined_prototype);
    RUN_TEST(predefined_constructor);
    RUN_TEST(predefined_toString);
    RUN_TEST(predefined_valueOf);
    RUN_TEST(predefined_undefined);
    RUN_TEST(predefined_keywords);

    /* Equality */
    RUN_TEST(atom_equality);

    /* Stress test */
    RUN_TEST(atom_intern_many);

    /* Bloom filter extended */
    RUN_TEST(bloom_false_positive_rate);
    RUN_TEST(bloom_all_interned_found);

    /* Hash collision handling */
    RUN_TEST(atom_hash_distinct);
    RUN_TEST(atom_collision_still_works);

    /* Table growth / rehash */
    RUN_TEST(atom_table_growth);

    /* Extended pre-interned atoms */
    RUN_TEST(predefined_object_builtins);
    RUN_TEST(predefined_array_methods);
    RUN_TEST(predefined_type_names);

    /* Deduplication verification */
    RUN_TEST(atom_dedup_after_growth);

    /* Edge cases */
    RUN_TEST(atom_intern_empty_string);
    RUN_TEST(atom_intern_single_char);
    RUN_TEST(atom_intern_long_string);
    RUN_TEST(atom_lookup_with_length);
    RUN_TEST(atom_length_invalid_id);
    RUN_TEST(atom_hash_invalid_id);
}
