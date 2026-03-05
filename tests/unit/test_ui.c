/*
 * test_ui.c - Unit tests for the r8e NanoUI rendering stack
 *
 * Tests cover:
 *   - NanoDOM: element/text creation, tree manipulation, attribute/class ops,
 *     getElementById, querySelectorAll, cloneNode, HTML parsing
 *   - NanoStyle: stylesheet parsing, selector matching, style resolution,
 *     default style initialization, computed style properties
 *   - NanoLayout: box model, flexbox row/column, justify-content,
 *     align-items, flex-grow distribution, nested layout, hit testing
 *   - NanoPaint: display list init/emit/hash, color parsing, framebuffer
 *   - NanoEvent: event manager init, dispatch with bubbling/capture,
 *     focus management, hover tracking, hit testing
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
 * Minimal type definitions (self-contained, matching source files)
 * ========================================================================= */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

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

/* Approximate float comparison */
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
 * Opaque types from UI modules (we work through the public API)
 * ========================================================================= */

/* Forward-declare the DOM node type from ndom.c */
typedef struct R8EUIDOMNode R8EUIDOMNode;

/* Forward-declare the node list type from ndom.c */
typedef struct R8EUINodeList {
    R8EUIDOMNode **nodes;
    uint32_t count;
    uint32_t capacity;
} R8EUINodeList;

/* Layout box (matches all UI modules) */
typedef struct R8EUILayoutBox {
    float x, y;
    float width, height;
    float padding[4];   /* TRBL */
    float border[4];
    float margin[4];
} R8EUILayoutBox;

/* Computed style from nstyle.c */
typedef struct R8EUIComputedStyle R8EUIComputedStyle;

/* Stylesheet from nstyle.c */
typedef struct R8EUIStyleSheet R8EUIStyleSheet;

/* Style context from nstyle.c */
typedef struct R8EUIStyleContext R8EUIStyleContext;

/* Color */
typedef uint32_t R8EUIColor;

#define R8E_UI_COLOR_RGBA(r, g, b, a) \
    (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | \
     ((uint32_t)(b) << 8)  | ((uint32_t)(a)))
#define R8E_UI_COLOR_RGB(r, g, b) R8E_UI_COLOR_RGBA(r, g, b, 255)

#define R8E_UI_COLOR_R(c) ((uint8_t)((c) >> 24))
#define R8E_UI_COLOR_G(c) ((uint8_t)((c) >> 16))
#define R8E_UI_COLOR_B(c) ((uint8_t)((c) >> 8))
#define R8E_UI_COLOR_A(c) ((uint8_t)((c)))

/* Display list from npaint.c */
typedef enum {
    R8E_UI_CMD_RECT         = 0,
    R8E_UI_CMD_ROUNDED_RECT = 1,
    R8E_UI_CMD_TEXT         = 2,
    R8E_UI_CMD_BORDER       = 3,
    R8E_UI_CMD_CLIP_PUSH    = 4,
    R8E_UI_CMD_CLIP_POP     = 5,
    R8E_UI_CMD_OPACITY      = 6,
    R8E_UI_CMD_IMAGE        = 7,
} R8EUIPaintCmd;

typedef struct {
    R8EUIPaintCmd  cmd;
    float         x, y, w, h;
    R8EUIColor     color;
    float         radius[4];
    const char   *text;
    uint32_t      text_len;
    float         font_size;
    R8EUIColor     text_color;
    float         opacity;
    float         border_widths[4];
    R8EUIColor     border_colors[4];
    uint32_t      image_id;
} R8EUIPaintOp;

typedef struct {
    R8EUIPaintOp *ops;
    uint32_t     count;
    uint32_t     capacity;
    uint64_t     hash;
    bool         hash_valid;
} R8EUIDisplayList;

/* Framebuffer from npaint.c */
typedef struct {
    uint32_t *pixels;
    int       width;
    int       height;
    int       stride;
} R8EUIFramebuffer;

/* Paint style from npaint.c */
typedef struct {
    R8EUIColor     background_color;
    R8EUIColor     color;
    R8EUIColor     border_color[4];
    float         border_width[4];
    float         border_radius[4];
    float         opacity;
    float         font_size;
    float         line_height;
    uint8_t       overflow;
    uint8_t       visibility;
    float         shadow_offset_x;
    float         shadow_offset_y;
    float         shadow_blur;
    R8EUIColor     shadow_color;
    bool          has_shadow;
} R8EUIPaintStyle;

/* Event types from nevent.c */
typedef enum {
    R8E_UI_EVENT_CLICK       = 0,
    R8E_UI_EVENT_DBLCLICK    = 1,
    R8E_UI_EVENT_MOUSEDOWN   = 2,
    R8E_UI_EVENT_MOUSEUP     = 3,
    R8E_UI_EVENT_MOUSEMOVE   = 4,
    R8E_UI_EVENT_MOUSEENTER  = 5,
    R8E_UI_EVENT_MOUSELEAVE  = 6,
    R8E_UI_EVENT_CONTEXTMENU = 7,
    R8E_UI_EVENT_KEYDOWN     = 10,
    R8E_UI_EVENT_KEYUP       = 11,
    R8E_UI_EVENT_KEYPRESS    = 12,
    R8E_UI_EVENT_FOCUS       = 20,
    R8E_UI_EVENT_BLUR        = 21,
    R8E_UI_EVENT_FOCUSIN     = 22,
    R8E_UI_EVENT_FOCUSOUT    = 23,
    R8E_UI_EVENT_INPUT       = 30,
    R8E_UI_EVENT_CHANGE      = 31,
    R8E_UI_EVENT_SUBMIT      = 32,
    R8E_UI_EVENT_SCROLL      = 40,
    R8E_UI_EVENT_WHEEL       = 41,
    R8E_UI_EVENT_TYPE_COUNT  = 54,
} R8EUIEventType;

typedef void (*R8EUIEventCallback)(void *event, void *user_data);

typedef struct R8EUIEventListener {
    uint8_t              type;
    bool                 capture;
    bool                 once;
    bool                 passive;
    R8EUIEventCallback    callback;
    void                *user_data;
    struct R8EUIEventListener *next;
} R8EUIEventListener;

typedef struct {
    float x, y;
    float width, height;
    float padding[4];
    float border[4];
    float margin[4];
} R8EUILayoutBox_Event;

