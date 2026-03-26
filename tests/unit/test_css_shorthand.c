/*
 * test_css_shorthand.c - Tests for CSS shorthand property expansion
 *
 * Tests that shorthand CSS properties (background, border, flex, etc.)
 * are correctly expanded into their longhand equivalents during parsing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* Test macros (shared globals from test_runner.c) */
extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; \
    g_tests_run++; \
    printf("  %-60s ", #fn); \
    fflush(stdout); \
    test_##fn(); \
    if (g_assert_fail) { g_tests_failed++; printf("FAIL\n"); } \
    else { g_tests_passed++; printf("ok\n"); } \
    fflush(stdout); \
} while (0)

#define TEST(name) static void test_##name(void)

#define ASSERT_TRUE(expr) do {                                      \
    if (!(expr)) {                                                  \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n"              \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) > (eps)) { \
        fprintf(stderr, "    ASSERT_FLOAT_EQ failed: %s ~= %s\n" \
                "      got %f vs %f (eps=%f)\n" \
                "      at %s:%d\n", \
                #a, #b, _a, _b, (float)(eps), __FILE__, __LINE__); \
        g_assert_fail = 1; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do {                                        \
    uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);               \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ failed: %s == %s\n"          \
                "      got 0x%llx vs 0x%llx\n"                      \
                "      at %s:%d\n",                                 \
                #a, #b,                                             \
                (unsigned long long)_a, (unsigned long long)_b,     \
                __FILE__, __LINE__);                                 \
        g_assert_fail = 1; return;                                  \
    }                                                               \
} while (0)

/* =========================================================================
 * R8EUIComputedStyle layout (must match r8e_style.c exactly)
 * ========================================================================= */

typedef struct R8EUIComputedStyle {
    uint8_t  display;
    uint8_t  position;
    uint8_t  flex_direction;
    uint8_t  justify_content;
    uint8_t  align_items;
    uint8_t  align_self;
    uint8_t  text_align;
    uint8_t  overflow;
    uint8_t  visibility;
    uint8_t  flex_wrap;
    uint8_t  cursor;
    uint8_t  _pad;

    float    width, height;
    float    min_width, min_height;
    float    max_width, max_height;

    float    padding[4];
    float    margin[4];
    float    border_width[4];

    float    flex_grow;
    float    flex_shrink;
    float    flex_basis;
    float    gap;
    int32_t  order;

    float    font_size;
    float    line_height;
    uint16_t font_weight;
    uint16_t font_family_atom;

    uint32_t color;
    uint32_t background_color;
    uint32_t border_color;
    float    opacity;

    float    border_radius[4];

    float    top, right, bottom, left;
    int32_t  z_index;
} R8EUIComputedStyle;

/* DOM node type (forward) */
typedef struct R8EUIDOMNode R8EUIDOMNode;

/* Stylesheet type (forward) */
typedef struct R8EUIStyleSheet R8EUIStyleSheet;
typedef struct R8EUIStyleContext R8EUIStyleContext;

/* Extern API from r8e_style.c / ndom.c */
extern R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag);
extern void r8e_ui_dom_tree_free(R8EUIDOMNode *node);

extern R8EUIStyleSheet *r8e_ui_stylesheet_create(void);
extern void r8e_ui_stylesheet_free(R8EUIStyleSheet *sheet);
extern void r8e_ui_stylesheet_parse(R8EUIStyleSheet *sheet, const char *css);

extern R8EUIStyleContext *r8e_ui_style_context_create(void);
extern void r8e_ui_style_context_free(R8EUIStyleContext *ctx);
extern bool r8e_ui_style_context_add_sheet(R8EUIStyleContext *ctx,
                                            R8EUIStyleSheet *sheet);
extern void r8e_ui_style_context_resolve(R8EUIStyleContext *ctx,
                                          R8EUIDOMNode *root);
extern const R8EUIComputedStyle *r8e_ui_style_get_computed(
    const R8EUIDOMNode *node);
extern void r8e_ui_style_free_tree(R8EUIDOMNode *root);

/* Helper: parse CSS, apply to a div node, return its computed style.
 * Caller must free sheet, ctx, and node via cleanup(). */
static const R8EUIComputedStyle *resolve_css(
    const char *css,
    R8EUIStyleSheet **out_sheet,
    R8EUIStyleContext **out_ctx,
    R8EUIDOMNode **out_node) {

    *out_sheet = r8e_ui_stylesheet_create();
    r8e_ui_stylesheet_parse(*out_sheet, css);

    *out_ctx = r8e_ui_style_context_create();
    r8e_ui_style_context_add_sheet(*out_ctx, *out_sheet);

    *out_node = r8e_ui_dom_create_element("div");
    r8e_ui_style_context_resolve(*out_ctx, *out_node);

    return r8e_ui_style_get_computed(*out_node);
}

static void cleanup(R8EUIStyleSheet *sheet, R8EUIStyleContext *ctx,
                    R8EUIDOMNode *node) {
    r8e_ui_style_context_free(ctx);
    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(node);
    r8e_ui_dom_tree_free(node);
}

/* =========================================================================
 * Tests
 * ========================================================================= */

