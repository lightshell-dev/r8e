/*
 * r8e_dom_bridge.c - DOM-to-JS Bridge
 *
 * Exposes C-side DOM nodes (R8EUIDOMNode) as JS objects with native
 * property getters/setters. When JS code writes element.style.color = "red",
 * the setter triggers style invalidation. When JS reads element.textContent,
 * the getter returns the text.
 *
 * See CLAUDE.md Section 12.2 (R8EUIDOM).
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../../include/r8e_types.h"
#include "../../include/r8e_api.h"

/* =========================================================================
 * Forward declarations for DOM types and functions from r8e_dom.c
 * ========================================================================= */

/* DOM node types */
#define R8E_BRIDGE_NODE_ELEMENT  1
#define R8E_BRIDGE_NODE_TEXT     3
#define R8E_BRIDGE_NODE_DOCUMENT 9

/* DOM node structure (must match r8e_dom.c R8EUIDOMNode layout) */
typedef struct R8EUIDOMNode {
    uint8_t           type;
    uint8_t           pad[3];
    uint32_t          flags;
    uint32_t          tag_atom;
    uint32_t          id_atom;
    void             *attrs;
    uint16_t          attr_count;
    uint16_t          attr_capacity;
    struct {
        uint32_t *atoms;
        uint16_t  count;
        uint16_t  capacity;
    }                  class_list;
    void              *inline_style;
    struct R8EUIDOMNode *parent;
    struct R8EUIDOMNode *first_child;
    struct R8EUIDOMNode *last_child;
    struct R8EUIDOMNode *next_sibling;
    struct R8EUIDOMNode *prev_sibling;
    uint32_t          child_count;
    /* layout box: 13 floats */
    float             layout_x, layout_y;
    float             layout_width, layout_height;
    float             layout_padding[4];
    float             layout_border[4];
    float             layout_margin[4];
    void             *computed;
    char             *text;
    uint32_t          text_len;
    void             *listeners;
    uint16_t          listener_count;
    uint16_t          listener_cap;
    struct R8EUIDOMNode *owner_doc;
} R8EUIDOMNode;

/* External DOM functions from r8e_dom.c */
extern R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag);
extern R8EUIDOMNode *r8e_ui_dom_create_text_node_cstr(const char *text);
extern R8EUIDOMNode *r8e_ui_dom_append_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);
extern R8EUIDOMNode *r8e_ui_dom_remove_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);
extern R8EUIDOMNode *r8e_ui_dom_insert_before(R8EUIDOMNode *parent, R8EUIDOMNode *new_child,
                                               R8EUIDOMNode *ref_child);
extern void r8e_ui_dom_set_text_content(R8EUIDOMNode *node, const char *text);
extern char *r8e_ui_dom_get_text_content(const R8EUIDOMNode *node);
extern void r8e_ui_dom_set_attribute(R8EUIDOMNode *node, const char *name,
                                      const char *value);
extern const char *r8e_ui_dom_get_attribute(const R8EUIDOMNode *node, const char *name);
extern bool r8e_ui_dom_has_attribute(const R8EUIDOMNode *node, const char *name);
extern void r8e_ui_dom_set_class_name(R8EUIDOMNode *node, const char *class_name);
extern void r8e_ui_dom_set_style_property(R8EUIDOMNode *node, uint16_t property_id,
                                           const char *value);
extern const char *r8e_ui_dom_get_style_property(const R8EUIDOMNode *node,
                                                  uint16_t property_id);
extern void r8e_ui_dom_add_event_listener(R8EUIDOMNode *node, const char *event_type,
                                           R8EValue callback, bool use_capture);
extern void r8e_ui_dom_tree_free(R8EUIDOMNode *node);

/* External atom functions */
extern uint32_t r8e_atom_intern_cstr(void *ctx, const char *cstr);

/* =========================================================================
 * Create a Tier 2 object with large capacity
 *
 * r8e_make_object creates Tier 1 (max 4 props). DOM wrappers need 16+
 * properties, so we create Tier 2 objects directly matching the
 * interpreter's internal layout.
 * ========================================================================= */