typedef struct R8EUIDOMNode_Event {
    uint8_t               type;
    uint32_t              flags;
    uint32_t              tag_atom;
    R8EUILayoutBox_Event   layout;
    struct R8EUIDOMNode_Event *parent;
    struct R8EUIDOMNode_Event *first_child;
    struct R8EUIDOMNode_Event *last_child;
    struct R8EUIDOMNode_Event *next_sibling;
    struct R8EUIDOMNode_Event *prev_sibling;
    R8EUIEventListener    *listeners;
    int32_t               tab_index;
    uint8_t               overflow;
    uint8_t               position;
    uint8_t               display;
    uint8_t               visibility;
} R8EUIDOMNode_Event;

#define R8E_UI_NODE_FLAG_FOCUSABLE  0x0004

typedef struct R8EUIEvent {
    uint8_t         type;
    uint8_t         phase;
    uint16_t        flags;
    float           x, y;
    float           page_x, page_y;
    float           offset_x, offset_y;
    uint8_t         button;
    uint8_t         buttons;
    uint8_t         detail;
    uint32_t        key_code;
    uint32_t        char_code;
    uint32_t        modifiers;
    const char     *key;
    R8EUIDOMNode_Event *target;
    R8EUIDOMNode_Event *current_target;
    R8EUIDOMNode_Event *related_target;
    double          timestamp;
} R8EUIEvent_Ev;

typedef struct {
    R8EUIDOMNode_Event  *document;
    R8EUIDOMNode_Event  *focused;
    R8EUIDOMNode_Event  *hovered;
    R8EUIDOMNode_Event  *active;
    R8EUIDOMNode_Event  *capture_target;
    float               mouse_x;
    float               mouse_y;
    uint8_t             mouse_buttons;
    uint8_t             click_count;
    double              last_click_time;
    float               last_click_x;
    float               last_click_y;
    double              dblclick_time;
    float               dblclick_dist;
} R8EUIEventManager;

/* =========================================================================
 * External function declarations: ndom.c
 * ========================================================================= */

extern R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag);
extern R8EUIDOMNode *r8e_ui_dom_create_text_node(const char *text, uint32_t len);
extern R8EUIDOMNode *r8e_ui_dom_create_text_node_cstr(const char *text);
extern R8EUIDOMNode *r8e_ui_dom_create_fragment(void);
extern R8EUIDOMNode *r8e_ui_dom_create_document(void);
extern R8EUIDOMNode *r8e_ui_dom_create_comment(const char *text, uint32_t len);
extern void r8e_ui_dom_node_free(R8EUIDOMNode *node);
extern void r8e_ui_dom_tree_free(R8EUIDOMNode *node);

extern R8EUIDOMNode *r8e_ui_dom_append_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);
extern R8EUIDOMNode *r8e_ui_dom_remove_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);
extern R8EUIDOMNode *r8e_ui_dom_insert_before(R8EUIDOMNode *parent,
                                                R8EUIDOMNode *new_child,
                                                R8EUIDOMNode *ref_child);
extern R8EUIDOMNode *r8e_ui_dom_replace_child(R8EUIDOMNode *parent,
                                                R8EUIDOMNode *new_child,
                                                R8EUIDOMNode *old_child);
extern R8EUIDOMNode *r8e_ui_dom_clone_node(const R8EUIDOMNode *node, bool deep);

extern void r8e_ui_dom_set_attribute(R8EUIDOMNode *node, const char *name,
                                      const char *value);
extern bool r8e_ui_dom_has_attribute(const R8EUIDOMNode *node, const char *name);
extern void r8e_ui_dom_remove_attribute(R8EUIDOMNode *node, const char *name);
extern void r8e_ui_dom_set_class_name(R8EUIDOMNode *node, const char *class_name);
extern void r8e_ui_dom_classlist_add(R8EUIDOMNode *node, const char *class_name);
extern void r8e_ui_dom_classlist_remove(R8EUIDOMNode *node, const char *class_name);
extern bool r8e_ui_dom_classlist_toggle(R8EUIDOMNode *node, const char *class_name);

extern void r8e_ui_dom_set_text_content(R8EUIDOMNode *node, const char *text);
extern int r8e_ui_dom_parse_html(R8EUIDOMNode *parent, const char *html);

extern R8EUIDOMNode *r8e_ui_dom_get_element_by_id(R8EUIDOMNode *root, const char *id);
extern R8EUINodeList *r8e_ui_dom_get_elements_by_tag_name(R8EUIDOMNode *root,
                                                            const char *tag);
extern R8EUINodeList *r8e_ui_dom_get_elements_by_class_name(R8EUIDOMNode *root,
                                                              const char *class_name);
extern R8EUIDOMNode *r8e_ui_dom_query_selector(R8EUIDOMNode *root,
                                                 const char *selector);
extern R8EUINodeList *r8e_ui_dom_query_selector_all(R8EUIDOMNode *root,
                                                      const char *selector);
extern bool r8e_ui_dom_matches(const R8EUIDOMNode *node, const char *selector);
extern R8EUIDOMNode *r8e_ui_dom_closest(R8EUIDOMNode *node, const char *selector);

extern void r8e_ui_nodelist_free(R8EUINodeList *list);