/* --- background shorthand --- */
TEST(css_shorthand_background) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { background: #ff0000; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    /* #ff0000 -> 0xFF0000FF (RGBA) */
    ASSERT_EQ(s->background_color, 0xFF0000FFu);
    cleanup(sheet, ctx, node);
}

TEST(css_shorthand_background_named) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { background: white; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->background_color, 0xFFFFFFFFu);
    cleanup(sheet, ctx, node);
}

/* --- border shorthand --- */
TEST(css_shorthand_border) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { border: 2px solid #cc0000; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    /* border-width should be set to 2px on all sides (TRBL via ncss_parse_trbl) */
    ASSERT_FLOAT_EQ(s->border_width[0], 2.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_width[1], 2.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_width[2], 2.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_width[3], 2.0f, 0.001f);
    /* border-color should be #cc0000 -> 0xCC0000FF */
    ASSERT_EQ(s->border_color, 0xCC0000FFu);
    cleanup(sheet, ctx, node);
}

TEST(css_shorthand_border_top) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { border-top: 3px solid blue; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->border_width[0], 3.0f, 0.001f);
    /* Other sides should remain at default (0) */
    ASSERT_FLOAT_EQ(s->border_width[1], 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_width[2], 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_width[3], 0.0f, 0.001f);
    /* blue = 0x0000FFFF */
    ASSERT_EQ(s->border_color, 0x0000FFFFu);
    cleanup(sheet, ctx, node);
}

/* --- flex shorthand --- */
TEST(css_shorthand_flex_single) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { flex: 1; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->flex_grow, 1.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->flex_shrink, 1.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->flex_basis, 0.0f, 0.001f);
    cleanup(sheet, ctx, node);
}

TEST(css_shorthand_flex_three) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { flex: 0 0 auto; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->flex_grow, 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->flex_shrink, 0.0f, 0.001f);
    /* auto = -1.0f (NCSS_AUTO) */
    ASSERT_FLOAT_EQ(s->flex_basis, -1.0f, 0.001f);
    cleanup(sheet, ctx, node);
}

TEST(css_shorthand_flex_none) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { flex: none; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->flex_grow, 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->flex_shrink, 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->flex_basis, -1.0f, 0.001f);
    cleanup(sheet, ctx, node);
}

/* --- margin multi-value (already supported via ncss_parse_trbl, verify) --- */
TEST(css_shorthand_margin_two_values) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { margin: 10px 20px; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->margin[0], 10.0f, 0.001f); /* top */
    ASSERT_FLOAT_EQ(s->margin[1], 20.0f, 0.001f); /* right */
    ASSERT_FLOAT_EQ(s->margin[2], 10.0f, 0.001f); /* bottom */
    ASSERT_FLOAT_EQ(s->margin[3], 20.0f, 0.001f); /* left */
    cleanup(sheet, ctx, node);
}

TEST(css_shorthand_margin_four_values) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { margin: 1px 2px 3px 4px; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->margin[0], 1.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->margin[1], 2.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->margin[2], 3.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->margin[3], 4.0f, 0.001f);
    cleanup(sheet, ctx, node);
}

/* --- padding multi-value --- */
TEST(css_shorthand_padding_two_values) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { padding: 5px 15px; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->padding[0], 5.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->padding[1], 15.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->padding[2], 5.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->padding[3], 15.0f, 0.001f);
    cleanup(sheet, ctx, node);
}

/* --- border-radius multi-value --- */
TEST(css_shorthand_border_radius_four) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { border-radius: 12px 12px 3px 12px; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->border_radius[0], 12.0f, 0.001f); /* TL */
    ASSERT_FLOAT_EQ(s->border_radius[1], 12.0f, 0.001f); /* TR */
    ASSERT_FLOAT_EQ(s->border_radius[2], 3.0f, 0.001f);  /* BR */
    ASSERT_FLOAT_EQ(s->border_radius[3], 12.0f, 0.001f); /* BL */
    cleanup(sheet, ctx, node);
}

TEST(css_shorthand_border_radius_single) {
    R8EUIStyleSheet *sheet; R8EUIStyleContext *ctx; R8EUIDOMNode *node;
    const R8EUIComputedStyle *s = resolve_css(
        "div { border-radius: 8px; }", &sheet, &ctx, &node);
    ASSERT_NOT_NULL(s);
    ASSERT_FLOAT_EQ(s->border_radius[0], 8.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_radius[1], 8.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_radius[2], 8.0f, 0.001f);
    ASSERT_FLOAT_EQ(s->border_radius[3], 8.0f, 0.001f);
    cleanup(sheet, ctx, node);
}

/* =========================================================================
 * Suite Runner
 * ========================================================================= */

void run_css_shorthand_tests(void) {
    RUN_TEST(css_shorthand_background);
    RUN_TEST(css_shorthand_background_named);
    RUN_TEST(css_shorthand_border);
    RUN_TEST(css_shorthand_border_top);
    RUN_TEST(css_shorthand_flex_single);
    RUN_TEST(css_shorthand_flex_three);
    RUN_TEST(css_shorthand_flex_none);
    RUN_TEST(css_shorthand_margin_two_values);
    RUN_TEST(css_shorthand_margin_four_values);
    RUN_TEST(css_shorthand_padding_two_values);
    RUN_TEST(css_shorthand_border_radius_four);
    RUN_TEST(css_shorthand_border_radius_single);
}