/* Must match r8e_api.c / r8e_interp.c layout exactly */
typedef struct { R8EValue key; R8EValue val; } BridgePropPair;

typedef struct {
    uint32_t       flags;
    uint32_t       proto_id;
    uint8_t        count;
    uint8_t        capacity;
    uint8_t        pad[6];
    BridgePropPair *props;
} BridgeObjTier2;

static R8EValue bridge_make_large_object(uint8_t capacity) {
    BridgeObjTier2 *t2 = (BridgeObjTier2 *)calloc(1, sizeof(BridgeObjTier2));
    if (!t2) return R8E_UNDEFINED;

    /* GC kind OBJECT = 0 in bits [7:5], tier 2 in bits [1:0] */
    t2->flags = (0u << 5) | 2u;
    t2->proto_id = 1; /* PROTO_OBJECT */
    t2->count = 0;
    t2->capacity = capacity;
    t2->props = (BridgePropPair *)calloc(capacity, sizeof(BridgePropPair));
    if (!t2->props) {
        free(t2);
        return R8E_UNDEFINED;
    }

    return r8e_from_pointer(t2);
}

/* =========================================================================
 * Hidden property key for storing the DOM node pointer
 * ========================================================================= */

#define DOM_NODE_HIDDEN_KEY "__r8e_dom_node__"

/* =========================================================================
 * Helper: extract DOM node from a JS wrapper object
 * ========================================================================= */

static R8EUIDOMNode *bridge_get_node(R8EContext *ctx, R8EValue obj) {
    R8EValue node_val = r8e_get_prop(ctx, obj, DOM_NODE_HIDDEN_KEY);
    if (!R8E_IS_POINTER(node_val)) return NULL;
    return (R8EUIDOMNode *)r8e_get_pointer(node_val);
}

/* =========================================================================
 * Helper: extract C string from a JS value (caller must use buf for inline)
 * ========================================================================= */

static const char *bridge_get_str(R8EValue val, char *buf, size_t bufsize) {
    (void)bufsize;
    size_t len = 0;
    const char *s = r8e_get_cstring(val, buf, &len);
    return s;
}

/* =========================================================================
 * Forward declaration for wrapping
 * ========================================================================= */

R8EValue r8e_ui_dom_wrap_element(R8EContext *ctx, R8EUIDOMNode *node);

/* =========================================================================
 * Simple style property ID from name (atom-based, matching r8e_dom.c)
 *
 * We use a simple hash to map CSS property names to uint16_t IDs.
 * This matches the approach in r8e_dom.c where property_id is a small atom.
 * ========================================================================= */

static uint16_t bridge_style_prop_id(const char *name) {
    if (!name) return 0;
    /* Use FNV-1a hash truncated to uint16_t, same as r8e_dom.c uses atoms */
    uint32_t hash = 2166136261U;
    for (const char *p = name; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619U;
    }
    /* Reserve 0 as invalid, use range 1..65535 */
    uint16_t id = (uint16_t)(hash & 0xFFFF);
    if (id == 0) id = 1;
    return id;
}

/* =========================================================================
 * Style sub-object: getters/setters for element.style.* properties
 *
 * The style object stores a back-pointer to its parent DOM wrapper.
 * Individual CSS properties are set/get via native accessors.
 * For simplicity, we support a generic setProperty/getPropertyValue API
 * and also provide common properties (color, backgroundColor, etc.) as
 * accessor properties.
 * ========================================================================= */

/* element.style.setProperty(name, value) */
static R8EValue bridge_style_set_property(R8EContext *ctx, R8EValue this_val,
                                           int argc, const R8EValue *argv) {
    if (argc < 2) return R8E_UNDEFINED;

    R8EValue wrapper = r8e_get_prop(ctx, this_val, "__owner__");
    R8EUIDOMNode *node = bridge_get_node(ctx, wrapper);
    if (!node) return R8E_UNDEFINED;

    char buf1[16], buf2[256];
    const char *prop_name = bridge_get_str(argv[0], buf1, sizeof(buf1));
    const char *prop_val  = bridge_get_str(argv[1], buf2, sizeof(buf2));
    if (!prop_name || !prop_val) return R8E_UNDEFINED;

    uint16_t prop_id = bridge_style_prop_id(prop_name);
    r8e_ui_dom_set_style_property(node, prop_id, prop_val);
    return R8E_UNDEFINED;
}