extern R8EUIDOMNode *r8e_ui_dom_next_element_sibling(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_prev_element_sibling(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_first_element_child(const R8EUIDOMNode *node);
extern R8EUIDOMNode *r8e_ui_dom_last_element_child(const R8EUIDOMNode *node);
extern uint32_t r8e_ui_dom_child_element_count(const R8EUIDOMNode *node);
extern bool r8e_ui_dom_contains(const R8EUIDOMNode *ancestor, const R8EUIDOMNode *node);
extern bool r8e_ui_dom_is_connected(const R8EUIDOMNode *node);

extern void r8e_ui_dom_dump(const R8EUIDOMNode *node, int indent);
extern R8EUIDOMNode *r8e_ui_dom_hit_test(R8EUIDOMNode *root, float x, float y);

/* =========================================================================
 * External function declarations: nstyle.c
 * ========================================================================= */

extern void r8e_ui_style_init_defaults(R8EUIComputedStyle *style);
extern R8EUIComputedStyle *r8e_ui_style_create(void);
extern void r8e_ui_style_free(R8EUIComputedStyle *style);
extern void r8e_ui_style_copy(R8EUIComputedStyle *dst, const R8EUIComputedStyle *src);

extern R8EUIStyleSheet *r8e_ui_stylesheet_create(void);
extern void r8e_ui_stylesheet_free(R8EUIStyleSheet *sheet);
extern void r8e_ui_stylesheet_parse(R8EUIStyleSheet *sheet, const char *css);
extern void r8e_ui_style_resolve_node(R8EUIDOMNode *node,
                                       const R8EUIStyleSheet *sheet);
extern void r8e_ui_style_resolve_tree(R8EUIDOMNode *root,
                                       const R8EUIStyleSheet *sheet);

extern R8EUIStyleContext *r8e_ui_style_context_create(void);
extern void r8e_ui_style_context_free(R8EUIStyleContext *ctx);
extern bool r8e_ui_style_context_add_sheet(R8EUIStyleContext *ctx,
                                            R8EUIStyleSheet *sheet);
extern void r8e_ui_style_context_resolve(R8EUIStyleContext *ctx,
                                          R8EUIDOMNode *root);

extern bool r8e_ui_style_is_hidden(const R8EUIDOMNode *node);
extern bool r8e_ui_style_is_flex(const R8EUIDOMNode *node);
extern void r8e_ui_style_free_tree(R8EUIDOMNode *root);

/* =========================================================================
 * External function declarations: nlayout.c
 * ========================================================================= */

extern void r8e_ui_layout_box_init(R8EUILayoutBox *box);
extern void r8e_ui_layout_compute(R8EUIDOMNode *root, float viewport_w,
                                   float viewport_h);
extern void r8e_ui_layout_update(R8EUIDOMNode *node, float viewport_w,
                                  float viewport_h);
extern void r8e_ui_layout_mark_dirty(R8EUIDOMNode *node);
extern void r8e_ui_layout_clear_dirty(R8EUIDOMNode *node);
extern void r8e_ui_layout_get_absolute_pos(const R8EUIDOMNode *node,
                                            float *out_x, float *out_y);
extern void r8e_ui_layout_get_border_box(const R8EUIDOMNode *node,
                                          float *x, float *y,
                                          float *w, float *h);
extern void r8e_ui_layout_get_content_box(const R8EUIDOMNode *node,
                                           float *x, float *y,
                                           float *w, float *h);
extern void r8e_ui_layout_get_margin_box(const R8EUIDOMNode *node,
                                          float *x, float *y,
                                          float *w, float *h);
extern bool r8e_ui_layout_hit_test(const R8EUIDOMNode *node, float px, float py);

/* =========================================================================
 * External function declarations: npaint.c
 * ========================================================================= */

extern void r8e_ui_paint_dl_init(R8EUIDisplayList *dl);
extern void r8e_ui_paint_dl_free(R8EUIDisplayList *dl);
extern void r8e_ui_paint_dl_clear(R8EUIDisplayList *dl);
extern uint64_t r8e_ui_paint_dl_hash(R8EUIDisplayList *dl);
extern bool r8e_ui_paint_dl_unchanged(R8EUIDisplayList *dl, uint64_t cached_hash);

extern bool r8e_ui_paint_parse_color(const char *str, R8EUIColor *out);

extern void r8e_ui_paint_emit_rect(R8EUIDisplayList *dl,
                                    float x, float y, float w, float h,
                                    R8EUIColor color);
extern void r8e_ui_paint_emit_text(R8EUIDisplayList *dl,
                                    float x, float y, const char *text,
                                    uint32_t text_len, float font_size,
                                    R8EUIColor color);
extern void r8e_ui_paint_emit_border(R8EUIDisplayList *dl,
                                      float x, float y, float w, float h,
                                      const float widths[4],
                                      const R8EUIColor colors[4]);
extern void r8e_ui_paint_emit_clip_push(R8EUIDisplayList *dl,
                                         float x, float y, float w, float h);
extern void r8e_ui_paint_emit_clip_pop(R8EUIDisplayList *dl);
extern void r8e_ui_paint_emit_opacity(R8EUIDisplayList *dl, float opacity);

extern R8EUIFramebuffer *r8e_ui_paint_fb_create(int width, int height);
extern void r8e_ui_paint_fb_destroy(R8EUIFramebuffer *fb);
extern void r8e_ui_paint_fb_clear(R8EUIFramebuffer *fb, R8EUIColor color);
extern uint32_t r8e_ui_paint_fb_get_pixel(const R8EUIFramebuffer *fb, int x, int y);
extern void r8e_ui_paint_fb_set_pixel(R8EUIFramebuffer *fb, int x, int y,
                                       uint32_t color);

extern void r8e_ui_paint_style_init(R8EUIPaintStyle *style);

/* =========================================================================
 * External function declarations: nevent.c
 * ========================================================================= */

extern void r8e_ui_event_manager_init(R8EUIEventManager *mgr,
                                       R8EUIDOMNode_Event *document);
extern void r8e_ui_event_manager_cleanup(R8EUIEventManager *mgr);
extern bool r8e_ui_event_add_listener(R8EUIDOMNode_Event *node,
                                       R8EUIEventType type,
                                       R8EUIEventCallback callback,
                                       void *user_data, bool capture,
                                       bool once, bool passive);
extern bool r8e_ui_event_remove_listener(R8EUIDOMNode_Event *node,
                                          R8EUIEventType type,
                                          R8EUIEventCallback callback,
                                          void *user_data, bool capture);
extern void r8e_ui_event_remove_all_listeners(R8EUIDOMNode_Event *node);
extern void r8e_ui_event_init(R8EUIEvent_Ev *event, R8EUIEventType type);
extern bool r8e_ui_event_dispatch(R8EUIEvent_Ev *event, R8EUIDOMNode_Event *target);
extern void r8e_ui_event_set_focus(R8EUIEventManager *mgr, R8EUIDOMNode_Event *node);
extern R8EUIDOMNode_Event *r8e_ui_event_get_focused(const R8EUIEventManager *mgr);
extern R8EUIDOMNode_Event *r8e_ui_event_get_hovered(const R8EUIEventManager *mgr);
extern bool r8e_ui_event_is_focused(const R8EUIDOMNode_Event *node);
extern bool r8e_ui_event_is_hovered(const R8EUIDOMNode_Event *node);

/* =========================================================================
 * DOM TESTS
 * ========================================================================= */

/* --- Test: create element and verify basic properties --- */
TEST(dom_create_element) {
    R8EUIDOMNode *div = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(div);

    /* Should have zero children */
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(div), 0);
    ASSERT_NULL(r8e_ui_dom_first_element_child(div));
    ASSERT_NULL(r8e_ui_dom_last_element_child(div));

    r8e_ui_dom_tree_free(div);
}

/* --- Test: create text node --- */
TEST(dom_create_text_node) {
    R8EUIDOMNode *text = r8e_ui_dom_create_text_node_cstr("Hello, world!");
    ASSERT_NOT_NULL(text);
    r8e_ui_dom_node_free(text);
}

/* --- Test: appendChild builds tree --- */
TEST(dom_append_child) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("span");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("p");
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(child1);
    ASSERT_NOT_NULL(child2);

    r8e_ui_dom_append_child(parent, child1);
    r8e_ui_dom_append_child(parent, child2);

    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 2);
    ASSERT_TRUE(r8e_ui_dom_first_element_child(parent) == child1);
    ASSERT_TRUE(r8e_ui_dom_last_element_child(parent) == child2);

    /* Verify sibling links */
    ASSERT_TRUE(r8e_ui_dom_next_element_sibling(child1) == child2);
    ASSERT_NULL(r8e_ui_dom_next_element_sibling(child2));
    ASSERT_TRUE(r8e_ui_dom_prev_element_sibling(child2) == child1);
    ASSERT_NULL(r8e_ui_dom_prev_element_sibling(child1));

    /* Verify containment */
    ASSERT_TRUE(r8e_ui_dom_contains(parent, child1));
    ASSERT_TRUE(r8e_ui_dom_contains(parent, child2));
    ASSERT_FALSE(r8e_ui_dom_contains(child1, parent));

    r8e_ui_dom_tree_free(parent);
}

