/*
 * r8e_test_stubs.c - Stub implementations for missing symbols in tests
 *
 * This file provides weak stubs for functions referenced by test files
 * but not yet implemented in source files. When real implementations
 * exist, the linker will prefer them over these weak stubs.
 *
 * Only compiled when R8E_TESTING is defined (i.e., `make test`).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_TESTING
#error "This file should only be compiled for test builds"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

static inline R8EValue stub_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline R8EValue stub_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}

/* Forward-declare opaque types used by tests */
typedef struct R8EContext R8EContext;
typedef struct R8EFunction R8EFunction;
typedef struct R8ERealm R8ERealm;

/* Attribute for weak symbols (macOS / Linux) */
#if defined(__GNUC__) || defined(__clang__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

typedef int R8EStatus;
#define R8E_OK 0
#define R8E_ERROR (-1)

/* =========================================================================
 * Promise module stubs
 * ========================================================================= */
typedef struct R8EPromiseContext R8EPromiseContext;
typedef void (*R8EUnhandledRejectionFn)(R8EPromiseContext *ctx,
    R8EValue promise, R8EValue reason, void *user_data);

/* Minimal R8EPromiseContext for stubs */
typedef struct R8EMicrotaskQueue {
    void *items;
    uint32_t head, tail, capacity, count;
} R8EMicrotaskQueue;

struct R8EPromiseContextStub {
    R8EMicrotaskQueue queue;
    R8EUnhandledRejectionFn unhandled_handler;
    void *unhandled_user_data;
};

WEAK R8EPromiseContext *r8e_promise_context_new(void) {
    struct R8EPromiseContextStub *ctx = calloc(1, sizeof(*ctx));
    return (R8EPromiseContext *)ctx;
}

WEAK void r8e_promise_context_free(R8EPromiseContext *ctx) {
    free(ctx);
}

WEAK void r8e_microtask_drain(R8EPromiseContext *ctx) {
    (void)ctx;
}

WEAK uint32_t r8e_microtask_pending_count(R8EPromiseContext *ctx) {
    (void)ctx;
    return 0;
}

/* =========================================================================
 * Iterator module stubs - now provided by r8e_iterator.c Section 15
 * ========================================================================= */
typedef struct R8EIterContext R8EIterContext;

/* All iterator stubs removed - real implementations in r8e_iterator.c */

/* =========================================================================
 * Proxy/Reflect stubs - now provided by r8e_proxy.c Section G
 * ========================================================================= */

/* All proxy and reflect stubs removed - real implementations in r8e_proxy.c */

/* =========================================================================
 * Bytecode stubs (r8e_bc_destroy is static in r8e_bc.c but extern in r8e_api.c)
 * ========================================================================= */
typedef struct R8EBytecodeBuffer R8EBytecodeBuffer;
WEAK void r8e_bc_destroy(R8EBytecodeBuffer *bc) { (void)bc; }

/* =========================================================================
 * WeakRef / FinalizationRegistry stubs
 * ========================================================================= */

WEAK R8EValue r8e_finreg_new(R8EContext *ctx, R8EValue callback) {
    (void)ctx; (void)callback;
    return R8E_UNDEFINED;
}

WEAK bool r8e_finreg_register(R8EContext *ctx, R8EValue finreg,
                               R8EValue target, R8EValue held_value,
                               R8EValue unregister_token) {
    (void)ctx; (void)finreg; (void)target;
    (void)held_value; (void)unregister_token;
    return false;
}

WEAK bool r8e_finreg_unregister(R8EContext *ctx, R8EValue finreg,
                                 R8EValue unregister_token) {
    (void)ctx; (void)finreg; (void)unregister_token;
    return false;
}

WEAK void r8e_weak_table_init(R8EContext *ctx) {
    (void)ctx;
}

WEAK void r8e_weak_table_free(R8EContext *ctx) {
    (void)ctx;
}