/* element.style.getPropertyValue(name) */
static R8EValue bridge_style_get_property_value(R8EContext *ctx, R8EValue this_val,
                                                 int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_UNDEFINED;

    R8EValue wrapper = r8e_get_prop(ctx, this_val, "__owner__");
    R8EUIDOMNode *node = bridge_get_node(ctx, wrapper);
    if (!node) return R8E_UNDEFINED;

    char buf[16];
    const char *prop_name = bridge_get_str(argv[0], buf, sizeof(buf));
    if (!prop_name) return R8E_UNDEFINED;

    uint16_t prop_id = bridge_style_prop_id(prop_name);
    const char *val = r8e_ui_dom_get_style_property(node, prop_id);
    if (!val) return r8e_make_cstring(ctx, "");
    return r8e_make_cstring(ctx, val);
}

/* Generic style property getter - uses property name from the accessor name */
static R8EValue bridge_style_generic_getter(R8EContext *ctx, R8EValue this_val,
                                             int argc, const R8EValue *argv,
                                             const char *css_name) {
    (void)argc; (void)argv;
    R8EValue wrapper = r8e_get_prop(ctx, this_val, "__owner__");
    R8EUIDOMNode *node = bridge_get_node(ctx, wrapper);
    if (!node) return R8E_UNDEFINED;

    uint16_t prop_id = bridge_style_prop_id(css_name);
    const char *val = r8e_ui_dom_get_style_property(node, prop_id);
    if (!val) return r8e_make_cstring(ctx, "");
    return r8e_make_cstring(ctx, val);
}

static R8EValue bridge_style_generic_setter(R8EContext *ctx, R8EValue this_val,
                                             int argc, const R8EValue *argv,
                                             const char *css_name) {
    if (argc < 1) return R8E_UNDEFINED;
    R8EValue wrapper = r8e_get_prop(ctx, this_val, "__owner__");
    R8EUIDOMNode *node = bridge_get_node(ctx, wrapper);
    if (!node) return R8E_UNDEFINED;

    char buf[256];
    const char *val = bridge_get_str(argv[0], buf, sizeof(buf));
    uint16_t prop_id = bridge_style_prop_id(css_name);
    r8e_ui_dom_set_style_property(node, prop_id, val ? val : "");
    return R8E_UNDEFINED;
}

/* Macro to generate style property getter/setter pairs */
#define STYLE_ACCESSOR(camel_name, css_name)                                    \
static R8EValue bridge_style_get_##camel_name(R8EContext *ctx, R8EValue this_val,\
                                               int argc, const R8EValue *argv) {\
    return bridge_style_generic_getter(ctx, this_val, argc, argv, css_name);    \
}                                                                               \
static R8EValue bridge_style_set_##camel_name(R8EContext *ctx, R8EValue this_val,\
                                               int argc, const R8EValue *argv) {\
    return bridge_style_generic_setter(ctx, this_val, argc, argv, css_name);    \
}

STYLE_ACCESSOR(color, "color")
STYLE_ACCESSOR(backgroundColor, "background-color")
STYLE_ACCESSOR(fontSize, "font-size")
STYLE_ACCESSOR(fontFamily, "font-family")
STYLE_ACCESSOR(fontWeight, "font-weight")
STYLE_ACCESSOR(width, "width")
STYLE_ACCESSOR(height, "height")
STYLE_ACCESSOR(margin, "margin")
STYLE_ACCESSOR(padding, "padding")
STYLE_ACCESSOR(border, "border")
STYLE_ACCESSOR(display, "display")
STYLE_ACCESSOR(position, "position")
STYLE_ACCESSOR(top, "top")
STYLE_ACCESSOR(left, "left")
STYLE_ACCESSOR(right, "right")
STYLE_ACCESSOR(bottom, "bottom")

