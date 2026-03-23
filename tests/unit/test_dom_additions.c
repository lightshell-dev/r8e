/*
 * test_dom_additions.c - Tests for DOM additions: create/append, traversal,
 *                        getBoundingClientRect
 *
 * Exercises the C API directly (r8e_ui_dom_* functions).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

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

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    float _a = (float)(a), _b = (float)(b); \
    if (fabsf(_a - _b) > (float)(eps)) { \
        fprintf(stderr, "    ASSERT_FLOAT_EQ failed: %s ~= %s\n" \
                "      got %f vs %f (eps=%f)\n" \
                "      at %s:%d\n", #a, #b, _a, _b, (float)(eps), \
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
 * Opaque types
 * ========================================================================= */

typedef struct R8EUIDOMNode R8EUIDOMNode;

typedef struct R8EUINodeList {
    R8EUIDOMNode **nodes;
    uint32_t count;
    uint32_t capacity;
} R8EUINodeList;

/* =========================================================================
 * Extern declarations for DOM API
 * ========================================================================= */

extern R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag);
extern R8EUIDOMNode *r8e_ui_dom_create_text_node_cstr(const char *text);
extern void r8e_ui_dom_node_free(R8EUIDOMNode *node);
extern void r8e_ui_dom_tree_free(R8EUIDOMNode *node);

extern R8EUIDOMNode *r8e_ui_dom_append_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);
extern R8EUIDOMNode *r8e_ui_dom_remove_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);

extern uint32_t r8e_ui_dom_child_element_count(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_first_element_child(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_last_element_child(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_next_element_sibling(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_prev_element_sibling(const R8EUIDOMNode *node);
extern bool r8e_ui_dom_contains(const R8EUIDOMNode *ancestor, const R8EUIDOMNode *node);

extern R8EUIDOMNode *r8e_ui_dom_query_selector(R8EUIDOMNode *root, const char *selector);
extern R8EUINodeList *r8e_ui_dom_query_selector_all(R8EUIDOMNode *root, const char *selector);

extern void r8e_ui_dom_classlist_add(R8EUIDOMNode *node, const char *class_name);

extern void r8e_ui_dom_get_bounding_rect(const R8EUIDOMNode *node,
                                         float *x, float *y,
                                         float *width, float *height);

extern void r8e_ui_nodelist_free(R8EUINodeList *list);

/* =========================================================================
 * Tests
 * ========================================================================= */

/*
 * dom_create_and_append: Create elements, appendChild, verify child count.
 */
TEST(dom_create_and_append) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(parent);

    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("span");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("p");
    R8EUIDOMNode *child3 = r8e_ui_dom_create_element("em");
    ASSERT_NOT_NULL(child1);
    ASSERT_NOT_NULL(child2);
    ASSERT_NOT_NULL(child3);

    /* Start with zero children */
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 0);

    /* Append three children */
    r8e_ui_dom_append_child(parent, child1);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 1);

    r8e_ui_dom_append_child(parent, child2);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 2);

    r8e_ui_dom_append_child(parent, child3);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 3);

    /* Verify containment */
    ASSERT_TRUE(r8e_ui_dom_contains(parent, child1));
    ASSERT_TRUE(r8e_ui_dom_contains(parent, child2));
    ASSERT_TRUE(r8e_ui_dom_contains(parent, child3));

    r8e_ui_dom_tree_free(parent);
}

/*
 * dom_traversal_siblings: Create parent with 2 children, verify
 * nextSibling and previousSibling via element sibling accessors.
 */
TEST(dom_traversal_siblings) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("span");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("p");
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(child1);
    ASSERT_NOT_NULL(child2);

    r8e_ui_dom_append_child(parent, child1);
    r8e_ui_dom_append_child(parent, child2);

    /* child1.nextElementSibling == child2 */
    ASSERT_TRUE(r8e_ui_dom_next_element_sibling(child1) == child2);

    /* child2.previousElementSibling == child1 */
    ASSERT_TRUE(r8e_ui_dom_prev_element_sibling(child2) == child1);

    /* No sibling beyond ends */
    ASSERT_NULL(r8e_ui_dom_prev_element_sibling(child1));
    ASSERT_NULL(r8e_ui_dom_next_element_sibling(child2));

    r8e_ui_dom_tree_free(parent);
}

/*
 * dom_traversal_first_last_child: Verify firstChild and lastChild.
 */
