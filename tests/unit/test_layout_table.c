/*
 * test_layout_table.c - Unit tests for table layout and scroll support
 *
 * Tests cover:
 *   - Table layout: basic 2x2 grid, column auto-sizing, row heights
 *   - Scroll support: scrollable detection, scroll offset clamping,
 *     scroll delta application, find scrollable ancestor
 *
 * These tests create layout-compatible R8EUIDOMNode structs directly
 * (matching the nlayout.c struct layout) to avoid the DOM struct mismatch.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <float.h>

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
 * Type definitions matching nlayout.c R8EUIDOMNode exactly
 * ========================================================================= */

typedef struct {
    float x, y;
    float width, height;
    float padding[4];   /* TRBL */
    float border[4];
    float margin[4];
} TestLayoutBox;

typedef enum {
    TEST_DISPLAY_BLOCK       = 0,
    TEST_DISPLAY_INLINE      = 1,
    TEST_DISPLAY_FLEX        = 2,
    TEST_DISPLAY_NONE        = 3,
    TEST_DISPLAY_TABLE       = 4,
    TEST_DISPLAY_TABLE_ROW   = 5,
    TEST_DISPLAY_TABLE_CELL  = 6,
} TestDisplay;

typedef enum {
    TEST_POSITION_STATIC   = 0,
    TEST_POSITION_RELATIVE = 1,
    TEST_POSITION_ABSOLUTE = 2,
    TEST_POSITION_FIXED    = 3,
} TestPosition;

typedef enum {
    TEST_OVERFLOW_VISIBLE = 0,
    TEST_OVERFLOW_HIDDEN  = 1,
    TEST_OVERFLOW_SCROLL  = 2,
    TEST_OVERFLOW_AUTO    = 3,
} TestOverflow;

typedef enum {
    TEST_FLEX_ROW            = 0,
    TEST_FLEX_COLUMN         = 1,
    TEST_FLEX_ROW_REVERSE    = 2,
    TEST_FLEX_COLUMN_REVERSE = 3,
} TestFlexDirection;

typedef enum {
    TEST_JUSTIFY_FLEX_START    = 0,
    TEST_JUSTIFY_FLEX_END      = 1,
    TEST_JUSTIFY_CENTER        = 2,
    TEST_JUSTIFY_SPACE_BETWEEN = 3,
    TEST_JUSTIFY_SPACE_AROUND  = 4,
    TEST_JUSTIFY_SPACE_EVENLY  = 5,
} TestJustifyContent;

typedef enum {
    TEST_ALIGN_FLEX_START = 0,
    TEST_ALIGN_FLEX_END   = 1,
    TEST_ALIGN_CENTER     = 2,
    TEST_ALIGN_STRETCH    = 3,
    TEST_ALIGN_BASELINE   = 4,
} TestAlignItems;

typedef enum {
    TEST_ALIGN_CONTENT_FLEX_START    = 0,
    TEST_ALIGN_CONTENT_FLEX_END      = 1,
    TEST_ALIGN_CONTENT_CENTER        = 2,
    TEST_ALIGN_CONTENT_STRETCH       = 3,
    TEST_ALIGN_CONTENT_SPACE_BETWEEN = 4,
    TEST_ALIGN_CONTENT_SPACE_AROUND  = 5,
} TestAlignContent;

typedef enum {
    TEST_FLEX_NOWRAP       = 0,
    TEST_FLEX_WRAP         = 1,
    TEST_FLEX_WRAP_REVERSE = 2,
} TestFlexWrap;

typedef enum {
    TEST_DIM_AUTO    = 0,
    TEST_DIM_PX      = 1,
    TEST_DIM_PERCENT = 2,
    TEST_DIM_NONE    = 3,
} TestDimType;

typedef struct {
    TestDimType type;
    float       value;
} TestDimValue;

#define TEST_DIM_AUTO_VAL   ((TestDimValue){ TEST_DIM_AUTO, 0.0f })
#define TEST_DIM_NONE_VAL   ((TestDimValue){ TEST_DIM_NONE, 0.0f })
#define TEST_DIM_PX_VAL(v)  ((TestDimValue){ TEST_DIM_PX, (v) })