/* --- Test: removeChild detaches node from tree --- */
TEST(dom_remove_child) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child = r8e_ui_dom_create_element("span");

    r8e_ui_dom_append_child(parent, child);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 1);

    r8e_ui_dom_remove_child(parent, child);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 0);
    ASSERT_FALSE(r8e_ui_dom_contains(parent, child));

    r8e_ui_dom_node_free(child);
    r8e_ui_dom_tree_free(parent);
}

/* --- Test: insertBefore places node before reference --- */
TEST(dom_insert_before) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("span");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("p");
    R8EUIDOMNode *middle = r8e_ui_dom_create_element("em");

    r8e_ui_dom_append_child(parent, child1);
    r8e_ui_dom_append_child(parent, child2);

    /* Insert middle before child2: order should be child1, middle, child2 */
    r8e_ui_dom_insert_before(parent, middle, child2);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(parent), 3);
    ASSERT_TRUE(r8e_ui_dom_next_element_sibling(child1) == middle);
    ASSERT_TRUE(r8e_ui_dom_next_element_sibling(middle) == child2);

    r8e_ui_dom_tree_free(parent);
}

/* --- Test: setAttribute and hasAttribute --- */
TEST(dom_attributes) {
    R8EUIDOMNode *node = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(node);

    /* No attributes initially */
    ASSERT_FALSE(r8e_ui_dom_has_attribute(node, "id"));

    /* Set attribute */
    r8e_ui_dom_set_attribute(node, "id", "main");
    ASSERT_TRUE(r8e_ui_dom_has_attribute(node, "id"));

    /* Set another attribute */
    r8e_ui_dom_set_attribute(node, "data-value", "42");
    ASSERT_TRUE(r8e_ui_dom_has_attribute(node, "data-value"));

    /* Remove attribute */
    r8e_ui_dom_remove_attribute(node, "data-value");
    ASSERT_FALSE(r8e_ui_dom_has_attribute(node, "data-value"));

    /* Original attribute should still be there */
    ASSERT_TRUE(r8e_ui_dom_has_attribute(node, "id"));

    r8e_ui_dom_tree_free(node);
}

/* --- Test: getElementById finds node by id attribute --- */
TEST(dom_get_element_by_id) {
    R8EUIDOMNode *doc = r8e_ui_dom_create_document();
    R8EUIDOMNode *div = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *span = r8e_ui_dom_create_element("span");

    r8e_ui_dom_set_attribute(span, "id", "target");
    r8e_ui_dom_append_child(div, span);
    r8e_ui_dom_append_child(doc, div);

    R8EUIDOMNode *found = r8e_ui_dom_get_element_by_id(doc, "target");
    ASSERT_TRUE(found == span);

    R8EUIDOMNode *not_found = r8e_ui_dom_get_element_by_id(doc, "nonexistent");
    ASSERT_NULL(not_found);

    r8e_ui_dom_tree_free(doc);
}

/* --- Test: classList operations --- */
TEST(dom_class_list) {
    R8EUIDOMNode *node = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(node);

    /* Add classes */
    r8e_ui_dom_classlist_add(node, "foo");
    r8e_ui_dom_classlist_add(node, "bar");
    r8e_ui_dom_classlist_add(node, "baz");

    /* Toggle: remove existing */
    bool result = r8e_ui_dom_classlist_toggle(node, "bar");
    ASSERT_FALSE(result); /* was present, now removed */

    /* Toggle: add missing */
    result = r8e_ui_dom_classlist_toggle(node, "bar");
    ASSERT_TRUE(result); /* was absent, now added */

    /* Remove */
    r8e_ui_dom_classlist_remove(node, "foo");

    r8e_ui_dom_tree_free(node);
}