/**
 * Create the style sub-object for an element wrapper.
 */
static R8EValue bridge_create_style_object(R8EContext *ctx, R8EValue owner_wrapper) {
    /* Use Tier 2 with capacity for ~20 style property accessors */
    R8EValue style = bridge_make_large_object(24);
    if (R8E_IS_UNDEFINED(style)) return R8E_UNDEFINED;

    /* Store back-pointer to owning element wrapper */
    r8e_set_prop(ctx, style, "__owner__", owner_wrapper);

    /* Register setProperty / getPropertyValue methods */
    R8EValue set_fn = r8e_make_native_func(ctx, bridge_style_set_property, "setProperty", 2);
    r8e_set_prop(ctx, style, "setProperty", set_fn);

    R8EValue get_fn = r8e_make_native_func(ctx, bridge_style_get_property_value, "getPropertyValue", 1);
    r8e_set_prop(ctx, style, "getPropertyValue", get_fn);

    /* Register common CSS property accessors */
    #define REG_STYLE(js_name, camel)                                          \
        r8e_define_accessor(ctx, style, js_name,                               \
            r8e_make_native_func(ctx, bridge_style_get_##camel, "get_" #camel, 0), \
            r8e_make_native_func(ctx, bridge_style_set_##camel, "set_" #camel, 1))

    REG_STYLE("color", color);
    REG_STYLE("backgroundColor", backgroundColor);
    REG_STYLE("fontSize", fontSize);
    REG_STYLE("fontFamily", fontFamily);
    REG_STYLE("fontWeight", fontWeight);
    REG_STYLE("width", width);
    REG_STYLE("height", height);
    REG_STYLE("margin", margin);
    REG_STYLE("padding", padding);
    REG_STYLE("border", border);
    REG_STYLE("display", display);
    REG_STYLE("position", position);
    REG_STYLE("top", top);
    REG_STYLE("left", left);
    REG_STYLE("right", right);
    REG_STYLE("bottom", bottom);

    #undef REG_STYLE

    return style;
}

/* =========================================================================
 * Element wrapper: textContent getter/setter
 * ========================================================================= */

static R8EValue bridge_get_text_content(R8EContext *ctx, R8EValue this_val,
                                         int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_UNDEFINED;

    char *text = r8e_ui_dom_get_text_content(node);
    if (!text) return r8e_make_cstring(ctx, "");
    R8EValue result = r8e_make_cstring(ctx, text);
    free(text);
    return result;
}

static R8EValue bridge_set_text_content(R8EContext *ctx, R8EValue this_val,
                                         int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_UNDEFINED;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_UNDEFINED;

    char buf[256];
    const char *text = bridge_get_str(argv[0], buf, sizeof(buf));
    r8e_ui_dom_set_text_content(node, text ? text : "");
    return R8E_UNDEFINED;
}

/* =========================================================================
 * Element wrapper: className getter/setter
 * ========================================================================= */

static R8EValue bridge_get_class_name(R8EContext *ctx, R8EValue this_val,
                                       int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return r8e_make_cstring(ctx, "");

    const char *cn = r8e_ui_dom_get_attribute(node, "class");
    return r8e_make_cstring(ctx, cn ? cn : "");
}

static R8EValue bridge_set_class_name(R8EContext *ctx, R8EValue this_val,
                                       int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_UNDEFINED;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_UNDEFINED;

    char buf[256];
    const char *cn = bridge_get_str(argv[0], buf, sizeof(buf));
    r8e_ui_dom_set_class_name(node, cn ? cn : "");
    return R8E_UNDEFINED;
}

/* =========================================================================
 * Element wrapper: tree navigation getters (parentNode, firstChild, etc.)
 * ========================================================================= */

static R8EValue bridge_get_parent_node(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node || !node->parent) return R8E_NULL;
    return r8e_ui_dom_wrap_element(ctx, node->parent);
}

static R8EValue bridge_get_first_child(R8EContext *ctx, R8EValue this_val,
                                        int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node || !node->first_child) return R8E_NULL;
    return r8e_ui_dom_wrap_element(ctx, node->first_child);
}

static R8EValue bridge_get_last_child(R8EContext *ctx, R8EValue this_val,
                                       int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node || !node->last_child) return R8E_NULL;
    return r8e_ui_dom_wrap_element(ctx, node->last_child);
}