typedef struct {
    TestDisplay       display;
    TestPosition      position;
    TestOverflow      overflow;

    TestDimValue      width;
    TestDimValue      height;
    TestDimValue      min_width;
    TestDimValue      min_height;
    TestDimValue      max_width;
    TestDimValue      max_height;

    float             padding[4];
    float             border_width[4];
    float             margin[4];
    bool              margin_auto[4];

    TestDimValue      top;
    TestDimValue      right;
    TestDimValue      bottom;
    TestDimValue      left;

    TestFlexDirection  flex_direction;
    TestJustifyContent justify_content;
    TestAlignItems     align_items;
    TestAlignItems     align_self;
    TestAlignContent   align_content;
    TestFlexWrap       flex_wrap;
    float              flex_grow;
    float              flex_shrink;
    TestDimValue       flex_basis;
    float              gap_row;
    float              gap_column;
    int                order;

    float              font_size;
    float              line_height;
} TestComputedStyle;

typedef enum {
    TEST_NODE_ELEMENT  = 1,
    TEST_NODE_TEXT     = 3,
    TEST_NODE_DOCUMENT = 9,
    TEST_NODE_FRAGMENT = 11,
} TestNodeType;

typedef struct TestDOMNode TestDOMNode;

struct TestDOMNode {
    uint8_t            type;
    uint32_t           flags;
    uint32_t           tag_atom;
    TestComputedStyle  style;
    TestLayoutBox      layout;

    const char        *text;
    uint32_t           text_len;

    TestDOMNode       *parent;
    TestDOMNode       *first_child;
    TestDOMNode       *last_child;
    TestDOMNode       *next_sibling;
    TestDOMNode       *prev_sibling;

    float              scroll_x;
    float              scroll_y;
    float              scroll_width;
    float              scroll_height;
    float              baseline;
    bool               scrollable;
};

/* =========================================================================
 * External layout functions (nlayout.c)
 *
 * These are declared with TestDOMNode which has the same binary layout
 * as nlayout.c's R8EUIDOMNode.
 * ========================================================================= */

extern void r8e_ui_layout_style_init(TestComputedStyle *style);
extern void r8e_ui_layout_box_init(TestLayoutBox *box);
extern void r8e_ui_layout_compute(TestDOMNode *root, float viewport_w,
                                   float viewport_h);
extern void r8e_ui_layout_get_content_box(const TestDOMNode *node,
                                           float *x, float *y,
                                           float *w, float *h);
extern void r8e_ui_layout_get_border_box(const TestDOMNode *node,
                                          float *x, float *y,
                                          float *w, float *h);
extern void r8e_ui_layout_update_scrollable(TestDOMNode *node);
extern bool r8e_ui_layout_scroll(TestDOMNode *node,
                                  float delta_x, float delta_y);
extern TestDOMNode *r8e_ui_layout_find_scrollable(TestDOMNode *node);

/* =========================================================================
 * Helper: create and initialize a layout-compatible DOM node
 * ========================================================================= */

static TestDOMNode *test_create_node(uint8_t type) {
    TestDOMNode *node = (TestDOMNode *)calloc(1, sizeof(TestDOMNode));
    if (!node) return NULL;
    node->type = type;
    r8e_ui_layout_style_init(&node->style);
    r8e_ui_layout_box_init(&node->layout);
    return node;
}

static TestDOMNode *test_create_element(void) {
    return test_create_node(TEST_NODE_ELEMENT);
}

static TestDOMNode *test_create_text(const char *text) {
    TestDOMNode *node = test_create_node(TEST_NODE_TEXT);
    if (!node) return NULL;
    node->text = text;
    node->text_len = (uint32_t)strlen(text);
    return node;
}

static void test_append_child(TestDOMNode *parent, TestDOMNode *child) {
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = NULL;
    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
}

static void test_free_tree(TestDOMNode *node) {
    if (!node) return;
    TestDOMNode *child = node->first_child;
    while (child) {
        TestDOMNode *next = child->next_sibling;
        test_free_tree(child);
        child = next;
    }
    free(node);
}

/* =========================================================================
 * Table Layout Tests
 * ========================================================================= */