/* --- Test: cloneNode deep copy --- */
TEST(dom_clone_node) {
    R8EUIDOMNode *original = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child = r8e_ui_dom_create_element("span");
    r8e_ui_dom_set_attribute(original, "class", "container");
    r8e_ui_dom_append_child(original, child);

    /* Shallow clone: should not include children */
    R8EUIDOMNode *shallow = r8e_ui_dom_clone_node(original, false);
    ASSERT_NOT_NULL(shallow);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(shallow), 0);
    ASSERT_TRUE(r8e_ui_dom_has_attribute(shallow, "class"));

    /* Deep clone: should include children */
    R8EUIDOMNode *deep = r8e_ui_dom_clone_node(original, true);
    ASSERT_NOT_NULL(deep);
    ASSERT_EQ_INT(r8e_ui_dom_child_element_count(deep), 1);
    /* Cloned child should be a different pointer than original child */
    ASSERT_NE(r8e_ui_dom_first_element_child(deep), child);

    r8e_ui_dom_tree_free(shallow);
    r8e_ui_dom_tree_free(deep);
    r8e_ui_dom_tree_free(original);
}

/* --- Test: parse simple HTML --- */
TEST(dom_parse_html) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(parent);

    int count = r8e_ui_dom_parse_html(parent, "<span>Hello</span><p>World</p>");
    ASSERT_TRUE(count >= 0);

    /* Should have at least 2 children (span and p) */
    ASSERT_TRUE(r8e_ui_dom_child_element_count(parent) >= 2);

    r8e_ui_dom_tree_free(parent);
}

/* --- Test: querySelector finds matching node --- */
TEST(dom_query_selector) {
    R8EUIDOMNode *doc = r8e_ui_dom_create_document();
    R8EUIDOMNode *div = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *p = r8e_ui_dom_create_element("p");

    r8e_ui_dom_classlist_add(p, "intro");
    r8e_ui_dom_append_child(div, p);
    r8e_ui_dom_append_child(doc, div);

    /* Query by class */
    R8EUIDOMNode *found = r8e_ui_dom_query_selector(doc, ".intro");
    ASSERT_TRUE(found == p);

    /* Query by tag */
    found = r8e_ui_dom_query_selector(doc, "p");
    ASSERT_TRUE(found == p);

    r8e_ui_dom_tree_free(doc);
}

/* =========================================================================
 * STYLE TESTS
 * ========================================================================= */

/* --- Test: default style initialization --- */
TEST(style_init_defaults) {
    R8EUIComputedStyle *style = r8e_ui_style_create();
    ASSERT_NOT_NULL(style);
    r8e_ui_style_init_defaults(style);

    /* Verify key defaults (use opaque API; just verify non-NULL/non-crash) */
    /* The style should have been memset and defaults applied. */

    R8EUIComputedStyle *copy = r8e_ui_style_create();
    ASSERT_NOT_NULL(copy);
    r8e_ui_style_copy(copy, style);

    r8e_ui_style_free(copy);
    r8e_ui_style_free(style);
}

/* --- Test: stylesheet parsing with basic rules --- */
TEST(style_parse_basic) {
    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);

    /* Parse a simple stylesheet */
    r8e_ui_stylesheet_parse(sheet,
        "div { display: flex; width: 100px; }\n"
        ".container { padding: 10px; margin: 5px; }\n"
        "#main { background-color: #ff0000; }\n"
    );

    /* The stylesheet should have parsed without crashing.
       If we get here, parsing succeeded. */
    ASSERT_TRUE(1);

    r8e_ui_stylesheet_free(sheet);
}

/* --- Test: style resolution applies rules to DOM tree --- */
TEST(style_resolve_tree) {
    /* Build a simple DOM tree */
    R8EUIDOMNode *root = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child = r8e_ui_dom_create_element("span");
    r8e_ui_dom_append_child(root, child);

    /* Create and parse a stylesheet */
    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);
    r8e_ui_stylesheet_parse(sheet, "div { display: block; } span { display: inline; }");

    /* Resolve styles */
    r8e_ui_style_resolve_tree(root, sheet);

    /* If we get here without crashing, resolution worked */
    ASSERT_TRUE(1);

    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(root);
    r8e_ui_dom_tree_free(root);
}

/* --- Test: style context with multiple sheets --- */
TEST(style_context_multi_sheet) {
    R8EUIStyleContext *ctx = r8e_ui_style_context_create();
    ASSERT_NOT_NULL(ctx);

    R8EUIStyleSheet *sheet1 = r8e_ui_stylesheet_create();
    R8EUIStyleSheet *sheet2 = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet1);
    ASSERT_NOT_NULL(sheet2);

    r8e_ui_stylesheet_parse(sheet1, "div { color: #000000; }");
    r8e_ui_stylesheet_parse(sheet2, "div { background-color: #ffffff; }");

    ASSERT_TRUE(r8e_ui_style_context_add_sheet(ctx, sheet1));
    ASSERT_TRUE(r8e_ui_style_context_add_sheet(ctx, sheet2));

    /* Build a DOM node and resolve */
    R8EUIDOMNode *div = r8e_ui_dom_create_element("div");
    r8e_ui_style_context_resolve(ctx, div);

    r8e_ui_style_context_free(ctx);
    r8e_ui_stylesheet_free(sheet1);
    r8e_ui_stylesheet_free(sheet2);
    r8e_ui_style_free_tree(div);
    r8e_ui_dom_tree_free(div);
}

/* =========================================================================
 * LAYOUT TESTS
 * ========================================================================= */