static R8EValue bridge_get_next_sibling(R8EContext *ctx, R8EValue this_val,
                                         int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node || !node->next_sibling) return R8E_NULL;
    return r8e_ui_dom_wrap_element(ctx, node->next_sibling);
}

static R8EValue bridge_get_prev_sibling(R8EContext *ctx, R8EValue this_val,
                                         int argc, const R8EValue *argv) {
    (void)argc; (void)argv;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node || !node->prev_sibling) return R8E_NULL;
    return r8e_ui_dom_wrap_element(ctx, node->prev_sibling);
}

/* =========================================================================
 * Element wrapper: DOM manipulation methods
 * ========================================================================= */

/* appendChild(child) */
static R8EValue bridge_append_child(R8EContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_UNDEFINED;
    R8EUIDOMNode *parent = bridge_get_node(ctx, this_val);
    R8EUIDOMNode *child  = bridge_get_node(ctx, argv[0]);
    if (!parent || !child) return R8E_UNDEFINED;

    r8e_ui_dom_append_child(parent, child);
    return argv[0];
}

/* removeChild(child) */
static R8EValue bridge_remove_child(R8EContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_UNDEFINED;
    R8EUIDOMNode *parent = bridge_get_node(ctx, this_val);
    R8EUIDOMNode *child  = bridge_get_node(ctx, argv[0]);
    if (!parent || !child) return R8E_UNDEFINED;

    r8e_ui_dom_remove_child(parent, child);
    return argv[0];
}

/* insertBefore(newChild, refChild) */
static R8EValue bridge_insert_before(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    if (argc < 2) return R8E_UNDEFINED;
    R8EUIDOMNode *parent    = bridge_get_node(ctx, this_val);
    R8EUIDOMNode *new_child = bridge_get_node(ctx, argv[0]);
    R8EUIDOMNode *ref_child = NULL;
    if (!R8E_IS_NULL(argv[1]) && !R8E_IS_UNDEFINED(argv[1])) {
        ref_child = bridge_get_node(ctx, argv[1]);
    }
    if (!parent || !new_child) return R8E_UNDEFINED;

    r8e_ui_dom_insert_before(parent, new_child, ref_child);
    return argv[0];
}

/* setAttribute(name, value) */
static R8EValue bridge_set_attribute(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    if (argc < 2) return R8E_UNDEFINED;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_UNDEFINED;

    char buf1[64], buf2[256];
    const char *name = bridge_get_str(argv[0], buf1, sizeof(buf1));
    const char *val  = bridge_get_str(argv[1], buf2, sizeof(buf2));
    if (!name) return R8E_UNDEFINED;

    r8e_ui_dom_set_attribute(node, name, val ? val : "");
    return R8E_UNDEFINED;
}

/* getAttribute(name) */
static R8EValue bridge_get_attribute(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_NULL;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_NULL;

    char buf[64];
    const char *name = bridge_get_str(argv[0], buf, sizeof(buf));
    if (!name) return R8E_NULL;

    const char *val = r8e_ui_dom_get_attribute(node, name);
    if (!val) return R8E_NULL;
    return r8e_make_cstring(ctx, val);
}

/* hasAttribute(name) */
static R8EValue bridge_has_attribute(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    if (argc < 1) return R8E_FALSE;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_FALSE;

    char buf[64];
    const char *name = bridge_get_str(argv[0], buf, sizeof(buf));
    if (!name) return R8E_FALSE;

    return r8e_ui_dom_has_attribute(node, name) ? R8E_TRUE : R8E_FALSE;
}