/* --- Test: basic 2x2 table layout --- */
TEST(layout_table_basic) {
    /* Create table > 2 rows > 2 cells each */
    TestDOMNode *table = test_create_element();
    ASSERT_NOT_NULL(table);
    table->style.display = TEST_DISPLAY_TABLE;
    table->style.width = TEST_DIM_PX_VAL(400.0f);

    TestDOMNode *row1 = test_create_element();
    row1->style.display = TEST_DISPLAY_TABLE_ROW;
    TestDOMNode *row2 = test_create_element();
    row2->style.display = TEST_DISPLAY_TABLE_ROW;

    TestDOMNode *td11 = test_create_element();
    td11->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td12 = test_create_element();
    td12->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td21 = test_create_element();
    td21->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td22 = test_create_element();
    td22->style.display = TEST_DISPLAY_TABLE_CELL;

    /* Add text to each cell */
    test_append_child(td11, test_create_text("A"));
    test_append_child(td12, test_create_text("B"));
    test_append_child(td21, test_create_text("C"));
    test_append_child(td22, test_create_text("D"));

    /* Build tree */
    test_append_child(row1, td11);
    test_append_child(row1, td12);
    test_append_child(row2, td21);
    test_append_child(row2, td22);
    test_append_child(table, row1);
    test_append_child(table, row2);

    /* Compute layout */
    r8e_ui_layout_compute(table, 800.0f, 600.0f);

    /* Verify cells have finite positions */
    ASSERT_TRUE(isfinite(td11->layout.x));
    ASSERT_TRUE(isfinite(td12->layout.x));
    ASSERT_TRUE(isfinite(td21->layout.x));
    ASSERT_TRUE(isfinite(td22->layout.x));

    /* Cell (0,1) should be to the right of cell (0,0) within the row */
    ASSERT_TRUE(td12->layout.x > td11->layout.x);

    /* Row 2 should be below row 1 */
    ASSERT_TRUE(row2->layout.y > row1->layout.y);

    /* Cells in the same column should have the same x offset within their row */
    ASSERT_FLOAT_EQ(td11->layout.x, td21->layout.x, 1.0f);

    /* Table should have a positive width */
    ASSERT_TRUE(table->layout.width > 0.0f);

    /* Table should have a positive height (two rows) */
    ASSERT_TRUE(table->layout.height > 0.0f);

    test_free_tree(table);
}

/* --- Test: table auto column width sizing --- */
TEST(layout_table_auto_column_width) {
    TestDOMNode *table = test_create_element();
    table->style.display = TEST_DISPLAY_TABLE;
    table->style.width = TEST_DIM_PX_VAL(600.0f);

    TestDOMNode *row1 = test_create_element();
    row1->style.display = TEST_DISPLAY_TABLE_ROW;
    TestDOMNode *row2 = test_create_element();
    row2->style.display = TEST_DISPLAY_TABLE_ROW;

    TestDOMNode *td11 = test_create_element();
    td11->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td12 = test_create_element();
    td12->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td21 = test_create_element();
    td21->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td22 = test_create_element();
    td22->style.display = TEST_DISPLAY_TABLE_CELL;

    /* Column 0: "Short" and "A much longer text here" (wider) */
    test_append_child(td11, test_create_text("Short"));
    test_append_child(td12, test_create_text("X"));
    test_append_child(td21, test_create_text("A much longer text here"));
    test_append_child(td22, test_create_text("Y"));

    test_append_child(row1, td11);
    test_append_child(row1, td12);
    test_append_child(row2, td21);
    test_append_child(row2, td22);
    test_append_child(table, row1);
    test_append_child(table, row2);

    r8e_ui_layout_compute(table, 800.0f, 600.0f);

    /* Both cells in column 0 should have the same width */
    ASSERT_FLOAT_EQ(td11->layout.width, td21->layout.width, 1.0f);

    /* Both cells in column 1 should have the same width */
    ASSERT_FLOAT_EQ(td12->layout.width, td22->layout.width, 1.0f);

    /* Column 0 should be wider than column 1 because of longer text */
    ASSERT_TRUE(td11->layout.width > td12->layout.width);

    /* Total width should approximate the table width */
    ASSERT_TRUE(table->layout.width > 0.0f);

    test_free_tree(table);
}

/* --- Test: table row heights are uniform within a row --- */
TEST(layout_table_row_heights) {
    TestDOMNode *table = test_create_element();
    table->style.display = TEST_DISPLAY_TABLE;
    table->style.width = TEST_DIM_PX_VAL(300.0f);

    TestDOMNode *row = test_create_element();
    row->style.display = TEST_DISPLAY_TABLE_ROW;

    TestDOMNode *td1 = test_create_element();
    td1->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td2 = test_create_element();
    td2->style.display = TEST_DISPLAY_TABLE_CELL;

    test_append_child(td1, test_create_text("Short"));
    test_append_child(td2, test_create_text("Also short"));

    test_append_child(row, td1);
    test_append_child(row, td2);
    test_append_child(table, row);

    r8e_ui_layout_compute(table, 800.0f, 600.0f);

    /* Both cells should have the same height within the row */
    ASSERT_FLOAT_EQ(td1->layout.height, td2->layout.height, 1.0f);
    ASSERT_TRUE(td1->layout.height > 0.0f);

    /* Row height should match cell height */
    ASSERT_TRUE(row->layout.height > 0.0f);

    test_free_tree(table);
}