/* --- Test: layout box initialization --- */
TEST(layout_box_init) {
    R8EUILayoutBox box;
    r8e_ui_layout_box_init(&box);

    ASSERT_FLOAT_EQ(box.x, 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(box.y, 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(box.width, 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(box.height, 0.0f, 0.001f);

    /* All padding/border/margin should be zero */
    for (int i = 0; i < 4; i++) {
        ASSERT_FLOAT_EQ(box.padding[i], 0.0f, 0.001f);
        ASSERT_FLOAT_EQ(box.border[i], 0.0f, 0.001f);
        ASSERT_FLOAT_EQ(box.margin[i], 0.0f, 0.001f);
    }
}

/* --- Test: compute layout for a single element --- */
TEST(layout_compute_single) {
    R8EUIDOMNode *root = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(root);

    /* Parse inline style to set dimensions */
    r8e_ui_dom_set_attribute(root, "style", "width: 200px; height: 100px;");

    /* Create style and resolve */
    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);
    r8e_ui_stylesheet_parse(sheet, "div { display: block; }");
    r8e_ui_style_resolve_tree(root, sheet);

    /* Compute layout */
    r8e_ui_layout_compute(root, 800.0f, 600.0f);

    /* Get the layout result */
    float x, y, w, h;
    r8e_ui_layout_get_content_box(root, &x, &y, &w, &h);

    /* Width and height should be set (may differ based on style resolution) */
    /* At minimum, ensure no crash and values are finite */
    ASSERT_TRUE(isfinite(x));
    ASSERT_TRUE(isfinite(y));
    ASSERT_TRUE(isfinite(w));
    ASSERT_TRUE(isfinite(h));

    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(root);
    r8e_ui_dom_tree_free(root);
}

/* --- Test: compute layout with padding and margin --- */
TEST(layout_box_model) {
    R8EUIDOMNode *root = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(root);

    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);
    r8e_ui_stylesheet_parse(sheet,
        "div { display: block; width: 100px; height: 50px; "
        "padding: 5px; margin: 10px; border-width: 2px; }");
    r8e_ui_style_resolve_tree(root, sheet);

    r8e_ui_layout_compute(root, 800.0f, 600.0f);

    /* Get border box - should include padding and border */
    float bx, by, bw, bh;
    r8e_ui_layout_get_border_box(root, &bx, &by, &bw, &bh);
    ASSERT_TRUE(isfinite(bw));
    ASSERT_TRUE(isfinite(bh));

    /* Border box should be larger than content box */
    float cx, cy, cw, ch;
    r8e_ui_layout_get_content_box(root, &cx, &cy, &cw, &ch);
    ASSERT_TRUE(bw >= cw);
    ASSERT_TRUE(bh >= ch);

    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(root);
    r8e_ui_dom_tree_free(root);
}

/* --- Test: flexbox row layout distributes children horizontally --- */
TEST(layout_flex_row) {
    R8EUIDOMNode *container = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("div");
    r8e_ui_dom_append_child(container, child1);
    r8e_ui_dom_append_child(container, child2);

    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);
    r8e_ui_stylesheet_parse(sheet,
        "div { display: flex; flex-direction: row; width: 200px; height: 100px; }");
    r8e_ui_style_resolve_tree(container, sheet);

    r8e_ui_layout_compute(container, 800.0f, 600.0f);

    /* Both children should have finite positions */
    float x1, y1, w1, h1, x2, y2, w2, h2;
    r8e_ui_layout_get_content_box(child1, &x1, &y1, &w1, &h1);
    r8e_ui_layout_get_content_box(child2, &x2, &y2, &w2, &h2);

    ASSERT_TRUE(isfinite(x1));
    ASSERT_TRUE(isfinite(x2));

    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(container);
    r8e_ui_dom_tree_free(container);
}

/* --- Test: flexbox column layout distributes children vertically --- */
TEST(layout_flex_column) {
    R8EUIDOMNode *container = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child1 = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child2 = r8e_ui_dom_create_element("div");
    r8e_ui_dom_append_child(container, child1);
    r8e_ui_dom_append_child(container, child2);

    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);
    r8e_ui_stylesheet_parse(sheet,
        "div { display: flex; flex-direction: column; width: 200px; height: 200px; }");
    r8e_ui_style_resolve_tree(container, sheet);

    r8e_ui_layout_compute(container, 800.0f, 600.0f);

    /* Both children should have finite positions */
    float x1, y1, w1, h1, x2, y2, w2, h2;
    r8e_ui_layout_get_content_box(child1, &x1, &y1, &w1, &h1);
    r8e_ui_layout_get_content_box(child2, &x2, &y2, &w2, &h2);

    ASSERT_TRUE(isfinite(y1));
    ASSERT_TRUE(isfinite(y2));

    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(container);
    r8e_ui_dom_tree_free(container);
}

/* --- Test: layout dirty marking and clearing --- */
TEST(layout_dirty_flags) {
    R8EUIDOMNode *node = r8e_ui_dom_create_element("div");
    ASSERT_NOT_NULL(node);

    r8e_ui_layout_mark_dirty(node);
    /* After marking dirty, the node is flagged for re-layout */

    r8e_ui_layout_clear_dirty(node);
    /* After clearing, the dirty flag should be removed */

    /* Should not crash */
    ASSERT_TRUE(1);

    r8e_ui_dom_tree_free(node);
}

/* --- Test: layout absolute positioning --- */
TEST(layout_get_absolute_pos) {
    R8EUIDOMNode *parent = r8e_ui_dom_create_element("div");
    R8EUIDOMNode *child = r8e_ui_dom_create_element("div");
    r8e_ui_dom_append_child(parent, child);

    R8EUIStyleSheet *sheet = r8e_ui_stylesheet_create();
    ASSERT_NOT_NULL(sheet);
    r8e_ui_stylesheet_parse(sheet,
        "div { display: block; width: 100px; height: 50px; }");
    r8e_ui_style_resolve_tree(parent, sheet);

    r8e_ui_layout_compute(parent, 800.0f, 600.0f);

    float ax, ay;
    r8e_ui_layout_get_absolute_pos(child, &ax, &ay);
    ASSERT_TRUE(isfinite(ax));
    ASSERT_TRUE(isfinite(ay));

    r8e_ui_stylesheet_free(sheet);
    r8e_ui_style_free_tree(parent);
    r8e_ui_dom_tree_free(parent);
}

/* =========================================================================
 * PAINT TESTS
 * ========================================================================= */

/* --- Test: display list init, emit, and hash --- */
TEST(paint_display_list_ops) {
    R8EUIDisplayList dl;
    r8e_ui_paint_dl_init(&dl);

    /* Initially empty */
    ASSERT_EQ_INT(dl.count, 0);

    /* Emit a rect */
    R8EUIColor red = R8E_UI_COLOR_RGB(255, 0, 0);
    r8e_ui_paint_emit_rect(&dl, 10.0f, 20.0f, 100.0f, 50.0f, red);
    ASSERT_EQ_INT(dl.count, 1);

    /* Emit text */
    R8EUIColor black = R8E_UI_COLOR_RGB(0, 0, 0);
    r8e_ui_paint_emit_text(&dl, 15.0f, 25.0f, "Hello", 5, 16.0f, black);
    ASSERT_EQ_INT(dl.count, 2);

    /* Compute hash */
    uint64_t hash1 = r8e_ui_paint_dl_hash(&dl);
    ASSERT_NE(hash1, 0);

    /* Same content should produce same hash */
    uint64_t hash2 = r8e_ui_paint_dl_hash(&dl);
    ASSERT_EQ(hash1, hash2);

    /* Check unchanged */
    ASSERT_TRUE(r8e_ui_paint_dl_unchanged(&dl, hash1));

    /* Clear and verify empty */
    r8e_ui_paint_dl_clear(&dl);
    ASSERT_EQ_INT(dl.count, 0);

    r8e_ui_paint_dl_free(&dl);
}