/* addEventListener(type, callback, [capture]) */
static R8EValue bridge_add_event_listener(R8EContext *ctx, R8EValue this_val,
                                           int argc, const R8EValue *argv) {
    if (argc < 2) return R8E_UNDEFINED;
    R8EUIDOMNode *node = bridge_get_node(ctx, this_val);
    if (!node) return R8E_UNDEFINED;

    char buf[32];
    const char *type = bridge_get_str(argv[0], buf, sizeof(buf));
    if (!type) return R8E_UNDEFINED;

    R8EValue callback = argv[1];
    bool capture = (argc >= 3 && argv[2] == R8E_TRUE);

    r8e_ui_dom_add_event_listener(node, type, callback, capture);
    return R8E_UNDEFINED;
}

/* =========================================================================
 * r8e_ui_dom_wrap_element - Create a JS object wrapping a DOM node
 *
 * This is the core function. It creates a JS object with:
 * - Hidden __r8e_dom_node__ property pointing to the C-side node
 * - Accessor properties: textContent, className, parentNode, etc.
 * - Methods: appendChild, removeChild, insertBefore, setAttribute, etc.
 * - A style sub-object with CSS property accessors
 * ========================================================================= */

R8EValue r8e_ui_dom_wrap_element(R8EContext *ctx, R8EUIDOMNode *node) {
    if (!ctx || !node) return R8E_UNDEFINED;

    /* Use Tier 2 object with capacity for ~20 properties */
    R8EValue wrapper = bridge_make_large_object(24);
    if (R8E_IS_UNDEFINED(wrapper)) return R8E_UNDEFINED;

    /* Store the node pointer as a hidden property */
    r8e_set_prop(ctx, wrapper, DOM_NODE_HIDDEN_KEY, r8e_from_pointer(node));

    /* -- Accessor properties -- */

    /* textContent */
    r8e_define_accessor(ctx, wrapper, "textContent",
        r8e_make_native_func(ctx, bridge_get_text_content, "get_textContent", 0),
        r8e_make_native_func(ctx, bridge_set_text_content, "set_textContent", 1));

    /* className */
    r8e_define_accessor(ctx, wrapper, "className",
        r8e_make_native_func(ctx, bridge_get_class_name, "get_className", 0),
        r8e_make_native_func(ctx, bridge_set_class_name, "set_className", 1));

    /* Tree navigation (read-only getters, no setter) */
    r8e_define_accessor(ctx, wrapper, "parentNode",
        r8e_make_native_func(ctx, bridge_get_parent_node, "get_parentNode", 0),
        R8E_UNDEFINED);
    r8e_define_accessor(ctx, wrapper, "firstChild",
        r8e_make_native_func(ctx, bridge_get_first_child, "get_firstChild", 0),
        R8E_UNDEFINED);
    r8e_define_accessor(ctx, wrapper, "lastChild",
        r8e_make_native_func(ctx, bridge_get_last_child, "get_lastChild", 0),
        R8E_UNDEFINED);
    r8e_define_accessor(ctx, wrapper, "nextSibling",
        r8e_make_native_func(ctx, bridge_get_next_sibling, "get_nextSibling", 0),
        R8E_UNDEFINED);
    r8e_define_accessor(ctx, wrapper, "previousSibling",
        r8e_make_native_func(ctx, bridge_get_prev_sibling, "get_previousSibling", 0),
        R8E_UNDEFINED);

    /* -- Methods -- */
    r8e_set_prop(ctx, wrapper, "appendChild",
        r8e_make_native_func(ctx, bridge_append_child, "appendChild", 1));
    r8e_set_prop(ctx, wrapper, "removeChild",
        r8e_make_native_func(ctx, bridge_remove_child, "removeChild", 1));
    r8e_set_prop(ctx, wrapper, "insertBefore",
        r8e_make_native_func(ctx, bridge_insert_before, "insertBefore", 2));
    r8e_set_prop(ctx, wrapper, "setAttribute",
        r8e_make_native_func(ctx, bridge_set_attribute, "setAttribute", 2));
    r8e_set_prop(ctx, wrapper, "getAttribute",
        r8e_make_native_func(ctx, bridge_get_attribute, "getAttribute", 1));
    r8e_set_prop(ctx, wrapper, "hasAttribute",
        r8e_make_native_func(ctx, bridge_has_attribute, "hasAttribute", 1));
    r8e_set_prop(ctx, wrapper, "addEventListener",
        r8e_make_native_func(ctx, bridge_add_event_listener, "addEventListener", 2));

    /* -- Style sub-object -- */
    R8EValue style = bridge_create_style_object(ctx, wrapper);
    r8e_set_prop(ctx, wrapper, "style", style);

    /* -- Tag name (read-only) -- */
    if (node->type == R8E_BRIDGE_NODE_ELEMENT && node->tag_atom != 0) {
        /* We don't have easy access to the DOM's internal atom table from here,
         * so we store tagName separately. For now, store as unknown. */
    }

    return wrapper;
}