/* --- Test: table with 3 columns --- */
TEST(layout_table_three_cols) {
    TestDOMNode *table = test_create_element();
    table->style.display = TEST_DISPLAY_TABLE;
    table->style.width = TEST_DIM_PX_VAL(600.0f);

    TestDOMNode *row = test_create_element();
    row->style.display = TEST_DISPLAY_TABLE_ROW;

    TestDOMNode *td1 = test_create_element();
    td1->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td2 = test_create_element();
    td2->style.display = TEST_DISPLAY_TABLE_CELL;
    TestDOMNode *td3 = test_create_element();
    td3->style.display = TEST_DISPLAY_TABLE_CELL;

    test_append_child(td1, test_create_text("Col1"));
    test_append_child(td2, test_create_text("Col2"));
    test_append_child(td3, test_create_text("Col3"));

    test_append_child(row, td1);
    test_append_child(row, td2);
    test_append_child(row, td3);
    test_append_child(table, row);

    r8e_ui_layout_compute(table, 800.0f, 600.0f);

    /* All three cells should be positioned left to right */
    ASSERT_TRUE(td2->layout.x > td1->layout.x);
    ASSERT_TRUE(td3->layout.x > td2->layout.x);

    /* All cells should have positive width */
    ASSERT_TRUE(td1->layout.width > 0.0f);
    ASSERT_TRUE(td2->layout.width > 0.0f);
    ASSERT_TRUE(td3->layout.width > 0.0f);

    test_free_tree(table);
}

/* =========================================================================
 * Scroll Support Tests
 * ========================================================================= */

/* --- Test: overflow scroll element becomes scrollable --- */
TEST(layout_scroll_offset) {
    TestDOMNode *container = test_create_element();
    container->style.display = TEST_DISPLAY_BLOCK;
    container->style.width = TEST_DIM_PX_VAL(400.0f);
    container->style.height = TEST_DIM_PX_VAL(200.0f);
    container->style.overflow = TEST_OVERFLOW_SCROLL;

    /* Two children each 150px tall -> 300px total > 200px container */
    TestDOMNode *child1 = test_create_element();
    child1->style.display = TEST_DISPLAY_BLOCK;
    child1->style.width = TEST_DIM_PX_VAL(400.0f);
    child1->style.height = TEST_DIM_PX_VAL(150.0f);

    TestDOMNode *child2 = test_create_element();
    child2->style.display = TEST_DISPLAY_BLOCK;
    child2->style.width = TEST_DIM_PX_VAL(400.0f);
    child2->style.height = TEST_DIM_PX_VAL(150.0f);

    test_append_child(container, child1);
    test_append_child(container, child2);

    r8e_ui_layout_compute(container, 800.0f, 600.0f);

    /* Container should be scrollable since 300px content > 200px height */
    ASSERT_TRUE(container->scrollable);

    /* Scroll offset should start at 0 */
    ASSERT_FLOAT_EQ(container->scroll_y, 0.0f, 0.001f);

    /* Apply scroll and verify it took effect */
    bool scrolled = r8e_ui_layout_scroll(container, 0.0f, 50.0f);
    ASSERT_TRUE(scrolled);
    ASSERT_FLOAT_EQ(container->scroll_y, 50.0f, 0.001f);

    test_free_tree(container);
}

/* --- Test: scroll offset clamping --- */
TEST(layout_scroll_clamp) {
    TestDOMNode *container = test_create_element();
    container->style.display = TEST_DISPLAY_BLOCK;
    container->style.width = TEST_DIM_PX_VAL(300.0f);
    container->style.height = TEST_DIM_PX_VAL(200.0f);
    container->style.overflow = TEST_OVERFLOW_SCROLL;

    TestDOMNode *child = test_create_element();
    child->style.display = TEST_DISPLAY_BLOCK;
    child->style.width = TEST_DIM_PX_VAL(300.0f);
    child->style.height = TEST_DIM_PX_VAL(500.0f);

    test_append_child(container, child);

    r8e_ui_layout_compute(container, 800.0f, 600.0f);

    ASSERT_TRUE(container->scrollable);

    /* Max scroll = 500 - 200 = 300 */
    /* Try to scroll way past max */
    r8e_ui_layout_scroll(container, 0.0f, 10000.0f);
    ASSERT_TRUE(container->scroll_y <= 300.0f + 0.01f);

    /* Try to scroll negative */
    r8e_ui_layout_scroll(container, 0.0f, -20000.0f);
    ASSERT_TRUE(container->scroll_y >= 0.0f);

    test_free_tree(container);
}