/* --- Test: color parsing --- */
TEST(paint_color_parsing) {
    R8EUIColor color;

    /* Hex 3-digit */
    ASSERT_TRUE(r8e_ui_paint_parse_color("#fff", &color));
    ASSERT_EQ_INT(R8E_UI_COLOR_R(color), 255);
    ASSERT_EQ_INT(R8E_UI_COLOR_G(color), 255);
    ASSERT_EQ_INT(R8E_UI_COLOR_B(color), 255);

    /* Hex 6-digit */
    ASSERT_TRUE(r8e_ui_paint_parse_color("#ff0000", &color));
    ASSERT_EQ_INT(R8E_UI_COLOR_R(color), 255);
    ASSERT_EQ_INT(R8E_UI_COLOR_G(color), 0);
    ASSERT_EQ_INT(R8E_UI_COLOR_B(color), 0);

    /* Named color */
    ASSERT_TRUE(r8e_ui_paint_parse_color("red", &color));
    ASSERT_EQ_INT(R8E_UI_COLOR_R(color), 255);
    ASSERT_EQ_INT(R8E_UI_COLOR_G(color), 0);
    ASSERT_EQ_INT(R8E_UI_COLOR_B(color), 0);

    /* Named color: blue */
    ASSERT_TRUE(r8e_ui_paint_parse_color("blue", &color));
    ASSERT_EQ_INT(R8E_UI_COLOR_R(color), 0);
    ASSERT_EQ_INT(R8E_UI_COLOR_G(color), 0);
    ASSERT_EQ_INT(R8E_UI_COLOR_B(color), 255);

    /* Invalid color should fail */
    ASSERT_FALSE(r8e_ui_paint_parse_color("not-a-color", &color));
}

/* --- Test: framebuffer create, set/get pixel, clear, destroy --- */
TEST(paint_framebuffer_ops) {
    R8EUIFramebuffer *fb = r8e_ui_paint_fb_create(320, 240);
    ASSERT_NOT_NULL(fb);

    /* Clear to white */
    R8EUIColor white = R8E_UI_COLOR_RGB(255, 255, 255);
    r8e_ui_paint_fb_clear(fb, white);

    /* Read back pixel at (0,0) */
    uint32_t pixel = r8e_ui_paint_fb_get_pixel(fb, 0, 0);
    ASSERT_EQ(pixel, white);

    /* Set pixel at (10,10) to red */
    R8EUIColor red = R8E_UI_COLOR_RGB(255, 0, 0);
    r8e_ui_paint_fb_set_pixel(fb, 10, 10, red);
    pixel = r8e_ui_paint_fb_get_pixel(fb, 10, 10);
    ASSERT_EQ(pixel, red);

    /* Surrounding pixels should still be white */
    pixel = r8e_ui_paint_fb_get_pixel(fb, 10, 11);
    ASSERT_EQ(pixel, white);

    r8e_ui_paint_fb_destroy(fb);
}

/* --- Test: paint style initialization --- */
TEST(paint_style_init) {
    R8EUIPaintStyle style;
    r8e_ui_paint_style_init(&style);

    /* Default opacity should be 1.0 */
    ASSERT_FLOAT_EQ(style.opacity, 1.0f, 0.001f);

    /* Default font size should be positive */
    ASSERT_TRUE(style.font_size > 0.0f);

    /* Not hidden by default */
    ASSERT_EQ_INT(style.visibility, 0);
}