WEAK R8EValue r8e_weakref_test_make_object(R8EContext *ctx) {
    (void)ctx;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_weakref_test_make_callback(R8EContext *ctx) {
    (void)ctx;
    return R8E_UNDEFINED;
}

/* Map iterator stubs */
WEAK R8EValue r8e_map_keys(R8EContext *ctx, R8EValue map) {
    (void)ctx; (void)map;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_map_values(R8EContext *ctx, R8EValue map) {
    (void)ctx; (void)map;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_map_entries(R8EContext *ctx, R8EValue map) {
    (void)ctx; (void)map;
    return R8E_UNDEFINED;
}

/* Set iterator stubs */
WEAK R8EValue r8e_set_values(R8EContext *ctx, R8EValue set) {
    (void)ctx; (void)set;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_set_entries(R8EContext *ctx, R8EValue set) {
    (void)ctx; (void)set;
    return R8E_UNDEFINED;
}

/* =========================================================================
 * Security deep stubs
 * ========================================================================= */
typedef struct R8ETypedArrayBuffer R8ETypedArrayBuffer;
typedef int R8ETypedKind;

/* r8e_typedarray_create/length/destroy exist in r8e_arena.c - not stubbed.
   r8e_typedarray_get/set exist with different signatures - not stubbed.
   r8e_bounds_clamp is static inline in r8e_arena.c, so we need a stub. */
WEAK int32_t r8e_bounds_clamp(int32_t idx, int32_t length) {
    if (idx < 0) return 0;
    if (idx >= length) return length > 0 ? length - 1 : 0;
    return idx;
}

/* r8e_capability_create_timer, r8e_capability_check_timer, r8e_capability_check_net
   exist in r8e_capability.c - not stubbed */

/* =========================================================================
 * UI module stubs
 * ========================================================================= */

/* DOM types */
typedef struct R8EUIDOMNode R8EUIDOMNode;
typedef struct R8EUINodeList {
    R8EUIDOMNode **nodes;
    uint32_t count;
} R8EUINodeList;

typedef struct R8EUIComputedStyle R8EUIComputedStyle;
typedef struct R8EUIStyleSheet R8EUIStyleSheet;
typedef struct R8EUIStyleContext R8EUIStyleContext;
typedef struct R8EUILayoutBox R8EUILayoutBox;
typedef struct R8EUIDisplayList R8EUIDisplayList;
typedef struct R8EUIFramebuffer R8EUIFramebuffer;
typedef struct R8EUIPaintStyle R8EUIPaintStyle;
typedef struct R8EUIEventManager R8EUIEventManager;
typedef struct R8EUIDOMNode_Event R8EUIDOMNode_Event;
typedef struct R8EUIEvent_Ev R8EUIEvent_Ev;
typedef struct R8EUIColor { uint8_t r, g, b, a; } R8EUIColor;
typedef int R8EUIEventType;
typedef void (*R8EUIEventHandler)(R8EUIEvent_Ev *event, void *user_data);

/* DOM creation */
WEAK R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag) {
    (void)tag; return NULL;
}
WEAK R8EUIDOMNode *r8e_ui_dom_create_text_node(const char *text, uint32_t len) {
    (void)text; (void)len; return NULL;
}
WEAK R8EUIDOMNode *r8e_ui_dom_create_text_node_cstr(const char *text) {
    (void)text; return NULL;
}
WEAK R8EUIDOMNode *r8e_ui_dom_create_fragment(void) { return NULL; }
WEAK R8EUIDOMNode *r8e_ui_dom_create_document(void) { return NULL; }
WEAK R8EUIDOMNode *r8e_ui_dom_create_comment(const char *text, uint32_t len) {
    (void)text; (void)len; return NULL;
}
WEAK void r8e_ui_dom_node_free(R8EUIDOMNode *node) { (void)node; }
WEAK void r8e_ui_dom_tree_free(R8EUIDOMNode *node) { (void)node; }

/* DOM tree manipulation */
WEAK R8EUIDOMNode *r8e_ui_dom_append_child(R8EUIDOMNode *p, R8EUIDOMNode *c) {
    (void)p; (void)c; return c;
}
WEAK R8EUIDOMNode *r8e_ui_dom_remove_child(R8EUIDOMNode *p, R8EUIDOMNode *c) {
    (void)p; (void)c; return c;
}
WEAK R8EUIDOMNode *r8e_ui_dom_insert_before(R8EUIDOMNode *p,
    R8EUIDOMNode *new_child, R8EUIDOMNode *ref) {
    (void)p; (void)new_child; (void)ref; return new_child;
}
WEAK R8EUIDOMNode *r8e_ui_dom_replace_child(R8EUIDOMNode *p,
    R8EUIDOMNode *new_child, R8EUIDOMNode *old_child) {
    (void)p; (void)new_child; (void)old_child; return old_child;
}
WEAK R8EUIDOMNode *r8e_ui_dom_clone_node(const R8EUIDOMNode *node, bool deep) {
    (void)node; (void)deep; return NULL;
}

/* DOM attributes */
WEAK void r8e_ui_dom_set_attribute(R8EUIDOMNode *n, const char *name,
    const char *value) { (void)n; (void)name; (void)value; }
WEAK bool r8e_ui_dom_has_attribute(const R8EUIDOMNode *n, const char *name) {
    (void)n; (void)name; return false;
}
WEAK void r8e_ui_dom_remove_attribute(R8EUIDOMNode *n, const char *name) {
    (void)n; (void)name;
}
WEAK void r8e_ui_dom_set_class_name(R8EUIDOMNode *n, const char *cn) {
    (void)n; (void)cn;
}
WEAK void r8e_ui_dom_classlist_add(R8EUIDOMNode *n, const char *cn) {
    (void)n; (void)cn;
}
WEAK void r8e_ui_dom_classlist_remove(R8EUIDOMNode *n, const char *cn) {
    (void)n; (void)cn;
}
WEAK bool r8e_ui_dom_classlist_toggle(R8EUIDOMNode *n, const char *cn) {
    (void)n; (void)cn; return false;
}
WEAK void r8e_ui_dom_set_text_content(R8EUIDOMNode *n, const char *t) {
    (void)n; (void)t;
}
WEAK int r8e_ui_dom_parse_html(R8EUIDOMNode *parent, const char *html) {
    (void)parent; (void)html; return 0;
}

/* DOM queries */
WEAK R8EUIDOMNode *r8e_ui_dom_get_element_by_id(R8EUIDOMNode *root, const char *id) {
    (void)root; (void)id; return NULL;
}
WEAK R8EUINodeList *r8e_ui_dom_get_elements_by_tag_name(R8EUIDOMNode *root,
    const char *tag) { (void)root; (void)tag; return NULL; }
WEAK R8EUINodeList *r8e_ui_dom_get_elements_by_class_name(R8EUIDOMNode *root,
    const char *cn) { (void)root; (void)cn; return NULL; }
WEAK R8EUIDOMNode *r8e_ui_dom_query_selector(R8EUIDOMNode *root,
    const char *sel) { (void)root; (void)sel; return NULL; }
WEAK R8EUINodeList *r8e_ui_dom_query_selector_all(R8EUIDOMNode *root,
    const char *sel) { (void)root; (void)sel; return NULL; }
WEAK bool r8e_ui_dom_matches(const R8EUIDOMNode *n, const char *sel) {
    (void)n; (void)sel; return false;
}
WEAK R8EUIDOMNode *r8e_ui_dom_closest(R8EUIDOMNode *n, const char *sel) {
    (void)n; (void)sel; return NULL;
}
WEAK void r8e_ui_nodelist_free(R8EUINodeList *list) { (void)list; }

/* DOM traversal */
WEAK R8EUIDOMNode *r8e_ui_dom_next_element_sibling(const R8EUIDOMNode *n) {
    (void)n; return NULL;
}
WEAK R8EUIDOMNode *r8e_ui_dom_prev_element_sibling(const R8EUIDOMNode *n) {
    (void)n; return NULL;
}
WEAK R8EUIDOMNode *r8e_ui_dom_first_element_child(const R8EUIDOMNode *n) {
    (void)n; return NULL;
}
WEAK R8EUIDOMNode *r8e_ui_dom_last_element_child(const R8EUIDOMNode *n) {
    (void)n; return NULL;
}
WEAK uint32_t r8e_ui_dom_child_element_count(const R8EUIDOMNode *n) {
    (void)n; return 0;
}
WEAK bool r8e_ui_dom_contains(const R8EUIDOMNode *a, const R8EUIDOMNode *n) {
    (void)a; (void)n; return false;
}
WEAK bool r8e_ui_dom_is_connected(const R8EUIDOMNode *n) {
    (void)n; return false;
}
WEAK void r8e_ui_dom_dump(const R8EUIDOMNode *n, int indent) {
    (void)n; (void)indent;
}
WEAK R8EUIDOMNode *r8e_ui_dom_hit_test(R8EUIDOMNode *root, float x, float y) {
    (void)root; (void)x; (void)y; return NULL;
}

/* Style */
WEAK void r8e_ui_style_init_defaults(R8EUIComputedStyle *style) { (void)style; }
WEAK R8EUIComputedStyle *r8e_ui_style_create(void) { return NULL; }
WEAK void r8e_ui_style_free(R8EUIComputedStyle *s) { (void)s; }
WEAK void r8e_ui_style_copy(R8EUIComputedStyle *d, const R8EUIComputedStyle *s) {
    (void)d; (void)s;
}
WEAK R8EUIStyleSheet *r8e_ui_stylesheet_create(void) { return NULL; }
WEAK void r8e_ui_stylesheet_free(R8EUIStyleSheet *s) { (void)s; }
WEAK void r8e_ui_stylesheet_parse(R8EUIStyleSheet *s, const char *css) {
    (void)s; (void)css;
}
WEAK void r8e_ui_style_resolve_node(R8EUIDOMNode *n,
    R8EUIStyleSheet *sheet) { (void)n; (void)sheet; }
WEAK void r8e_ui_style_resolve_tree(R8EUIDOMNode *root,
    R8EUIStyleSheet *sheet) { (void)root; (void)sheet; }
WEAK R8EUIStyleContext *r8e_ui_style_context_create(void) { return NULL; }
WEAK void r8e_ui_style_context_free(R8EUIStyleContext *c) { (void)c; }
WEAK bool r8e_ui_style_context_add_sheet(R8EUIStyleContext *c,
    R8EUIStyleSheet *s) { (void)c; (void)s; return false; }
WEAK void r8e_ui_style_context_resolve(R8EUIStyleContext *c,
    R8EUIDOMNode *root) { (void)c; (void)root; }
WEAK bool r8e_ui_style_is_hidden(const R8EUIDOMNode *n) {
    (void)n; return false;
}
WEAK bool r8e_ui_style_is_flex(const R8EUIDOMNode *n) {
    (void)n; return false;
}
WEAK void r8e_ui_style_free_tree(R8EUIDOMNode *root) { (void)root; }

/* Layout */
WEAK void r8e_ui_layout_box_init(R8EUILayoutBox *box) { (void)box; }
WEAK void r8e_ui_layout_compute(R8EUIDOMNode *root, float vw, float vh) {
    (void)root; (void)vw; (void)vh;
}
WEAK void r8e_ui_layout_update(R8EUIDOMNode *node, float vw, float vh) {
    (void)node; (void)vw; (void)vh;
}
WEAK void r8e_ui_layout_mark_dirty(R8EUIDOMNode *n) { (void)n; }
WEAK void r8e_ui_layout_clear_dirty(R8EUIDOMNode *n) { (void)n; }
WEAK void r8e_ui_layout_get_absolute_pos(const R8EUIDOMNode *n,
    float *x, float *y) { (void)n; if(x)*x=0; if(y)*y=0; }
WEAK void r8e_ui_layout_get_border_box(const R8EUIDOMNode *n,
    float *x, float *y, float *w, float *h) {
    (void)n; if(x)*x=0; if(y)*y=0; if(w)*w=0; if(h)*h=0;
}
WEAK void r8e_ui_layout_get_content_box(const R8EUIDOMNode *n,
    float *x, float *y, float *w, float *h) {
    (void)n; if(x)*x=0; if(y)*y=0; if(w)*w=0; if(h)*h=0;
}
WEAK void r8e_ui_layout_get_margin_box(const R8EUIDOMNode *n,
    float *x, float *y, float *w, float *h) {
    (void)n; if(x)*x=0; if(y)*y=0; if(w)*w=0; if(h)*h=0;
}
WEAK bool r8e_ui_layout_hit_test(const R8EUIDOMNode *n, float px, float py) {
    (void)n; (void)px; (void)py; return false;
}

/* Paint */
WEAK void r8e_ui_paint_dl_init(R8EUIDisplayList *dl) { (void)dl; }
WEAK void r8e_ui_paint_dl_free(R8EUIDisplayList *dl) { (void)dl; }
WEAK void r8e_ui_paint_dl_clear(R8EUIDisplayList *dl) { (void)dl; }
WEAK uint64_t r8e_ui_paint_dl_hash(R8EUIDisplayList *dl) { (void)dl; return 0; }
WEAK bool r8e_ui_paint_dl_unchanged(R8EUIDisplayList *dl, uint64_t h) {
    (void)dl; (void)h; return false;
}
WEAK bool r8e_ui_paint_parse_color(const char *str, R8EUIColor *out) {
    (void)str; (void)out; return false;
}
WEAK void r8e_ui_paint_emit_rect(R8EUIDisplayList *dl,
    float x, float y, float w, float h, R8EUIColor c) {
    (void)dl; (void)x; (void)y; (void)w; (void)h; (void)c;
}
WEAK void r8e_ui_paint_emit_text(R8EUIDisplayList *dl,
    float x, float y, const char *text, uint32_t len, R8EUIColor color,
    float size) {
    (void)dl; (void)x; (void)y; (void)text; (void)len; (void)color; (void)size;
}
WEAK void r8e_ui_paint_emit_border(R8EUIDisplayList *dl,
    float x, float y, float w, float h,
    float top, float right, float bottom, float left, R8EUIColor c) {
    (void)dl; (void)x; (void)y; (void)w; (void)h;
    (void)top; (void)right; (void)bottom; (void)left; (void)c;
}
WEAK void r8e_ui_paint_emit_clip_push(R8EUIDisplayList *dl,
    float x, float y, float w, float h) {
    (void)dl; (void)x; (void)y; (void)w; (void)h;
}
WEAK void r8e_ui_paint_emit_clip_pop(R8EUIDisplayList *dl) { (void)dl; }
WEAK void r8e_ui_paint_emit_opacity(R8EUIDisplayList *dl, float o) {
    (void)dl; (void)o;
}
WEAK R8EUIFramebuffer *r8e_ui_paint_fb_create(int w, int h) {
    (void)w; (void)h; return NULL;
}
WEAK void r8e_ui_paint_fb_destroy(R8EUIFramebuffer *fb) { (void)fb; }
WEAK void r8e_ui_paint_fb_clear(R8EUIFramebuffer *fb, R8EUIColor c) {
    (void)fb; (void)c;
}
WEAK uint32_t r8e_ui_paint_fb_get_pixel(const R8EUIFramebuffer *fb, int x, int y) {
    (void)fb; (void)x; (void)y; return 0;
}
WEAK void r8e_ui_paint_fb_set_pixel(R8EUIFramebuffer *fb, int x, int y,
    R8EUIColor c) { (void)fb; (void)x; (void)y; (void)c; }
WEAK void r8e_ui_paint_style_init(R8EUIPaintStyle *style) { (void)style; }

/* Events */
WEAK void r8e_ui_event_manager_init(R8EUIEventManager *mgr,
    R8EUIDOMNode_Event *root) { (void)mgr; (void)root; }
WEAK void r8e_ui_event_manager_cleanup(R8EUIEventManager *mgr) { (void)mgr; }
WEAK bool r8e_ui_event_add_listener(R8EUIDOMNode_Event *node,
    R8EUIEventType type, R8EUIEventHandler handler, void *user_data,
    bool capture) {
    (void)node; (void)type; (void)handler; (void)user_data; (void)capture;
    return false;
}
WEAK bool r8e_ui_event_remove_listener(R8EUIDOMNode_Event *node,
    R8EUIEventType type, R8EUIEventHandler handler, void *user_data) {
    (void)node; (void)type; (void)handler; (void)user_data;
    return false;
}
WEAK void r8e_ui_event_remove_all_listeners(R8EUIDOMNode_Event *node) { (void)node; }
WEAK void r8e_ui_event_init(R8EUIEvent_Ev *event, R8EUIEventType type) {
    (void)event; (void)type;
}
WEAK bool r8e_ui_event_dispatch(R8EUIEvent_Ev *event, R8EUIDOMNode_Event *target) {
    (void)event; (void)target; return false;
}
WEAK void r8e_ui_event_set_focus(R8EUIEventManager *mgr, R8EUIDOMNode_Event *n) {
    (void)mgr; (void)n;
}
WEAK R8EUIDOMNode_Event *r8e_ui_event_get_focused(const R8EUIEventManager *mgr) {
    (void)mgr; return NULL;
}
WEAK R8EUIDOMNode_Event *r8e_ui_event_get_hovered(const R8EUIEventManager *mgr) {
    (void)mgr; return NULL;
}
WEAK bool r8e_ui_event_is_focused(const R8EUIDOMNode_Event *n) {
    (void)n; return false;
}
WEAK bool r8e_ui_event_is_hovered(const R8EUIDOMNode_Event *n) {
    (void)n; return false;
}

/* =========================================================================
 * Regex stubs (test uses r8e_regex_* but source uses r8e_regexp_*)
 * Now implemented in r8e_regexp.c - stubs match the test API signatures.
 * ========================================================================= */
typedef struct R8ERegex R8ERegex;

typedef struct {
    int32_t  start;
    int32_t  end;
    int32_t  captures_start[32];
    int32_t  captures_end[32];
    uint16_t capture_count;
    uint8_t  engine_used;
} R8ERegexMatch_Stub;

WEAK R8ERegex *r8e_regex_compile(const char *pattern, uint32_t pattern_len,
                                  uint32_t flags, char *error_buf,
                                  uint32_t error_buf_size) {
    (void)pattern; (void)pattern_len; (void)flags;
    (void)error_buf; (void)error_buf_size;
    return NULL;
}

WEAK void r8e_regex_free(R8ERegex *re) { (void)re; }

WEAK int r8e_regex_exec(R8ERegex *re, const char *input, uint32_t input_len,
                         int32_t start_offset, R8ERegexMatch_Stub *match) {
    (void)re; (void)input; (void)input_len; (void)start_offset; (void)match;
    return -1;
}

WEAK int r8e_regex_test(R8ERegex *re, const char *input, uint32_t input_len) {
    (void)re; (void)input; (void)input_len;
    return 0;
}

WEAK uint8_t r8e_regex_engine(const R8ERegex *re) { (void)re; return 0; }
WEAK uint32_t r8e_regex_flags(const R8ERegex *re) { (void)re; return 0; }
WEAK uint16_t r8e_regex_group_count(const R8ERegex *re) { (void)re; return 0; }

static uint32_t g_regex_fuel = 1000000;
WEAK uint32_t r8e_regex_fuel_limit(void) { return g_regex_fuel; }
WEAK void r8e_regex_set_fuel_limit(uint32_t limit) { g_regex_fuel = limit; }

/* r8e_async_generator_next exists in r8e_iterator.c - not stubbed */

/* =========================================================================
 * API function stubs (declared in r8e_api.h, not yet implemented)
 * ========================================================================= */

WEAK R8EContext *r8e_context_new(void) {
    R8EContext *ctx = calloc(1, 4096); /* large enough for any ctx definition */
    return ctx;
}

WEAK void r8e_context_free(R8EContext *ctx) {
    free(ctx);
}

WEAK R8EContext *r8e_context_new_alloc(const void *alloc) {
    (void)alloc;
    return r8e_context_new();
}

WEAK R8EValue r8e_eval(R8EContext *ctx, const char *source, size_t len) {
    (void)ctx; (void)source; (void)len;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_eval_file(R8EContext *ctx, const char *source, size_t len,
                             const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_compile(R8EContext *ctx, const char *source, size_t len,
                           const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_call(R8EContext *ctx, R8EValue func, R8EValue this_val,
                        int argc, const R8EValue *argv) {
    (void)ctx; (void)func; (void)this_val; (void)argc; (void)argv;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_make_number(double d) {
    int32_t i;
    if (d == (double)(int32_t)d && d != 0.0) {
        i = (int32_t)d;
        return stub_from_int32(i);
    }
    R8EValue v;
    memcpy(&v, &d, sizeof(double));
    return v;
}

WEAK R8EValue r8e_make_string(R8EContext *ctx, const char *str, size_t len) {
    (void)ctx; (void)str; (void)len;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_make_cstring(R8EContext *ctx, const char *str) {
    (void)ctx; (void)str;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_make_object(R8EContext *ctx) {
    (void)ctx;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_make_array(R8EContext *ctx, uint32_t capacity) {
    (void)ctx; (void)capacity;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_make_native_func(R8EContext *ctx, void *func,
                                    const char *name, int argc) {
    (void)ctx; (void)func; (void)name; (void)argc;
    return R8E_UNDEFINED;
}

WEAK R8EStatus r8e_set_global_func(R8EContext *ctx, const char *name,
                                    void *func, int argc) {
    (void)ctx; (void)name; (void)func; (void)argc;
    return R8E_OK;
}

WEAK R8EStatus r8e_set_global(R8EContext *ctx, const char *name, R8EValue val) {
    (void)ctx; (void)name; (void)val;
    return R8E_OK;
}

WEAK R8EValue r8e_get_global(R8EContext *ctx, const char *name) {
    (void)ctx; (void)name;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_get_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    (void)ctx; (void)obj; (void)name;
    return R8E_UNDEFINED;
}

WEAK R8EStatus r8e_set_prop(R8EContext *ctx, R8EValue obj, const char *name,
                             R8EValue val) {
    (void)ctx; (void)obj; (void)name; (void)val;
    return R8E_OK;
}

WEAK bool r8e_delete_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    (void)ctx; (void)obj; (void)name;
    return false;
}

WEAK bool r8e_has_prop(R8EContext *ctx, R8EValue obj, const char *name) {
    (void)ctx; (void)obj; (void)name;
    return false;
}

WEAK R8EValue r8e_get_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom) {
    (void)ctx; (void)obj; (void)atom;
    return R8E_UNDEFINED;
}

WEAK R8EStatus r8e_set_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom,
                                  R8EValue val) {
    (void)ctx; (void)obj; (void)atom; (void)val;
    return R8E_OK;
}

WEAK R8EValue r8e_get_element(R8EContext *ctx, R8EValue arr, uint32_t index) {
    (void)ctx; (void)arr; (void)index;
    return R8E_UNDEFINED;
}

WEAK R8EStatus r8e_set_element(R8EContext *ctx, R8EValue arr, uint32_t index,
                                R8EValue val) {
    (void)ctx; (void)arr; (void)index; (void)val;
    return R8E_OK;
}

WEAK int32_t r8e_get_length(R8EContext *ctx, R8EValue v) {
    (void)ctx; (void)v;
    return -1;
}

WEAK bool r8e_is_string(R8EValue v) {
    (void)v;
    return false;
}

WEAK bool r8e_is_function(R8EValue v) {
    (void)v;
    return false;
}

WEAK bool r8e_is_array(R8EValue v) {
    (void)v;
    return false;
}

WEAK bool r8e_to_bool(R8EValue v) {
    return v != R8E_UNDEFINED && v != R8E_NULL && v != R8E_FALSE &&
           !(v == stub_from_int32(0));
}

WEAK R8EValue r8e_clear_exception(R8EContext *ctx) {
    (void)ctx;
    return R8E_UNDEFINED;
}

WEAK const char *r8e_get_cstring(R8EValue v, char *buf, size_t *outlen) {
    (void)v; (void)buf;
    if (outlen) *outlen = 0;
    return "";
}

WEAK void r8e_gc_collect(R8EContext *ctx) { (void)ctx; }

WEAK void r8e_gc_stats(const R8EContext *ctx, void *stats) {
    (void)ctx;
    if (stats) memset(stats, 0, 48);
}

WEAK void r8e_value_retain(R8EValue v) { (void)v; }
WEAK void r8e_value_release(R8EContext *ctx, R8EValue v) { (void)ctx; (void)v; }

WEAK void r8e_set_module_loader(R8EContext *ctx, void *loader) {
    (void)ctx; (void)loader;
}

WEAK R8EValue r8e_eval_module(R8EContext *ctx, const char *source, size_t len,
                               const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

WEAK void r8e_dump_bytecode(const R8EFunction *func) { (void)func; }
WEAK void r8e_dump_value(R8EValue v) { (void)v; }

WEAK R8EValue r8e_throw_type_error(R8EContext *ctx, const char *fmt, ...) {
    (void)ctx; (void)fmt;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_throw_range_error(R8EContext *ctx, const char *fmt, ...) {
    (void)ctx; (void)fmt;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_throw_syntax_error(R8EContext *ctx, const char *fmt, ...) {
    (void)ctx; (void)fmt;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_throw_reference_error(R8EContext *ctx, const char *fmt, ...) {
    (void)ctx; (void)fmt;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_throw_error(R8EContext *ctx, const char *fmt, ...) {
    (void)ctx; (void)fmt;
    return R8E_UNDEFINED;
}

WEAK int r8e_realm_new(R8EContext *ctx) { (void)ctx; return -1; }
WEAK int r8e_realm_switch(R8EContext *ctx, int realm_id) {
    (void)ctx; (void)realm_id; return -1;
}
WEAK R8EStatus r8e_realm_set_limits(R8EContext *ctx, int realm_id,
                                     const void *limits) {
    (void)ctx; (void)realm_id; (void)limits;
    return R8E_OK;
}
WEAK R8EStatus r8e_realm_destroy(R8EContext *ctx, int realm_id) {
    (void)ctx; (void)realm_id;
    return R8E_OK;
}

/* =========================================================================
 * Compiler / Linker stubs
 * ========================================================================= */

typedef struct R8EAtomTable R8EAtomTable;

WEAK void r8e_atom_table_free(R8EAtomTable *table) {
    (void)table;
}

WEAK R8EValue r8e_compile_module(R8EContext *ctx, const char *source,
                                  size_t len, const char *filename) {
    (void)ctx; (void)source; (void)len; (void)filename;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_exec_function(R8EContext *ctx, R8EValue func) {
    (void)ctx; (void)func;
    return R8E_UNDEFINED;
}

WEAK R8EValue r8e_construct_function(R8EContext *ctx, R8EValue func,
                                      int argc, const R8EValue *argv,
                                      R8EValue new_target) {
    (void)ctx; (void)func; (void)argc; (void)argv; (void)new_target;
    return R8E_UNDEFINED;
}