/* =========================================================================
 * document global: createElement, createTextNode, body
 * ========================================================================= */

/* Global document body node (one per engine) */
static R8EUIDOMNode *g_bridge_body_node = NULL;

static R8EUIDOMNode *bridge_ensure_body(void) {
    if (!g_bridge_body_node) {
        g_bridge_body_node = r8e_ui_dom_create_element("body");
    }
    return g_bridge_body_node;
}

/* document.createElement(tag) */
static R8EValue bridge_document_create_element(R8EContext *ctx, R8EValue this_val,
                                                int argc, const R8EValue *argv) {
    (void)this_val;
    if (argc < 1) return R8E_UNDEFINED;

    char buf[64];
    const char *tag = bridge_get_str(argv[0], buf, sizeof(buf));
    if (!tag) return R8E_UNDEFINED;

    R8EUIDOMNode *node = r8e_ui_dom_create_element(tag);
    if (!node) return R8E_UNDEFINED;

    return r8e_ui_dom_wrap_element(ctx, node);
}

/* document.createTextNode(text) */
static R8EValue bridge_document_create_text_node(R8EContext *ctx, R8EValue this_val,
                                                  int argc, const R8EValue *argv) {
    (void)this_val;
    if (argc < 1) return R8E_UNDEFINED;

    char buf[256];
    const char *text = bridge_get_str(argv[0], buf, sizeof(buf));
    if (!text) text = "";

    R8EUIDOMNode *node = r8e_ui_dom_create_text_node_cstr(text);
    if (!node) return R8E_UNDEFINED;

    return r8e_ui_dom_wrap_element(ctx, node);
}

/* document.body getter */
static R8EValue bridge_document_get_body(R8EContext *ctx, R8EValue this_val,
                                          int argc, const R8EValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    R8EUIDOMNode *body = bridge_ensure_body();
    if (!body) return R8E_NULL;
    return r8e_ui_dom_wrap_element(ctx, body);
}

/* =========================================================================
 * r8e_ui_dom_bridge_init - Register the document global
 *
 * Call this after creating an R8EContext to make document.createElement,
 * document.body, etc. available to JS code.
 * ========================================================================= */

R8EStatus r8e_ui_dom_bridge_init(R8EContext *ctx) {
    if (!ctx) return R8E_ERROR;

    /* Create the document object (needs ~5 properties) */
    R8EValue doc = bridge_make_large_object(8);
    if (R8E_IS_UNDEFINED(doc)) return R8E_ERROR_OOM;

    /* document.createElement */
    r8e_set_prop(ctx, doc, "createElement",
        r8e_make_native_func(ctx, bridge_document_create_element, "createElement", 1));

    /* document.createTextNode */
    r8e_set_prop(ctx, doc, "createTextNode",
        r8e_make_native_func(ctx, bridge_document_create_text_node, "createTextNode", 1));

    /* document.body (accessor, read-only) */
    r8e_define_accessor(ctx, doc, "body",
        r8e_make_native_func(ctx, bridge_document_get_body, "get_body", 0),
        R8E_UNDEFINED);

    /* Register as global */
    return r8e_set_global(ctx, "document", doc);
}