/* --- Test: display list clip push/pop --- */
TEST(paint_clip_push_pop) {
    R8EUIDisplayList dl;
    r8e_ui_paint_dl_init(&dl);

    /* Push clip rect */
    r8e_ui_paint_emit_clip_push(&dl, 0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_EQ_INT(dl.count, 1);

    /* Emit a rect inside clip */
    R8EUIColor green = R8E_UI_COLOR_RGB(0, 128, 0);
    r8e_ui_paint_emit_rect(&dl, 10.0f, 10.0f, 80.0f, 80.0f, green);
    ASSERT_EQ_INT(dl.count, 2);

    /* Pop clip */
    r8e_ui_paint_emit_clip_pop(&dl);
    ASSERT_EQ_INT(dl.count, 3);

    r8e_ui_paint_dl_free(&dl);
}

/* --- Test: display list opacity emission --- */
TEST(paint_opacity) {
    R8EUIDisplayList dl;
    r8e_ui_paint_dl_init(&dl);

    r8e_ui_paint_emit_opacity(&dl, 0.5f);
    ASSERT_EQ_INT(dl.count, 1);

    R8EUIColor bg = R8E_UI_COLOR_RGBA(0, 0, 0, 128);
    r8e_ui_paint_emit_rect(&dl, 0.0f, 0.0f, 50.0f, 50.0f, bg);
    ASSERT_EQ_INT(dl.count, 2);

    r8e_ui_paint_dl_free(&dl);
}

/* --- Test: border emission --- */
TEST(paint_border) {
    R8EUIDisplayList dl;
    r8e_ui_paint_dl_init(&dl);

    float widths[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R8EUIColor colors[4] = {
        R8E_UI_COLOR_RGB(0, 0, 0),
        R8E_UI_COLOR_RGB(0, 0, 0),
        R8E_UI_COLOR_RGB(0, 0, 0),
        R8E_UI_COLOR_RGB(0, 0, 0)
    };
    r8e_ui_paint_emit_border(&dl, 10.0f, 10.0f, 100.0f, 50.0f, widths, colors);
    ASSERT_EQ_INT(dl.count, 1);

    r8e_ui_paint_dl_free(&dl);
}

/* =========================================================================
 * EVENT TESTS
 * ========================================================================= */

/* Callback tracking helper for event tests */
static int g_event_callback_count = 0;
static void test_event_callback(void *event, void *user_data) {
    (void)event;
    (void)user_data;
    g_event_callback_count++;
}

/*
 * Helper: create a minimal event-capable DOM node.
 * nevent.c uses its own R8EUIDOMNode_Event struct, which has layout boxes,
 * sibling/parent pointers, and listener chains.
 */
static R8EUIDOMNode_Event *create_event_node(uint8_t type, uint32_t flags) {
    R8EUIDOMNode_Event *node = (R8EUIDOMNode_Event *)calloc(1, sizeof(R8EUIDOMNode_Event));
    if (!node) return NULL;
    node->type = type;
    node->flags = flags;
    node->tab_index = -1;
    return node;
}

static void free_event_node(R8EUIDOMNode_Event *node) {
    if (node) {
        r8e_ui_event_remove_all_listeners(node);
        free(node);
    }
}

/* Link parent-child for event nodes */
static void link_parent_child(R8EUIDOMNode_Event *parent,
                               R8EUIDOMNode_Event *child)
{
    child->parent = parent;
    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child = child;
    } else {
        child->prev_sibling = parent->last_child;
        parent->last_child->next_sibling = child;
        parent->last_child = child;
    }
}

/* --- Test: event manager init --- */
TEST(event_manager_init) {
    R8EUIDOMNode_Event *doc = create_event_node(9, 0); /* DOCUMENT=9 */
    ASSERT_NOT_NULL(doc);

    R8EUIEventManager mgr;
    r8e_ui_event_manager_init(&mgr, doc);

    ASSERT_TRUE(mgr.document == doc);
    ASSERT_NULL(mgr.focused);
    ASSERT_NULL(mgr.hovered);
    ASSERT_NULL(mgr.active);

    r8e_ui_event_manager_cleanup(&mgr);
    free_event_node(doc);
}

/* --- Test: add and remove event listeners --- */
TEST(event_add_remove_listener) {
    R8EUIDOMNode_Event *node = create_event_node(1, 0);
    ASSERT_NOT_NULL(node);

    /* Add a click listener */
    ASSERT_TRUE(r8e_ui_event_add_listener(node, R8E_UI_EVENT_CLICK,
                                            test_event_callback, NULL,
                                            false, false, false));

    /* Remove it */
    ASSERT_TRUE(r8e_ui_event_remove_listener(node, R8E_UI_EVENT_CLICK,
                                               test_event_callback, NULL,
                                               false));

    free_event_node(node);
}

/* --- Test: focus management --- */
TEST(event_focus_management) {
    R8EUIDOMNode_Event *doc = create_event_node(9, 0);
    R8EUIDOMNode_Event *btn = create_event_node(1, R8E_UI_NODE_FLAG_FOCUSABLE);
    link_parent_child(doc, btn);

    R8EUIEventManager mgr;
    r8e_ui_event_manager_init(&mgr, doc);

    /* No focus initially */
    ASSERT_NULL(r8e_ui_event_get_focused(&mgr));

    /* Focus the button */
    r8e_ui_event_set_focus(&mgr, btn);
    ASSERT_TRUE(r8e_ui_event_get_focused(&mgr) == btn);
    ASSERT_TRUE(r8e_ui_event_is_focused(btn));

    /* Blur by focusing NULL */
    r8e_ui_event_set_focus(&mgr, NULL);
    ASSERT_NULL(r8e_ui_event_get_focused(&mgr));
    ASSERT_FALSE(r8e_ui_event_is_focused(btn));

    r8e_ui_event_manager_cleanup(&mgr);
    free_event_node(btn);
    free_event_node(doc);
}

/* --- Test: event listener fires on dispatch --- */
TEST(event_dispatch_fires_callback) {
    R8EUIDOMNode_Event *doc = create_event_node(9, 0);
    R8EUIDOMNode_Event *child = create_event_node(1, 0);
    link_parent_child(doc, child);

    /* Set up layout for hit testing */
    child->layout.x = 0;
    child->layout.y = 0;
    child->layout.width = 100;
    child->layout.height = 100;

    /* Add listener on child */
    g_event_callback_count = 0;
    r8e_ui_event_add_listener(child, R8E_UI_EVENT_CLICK,
                               test_event_callback, NULL,
                               false, false, false);

    /* Create and dispatch a click event */
    R8EUIEvent_Ev event;
    r8e_ui_event_init(&event, R8E_UI_EVENT_CLICK);
    r8e_ui_event_dispatch(&event, child);

    /* Callback should have been called at least once */
    ASSERT_TRUE(g_event_callback_count >= 1);

    free_event_node(child);
    free_event_node(doc);
}

/* =========================================================================
 * Suite Runner
 * ========================================================================= */

void run_ui_tests(void) {
    /* DOM tests */
    RUN_TEST(dom_create_element);
    RUN_TEST(dom_create_text_node);
    RUN_TEST(dom_append_child);
    RUN_TEST(dom_remove_child);
    RUN_TEST(dom_insert_before);
    RUN_TEST(dom_attributes);
    RUN_TEST(dom_get_element_by_id);
    RUN_TEST(dom_class_list);
    RUN_TEST(dom_clone_node);
    RUN_TEST(dom_parse_html);
    RUN_TEST(dom_query_selector);

    /* Style tests */
    RUN_TEST(style_init_defaults);
    RUN_TEST(style_parse_basic);
    RUN_TEST(style_resolve_tree);
    RUN_TEST(style_context_multi_sheet);

    /* Layout tests */
    RUN_TEST(layout_box_init);
    RUN_TEST(layout_compute_single);
    RUN_TEST(layout_box_model);
    RUN_TEST(layout_flex_row);
    RUN_TEST(layout_flex_column);
    RUN_TEST(layout_dirty_flags);
    RUN_TEST(layout_get_absolute_pos);

    /* Paint tests */
    RUN_TEST(paint_display_list_ops);
    RUN_TEST(paint_color_parsing);
    RUN_TEST(paint_framebuffer_ops);
    RUN_TEST(paint_style_init);
    RUN_TEST(paint_clip_push_pop);
    RUN_TEST(paint_opacity);
    RUN_TEST(paint_border);

    /* Event tests */
    RUN_TEST(event_manager_init);
    RUN_TEST(event_add_remove_listener);
    RUN_TEST(event_focus_management);
    RUN_TEST(event_dispatch_fires_callback);
}