/* --- Test: find scrollable ancestor --- */
TEST(layout_scroll_find_scrollable) {
    TestDOMNode *outer = test_create_element();
    outer->style.display = TEST_DISPLAY_BLOCK;
    outer->style.width = TEST_DIM_PX_VAL(400.0f);
    outer->style.height = TEST_DIM_PX_VAL(300.0f);
    outer->style.overflow = TEST_OVERFLOW_SCROLL;

    TestDOMNode *inner = test_create_element();
    inner->style.display = TEST_DISPLAY_BLOCK;
    inner->style.width = TEST_DIM_PX_VAL(400.0f);
    inner->style.height = TEST_DIM_PX_VAL(600.0f);

    TestDOMNode *deep = test_create_element();
    deep->style.display = TEST_DISPLAY_BLOCK;
    deep->style.width = TEST_DIM_PX_VAL(400.0f);
    deep->style.height = TEST_DIM_PX_VAL(100.0f);

    test_append_child(inner, deep);
    test_append_child(outer, inner);

    r8e_ui_layout_compute(outer, 800.0f, 600.0f);

    /* outer should be scrollable (600px content > 300px height) */
    ASSERT_TRUE(outer->scrollable);

    /* Finding scrollable from deep should return outer */
    TestDOMNode *found = r8e_ui_layout_find_scrollable(deep);
    ASSERT_EQ(found, outer);

    test_free_tree(outer);
}

/* --- Test: scroll on non-scrollable node returns false --- */
TEST(layout_scroll_non_scrollable) {
    TestDOMNode *div = test_create_element();
    div->style.display = TEST_DISPLAY_BLOCK;
    div->style.width = TEST_DIM_PX_VAL(300.0f);
    div->style.height = TEST_DIM_PX_VAL(200.0f);
    /* default overflow is visible */

    r8e_ui_layout_compute(div, 800.0f, 600.0f);

    /* Non-scrollable node should not accept scroll */
    bool scrolled = r8e_ui_layout_scroll(div, 0.0f, 50.0f);
    ASSERT_FALSE(scrolled);

    /* Find scrollable should return NULL */
    TestDOMNode *found = r8e_ui_layout_find_scrollable(div);
    ASSERT_NULL(found);

    test_free_tree(div);
}

/* --- Test: overflow auto becomes scrollable when content overflows --- */
TEST(layout_scroll_overflow_auto) {
    TestDOMNode *container = test_create_element();
    container->style.display = TEST_DISPLAY_BLOCK;
    container->style.width = TEST_DIM_PX_VAL(300.0f);
    container->style.height = TEST_DIM_PX_VAL(100.0f);
    container->style.overflow = TEST_OVERFLOW_AUTO;

    TestDOMNode *child = test_create_element();
    child->style.display = TEST_DISPLAY_BLOCK;
    child->style.width = TEST_DIM_PX_VAL(300.0f);
    child->style.height = TEST_DIM_PX_VAL(400.0f);

    test_append_child(container, child);

    r8e_ui_layout_compute(container, 800.0f, 600.0f);

    /* overflow:auto should be scrollable when content overflows */
    ASSERT_TRUE(container->scrollable);

    bool scrolled = r8e_ui_layout_scroll(container, 0.0f, 100.0f);
    ASSERT_TRUE(scrolled);

    test_free_tree(container);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_layout_table_tests(void) {
    printf("\n=== Layout Table & Scroll Tests ===\n");

    /* Table layout tests */
    RUN_TEST(layout_table_basic);
    RUN_TEST(layout_table_auto_column_width);
    RUN_TEST(layout_table_row_heights);
    RUN_TEST(layout_table_three_cols);

    /* Scroll support tests */
    RUN_TEST(layout_scroll_offset);
    RUN_TEST(layout_scroll_clamp);
    RUN_TEST(layout_scroll_find_scrollable);
    RUN_TEST(layout_scroll_non_scrollable);
    RUN_TEST(layout_scroll_overflow_auto);
}