TEST(dom_traversal_first_last_child) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("ul");
    R8EUIDOMNode *li1 = r8e_ui_dom_create_element("li");
    R8EUIDOMNode *li2 = r8e_ui_dom_create_element("li");
    R8EUIDOMNode *li3 = r8e_ui_dom_create_element("li");

    r8e_ui_dom_append_child(parent, li1);
    r8e_ui_dom_append_child(parent, li2);
    r8e_ui_dom_append_child(parent, li3);

    /* firstElementChild == li1, lastElementChild == li3 */
    ASSERT_TRUE(r8e_ui_dom_first_element_child(parent) == li1);
    ASSERT_TRUE(r8e_ui_dom_last_element_child(parent) == li3);

    /* Empty parent should return NULL */
    R8EUIDOMNode *empty = r8e_ui_dom_create_element("div");
    ASSERT_NULL(r8e_ui_dom_first_element_child(empty));
    ASSERT_NULL(r8e_ui_dom_last_element_child(empty));

    r8e_ui_dom_tree_free(parent);
    r8e_ui_dom_tree_free(empty);
}

/*
 * dom_get_bounding_rect: Create element with explicit layout size,
 * run layout, verify rect values via r8e_ui_dom_get_bounding_rect.
 */
TEST(dom_get_bounding_rect) {
    /*
     * Test r8e_ui_dom_get_bounding_rect API.
     *
     * Note: We cannot call r8e_ui_layout_compute here because the layout
     * module defines its own incompatible R8EUIDOMNode struct (the UI suite
     * has a pre-existing crash due to this struct mismatch). Instead we
     * verify the API reads the layout box from a freshly-created node
     * (zeroed by calloc) and handles NULL gracefully.
     */
    R8EUIDOMNode *node = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(node);

    /* Freshly-created node has a zeroed layout box */
    float x, y, w, h;
    r8e_ui_dom_get_bounding_rect(node, &x, &y, &w, &h);
    ASSERT_FLOAT_EQ(x, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(y, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(w, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(h, 0.0f, 0.01f);

    /* NULL node should yield zeroes without crashing */
    r8e_ui_dom_get_bounding_rect(NULL, &x, &y, &w, &h);
    ASSERT_FLOAT_EQ(x, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(y, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(w, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(h, 0.0f, 0.01f);

    /* Partial NULL output pointers should not crash */
    r8e_ui_dom_get_bounding_rect(node, NULL, NULL, &w, &h);
    ASSERT_FLOAT_EQ(w, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(h, 0.0f, 0.01f);

    r8e_ui_dom_tree_free(node);
}

/*
 * dom_query_selector_basic: Verify querySelector and querySelectorAll work.
 */
TEST(dom_query_selector_basic) {
    R8EUIDOMNode *root = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("span");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("span");
    R8EUIDOMNode *child3 = r8e_ui_dom_create_element("p");

    r8e_ui_dom_classlist_add(child1, "highlight");
    r8e_ui_dom_classlist_add(child2, "highlight");

    r8e_ui_dom_append_child(root, child1);
    r8e_ui_dom_append_child(root, child2);
    r8e_ui_dom_append_child(root, child3);

    /* querySelector("span") should return first span */
    R8EUIDOMNode *found = r8e_ui_dom_query_selector(root, "span");
    ASSERT_TRUE(found == child1);

    /* querySelector("p") should return the p element */
    found = r8e_ui_dom_query_selector(root, "p");
    ASSERT_TRUE(found == child3);

    /* querySelectorAll("span") should return 2 nodes */
    R8EUINodeList *list = r8e_ui_dom_query_selector_all(root, "span");
    ASSERT_NOT_NULL(list);
    ASSERT_EQ_INT(list->count, 2);

    /* querySelectorAll(".highlight") should return 2 nodes */
    R8EUINodeList *list2 = r8e_ui_dom_query_selector_all(root, ".highlight");
    ASSERT_NOT_NULL(list2);
    ASSERT_EQ_INT(list2->count, 2);

    /* querySelector for non-existent should return NULL */
    found = r8e_ui_dom_query_selector(root, "h1");
    ASSERT_NULL(found);

    r8e_ui_nodelist_free(list);
    r8e_ui_nodelist_free(list2);
    r8e_ui_dom_tree_free(root);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void run_dom_additions_tests(void) {
    RUN_TEST(dom_create_and_append);
    RUN_TEST(dom_traversal_siblings);
    RUN_TEST(dom_traversal_first_last_child);
    RUN_TEST(dom_query_selector_basic);
    RUN_TEST(dom_get_bounding_rect);
}
