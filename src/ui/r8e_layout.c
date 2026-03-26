/*
 * nlayout.c - R8EUILayout: Flexbox + Block Layout Engine
 *
 * Part of the r8e JavaScript engine R8EUIUI rendering stack.
 * See CLAUDE.md Section 12.4 (R8EUILayout).
 *
 * Two-pass layout algorithm:
 *   1. Measure (bottom-up): determine intrinsic sizes
 *   2. Layout (top-down): assign final positions
 *
 * Supports:
 *   - Block layout: vertical stacking, margin collapse
 *   - Inline layout: horizontal flow, line wrapping
 *   - Flexbox layout (CSS spec): all flex-direction, justify-content,
 *     align-items, flex-grow/shrink/basis, flex-wrap, gap
 *   - Positioning: relative, absolute, fixed
 *   - Overflow: hidden (clip), scroll, visible
 *   - Auto margins for centering
 *   - min/max width/height constraints
 *   - Text measurement stubs
 *
 * React Native ships with ONLY flexbox (via Yoga, ~8,300 LOC) and powers
 * Instagram, Facebook, Discord, Shopify. We do it in ~3,500 LOC by
 * integrating directly with our DOM.
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
#include <math.h>
#include <float.h>

/* =========================================================================
 * Type Definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

#ifndef R8E_NLAYOUT_TYPES_DEFINED
#define R8E_NLAYOUT_TYPES_DEFINED

/* -------------------------------------------------------------------------
 * Layout box: computed position and size for each DOM node
 * ------------------------------------------------------------------------- */

typedef struct R8EUILayoutBox {
    float x, y;           /* position relative to parent content box */
    float width, height;  /* content area */
    float padding[4];     /* TRBL: top, right, bottom, left */
    float border[4];      /* TRBL */
    float margin[4];      /* TRBL */
} R8EUILayoutBox;

/* -------------------------------------------------------------------------
 * Display type
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_DISPLAY_BLOCK       = 0,
    R8E_UI_DISPLAY_INLINE      = 1,
    R8E_UI_DISPLAY_FLEX        = 2,
    R8E_UI_DISPLAY_NONE        = 3,
    R8E_UI_DISPLAY_TABLE       = 4,
    R8E_UI_DISPLAY_TABLE_ROW   = 5,
    R8E_UI_DISPLAY_TABLE_CELL  = 6,
} R8EUIDisplay;

/* -------------------------------------------------------------------------
 * Position type
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_POSITION_STATIC   = 0,
    R8E_UI_POSITION_RELATIVE = 1,
    R8E_UI_POSITION_ABSOLUTE = 2,
    R8E_UI_POSITION_FIXED    = 3,
} R8EUIPosition;

/* -------------------------------------------------------------------------
 * Overflow type
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_OVERFLOW_VISIBLE = 0,
    R8E_UI_OVERFLOW_HIDDEN  = 1,
    R8E_UI_OVERFLOW_SCROLL  = 2,
    R8E_UI_OVERFLOW_AUTO    = 3,
} R8EUIOverflow;

/* -------------------------------------------------------------------------
 * Flexbox enumerations
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_FLEX_ROW            = 0,
    R8E_UI_FLEX_COLUMN         = 1,
    R8E_UI_FLEX_ROW_REVERSE    = 2,
    R8E_UI_FLEX_COLUMN_REVERSE = 3,
} R8EUIFlexDirection;

typedef enum {
    R8E_UI_JUSTIFY_FLEX_START    = 0,
    R8E_UI_JUSTIFY_FLEX_END      = 1,
    R8E_UI_JUSTIFY_CENTER        = 2,
    R8E_UI_JUSTIFY_SPACE_BETWEEN = 3,
    R8E_UI_JUSTIFY_SPACE_AROUND  = 4,
    R8E_UI_JUSTIFY_SPACE_EVENLY  = 5,
} R8EUIJustifyContent;

typedef enum {
    R8E_UI_ALIGN_FLEX_START = 0,
    R8E_UI_ALIGN_FLEX_END   = 1,
    R8E_UI_ALIGN_CENTER     = 2,
    R8E_UI_ALIGN_STRETCH    = 3,
    R8E_UI_ALIGN_BASELINE   = 4,
} R8EUIAlignItems;

typedef enum {
    R8E_UI_ALIGN_CONTENT_FLEX_START    = 0,
    R8E_UI_ALIGN_CONTENT_FLEX_END      = 1,
    R8E_UI_ALIGN_CONTENT_CENTER        = 2,
    R8E_UI_ALIGN_CONTENT_STRETCH       = 3,
    R8E_UI_ALIGN_CONTENT_SPACE_BETWEEN = 4,
    R8E_UI_ALIGN_CONTENT_SPACE_AROUND  = 5,
} R8EUIAlignContent;

typedef enum {
    R8E_UI_FLEX_NOWRAP       = 0,
    R8E_UI_FLEX_WRAP         = 1,
    R8E_UI_FLEX_WRAP_REVERSE = 2,
} R8EUIFlexWrap;

/* -------------------------------------------------------------------------
 * Dimension value: can be auto, fixed px, or percentage
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_DIM_AUTO    = 0,
    R8E_UI_DIM_PX      = 1,
    R8E_UI_DIM_PERCENT = 2,
    R8E_UI_DIM_NONE    = 3,  /* used for min/max when not set */
} R8EUIDimType;

typedef struct {
    R8EUIDimType type;
    float       value;
} R8EUIDimValue;

#define R8E_UI_DIM_AUTO_VAL   ((R8EUIDimValue){ R8E_UI_DIM_AUTO, 0.0f })
#define R8E_UI_DIM_NONE_VAL   ((R8EUIDimValue){ R8E_UI_DIM_NONE, 0.0f })
#define R8E_UI_DIM_PX_VAL(v)  ((R8EUIDimValue){ R8E_UI_DIM_PX, (v) })
#define R8E_UI_DIM_PCT_VAL(v) ((R8EUIDimValue){ R8E_UI_DIM_PERCENT, (v) })

/* -------------------------------------------------------------------------
 * Computed style: resolved CSS values needed for layout
 * ------------------------------------------------------------------------- */

typedef struct R8EUIComputedStyle {
    R8EUIDisplay       display;
    R8EUIPosition      position;
    R8EUIOverflow      overflow;

    /* Dimensions */
    R8EUIDimValue      width;
    R8EUIDimValue      height;
    R8EUIDimValue      min_width;
    R8EUIDimValue      min_height;
    R8EUIDimValue      max_width;
    R8EUIDimValue      max_height;

    /* Box model edges: TRBL */
    float             padding[4];
    float             border_width[4];
    float             margin[4];
    bool              margin_auto[4];   /* true if margin is 'auto' */

    /* Positioning offsets */
    R8EUIDimValue      top;
    R8EUIDimValue      right;
    R8EUIDimValue      bottom;
    R8EUIDimValue      left;

    /* Flexbox */
    R8EUIFlexDirection  flex_direction;
    R8EUIJustifyContent justify_content;
    R8EUIAlignItems     align_items;
    R8EUIAlignItems     align_self;      /* per-child override */
    R8EUIAlignContent   align_content;
    R8EUIFlexWrap       flex_wrap;
    float              flex_grow;
    float              flex_shrink;
    R8EUIDimValue       flex_basis;
    float              gap_row;
    float              gap_column;
    int                order;           /* flex ordering */

    /* Typography (for text measurement) */
    float              font_size;
    float              line_height;
} R8EUIComputedStyle;

/* -------------------------------------------------------------------------
 * DOM node type (minimal, matches ndom.c when integrated)
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_NODE_ELEMENT  = 1,
    R8E_UI_NODE_TEXT     = 3,
    R8E_UI_NODE_DOCUMENT = 9,
    R8E_UI_NODE_FRAGMENT = 11,
} R8EUINodeType;

/* Forward declaration */
typedef struct R8EUIDOMNode R8EUIDOMNode;

struct R8EUIDOMNode {
    uint8_t            type;
    uint32_t           flags;
    uint32_t           tag_atom;
    R8EUIComputedStyle  style;
    R8EUILayoutBox      layout;

    /* Text content for text nodes */
    const char        *text;
    uint32_t           text_len;

    /* Tree pointers */
    R8EUIDOMNode       *parent;
    R8EUIDOMNode       *first_child;
    R8EUIDOMNode       *last_child;
    R8EUIDOMNode       *next_sibling;
    R8EUIDOMNode       *prev_sibling;

    /* Layout scratch data */
    float              scroll_x;
    float              scroll_y;
    float              scroll_width;
    float              scroll_height;
    float              baseline;       /* first baseline for align baseline */
    bool               scrollable;     /* true if overflow is auto/scroll */
};

/* Node flags */
#define R8E_UI_FLAG_DIRTY       0x0001
#define R8E_UI_FLAG_VISIBLE     0x0002
#define R8E_UI_FLAG_FOCUSABLE   0x0004
#define R8E_UI_FLAG_LAYOUT_DONE 0x0008

#endif /* R8E_NLAYOUT_TYPES_DEFINED */


/* =========================================================================
 * Internal Constants and Helpers
 * ========================================================================= */

/* Box model edge indices: Top, Right, Bottom, Left */
#define EDGE_TOP    0
#define EDGE_RIGHT  1
#define EDGE_BOTTOM 2
#define EDGE_LEFT   3

/* Sentinel for undefined/auto dimension */
#define R8E_UI_UNDEFINED (-1.0f)

/* Small epsilon for float comparisons */
#define R8E_UI_EPSILON 0.001f

/* Maximum number of flex lines for multi-line containers */
#define R8E_UI_MAX_FLEX_LINES 256

/* Maximum children for stack-allocated arrays (heap fallback above this) */
#define R8E_UI_MAX_STACK_CHILDREN 64

/* -------------------------------------------------------------------------
 * Float helpers
 * ------------------------------------------------------------------------- */

static inline float r8e_ui_maxf(float a, float b) {
    return a > b ? a : b;
}

static inline float r8e_ui_minf(float a, float b) {
    return a < b ? a : b;
}

static inline float r8e_ui_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline bool r8e_ui_is_undefined(float v) {
    return v <= R8E_UI_UNDEFINED + R8E_UI_EPSILON;
}

static inline float r8e_ui_defined_or(float v, float fallback) {
    return r8e_ui_is_undefined(v) ? fallback : v;
}

static inline float r8e_ui_absf(float v) {
    return v < 0.0f ? -v : v;
}

/* -------------------------------------------------------------------------
 * Dimension resolution
 * ------------------------------------------------------------------------- */

/*
 * Resolve a dimension value to a concrete pixel value.
 *
 * container_size: the containing block's size on this axis, or R8E_UI_UNDEFINED.
 * Returns R8E_UI_UNDEFINED if the value is auto or cannot be resolved.
 */
static float r8e_ui_resolve_dim(R8EUIDimValue dim, float container_size) {
    switch (dim.type) {
    case R8E_UI_DIM_PX:
        return dim.value;
    case R8E_UI_DIM_PERCENT:
        if (!r8e_ui_is_undefined(container_size)) {
            return container_size * dim.value / 100.0f;
        }
        return R8E_UI_UNDEFINED;
    case R8E_UI_DIM_AUTO:
    case R8E_UI_DIM_NONE:
    default:
        return R8E_UI_UNDEFINED;
    }
}

/*
 * Resolve a dimension, returning a default value if auto/undefined.
 */
static float r8e_ui_resolve_dim_def(R8EUIDimValue dim, float container_size,
                                  float def) {
    float v = r8e_ui_resolve_dim(dim, container_size);
    return r8e_ui_is_undefined(v) ? def : v;
}

/* -------------------------------------------------------------------------
 * Box model helpers
 * ------------------------------------------------------------------------- */

static inline float r8e_ui_padding_h(const R8EUIComputedStyle *s) {
    return s->padding[EDGE_LEFT] + s->padding[EDGE_RIGHT];
}

static inline float r8e_ui_padding_v(const R8EUIComputedStyle *s) {
    return s->padding[EDGE_TOP] + s->padding[EDGE_BOTTOM];
}

static inline float r8e_ui_border_h(const R8EUIComputedStyle *s) {
    return s->border_width[EDGE_LEFT] + s->border_width[EDGE_RIGHT];
}

static inline float r8e_ui_border_v(const R8EUIComputedStyle *s) {
    return s->border_width[EDGE_TOP] + s->border_width[EDGE_BOTTOM];
}

static inline float r8e_ui_margin_h(const R8EUIComputedStyle *s) {
    return s->margin[EDGE_LEFT] + s->margin[EDGE_RIGHT];
}

static inline float r8e_ui_margin_v(const R8EUIComputedStyle *s) {
    return s->margin[EDGE_TOP] + s->margin[EDGE_BOTTOM];
}

/* Total horizontal extra space (padding + border + margin) */
static inline float r8e_ui_box_extra_h(const R8EUIComputedStyle *s) {
    return r8e_ui_padding_h(s) + r8e_ui_border_h(s) + r8e_ui_margin_h(s);
}

/* Total vertical extra space (padding + border + margin) */
static inline float r8e_ui_box_extra_v(const R8EUIComputedStyle *s) {
    return r8e_ui_padding_v(s) + r8e_ui_border_v(s) + r8e_ui_margin_v(s);
}

/* Content box width to border box width */
static inline float r8e_ui_content_to_border_w(const R8EUIComputedStyle *s,
                                              float content_w) {
    return content_w + r8e_ui_padding_h(s) + r8e_ui_border_h(s);
}

/* Content box height to border box height */
static inline float r8e_ui_content_to_border_h(const R8EUIComputedStyle *s,
                                              float content_h) {
    return content_h + r8e_ui_padding_v(s) + r8e_ui_border_h(s);
}

/* -------------------------------------------------------------------------
 * Child counting and iteration
 * ------------------------------------------------------------------------- */

static int r8e_ui_count_children(const R8EUIDOMNode *node) {
    int count = 0;
    const R8EUIDOMNode *child = node->first_child;
    while (child) {
        count++;
        child = child->next_sibling;
    }
    return count;
}

/*
 * Count flow children (skip display:none and absolute/fixed positioned).
 */
static int r8e_ui_count_flow_children(const R8EUIDOMNode *node) {
    int count = 0;
    const R8EUIDOMNode *child = node->first_child;
    while (child) {
        if (child->style.display != R8E_UI_DISPLAY_NONE &&
            child->style.position != R8E_UI_POSITION_ABSOLUTE &&
            child->style.position != R8E_UI_POSITION_FIXED) {
            count++;
        }
        child = child->next_sibling;
    }
    return count;
}

/*
 * Check if a node participates in normal flow.
 */
static inline bool r8e_ui_is_in_flow(const R8EUIDOMNode *node) {
    return node->style.display != R8E_UI_DISPLAY_NONE &&
           node->style.position != R8E_UI_POSITION_ABSOLUTE &&
           node->style.position != R8E_UI_POSITION_FIXED;
}

/* -------------------------------------------------------------------------
 * Flexbox direction helpers
 * ------------------------------------------------------------------------- */

static inline bool r8e_ui_flex_is_row(R8EUIFlexDirection dir) {
    return dir == R8E_UI_FLEX_ROW || dir == R8E_UI_FLEX_ROW_REVERSE;
}

static inline bool r8e_ui_flex_is_column(R8EUIFlexDirection dir) {
    return dir == R8E_UI_FLEX_COLUMN || dir == R8E_UI_FLEX_COLUMN_REVERSE;
}

static inline bool r8e_ui_flex_is_reverse(R8EUIFlexDirection dir) {
    return dir == R8E_UI_FLEX_ROW_REVERSE || dir == R8E_UI_FLEX_COLUMN_REVERSE;
}

/* =========================================================================
 * Text Measurement (Stubs)
 *
 * Simple approximation: char_count * font_size * 0.6 for width,
 * font_size * line_height for height.
 * ========================================================================= */

/*
 * Measure text width for a given string at a given font size.
 * This is a stub that approximates each character as 0.6 * font_size wide.
 */
static float r8e_ui_measure_text_width(const char *text, uint32_t len,
                                     float font_size) {
    if (!text || len == 0 || font_size <= 0.0f) return 0.0f;
    return (float)len * font_size * 0.6f;
}

/*
 * Measure text height for a single line at a given font size and line height.
 */
static float r8e_ui_measure_text_height(float font_size, float line_height) {
    if (font_size <= 0.0f) return 0.0f;
    if (line_height <= 0.0f) line_height = 1.2f;
    return font_size * line_height;
}

/*
 * Measure text with word wrapping, returning the total height and the
 * maximum line width. Width constraint is the available width.
 */
static void r8e_ui_measure_text_wrapped(const char *text, uint32_t len,
                                      float font_size, float line_height,
                                      float max_width,
                                      float *out_width, float *out_height) {
    if (!text || len == 0 || font_size <= 0.0f) {
        if (out_width) *out_width = 0.0f;
        if (out_height) *out_height = 0.0f;
        return;
    }

    float char_w = font_size * 0.6f;
    float line_h = r8e_ui_measure_text_height(font_size, line_height);

    if (max_width <= 0.0f || r8e_ui_is_undefined(max_width)) {
        /* No wrapping constraint: single line */
        if (out_width) *out_width = (float)len * char_w;
        if (out_height) *out_height = line_h;
        return;
    }

    int chars_per_line = (int)(max_width / char_w);
    if (chars_per_line < 1) chars_per_line = 1;

    int num_lines = ((int)len + chars_per_line - 1) / chars_per_line;
    if (num_lines < 1) num_lines = 1;

    float actual_width;
    if ((int)len <= chars_per_line) {
        actual_width = (float)len * char_w;
    } else {
        actual_width = (float)chars_per_line * char_w;
    }

    if (out_width) *out_width = actual_width;
    if (out_height) *out_height = (float)num_lines * line_h;
}


/* =========================================================================
 * Forward Declarations
 * ========================================================================= */

static void r8e_ui_layout_node(R8EUIDOMNode *node, float container_w,
                             float container_h);
static void r8e_ui_measure_node(R8EUIDOMNode *node, float container_w,
                              float container_h);
static void r8e_ui_layout_block(R8EUIDOMNode *node, float container_w,
                              float container_h);
static void r8e_ui_layout_flex(R8EUIDOMNode *node, float container_w,
                             float container_h);
static void r8e_ui_layout_inline(R8EUIDOMNode *node, float container_w,
                               float container_h);
static void r8e_ui_layout_positioned(R8EUIDOMNode *node, float container_w,
                                   float container_h);
static void r8e_ui_layout_table(R8EUIDOMNode *node, float container_w,
                               float container_h);


/* =========================================================================
 * Constraint Application
 *
 * Apply min-width, max-width, min-height, max-height to a resolved size.
 * ========================================================================= */

static float r8e_ui_apply_min_max_width(const R8EUIComputedStyle *style,
                                      float width, float container_w) {
    float min_w = r8e_ui_resolve_dim_def(style->min_width, container_w, 0.0f);
    float max_w = r8e_ui_resolve_dim_def(style->max_width, container_w, FLT_MAX);
    return r8e_ui_clampf(width, min_w, max_w);
}

static float r8e_ui_apply_min_max_height(const R8EUIComputedStyle *style,
                                       float height, float container_h) {
    float min_h = r8e_ui_resolve_dim_def(style->min_height, container_h, 0.0f);
    float max_h = r8e_ui_resolve_dim_def(style->max_height, container_h, FLT_MAX);
    return r8e_ui_clampf(height, min_h, max_h);
}


/* =========================================================================
 * Margin Collapse (Block Layout)
 *
 * CSS margin collapse: adjacent vertical margins of block-level elements
 * merge. The larger margin wins instead of both being applied.
 * ========================================================================= */

/*
 * Collapse two vertical margins. Returns the collapsed margin value.
 * Both positive: max. Both negative: min (most negative). Mixed: sum.
 */
static float r8e_ui_collapse_margins(float margin_a, float margin_b) {
    if (margin_a >= 0.0f && margin_b >= 0.0f) {
        return r8e_ui_maxf(margin_a, margin_b);
    }
    if (margin_a < 0.0f && margin_b < 0.0f) {
        return r8e_ui_minf(margin_a, margin_b);
    }
    return margin_a + margin_b;
}


/* =========================================================================
 * Intrinsic Size Measurement (Bottom-Up Pass)
 *
 * Determines the minimum and preferred sizes of each node by walking the
 * tree bottom-up. Text nodes measure their text content. Element nodes
 * aggregate their children's sizes.
 * ========================================================================= */

/*
 * Measure a text node's intrinsic size.
 */
static void r8e_ui_measure_text_node(R8EUIDOMNode *node, float container_w) {
    if (!node || node->type != R8E_UI_NODE_TEXT) return;

    float font_size = node->style.font_size;
    if (font_size <= 0.0f) font_size = 16.0f;
    float line_h = node->style.line_height;
    if (line_h <= 0.0f) line_h = 1.2f;

    float text_w, text_h;
    r8e_ui_measure_text_wrapped(node->text, node->text_len,
                              font_size, line_h, container_w,
                              &text_w, &text_h);

    node->layout.width = text_w;
    node->layout.height = text_h;
    node->baseline = text_h - (font_size * (line_h - 1.0f) / 2.0f);
}

/*
 * Measure intrinsic sizes of a node and its subtree (bottom-up).
 */
static void r8e_ui_measure_node(R8EUIDOMNode *node, float container_w,
                              float container_h) {
    if (!node) return;
    if (node->style.display == R8E_UI_DISPLAY_NONE) return;

    /* Text nodes: measure text directly */
    if (node->type == R8E_UI_NODE_TEXT) {
        r8e_ui_measure_text_node(node, container_w);
        return;
    }

    /* Resolve explicit width/height */
    const R8EUIComputedStyle *s = &node->style;
    float resolved_w = r8e_ui_resolve_dim(s->width, container_w);
    float resolved_h = r8e_ui_resolve_dim(s->height, container_h);

    /* Available content area for children */
    float avail_w = resolved_w;
    if (!r8e_ui_is_undefined(avail_w)) {
        avail_w -= r8e_ui_padding_h(s) + r8e_ui_border_h(s);
        if (avail_w < 0.0f) avail_w = 0.0f;
    } else {
        avail_w = container_w;
        if (!r8e_ui_is_undefined(avail_w)) {
            avail_w -= r8e_ui_box_extra_h(s);
            if (avail_w < 0.0f) avail_w = 0.0f;
        }
    }

    float avail_h = resolved_h;
    if (r8e_ui_is_undefined(avail_h)) {
        avail_h = container_h;
    }

    /* Measure all children recursively */
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        r8e_ui_measure_node(child, avail_w, avail_h);
        child = child->next_sibling;
    }

    /* Compute intrinsic size from children if no explicit size */
    if (r8e_ui_is_undefined(resolved_w) || r8e_ui_is_undefined(resolved_h)) {
        float content_w = 0.0f;
        float content_h = 0.0f;

        if (s->display == R8E_UI_DISPLAY_FLEX) {
            /* Flex intrinsic size depends on direction */
            bool is_row = r8e_ui_flex_is_row(s->flex_direction);
            float main_size = 0.0f;
            float cross_size = 0.0f;
            int flow_count = 0;

            child = node->first_child;
            while (child) {
                if (r8e_ui_is_in_flow(child)) {
                    float child_main, child_cross;
                    if (is_row) {
                        child_main = child->layout.width +
                                     r8e_ui_margin_h(&child->style);
                        child_cross = child->layout.height +
                                      r8e_ui_margin_v(&child->style);
                    } else {
                        child_main = child->layout.height +
                                     r8e_ui_margin_v(&child->style);
                        child_cross = child->layout.width +
                                      r8e_ui_margin_h(&child->style);
                    }
                    main_size += child_main;
                    cross_size = r8e_ui_maxf(cross_size, child_cross);
                    flow_count++;
                }
                child = child->next_sibling;
            }

            /* Add gap between items */
            float gap = is_row ? s->gap_column : s->gap_row;
            if (flow_count > 1) {
                main_size += gap * (float)(flow_count - 1);
            }

            if (is_row) {
                content_w = main_size;
                content_h = cross_size;
            } else {
                content_w = cross_size;
                content_h = main_size;
            }
        } else {
            /* Block layout: stack vertically, max width */
            child = node->first_child;
            while (child) {
                if (r8e_ui_is_in_flow(child)) {
                    float cw = child->layout.width +
                               r8e_ui_margin_h(&child->style);
                    float ch = child->layout.height +
                               r8e_ui_margin_v(&child->style);
                    content_w = r8e_ui_maxf(content_w, cw);
                    content_h += ch;
                }
                child = child->next_sibling;
            }
        }

        if (r8e_ui_is_undefined(resolved_w)) {
            resolved_w = content_w + r8e_ui_padding_h(s) + r8e_ui_border_h(s);
            resolved_w = r8e_ui_apply_min_max_width(s, resolved_w, container_w);
        }
        if (r8e_ui_is_undefined(resolved_h)) {
            resolved_h = content_h + r8e_ui_padding_v(s) + r8e_ui_border_v(s);
            resolved_h = r8e_ui_apply_min_max_height(s, resolved_h, container_h);
        }
    }

    node->layout.width = r8e_ui_defined_or(resolved_w, 0.0f);
    node->layout.height = r8e_ui_defined_or(resolved_h, 0.0f);
}


/* =========================================================================
 * Block Layout
 *
 * Stack children vertically. Handle width/height/auto, margin collapse.
 * ========================================================================= */

static void r8e_ui_layout_block(R8EUIDOMNode *node, float container_w,
                              float container_h) {
    if (!node) return;

    const R8EUIComputedStyle *s = &node->style;
    R8EUILayoutBox *box = &node->layout;

    /* Copy padding and border to layout box */
    memcpy(box->padding, s->padding, sizeof(float) * 4);
    memcpy(box->border, s->border_width, sizeof(float) * 4);
    memcpy(box->margin, s->margin, sizeof(float) * 4);

    /* Resolve width */
    float resolved_w = r8e_ui_resolve_dim(s->width, container_w);
    if (r8e_ui_is_undefined(resolved_w)) {
        /* Block elements take full available width minus margins */
        resolved_w = container_w;
        if (!r8e_ui_is_undefined(resolved_w)) {
            resolved_w -= r8e_ui_margin_h(s) + r8e_ui_border_h(s) +
                          r8e_ui_padding_h(s);
            if (resolved_w < 0.0f) resolved_w = 0.0f;
        }
    }
    resolved_w = r8e_ui_apply_min_max_width(s, resolved_w, container_w);
    box->width = resolved_w;

    /* Handle auto margins for horizontal centering */
    if (s->margin_auto[EDGE_LEFT] && s->margin_auto[EDGE_RIGHT] &&
        !r8e_ui_is_undefined(container_w)) {
        float total_w = resolved_w + r8e_ui_padding_h(s) + r8e_ui_border_h(s);
        float remaining = container_w - total_w;
        if (remaining > 0.0f) {
            box->margin[EDGE_LEFT] = remaining / 2.0f;
            box->margin[EDGE_RIGHT] = remaining / 2.0f;
        } else {
            box->margin[EDGE_LEFT] = 0.0f;
            box->margin[EDGE_RIGHT] = 0.0f;
        }
    } else if (s->margin_auto[EDGE_LEFT] && !r8e_ui_is_undefined(container_w)) {
        float total_w = resolved_w + r8e_ui_padding_h(s) + r8e_ui_border_h(s) +
                        box->margin[EDGE_RIGHT];
        box->margin[EDGE_LEFT] = r8e_ui_maxf(0.0f, container_w - total_w);
    } else if (s->margin_auto[EDGE_RIGHT] && !r8e_ui_is_undefined(container_w)) {
        float total_w = resolved_w + r8e_ui_padding_h(s) + r8e_ui_border_h(s) +
                        box->margin[EDGE_LEFT];
        box->margin[EDGE_RIGHT] = r8e_ui_maxf(0.0f, container_w - total_w);
    }

    /* Content area available for children */
    float content_w = resolved_w;
    if (r8e_ui_is_undefined(content_w)) content_w = 0.0f;
    float content_h = r8e_ui_resolve_dim(s->height, container_h);

    /* Layout children vertically */
    float cursor_y = 0.0f;
    float prev_margin_bottom = 0.0f;
    bool first_child_flag = true;
    float max_child_width = 0.0f;

    R8EUIDOMNode *child = node->first_child;
    while (child) {
        if (child->style.display == R8E_UI_DISPLAY_NONE) {
            child = child->next_sibling;
            continue;
        }

        /* Absolute/fixed positioned: handle separately */
        if (child->style.position == R8E_UI_POSITION_ABSOLUTE ||
            child->style.position == R8E_UI_POSITION_FIXED) {
            r8e_ui_layout_node(child, content_w, content_h);
            r8e_ui_layout_positioned(child, content_w,
                                   r8e_ui_is_undefined(content_h) ?
                                   box->height : content_h);
            child = child->next_sibling;
            continue;
        }

        /* Layout child */
        r8e_ui_layout_node(child, content_w, content_h);

        /* Margin collapse */
        float child_margin_top = child->layout.margin[EDGE_TOP];
        float collapsed;
        if (first_child_flag) {
            collapsed = child_margin_top;
            first_child_flag = false;
        } else {
            collapsed = r8e_ui_collapse_margins(prev_margin_bottom,
                                              child_margin_top);
        }

        /* Position child */
        child->layout.x = child->layout.margin[EDGE_LEFT] +
                          box->padding[EDGE_LEFT] + box->border[EDGE_LEFT];
        child->layout.y = cursor_y + collapsed +
                          box->padding[EDGE_TOP] + box->border[EDGE_TOP];

        /* Advance cursor */
        float child_outer_h = child->layout.height +
                              r8e_ui_padding_v(&child->style) +
                              r8e_ui_border_v(&child->style);
        cursor_y = child->layout.y - box->padding[EDGE_TOP] -
                   box->border[EDGE_TOP] + child_outer_h;
        prev_margin_bottom = child->layout.margin[EDGE_BOTTOM];

        /* Track maximum child width for shrink-to-fit */
        float child_outer_w = child->layout.width +
                              r8e_ui_margin_h(&child->style) +
                              r8e_ui_padding_h(&child->style) +
                              r8e_ui_border_h(&child->style);
        max_child_width = r8e_ui_maxf(max_child_width, child_outer_w);

        child = child->next_sibling;
    }

    /* Resolve height if auto */
    float resolved_h = r8e_ui_resolve_dim(s->height, container_h);
    if (r8e_ui_is_undefined(resolved_h)) {
        resolved_h = cursor_y + prev_margin_bottom;
    }
    resolved_h = r8e_ui_apply_min_max_height(s, resolved_h, container_h);
    box->height = resolved_h;

    /* Handle auto vertical margins */
    if (s->margin_auto[EDGE_TOP] && s->margin_auto[EDGE_BOTTOM] &&
        !r8e_ui_is_undefined(container_h)) {
        float total_h = resolved_h + r8e_ui_padding_v(s) + r8e_ui_border_v(s);
        float remaining = container_h - total_h;
        if (remaining > 0.0f) {
            box->margin[EDGE_TOP] = remaining / 2.0f;
            box->margin[EDGE_BOTTOM] = remaining / 2.0f;
        }
    }

    /* Record scroll dimensions */
    node->scroll_width = r8e_ui_maxf(content_w, max_child_width);
    node->scroll_height = r8e_ui_maxf(resolved_h, cursor_y + prev_margin_bottom);
}


/* =========================================================================
 * Inline Layout
 *
 * Horizontal flow, line wrapping, text measurement.
 * ========================================================================= */

/* Line box: accumulates inline children on a single line */
typedef struct {
    float       x;           /* current x position on line */
    float       height;      /* maximum height of items on this line */
    float       baseline;    /* baseline position */
    float       width;       /* total width consumed */
    int         item_count;  /* number of items on this line */
} R8EUIInlineLine;

static void r8e_ui_layout_inline(R8EUIDOMNode *node, float container_w,
                                float container_h) {
    if (!node) return;

    const R8EUIComputedStyle *s = &node->style;
    R8EUILayoutBox *box = &node->layout;

    memcpy(box->padding, s->padding, sizeof(float) * 4);
    memcpy(box->border, s->border_width, sizeof(float) * 4);
    memcpy(box->margin, s->margin, sizeof(float) * 4);

    float resolved_w = r8e_ui_resolve_dim(s->width, container_w);
    if (r8e_ui_is_undefined(resolved_w)) {
        resolved_w = container_w;
        if (!r8e_ui_is_undefined(resolved_w)) {
            resolved_w -= r8e_ui_padding_h(s) + r8e_ui_border_h(s) +
                          r8e_ui_margin_h(s);
            if (resolved_w < 0.0f) resolved_w = 0.0f;
        }
    }

    float avail_w = r8e_ui_is_undefined(resolved_w) ? FLT_MAX : resolved_w;

    /* Line state */
    R8EUIInlineLine line;
    memset(&line, 0, sizeof(line));

    float total_height = 0.0f;
    float max_line_width = 0.0f;
    float line_y = 0.0f;

    R8EUIDOMNode *child = node->first_child;
    while (child) {
        if (child->style.display == R8E_UI_DISPLAY_NONE) {
            child = child->next_sibling;
            continue;
        }

        if (child->style.position == R8E_UI_POSITION_ABSOLUTE ||
            child->style.position == R8E_UI_POSITION_FIXED) {
            r8e_ui_layout_node(child, avail_w, container_h);
            r8e_ui_layout_positioned(child, avail_w, container_h);
            child = child->next_sibling;
            continue;
        }

        /* Layout the child to determine its size */
        r8e_ui_layout_node(child, avail_w, container_h);

        float child_w = child->layout.width +
                        r8e_ui_margin_h(&child->style) +
                        r8e_ui_padding_h(&child->style) +
                        r8e_ui_border_h(&child->style);
        float child_h = child->layout.height +
                        r8e_ui_margin_v(&child->style) +
                        r8e_ui_padding_v(&child->style) +
                        r8e_ui_border_v(&child->style);

        /* Check if we need to wrap to next line */
        if (line.item_count > 0 && line.x + child_w > avail_w) {
            /* Finalize current line */
            max_line_width = r8e_ui_maxf(max_line_width, line.x);
            line_y += line.height;
            line.x = 0.0f;
            line.height = 0.0f;
            line.item_count = 0;
        }

        /* Position child on current line */
        child->layout.x = line.x + child->layout.margin[EDGE_LEFT] +
                          box->padding[EDGE_LEFT] + box->border[EDGE_LEFT];
        child->layout.y = line_y + child->layout.margin[EDGE_TOP] +
                          box->padding[EDGE_TOP] + box->border[EDGE_TOP];

        line.x += child_w;
        line.height = r8e_ui_maxf(line.height, child_h);
        line.item_count++;

        child = child->next_sibling;
    }

    /* Finalize last line */
    if (line.item_count > 0) {
        max_line_width = r8e_ui_maxf(max_line_width, line.x);
        total_height = line_y + line.height;
    }

    /* Set dimensions */
    if (r8e_ui_is_undefined(resolved_w)) {
        box->width = max_line_width;
    } else {
        box->width = resolved_w;
    }

    float resolved_h = r8e_ui_resolve_dim(s->height, container_h);
    if (r8e_ui_is_undefined(resolved_h)) {
        box->height = total_height;
    } else {
        box->height = resolved_h;
    }

    box->width = r8e_ui_apply_min_max_width(s, box->width, container_w);
    box->height = r8e_ui_apply_min_max_height(s, box->height, container_h);
}


/* =========================================================================
 * Flexbox Layout
 *
 * Full CSS Flexbox algorithm implementation:
 *   - flex-direction: row, column, row-reverse, column-reverse
 *   - justify-content: flex-start, flex-end, center, space-between,
 *     space-around, space-evenly
 *   - align-items: flex-start, flex-end, center, stretch, baseline
 *   - flex-grow, flex-shrink, flex-basis
 *   - flex-wrap: nowrap, wrap, wrap-reverse
 *   - gap (row-gap, column-gap)
 *   - Cross-axis alignment
 *   - Multi-line flex container handling
 * ========================================================================= */

/* Per-item data during flex layout */
typedef struct {
    R8EUIDOMNode *node;
    float        flex_basis;     /* resolved flex-basis */
    float        flex_grow;
    float        flex_shrink;
    float        main_size;      /* computed main axis size */
    float        cross_size;     /* computed cross axis size */
    float        margin_main;    /* margin on main axis */
    float        margin_cross;   /* margin on cross axis */
    float        min_main;       /* min-width/min-height on main axis */
    float        max_main;       /* max-width/max-height on main axis */
    float        hypothetical;   /* hypothetical main size after clamping */
    bool         frozen;         /* item has been resolved */
    int          order;          /* CSS order property */
    float        baseline;       /* baseline for alignment */
} R8EUIFlexItem;

/* A flex line: group of items on one line */
typedef struct {
    int          start;         /* index of first item in this line */
    int          count;         /* number of items in this line */
    float        main_size;     /* total main size of items on this line */
    float        cross_size;    /* cross size of this line */
    float        remaining;     /* remaining space after sizing */
} R8EUIFlexLine;

/*
 * Resolve the flex-basis for an item.
 * Per CSS spec: if flex-basis is auto, use the item's main-axis size.
 * If flex-basis is a length/percentage, use that value.
 */
static float r8e_ui_resolve_flex_basis(const R8EUIFlexItem *item,
                                     R8EUIFlexDirection dir,
                                     float container_main) {
    const R8EUIComputedStyle *cs = &item->node->style;
    float basis = r8e_ui_resolve_dim(cs->flex_basis, container_main);

    if (!r8e_ui_is_undefined(basis)) {
        return basis;
    }

    /* flex-basis: auto -- use width or height depending on direction */
    if (r8e_ui_flex_is_row(dir)) {
        basis = r8e_ui_resolve_dim(cs->width, container_main);
    } else {
        basis = r8e_ui_resolve_dim(cs->height, container_main);
    }

    if (!r8e_ui_is_undefined(basis)) {
        return basis;
    }

    /* Content-based sizing: use the measured content size */
    if (r8e_ui_flex_is_row(dir)) {
        return item->node->layout.width;
    } else {
        return item->node->layout.height;
    }
}

/*
 * Get the main-axis size of a flex item including margins.
 */
static float r8e_ui_flex_item_outer_main(const R8EUIFlexItem *item) {
    return item->main_size + item->margin_main;
}

/*
 * Get the cross-axis size of a flex item including margins.
 */
static float r8e_ui_flex_item_outer_cross(const R8EUIFlexItem *item) {
    return item->cross_size + item->margin_cross;
}

/*
 * Collect flex items from children, skipping display:none and out-of-flow.
 */
static int r8e_ui_collect_flex_items(R8EUIDOMNode *node, R8EUIFlexItem *items,
                                   int max_items) {
    int count = 0;
    R8EUIDOMNode *child = node->first_child;

    while (child && count < max_items) {
        if (child->style.display == R8E_UI_DISPLAY_NONE ||
            child->style.position == R8E_UI_POSITION_ABSOLUTE ||
            child->style.position == R8E_UI_POSITION_FIXED) {
            child = child->next_sibling;
            continue;
        }

        R8EUIFlexItem *item = &items[count];
        memset(item, 0, sizeof(R8EUIFlexItem));
        item->node = child;
        item->flex_grow = child->style.flex_grow;
        item->flex_shrink = child->style.flex_shrink;
        item->order = child->style.order;
        item->baseline = child->baseline;

        count++;
        child = child->next_sibling;
    }

    return count;
}

/*
 * Sort flex items by CSS order property (stable insertion sort).
 */
static void r8e_ui_sort_flex_items_by_order(R8EUIFlexItem *items, int count) {
    for (int i = 1; i < count; i++) {
        R8EUIFlexItem temp = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].order > temp.order) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = temp;
    }
}

/*
 * Resolve all flex item sizes for the main axis.
 */
static void r8e_ui_resolve_flex_items(R8EUIFlexItem *items, int count,
                                    R8EUIFlexDirection dir,
                                    float container_main,
                                    float container_cross) {
    for (int i = 0; i < count; i++) {
        R8EUIFlexItem *item = &items[i];
        const R8EUIComputedStyle *cs = &item->node->style;
        bool is_row = r8e_ui_flex_is_row(dir);

        /* Resolve flex-basis */
        item->flex_basis = r8e_ui_resolve_flex_basis(item, dir, container_main);
        if (item->flex_basis < 0.0f) item->flex_basis = 0.0f;

        /* Margins on main/cross axis */
        if (is_row) {
            item->margin_main = cs->margin[EDGE_LEFT] +
                                cs->margin[EDGE_RIGHT] +
                                cs->padding[EDGE_LEFT] +
                                cs->padding[EDGE_RIGHT] +
                                cs->border_width[EDGE_LEFT] +
                                cs->border_width[EDGE_RIGHT];
            item->margin_cross = cs->margin[EDGE_TOP] +
                                 cs->margin[EDGE_BOTTOM] +
                                 cs->padding[EDGE_TOP] +
                                 cs->padding[EDGE_BOTTOM] +
                                 cs->border_width[EDGE_TOP] +
                                 cs->border_width[EDGE_BOTTOM];
        } else {
            item->margin_main = cs->margin[EDGE_TOP] +
                                cs->margin[EDGE_BOTTOM] +
                                cs->padding[EDGE_TOP] +
                                cs->padding[EDGE_BOTTOM] +
                                cs->border_width[EDGE_TOP] +
                                cs->border_width[EDGE_BOTTOM];
            item->margin_cross = cs->margin[EDGE_LEFT] +
                                 cs->margin[EDGE_RIGHT] +
                                 cs->padding[EDGE_LEFT] +
                                 cs->padding[EDGE_RIGHT] +
                                 cs->border_width[EDGE_LEFT] +
                                 cs->border_width[EDGE_RIGHT];
        }

        /* Min/max constraints on main axis */
        if (is_row) {
            item->min_main = r8e_ui_resolve_dim_def(cs->min_width,
                                                   container_main, 0.0f);
            item->max_main = r8e_ui_resolve_dim_def(cs->max_width,
                                                   container_main, FLT_MAX);
        } else {
            item->min_main = r8e_ui_resolve_dim_def(cs->min_height,
                                                   container_main, 0.0f);
            item->max_main = r8e_ui_resolve_dim_def(cs->max_height,
                                                   container_main, FLT_MAX);
        }

        /* Hypothetical main size: flex-basis clamped by min/max */
        item->hypothetical = r8e_ui_clampf(item->flex_basis,
                                          item->min_main, item->max_main);
        item->main_size = item->hypothetical;
        item->frozen = false;

        /* Cross size: resolve from explicit dimension or content */
        if (is_row) {
            float ch = r8e_ui_resolve_dim(cs->height, container_cross);
            if (!r8e_ui_is_undefined(ch)) {
                item->cross_size = ch;
            } else {
                item->cross_size = item->node->layout.height;
            }
        } else {
            float cw = r8e_ui_resolve_dim(cs->width, container_cross);
            if (!r8e_ui_is_undefined(cw)) {
                item->cross_size = cw;
            } else {
                item->cross_size = item->node->layout.width;
            }
        }
    }
}

/*
 * Break flex items into lines (for flex-wrap).
 * Returns the number of lines.
 */
static int r8e_ui_compute_flex_lines(const R8EUIFlexItem *items, int count,
                                   R8EUIFlexWrap wrap, float container_main,
                                   float gap, R8EUIFlexLine *lines,
                                   int max_lines) {
    if (count == 0) return 0;

    /* No wrapping: all items on one line */
    if (wrap == R8E_UI_FLEX_NOWRAP || r8e_ui_is_undefined(container_main)) {
        lines[0].start = 0;
        lines[0].count = count;
        lines[0].main_size = 0.0f;
        for (int i = 0; i < count; i++) {
            lines[0].main_size += r8e_ui_flex_item_outer_main(&items[i]);
        }
        if (count > 1) {
            lines[0].main_size += gap * (float)(count - 1);
        }
        return 1;
    }

    /* Wrapping: break into lines when items exceed container main size */
    int line_count = 0;
    int line_start = 0;
    float line_size = 0.0f;
    int line_items = 0;

    for (int i = 0; i < count && line_count < max_lines; i++) {
        float item_outer = r8e_ui_flex_item_outer_main(&items[i]);
        float gap_space = (line_items > 0) ? gap : 0.0f;

        if (line_items > 0 && line_size + gap_space + item_outer > container_main) {
            /* Finalize current line */
            lines[line_count].start = line_start;
            lines[line_count].count = line_items;
            lines[line_count].main_size = line_size;
            line_count++;

            /* Start new line */
            line_start = i;
            line_size = item_outer;
            line_items = 1;
        } else {
            line_size += gap_space + item_outer;
            line_items++;
        }
    }

    /* Finalize last line */
    if (line_items > 0 && line_count < max_lines) {
        lines[line_count].start = line_start;
        lines[line_count].count = line_items;
        lines[line_count].main_size = line_size;
        line_count++;
    }

    return line_count;
}

/*
 * Resolve flexible lengths for items on a single flex line.
 * Implements the CSS flex sizing algorithm:
 *   1. Determine available space
 *   2. Distribute positive/negative free space via flex-grow/flex-shrink
 *   3. Clamp to min/max and freeze items that hit constraints
 *   4. Repeat until all items are frozen
 */
static void r8e_ui_resolve_flex_line(R8EUIFlexItem *items, int start, int count,
                                   float container_main, float gap) {
    if (count == 0) return;

    /* Reset frozen flags */
    for (int i = start; i < start + count; i++) {
        items[i].frozen = false;
        items[i].main_size = items[i].hypothetical;
    }

    /* Iterative resolution loop */
    int max_iterations = count + 1;
    for (int iter = 0; iter < max_iterations; iter++) {
        /* Calculate used space and flex factor totals */
        float used_space = 0.0f;
        float total_grow = 0.0f;
        float total_shrink = 0.0f;
        int unfrozen_count = 0;

        for (int i = start; i < start + count; i++) {
            float gap_space = (i > start) ? gap : 0.0f;
            if (items[i].frozen) {
                used_space += items[i].main_size + items[i].margin_main +
                              gap_space;
            } else {
                used_space += items[i].hypothetical + items[i].margin_main +
                              gap_space;
                total_grow += items[i].flex_grow;
                total_shrink += items[i].flex_shrink * items[i].flex_basis;
                unfrozen_count++;
            }
        }

        if (unfrozen_count == 0) break;

        float free_space = container_main - used_space;
        if (r8e_ui_is_undefined(container_main)) free_space = 0.0f;

        bool growing = free_space > R8E_UI_EPSILON;
        bool shrinking = free_space < -R8E_UI_EPSILON;

        /* Freeze items with zero flex factor in the relevant direction */
        bool froze_any = false;
        for (int i = start; i < start + count; i++) {
            if (items[i].frozen) continue;

            if (growing && items[i].flex_grow <= 0.0f) {
                items[i].frozen = true;
                froze_any = true;
            } else if (shrinking && items[i].flex_shrink <= 0.0f) {
                items[i].frozen = true;
                froze_any = true;
            } else if (!growing && !shrinking) {
                items[i].frozen = true;
                froze_any = true;
            }
        }
        if (froze_any) continue;

        /* Distribute free space */
        bool clamped_any = false;
        for (int i = start; i < start + count; i++) {
            if (items[i].frozen) continue;

            float ratio;
            if (growing) {
                ratio = (total_grow > 0.0f) ?
                        items[i].flex_grow / total_grow : 0.0f;
            } else {
                float weighted = items[i].flex_shrink * items[i].flex_basis;
                ratio = (total_shrink > 0.0f) ?
                        weighted / total_shrink : 0.0f;
            }

            float new_size = items[i].hypothetical + free_space * ratio;
            float clamped = r8e_ui_clampf(new_size,
                                         items[i].min_main,
                                         items[i].max_main);

            if (r8e_ui_absf(clamped - new_size) > R8E_UI_EPSILON) {
                items[i].main_size = clamped;
                items[i].frozen = true;
                clamped_any = true;
            } else {
                items[i].main_size = new_size;
            }
        }

        if (!clamped_any) {
            /* All items sized without hitting constraints, done */
            for (int i = start; i < start + count; i++) {
                items[i].frozen = true;
            }
            break;
        }
    }

    /* Ensure non-negative sizes */
    for (int i = start; i < start + count; i++) {
        if (items[i].main_size < 0.0f) items[i].main_size = 0.0f;
    }
}

/*
 * Compute cross sizes for each flex line.
 */
static void r8e_ui_compute_flex_line_cross(R8EUIFlexItem *items,
                                          R8EUIFlexLine *lines, int line_count,
                                          R8EUIAlignItems container_align) {
    for (int l = 0; l < line_count; l++) {
        float max_cross = 0.0f;
        int start = lines[l].start;
        int count = lines[l].count;

        for (int i = start; i < start + count; i++) {
            float outer_cross = r8e_ui_flex_item_outer_cross(&items[i]);
            max_cross = r8e_ui_maxf(max_cross, outer_cross);
        }

        lines[l].cross_size = max_cross;

        /* Stretch items if align-items is stretch */
        if (container_align == R8E_UI_ALIGN_STRETCH) {
            for (int i = start; i < start + count; i++) {
                R8EUIAlignItems self_align = items[i].node->style.align_self;
                /* R8E_UI_ALIGN_FLEX_START as default means "use container" */
                bool use_stretch = (self_align == R8E_UI_ALIGN_STRETCH) ||
                                   (self_align == R8E_UI_ALIGN_FLEX_START &&
                                    container_align == R8E_UI_ALIGN_STRETCH);
                if (use_stretch) {
                    float avail = max_cross - items[i].margin_cross;
                    if (avail > items[i].cross_size) {
                        items[i].cross_size = avail;
                    }
                }
            }
        }
    }
}

/*
 * Position items along the main axis using justify-content.
 */
static void r8e_ui_justify_flex_line(R8EUIFlexItem *items, int start, int count,
                                   float container_main, float gap,
                                   R8EUIJustifyContent justify, bool reverse,
                                   float *positions) {
    if (count == 0) return;

    /* Calculate total size of items */
    float total_items = 0.0f;
    for (int i = start; i < start + count; i++) {
        total_items += items[i].main_size + items[i].margin_main;
    }

    /* Total gap space */
    float total_gap = (count > 1) ? gap * (float)(count - 1) : 0.0f;
    float free_space = container_main - total_items - total_gap;
    if (r8e_ui_is_undefined(container_main)) free_space = 0.0f;
    if (free_space < 0.0f) free_space = 0.0f;

    /* Starting offset and spacing based on justify-content */
    float initial_offset = 0.0f;
    float item_spacing = gap;

    switch (justify) {
    case R8E_UI_JUSTIFY_FLEX_START:
        initial_offset = 0.0f;
        break;
    case R8E_UI_JUSTIFY_FLEX_END:
        initial_offset = free_space;
        break;
    case R8E_UI_JUSTIFY_CENTER:
        initial_offset = free_space / 2.0f;
        break;
    case R8E_UI_JUSTIFY_SPACE_BETWEEN:
        initial_offset = 0.0f;
        if (count > 1) {
            item_spacing = gap + free_space / (float)(count - 1);
        }
        break;
    case R8E_UI_JUSTIFY_SPACE_AROUND:
        if (count > 0) {
            float per_item = free_space / (float)count;
            initial_offset = per_item / 2.0f;
            item_spacing = gap + per_item;
        }
        break;
    case R8E_UI_JUSTIFY_SPACE_EVENLY:
        if (count > 0) {
            float total_spaces = (float)(count + 1);
            float per_space = free_space / total_spaces;
            initial_offset = per_space;
            item_spacing = gap + per_space;
        }
        break;
    }

    /* Place items */
    float cursor = initial_offset;
    for (int idx = 0; idx < count; idx++) {
        int i;
        if (reverse) {
            i = start + count - 1 - idx;
        } else {
            i = start + idx;
        }

        /* Account for item margin-start */
        float margin_start, margin_end;
        bool is_row = true; /* caller determines this context */
        margin_start = items[i].margin_main / 2.0f;
        margin_end = items[i].margin_main / 2.0f;

        positions[i - start] = cursor + margin_start;
        cursor += items[i].main_size + items[i].margin_main;

        if (idx < count - 1) {
            cursor += item_spacing;
        }
    }
}

/*
 * Position items along the cross axis using align-items / align-self.
 */
static float r8e_ui_align_cross_item(const R8EUIFlexItem *item,
                                   float line_cross_size,
                                   R8EUIAlignItems container_align) {
    R8EUIAlignItems align = item->node->style.align_self;
    /* Use container align if align-self is not overridden */
    if (align == R8E_UI_ALIGN_FLEX_START &&
        container_align != R8E_UI_ALIGN_FLEX_START) {
        /* Check if this was actually set or just the default */
        /* For simplicity, we use container alignment */
        align = container_align;
    }

    float outer_cross = r8e_ui_flex_item_outer_cross(item);
    float avail = line_cross_size - outer_cross;
    if (avail < 0.0f) avail = 0.0f;

    float margin_cross_start = item->margin_cross / 2.0f;

    switch (align) {
    case R8E_UI_ALIGN_FLEX_START:
        return margin_cross_start;
    case R8E_UI_ALIGN_FLEX_END:
        return avail + margin_cross_start;
    case R8E_UI_ALIGN_CENTER:
        return avail / 2.0f + margin_cross_start;
    case R8E_UI_ALIGN_BASELINE:
        /* Approximate baseline alignment */
        return margin_cross_start;
    case R8E_UI_ALIGN_STRETCH:
        return margin_cross_start;
    default:
        return margin_cross_start;
    }
}

/*
 * Distribute cross-axis space between lines (align-content).
 */
static void r8e_ui_align_content_lines(R8EUIFlexLine *lines, int line_count,
                                     float container_cross, float gap_cross,
                                     R8EUIAlignContent align_content,
                                     float *line_offsets) {
    if (line_count == 0) return;

    float total_cross = 0.0f;
    for (int l = 0; l < line_count; l++) {
        total_cross += lines[l].cross_size;
    }
    float total_gap = (line_count > 1) ? gap_cross * (float)(line_count - 1) : 0.0f;
    float free_space = container_cross - total_cross - total_gap;
    if (r8e_ui_is_undefined(container_cross)) free_space = 0.0f;
    if (free_space < 0.0f) free_space = 0.0f;

    float initial_offset = 0.0f;
    float line_spacing = gap_cross;

    switch (align_content) {
    case R8E_UI_ALIGN_CONTENT_FLEX_START:
        initial_offset = 0.0f;
        break;
    case R8E_UI_ALIGN_CONTENT_FLEX_END:
        initial_offset = free_space;
        break;
    case R8E_UI_ALIGN_CONTENT_CENTER:
        initial_offset = free_space / 2.0f;
        break;
    case R8E_UI_ALIGN_CONTENT_STRETCH:
        initial_offset = 0.0f;
        if (line_count > 0) {
            float extra = free_space / (float)line_count;
            for (int l = 0; l < line_count; l++) {
                lines[l].cross_size += extra;
            }
        }
        break;
    case R8E_UI_ALIGN_CONTENT_SPACE_BETWEEN:
        initial_offset = 0.0f;
        if (line_count > 1) {
            line_spacing = gap_cross + free_space / (float)(line_count - 1);
        }
        break;
    case R8E_UI_ALIGN_CONTENT_SPACE_AROUND:
        if (line_count > 0) {
            float per_line = free_space / (float)line_count;
            initial_offset = per_line / 2.0f;
            line_spacing = gap_cross + per_line;
        }
        break;
    }

    float cursor = initial_offset;
    for (int l = 0; l < line_count; l++) {
        line_offsets[l] = cursor;
        cursor += lines[l].cross_size;
        if (l < line_count - 1) {
            cursor += line_spacing;
        }
    }
}

/*
 * Main flexbox layout algorithm.
 */
static void r8e_ui_layout_flex(R8EUIDOMNode *node, float container_w,
                             float container_h) {
    if (!node) return;

    const R8EUIComputedStyle *s = &node->style;
    R8EUILayoutBox *box = &node->layout;

    /* Copy box model */
    memcpy(box->padding, s->padding, sizeof(float) * 4);
    memcpy(box->border, s->border_width, sizeof(float) * 4);
    memcpy(box->margin, s->margin, sizeof(float) * 4);

    bool is_row = r8e_ui_flex_is_row(s->flex_direction);
    bool is_reverse = r8e_ui_flex_is_reverse(s->flex_direction);
    bool is_wrap_reverse = (s->flex_wrap == R8E_UI_FLEX_WRAP_REVERSE);

    /* Resolve container dimensions */
    float resolved_w = r8e_ui_resolve_dim(s->width, container_w);
    float resolved_h = r8e_ui_resolve_dim(s->height, container_h);

    /* Handle auto margins for centering */
    if (s->margin_auto[EDGE_LEFT] && s->margin_auto[EDGE_RIGHT] &&
        !r8e_ui_is_undefined(container_w) && !r8e_ui_is_undefined(resolved_w)) {
        float total_w = resolved_w + r8e_ui_padding_h(s) + r8e_ui_border_h(s);
        float remaining = container_w - total_w;
        if (remaining > 0.0f) {
            box->margin[EDGE_LEFT] = remaining / 2.0f;
            box->margin[EDGE_RIGHT] = remaining / 2.0f;
        }
    }

    if (r8e_ui_is_undefined(resolved_w)) {
        resolved_w = container_w;
        if (!r8e_ui_is_undefined(resolved_w)) {
            resolved_w -= r8e_ui_margin_h(s) + r8e_ui_border_h(s) +
                          r8e_ui_padding_h(s);
            if (resolved_w < 0.0f) resolved_w = 0.0f;
        }
    }
    resolved_w = r8e_ui_apply_min_max_width(s, resolved_w, container_w);

    if (r8e_ui_is_undefined(resolved_h)) {
        resolved_h = container_h;
        if (!r8e_ui_is_undefined(resolved_h)) {
            resolved_h -= r8e_ui_margin_v(s) + r8e_ui_border_v(s) +
                          r8e_ui_padding_v(s);
            if (resolved_h < 0.0f) resolved_h = 0.0f;
        }
    }
    resolved_h = r8e_ui_apply_min_max_height(s, resolved_h, container_h);

    /* Content area */
    float content_w = r8e_ui_defined_or(resolved_w, R8E_UI_UNDEFINED);
    float content_h = r8e_ui_defined_or(resolved_h, R8E_UI_UNDEFINED);

    /* Container main and cross sizes */
    float container_main = is_row ? content_w : content_h;
    float container_cross = is_row ? content_h : content_w;

    /* Gap values */
    float gap_main = is_row ? s->gap_column : s->gap_row;
    float gap_cross = is_row ? s->gap_row : s->gap_column;

    /* Collect flex items */
    int max_items = r8e_ui_count_children(node);
    R8EUIFlexItem stack_items[R8E_UI_MAX_STACK_CHILDREN];
    R8EUIFlexItem *items;
    bool items_heap = false;

    if (max_items <= R8E_UI_MAX_STACK_CHILDREN) {
        items = stack_items;
    } else {
        items = (R8EUIFlexItem *)malloc(sizeof(R8EUIFlexItem) * (size_t)max_items);
        if (!items) return;
        items_heap = true;
    }

    int item_count = r8e_ui_collect_flex_items(node, items, max_items);

    /* Sort by CSS order */
    r8e_ui_sort_flex_items_by_order(items, item_count);

    /* Pre-measure all items */
    for (int i = 0; i < item_count; i++) {
        r8e_ui_measure_node(items[i].node,
                          is_row ? container_main : container_cross,
                          is_row ? container_cross : container_main);
    }

    /* Resolve item sizes */
    r8e_ui_resolve_flex_items(items, item_count, s->flex_direction,
                            container_main, container_cross);

    /* Break into lines */
    R8EUIFlexLine lines[R8E_UI_MAX_FLEX_LINES];
    int line_count = r8e_ui_compute_flex_lines(items, item_count, s->flex_wrap,
                                             container_main, gap_main,
                                             lines, R8E_UI_MAX_FLEX_LINES);

    /* Resolve flexible lengths per line */
    for (int l = 0; l < line_count; l++) {
        r8e_ui_resolve_flex_line(items, lines[l].start, lines[l].count,
                               r8e_ui_defined_or(container_main, FLT_MAX),
                               gap_main);
    }

    /* Compute cross sizes per line */
    r8e_ui_compute_flex_line_cross(items, lines, line_count, s->align_items);

    /* Compute line offsets on cross axis */
    float line_offsets[R8E_UI_MAX_FLEX_LINES];
    memset(line_offsets, 0, sizeof(line_offsets));

    if (!r8e_ui_is_undefined(container_cross) && line_count > 0) {
        r8e_ui_align_content_lines(lines, line_count, container_cross,
                                 gap_cross, s->align_content, line_offsets);
    } else {
        /* No container cross size: stack lines sequentially */
        float offset = 0.0f;
        for (int l = 0; l < line_count; l++) {
            line_offsets[l] = offset;
            offset += lines[l].cross_size;
            if (l < line_count - 1) offset += gap_cross;
        }
    }

    /* Position items */
    float total_main_size = 0.0f;
    float total_cross_size = 0.0f;

    for (int l = 0; l < line_count; l++) {
        int start = lines[l].start;
        int count = lines[l].count;

        /* Compute main-axis positions */
        float main_positions[R8E_UI_MAX_STACK_CHILDREN];
        float *mp;
        bool mp_heap = false;
        if (count <= R8E_UI_MAX_STACK_CHILDREN) {
            mp = main_positions;
        } else {
            mp = (float *)malloc(sizeof(float) * (size_t)count);
            if (!mp) continue;
            mp_heap = true;
        }

        r8e_ui_justify_flex_line(items, start, count,
                               r8e_ui_defined_or(container_main, FLT_MAX),
                               gap_main, s->justify_content, is_reverse, mp);

        /* Position each item */
        float cross_offset = line_offsets[l];
        if (is_wrap_reverse && line_count > 1) {
            /* Reverse the cross-axis order of lines */
            float total_lines_cross = 0.0f;
            for (int ll = 0; ll < line_count; ll++) {
                total_lines_cross += lines[ll].cross_size;
                if (ll < line_count - 1) total_lines_cross += gap_cross;
            }
            cross_offset = total_lines_cross - cross_offset -
                           lines[l].cross_size;
        }

        for (int idx = 0; idx < count; idx++) {
            int i = start + idx;
            R8EUIDOMNode *child = items[i].node;
            R8EUILayoutBox *child_box = &child->layout;

            float main_pos = mp[idx];
            float cross_pos = cross_offset +
                              r8e_ui_align_cross_item(&items[i],
                                                     lines[l].cross_size,
                                                     s->align_items);

            if (is_row) {
                child_box->x = main_pos + box->padding[EDGE_LEFT] +
                               box->border[EDGE_LEFT];
                child_box->y = cross_pos + box->padding[EDGE_TOP] +
                               box->border[EDGE_TOP];
                child_box->width = items[i].main_size;
                child_box->height = items[i].cross_size;
            } else {
                child_box->x = cross_pos + box->padding[EDGE_LEFT] +
                               box->border[EDGE_LEFT];
                child_box->y = main_pos + box->padding[EDGE_TOP] +
                               box->border[EDGE_TOP];
                child_box->width = items[i].cross_size;
                child_box->height = items[i].main_size;
            }

            /* Copy box model to child */
            memcpy(child_box->padding, child->style.padding,
                   sizeof(float) * 4);
            memcpy(child_box->border, child->style.border_width,
                   sizeof(float) * 4);
            memcpy(child_box->margin, child->style.margin,
                   sizeof(float) * 4);

            /* Layout child's children */
            if (child->first_child) {
                r8e_ui_layout_node(child, child_box->width, child_box->height);
            }

            /* Track total size */
            if (is_row) {
                float end = main_pos + items[i].main_size +
                            items[i].margin_main;
                total_main_size = r8e_ui_maxf(total_main_size, end);
            } else {
                float end = main_pos + items[i].main_size +
                            items[i].margin_main;
                total_main_size = r8e_ui_maxf(total_main_size, end);
            }
        }

        total_cross_size = cross_offset + lines[l].cross_size;

        if (mp_heap) free(mp);
    }

    /* Set container size */
    if (is_row) {
        box->width = r8e_ui_is_undefined(content_w) ?
                     total_main_size :
                     r8e_ui_apply_min_max_width(s, content_w, container_w);
        box->height = r8e_ui_is_undefined(content_h) ?
                      total_cross_size :
                      r8e_ui_apply_min_max_height(s, content_h, container_h);
    } else {
        box->width = r8e_ui_is_undefined(content_w) ?
                     total_cross_size :
                     r8e_ui_apply_min_max_width(s, content_w, container_w);
        box->height = r8e_ui_is_undefined(content_h) ?
                      total_main_size :
                      r8e_ui_apply_min_max_height(s, content_h, container_h);
    }

    /* Handle positioned children */
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        if (child->style.position == R8E_UI_POSITION_ABSOLUTE ||
            child->style.position == R8E_UI_POSITION_FIXED) {
            r8e_ui_layout_node(child, box->width, box->height);
            r8e_ui_layout_positioned(child, box->width, box->height);
        }
        child = child->next_sibling;
    }

    /* Record scroll dimensions */
    node->scroll_width = r8e_ui_maxf(box->width, total_main_size);
    node->scroll_height = r8e_ui_maxf(box->height, total_cross_size);

    if (items_heap) free(items);
}


/* =========================================================================
 * Positioned Layout (absolute, fixed, relative)
 *
 * - relative: offset from normal flow position
 * - absolute: positioned relative to nearest positioned ancestor
 * - fixed: positioned relative to viewport (treated as absolute here)
 * ========================================================================= */

static void r8e_ui_layout_positioned(R8EUIDOMNode *node, float container_w,
                                   float container_h) {
    if (!node) return;

    const R8EUIComputedStyle *s = &node->style;
    R8EUILayoutBox *box = &node->layout;

    if (s->position == R8E_UI_POSITION_RELATIVE) {
        /* Relative: apply offsets to the already-computed position */
        float offset_x = r8e_ui_resolve_dim_def(s->left, container_w, 0.0f);
        float offset_y = r8e_ui_resolve_dim_def(s->top, container_h, 0.0f);

        /* Right/bottom are used only if left/top are auto */
        if (s->left.type == R8E_UI_DIM_AUTO && s->right.type != R8E_UI_DIM_AUTO) {
            offset_x = -r8e_ui_resolve_dim_def(s->right, container_w, 0.0f);
        }
        if (s->top.type == R8E_UI_DIM_AUTO && s->bottom.type != R8E_UI_DIM_AUTO) {
            offset_y = -r8e_ui_resolve_dim_def(s->bottom, container_h, 0.0f);
        }

        box->x += offset_x;
        box->y += offset_y;
        return;
    }

    if (s->position == R8E_UI_POSITION_ABSOLUTE ||
        s->position == R8E_UI_POSITION_FIXED) {

        float resolved_w = r8e_ui_resolve_dim(s->width, container_w);
        float resolved_h = r8e_ui_resolve_dim(s->height, container_h);

        /* Resolve explicit offsets */
        float left_val = r8e_ui_resolve_dim(s->left, container_w);
        float right_val = r8e_ui_resolve_dim(s->right, container_w);
        float top_val = r8e_ui_resolve_dim(s->top, container_h);
        float bottom_val = r8e_ui_resolve_dim(s->bottom, container_h);

        /* Horizontal positioning */
        if (!r8e_ui_is_undefined(left_val) && !r8e_ui_is_undefined(right_val)) {
            /* Both left and right specified */
            if (r8e_ui_is_undefined(resolved_w)) {
                /* Compute width from constraints */
                resolved_w = container_w - left_val - right_val -
                             r8e_ui_padding_h(s) - r8e_ui_border_h(s) -
                             r8e_ui_margin_h(s);
                if (resolved_w < 0.0f) resolved_w = 0.0f;
            }
            box->x = left_val + s->margin[EDGE_LEFT];
        } else if (!r8e_ui_is_undefined(left_val)) {
            box->x = left_val + s->margin[EDGE_LEFT];
        } else if (!r8e_ui_is_undefined(right_val)) {
            float w = r8e_ui_is_undefined(resolved_w) ? box->width : resolved_w;
            float outer_w = w + r8e_ui_padding_h(s) + r8e_ui_border_h(s) +
                            r8e_ui_margin_h(s);
            box->x = container_w - right_val - outer_w + s->margin[EDGE_LEFT];
        } else {
            /* No offset: keep static position (x = 0 + margin) */
            box->x = s->margin[EDGE_LEFT];
        }

        /* Vertical positioning */
        if (!r8e_ui_is_undefined(top_val) && !r8e_ui_is_undefined(bottom_val)) {
            if (r8e_ui_is_undefined(resolved_h)) {
                resolved_h = container_h - top_val - bottom_val -
                             r8e_ui_padding_v(s) - r8e_ui_border_v(s) -
                             r8e_ui_margin_v(s);
                if (resolved_h < 0.0f) resolved_h = 0.0f;
            }
            box->y = top_val + s->margin[EDGE_TOP];
        } else if (!r8e_ui_is_undefined(top_val)) {
            box->y = top_val + s->margin[EDGE_TOP];
        } else if (!r8e_ui_is_undefined(bottom_val)) {
            float h = r8e_ui_is_undefined(resolved_h) ? box->height : resolved_h;
            float outer_h = h + r8e_ui_padding_v(s) + r8e_ui_border_v(s) +
                            r8e_ui_margin_v(s);
            box->y = container_h - bottom_val - outer_h + s->margin[EDGE_TOP];
        } else {
            box->y = s->margin[EDGE_TOP];
        }

        /* Apply resolved dimensions */
        if (!r8e_ui_is_undefined(resolved_w)) {
            box->width = r8e_ui_apply_min_max_width(s, resolved_w, container_w);
        }
        if (!r8e_ui_is_undefined(resolved_h)) {
            box->height = r8e_ui_apply_min_max_height(s, resolved_h, container_h);
        }

        /* Copy box model */
        memcpy(box->padding, s->padding, sizeof(float) * 4);
        memcpy(box->border, s->border_width, sizeof(float) * 4);
        memcpy(box->margin, s->margin, sizeof(float) * 4);

        /* Handle auto margins for centering absolute elements */
        if (s->margin_auto[EDGE_LEFT] && s->margin_auto[EDGE_RIGHT] &&
            !r8e_ui_is_undefined(left_val) && !r8e_ui_is_undefined(right_val) &&
            !r8e_ui_is_undefined(resolved_w)) {
            float total_h_space = container_w - left_val - right_val -
                                  resolved_w - r8e_ui_padding_h(s) -
                                  r8e_ui_border_h(s);
            if (total_h_space > 0.0f) {
                box->margin[EDGE_LEFT] = total_h_space / 2.0f;
                box->margin[EDGE_RIGHT] = total_h_space / 2.0f;
                box->x = left_val + box->margin[EDGE_LEFT];
            }
        }

        if (s->margin_auto[EDGE_TOP] && s->margin_auto[EDGE_BOTTOM] &&
            !r8e_ui_is_undefined(top_val) && !r8e_ui_is_undefined(bottom_val) &&
            !r8e_ui_is_undefined(resolved_h)) {
            float total_v_space = container_h - top_val - bottom_val -
                                  resolved_h - r8e_ui_padding_v(s) -
                                  r8e_ui_border_v(s);
            if (total_v_space > 0.0f) {
                box->margin[EDGE_TOP] = total_v_space / 2.0f;
                box->margin[EDGE_BOTTOM] = total_v_space / 2.0f;
                box->y = top_val + box->margin[EDGE_TOP];
            }
        }
    }
}


/* =========================================================================
 * Table Layout
 *
 * Simple auto-sizing table layout:
 *   1. First pass: measure all cells to find max width per column
 *   2. Distribute remaining width proportionally
 *   3. Second pass: position cells in a grid based on column widths
 *      and row heights
 *
 * Table structure expected: <table> contains <tr> rows, each <tr> contains
 * <td>/<th> cells. <thead>/<tbody>/<tfoot> are treated as transparent
 * wrappers whose children (rows) are collected.
 * ========================================================================= */

/* Maximum number of columns/rows for stack allocation */
#define R8E_UI_MAX_TABLE_COLS 64
#define R8E_UI_MAX_TABLE_ROWS 128

/*
 * Check if a node is a table row (display: table-row).
 */
static inline bool r8e_ui_is_table_row(const R8EUIDOMNode *node) {
    return node && node->style.display == R8E_UI_DISPLAY_TABLE_ROW;
}

/*
 * Check if a node is a table cell (display: table-cell).
 */
static inline bool r8e_ui_is_table_cell(const R8EUIDOMNode *node) {
    return node && node->style.display == R8E_UI_DISPLAY_TABLE_CELL;
}

/*
 * Collect table rows from the table node. Rows may be direct children
 * or nested inside thead/tbody/tfoot section elements. Section elements
 * with display: block are treated as transparent row-group wrappers.
 */
static int r8e_ui_table_collect_rows(R8EUIDOMNode *table,
                                     R8EUIDOMNode **rows, int max_rows) {
    int count = 0;
    R8EUIDOMNode *child = table->first_child;
    while (child && count < max_rows) {
        if (child->style.display == R8E_UI_DISPLAY_NONE) {
            child = child->next_sibling;
            continue;
        }
        if (r8e_ui_is_table_row(child)) {
            rows[count++] = child;
        } else {
            /* Might be thead/tbody/tfoot — look inside for rows */
            R8EUIDOMNode *inner = child->first_child;
            while (inner && count < max_rows) {
                if (inner->style.display != R8E_UI_DISPLAY_NONE &&
                    r8e_ui_is_table_row(inner)) {
                    rows[count++] = inner;
                }
                inner = inner->next_sibling;
            }
        }
        child = child->next_sibling;
    }
    return count;
}

/*
 * Count the number of cells in a table row.
 */
static int r8e_ui_table_row_cell_count(const R8EUIDOMNode *row) {
    int count = 0;
    const R8EUIDOMNode *cell = row->first_child;
    while (cell) {
        if (cell->style.display != R8E_UI_DISPLAY_NONE &&
            r8e_ui_is_table_cell(cell)) {
            count++;
        }
        cell = cell->next_sibling;
    }
    return count;
}

/*
 * Layout a table element and all its rows/cells.
 */
static void r8e_ui_layout_table(R8EUIDOMNode *node, float container_w,
                                float container_h) {
    if (!node) return;

    const R8EUIComputedStyle *s = &node->style;
    R8EUILayoutBox *box = &node->layout;

    /* Copy box model edges */
    memcpy(box->padding, s->padding, sizeof(float) * 4);
    memcpy(box->border, s->border_width, sizeof(float) * 4);
    memcpy(box->margin, s->margin, sizeof(float) * 4);

    /* Resolve explicit table width */
    float resolved_w = r8e_ui_resolve_dim(s->width, container_w);
    if (r8e_ui_is_undefined(resolved_w)) {
        resolved_w = container_w;
        if (!r8e_ui_is_undefined(resolved_w)) {
            resolved_w -= r8e_ui_margin_h(s) + r8e_ui_border_h(s) +
                          r8e_ui_padding_h(s);
            if (resolved_w < 0.0f) resolved_w = 0.0f;
        }
    }

    float content_w = resolved_w;
    if (r8e_ui_is_undefined(content_w)) content_w = 0.0f;

    /* Collect all rows */
    R8EUIDOMNode *rows[R8E_UI_MAX_TABLE_ROWS];
    int num_rows = r8e_ui_table_collect_rows(node, rows, R8E_UI_MAX_TABLE_ROWS);

    /* Determine number of columns (max cells across all rows) */
    int num_cols = 0;
    for (int r = 0; r < num_rows; r++) {
        int nc = r8e_ui_table_row_cell_count(rows[r]);
        if (nc > num_cols) num_cols = nc;
    }
    if (num_cols > R8E_UI_MAX_TABLE_COLS) num_cols = R8E_UI_MAX_TABLE_COLS;

    /* === Pass 1: Measure cells, find max content width per column === */
    float col_widths[R8E_UI_MAX_TABLE_COLS];
    float row_heights[R8E_UI_MAX_TABLE_ROWS];
    memset(col_widths, 0, sizeof(float) * (size_t)num_cols);
    memset(row_heights, 0, sizeof(float) * (size_t)num_rows);

    for (int r = 0; r < num_rows; r++) {
        R8EUIDOMNode *cell = rows[r]->first_child;
        int c = 0;
        while (cell && c < num_cols) {
            if (cell->style.display == R8E_UI_DISPLAY_NONE) {
                cell = cell->next_sibling;
                continue;
            }
            if (!r8e_ui_is_table_cell(cell)) {
                cell = cell->next_sibling;
                continue;
            }

            /* Measure cell content (intrinsic size only, not block layout) */
            r8e_ui_measure_node(cell, content_w, container_h);

            /* Use the intrinsic measured size from measure_node */
            float cell_w = cell->layout.width;
            float cell_h = cell->layout.height;

            /* Check for explicit cell width */
            float explicit_w = r8e_ui_resolve_dim(cell->style.width, content_w);
            if (!r8e_ui_is_undefined(explicit_w)) {
                cell_w = explicit_w + r8e_ui_padding_h(&cell->style) +
                         r8e_ui_border_h(&cell->style);
            }

            if (cell_w > col_widths[c]) col_widths[c] = cell_w;
            if (cell_h > row_heights[r]) row_heights[r] = cell_h;

            c++;
            cell = cell->next_sibling;
        }
    }

    /* === Pass 1.5: Distribute remaining width proportionally === */
    float total_content_width = 0.0f;
    for (int c = 0; c < num_cols; c++) {
        total_content_width += col_widths[c];
    }

    if (total_content_width < content_w && num_cols > 0) {
        float remaining = content_w - total_content_width;
        float per_col = remaining / (float)num_cols;
        for (int c = 0; c < num_cols; c++) {
            col_widths[c] += per_col;
        }
        total_content_width = content_w;
    }

    /* === Pass 2: Position cells in the grid === */
    float pad_top = box->padding[EDGE_TOP] + box->border[EDGE_TOP];
    float pad_left = box->padding[EDGE_LEFT] + box->border[EDGE_LEFT];
    float cursor_y = 0.0f;

    for (int r = 0; r < num_rows; r++) {
        R8EUILayoutBox *row_box = &rows[r]->layout;
        memcpy(row_box->padding, rows[r]->style.padding, sizeof(float) * 4);
        memcpy(row_box->border, rows[r]->style.border_width, sizeof(float) * 4);
        memcpy(row_box->margin, rows[r]->style.margin, sizeof(float) * 4);

        row_box->x = pad_left;
        row_box->y = pad_top + cursor_y;
        row_box->width = total_content_width;
        row_box->height = row_heights[r];

        /* Position cells within the row */
        float cursor_x = 0.0f;
        R8EUIDOMNode *cell = rows[r]->first_child;
        int c = 0;
        while (cell && c < num_cols) {
            if (cell->style.display == R8E_UI_DISPLAY_NONE) {
                cell = cell->next_sibling;
                continue;
            }
            if (!r8e_ui_is_table_cell(cell)) {
                cell = cell->next_sibling;
                continue;
            }

            R8EUILayoutBox *cell_box = &cell->layout;
            memcpy(cell_box->padding, cell->style.padding, sizeof(float) * 4);
            memcpy(cell_box->border, cell->style.border_width, sizeof(float) * 4);
            memcpy(cell_box->margin, cell->style.margin, sizeof(float) * 4);

            /* Cell content width is column width minus padding/border */
            float cell_content_w = col_widths[c] -
                                   r8e_ui_padding_h(&cell->style) -
                                   r8e_ui_border_h(&cell->style);
            if (cell_content_w < 0.0f) cell_content_w = 0.0f;

            /* Cell content height is row height minus padding/border */
            float cell_content_h = row_heights[r] -
                                   r8e_ui_padding_v(&cell->style) -
                                   r8e_ui_border_v(&cell->style);
            if (cell_content_h < 0.0f) cell_content_h = 0.0f;

            cell_box->x = cursor_x + cell_box->padding[EDGE_LEFT] +
                          cell_box->border[EDGE_LEFT];
            cell_box->y = cell_box->padding[EDGE_TOP] +
                          cell_box->border[EDGE_TOP];
            cell_box->width = cell_content_w;
            cell_box->height = cell_content_h;

            /* Re-layout cell children with final width */
            R8EUIDOMNode *cell_child = cell->first_child;
            float child_y = 0.0f;
            while (cell_child) {
                if (cell_child->style.display != R8E_UI_DISPLAY_NONE) {
                    r8e_ui_layout_node(cell_child, cell_content_w, cell_content_h);
                    cell_child->layout.x = cell_child->layout.margin[EDGE_LEFT];
                    cell_child->layout.y = child_y +
                                           cell_child->layout.margin[EDGE_TOP];
                    child_y = cell_child->layout.y + cell_child->layout.height +
                              r8e_ui_padding_v(&cell_child->style) +
                              r8e_ui_border_v(&cell_child->style) +
                              cell_child->layout.margin[EDGE_BOTTOM];
                }
                cell_child = cell_child->next_sibling;
            }

            cursor_x += col_widths[c];
            c++;
            cell = cell->next_sibling;
        }

        cursor_y += row_heights[r];
    }

    /* Set table dimensions */
    box->width = r8e_ui_apply_min_max_width(s, total_content_width, container_w);

    float resolved_h = r8e_ui_resolve_dim(s->height, container_h);
    if (r8e_ui_is_undefined(resolved_h)) {
        resolved_h = cursor_y;
    }
    resolved_h = r8e_ui_apply_min_max_height(s, resolved_h, container_h);
    box->height = resolved_h;

    /* Record scroll dimensions */
    node->scroll_width = r8e_ui_maxf(box->width, total_content_width);
    node->scroll_height = r8e_ui_maxf(resolved_h, cursor_y);
}


/* =========================================================================
 * Scroll Support
 *
 * Elements with overflow: auto or overflow: scroll are scrollable.
 * The scroll offset is clamped to [0, content_height - element_height].
 * ========================================================================= */

/*
 * Mark a node as scrollable based on its overflow property and content size.
 * Called after layout to determine which nodes can scroll.
 */
void r8e_ui_layout_update_scrollable(R8EUIDOMNode *node) {
    if (!node) return;

    /* Check if overflow is auto, scroll, or hidden */
    if (node->style.overflow == R8E_UI_OVERFLOW_SCROLL ||
        node->style.overflow == R8E_UI_OVERFLOW_AUTO ||
        node->style.overflow == R8E_UI_OVERFLOW_HIDDEN) {
        /* scrollable if content exceeds element bounds */
        node->scrollable = (node->scroll_height > node->layout.height) ||
                          (node->scroll_width > node->layout.width);
    } else {
        node->scrollable = false;
    }

    /* Clamp current scroll offset */
    if (node->scrollable) {
        float max_scroll_y = node->scroll_height - node->layout.height;
        if (max_scroll_y < 0.0f) max_scroll_y = 0.0f;
        node->scroll_y = r8e_ui_clampf(node->scroll_y, 0.0f, max_scroll_y);

        float max_scroll_x = node->scroll_width - node->layout.width;
        if (max_scroll_x < 0.0f) max_scroll_x = 0.0f;
        node->scroll_x = r8e_ui_clampf(node->scroll_x, 0.0f, max_scroll_x);
    } else {
        node->scroll_x = 0.0f;
        node->scroll_y = 0.0f;
    }

    /* Recurse to children */
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        r8e_ui_layout_update_scrollable(child);
        child = child->next_sibling;
    }
}

/*
 * Apply a scroll delta (e.g., from mouse wheel) to a scrollable node.
 * Returns true if the scroll offset actually changed.
 *
 * @param node      The scrollable node.
 * @param delta_x   Horizontal scroll delta (positive = right).
 * @param delta_y   Vertical scroll delta (positive = down).
 */
bool r8e_ui_layout_scroll(R8EUIDOMNode *node, float delta_x, float delta_y) {
    if (!node || !node->scrollable) return false;

    float old_x = node->scroll_x;
    float old_y = node->scroll_y;

    node->scroll_y += delta_y;
    node->scroll_x += delta_x;

    /* Clamp */
    float max_scroll_y = node->scroll_height - node->layout.height;
    if (max_scroll_y < 0.0f) max_scroll_y = 0.0f;
    node->scroll_y = r8e_ui_clampf(node->scroll_y, 0.0f, max_scroll_y);

    float max_scroll_x = node->scroll_width - node->layout.width;
    if (max_scroll_x < 0.0f) max_scroll_x = 0.0f;
    node->scroll_x = r8e_ui_clampf(node->scroll_x, 0.0f, max_scroll_x);

    return (node->scroll_x != old_x) || (node->scroll_y != old_y);
}

/*
 * Find the nearest scrollable ancestor of a node (or the node itself).
 * Used for routing mouse wheel events to the right container.
 */
R8EUIDOMNode *r8e_ui_layout_find_scrollable(R8EUIDOMNode *node) {
    while (node) {
        if (node->scrollable) return node;
        node = node->parent;
    }
    return NULL;
}


/* =========================================================================
 * Main Layout Entry Point
 *
 * Dispatches to the appropriate layout algorithm based on the node's
 * display property.
 * ========================================================================= */

static void r8e_ui_layout_node(R8EUIDOMNode *node, float container_w,
                             float container_h) {
    if (!node) return;
    if (node->style.display == R8E_UI_DISPLAY_NONE) return;

    /* Text nodes: just measure, no further layout needed */
    if (node->type == R8E_UI_NODE_TEXT) {
        r8e_ui_measure_text_node(node, container_w);
        return;
    }

    switch (node->style.display) {
    case R8E_UI_DISPLAY_FLEX:
        r8e_ui_layout_flex(node, container_w, container_h);
        break;
    case R8E_UI_DISPLAY_INLINE:
        r8e_ui_layout_inline(node, container_w, container_h);
        break;
    case R8E_UI_DISPLAY_TABLE:
        r8e_ui_layout_table(node, container_w, container_h);
        break;
    case R8E_UI_DISPLAY_TABLE_ROW:
    case R8E_UI_DISPLAY_TABLE_CELL:
        /* Table rows and cells are laid out by their parent table */
        r8e_ui_layout_block(node, container_w, container_h);
        break;
    case R8E_UI_DISPLAY_BLOCK:
    default:
        r8e_ui_layout_block(node, container_w, container_h);
        break;
    }

    /* Apply relative positioning offset */
    if (node->style.position == R8E_UI_POSITION_RELATIVE) {
        r8e_ui_layout_positioned(node, container_w, container_h);
    }
}


/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * Initialize default style values for a R8EUIComputedStyle.
 */
void r8e_ui_layout_style_init(R8EUIComputedStyle *style) {
    if (!style) return;
    memset(style, 0, sizeof(R8EUIComputedStyle));

    style->display = R8E_UI_DISPLAY_BLOCK;
    style->position = R8E_UI_POSITION_STATIC;
    style->overflow = R8E_UI_OVERFLOW_VISIBLE;

    style->width = R8E_UI_DIM_AUTO_VAL;
    style->height = R8E_UI_DIM_AUTO_VAL;
    style->min_width = R8E_UI_DIM_NONE_VAL;
    style->min_height = R8E_UI_DIM_NONE_VAL;
    style->max_width = R8E_UI_DIM_NONE_VAL;
    style->max_height = R8E_UI_DIM_NONE_VAL;

    style->top = R8E_UI_DIM_AUTO_VAL;
    style->right = R8E_UI_DIM_AUTO_VAL;
    style->bottom = R8E_UI_DIM_AUTO_VAL;
    style->left = R8E_UI_DIM_AUTO_VAL;

    style->flex_direction = R8E_UI_FLEX_ROW;
    style->justify_content = R8E_UI_JUSTIFY_FLEX_START;
    style->align_items = R8E_UI_ALIGN_STRETCH;
    style->align_self = R8E_UI_ALIGN_FLEX_START;  /* inherit from container */
    style->align_content = R8E_UI_ALIGN_CONTENT_STRETCH;
    style->flex_wrap = R8E_UI_FLEX_NOWRAP;
    style->flex_grow = 0.0f;
    style->flex_shrink = 1.0f;
    style->flex_basis = R8E_UI_DIM_AUTO_VAL;

    style->gap_row = 0.0f;
    style->gap_column = 0.0f;
    style->order = 0;

    style->font_size = 16.0f;
    style->line_height = 1.2f;
}

/*
 * Initialize a layout box to zero.
 */
void r8e_ui_layout_box_init(R8EUILayoutBox *box) {
    if (!box) return;
    memset(box, 0, sizeof(R8EUILayoutBox));
}

/*
 * Perform full layout of a DOM tree rooted at the given node.
 *
 * @param root         Root node of the tree to lay out.
 * @param viewport_w   Viewport width in pixels.
 * @param viewport_h   Viewport height in pixels.
 */
void r8e_ui_layout_compute(R8EUIDOMNode *root, float viewport_w,
                         float viewport_h) {
    if (!root) return;

    /* Phase 1: Measure (bottom-up) */
    r8e_ui_measure_node(root, viewport_w, viewport_h);

    /* Phase 2: Layout (top-down) */
    root->layout.x = 0.0f;
    root->layout.y = 0.0f;

    r8e_ui_layout_node(root, viewport_w, viewport_h);

    /* Phase 3: Update scrollable state */
    r8e_ui_layout_update_scrollable(root);
}

/*
 * Perform incremental re-layout starting at a dirty subtree.
 * Only re-lays out nodes marked dirty and their ancestors.
 *
 * @param node         Dirty node to re-layout.
 * @param viewport_w   Viewport width.
 * @param viewport_h   Viewport height.
 */
void r8e_ui_layout_update(R8EUIDOMNode *node, float viewport_w,
                        float viewport_h) {
    if (!node) return;

    /* Walk up to find the nearest ancestor with a known container size */
    R8EUIDOMNode *layout_root = node;
    float container_w = viewport_w;
    float container_h = viewport_h;

    if (node->parent) {
        /* Use the parent's content area as the container */
        R8EUIDOMNode *parent = node->parent;
        container_w = parent->layout.width;
        container_h = parent->layout.height;
        layout_root = node;
    }

    /* Re-measure and re-layout the subtree */
    r8e_ui_measure_node(layout_root, container_w, container_h);
    r8e_ui_layout_node(layout_root, container_w, container_h);
}

/*
 * Mark a node as dirty (needs re-layout).
 * Propagates the dirty flag up to the root.
 */
void r8e_ui_layout_mark_dirty(R8EUIDOMNode *node) {
    R8EUIDOMNode *current = node;
    while (current) {
        if (current->flags & R8E_UI_FLAG_DIRTY) {
            break;  /* already dirty up to here */
        }
        current->flags |= R8E_UI_FLAG_DIRTY;
        current->flags &= ~R8E_UI_FLAG_LAYOUT_DONE;
        current = current->parent;
    }
}

/*
 * Clear dirty flags after layout is complete.
 */
void r8e_ui_layout_clear_dirty(R8EUIDOMNode *node) {
    if (!node) return;
    node->flags &= ~R8E_UI_FLAG_DIRTY;
    node->flags |= R8E_UI_FLAG_LAYOUT_DONE;

    R8EUIDOMNode *child = node->first_child;
    while (child) {
        r8e_ui_layout_clear_dirty(child);
        child = child->next_sibling;
    }
}

/*
 * Get the absolute position of a node by walking up the tree.
 *
 * @param node   The node to query.
 * @param abs_x  Output: absolute x position.
 * @param abs_y  Output: absolute y position.
 */
void r8e_ui_layout_get_absolute_pos(const R8EUIDOMNode *node,
                                  float *abs_x, float *abs_y) {
    float x = 0.0f, y = 0.0f;
    const R8EUIDOMNode *current = node;

    while (current) {
        x += current->layout.x;
        y += current->layout.y;
        current = current->parent;
    }

    if (abs_x) *abs_x = x;
    if (abs_y) *abs_y = y;
}

/*
 * Get the border box rectangle of a node (content + padding + border).
 *
 * @param node   The node to query.
 * @param x      Output: border box x (absolute).
 * @param y      Output: border box y (absolute).
 * @param w      Output: border box width.
 * @param h      Output: border box height.
 */
void r8e_ui_layout_get_border_box(const R8EUIDOMNode *node,
                                float *x, float *y,
                                float *w, float *h) {
    float abs_x, abs_y;
    r8e_ui_layout_get_absolute_pos(node, &abs_x, &abs_y);

    const R8EUILayoutBox *box = &node->layout;

    if (x) *x = abs_x - box->border[EDGE_LEFT] - box->padding[EDGE_LEFT];
    if (y) *y = abs_y - box->border[EDGE_TOP] - box->padding[EDGE_TOP];
    if (w) *w = box->width + r8e_ui_padding_h(&node->style) +
                r8e_ui_border_h(&node->style);
    if (h) *h = box->height + r8e_ui_padding_v(&node->style) +
                r8e_ui_border_v(&node->style);
}

/*
 * Get the margin box rectangle of a node (content + padding + border + margin).
 *
 * @param node   The node to query.
 * @param x      Output: margin box x (absolute).
 * @param y      Output: margin box y (absolute).
 * @param w      Output: margin box width.
 * @param h      Output: margin box height.
 */
void r8e_ui_layout_get_margin_box(const R8EUIDOMNode *node,
                                float *x, float *y,
                                float *w, float *h) {
    float bx, by, bw, bh;
    r8e_ui_layout_get_border_box(node, &bx, &by, &bw, &bh);

    const R8EUILayoutBox *box = &node->layout;
    if (x) *x = bx - box->margin[EDGE_LEFT];
    if (y) *y = by - box->margin[EDGE_TOP];
    if (w) *w = bw + box->margin[EDGE_LEFT] + box->margin[EDGE_RIGHT];
    if (h) *h = bh + box->margin[EDGE_TOP] + box->margin[EDGE_BOTTOM];
}

/*
 * Get the content box rectangle of a node.
 *
 * @param node   The node to query.
 * @param x      Output: content box x (absolute).
 * @param y      Output: content box y (absolute).
 * @param w      Output: content box width.
 * @param h      Output: content box height.
 */
void r8e_ui_layout_get_content_box(const R8EUIDOMNode *node,
                                 float *x, float *y,
                                 float *w, float *h) {
    float abs_x, abs_y;
    r8e_ui_layout_get_absolute_pos(node, &abs_x, &abs_y);

    if (x) *x = abs_x;
    if (y) *y = abs_y;
    if (w) *w = node->layout.width;
    if (h) *h = node->layout.height;
}

/*
 * Get the scroll dimensions for a node.
 *
 * @param node           The node to query.
 * @param scroll_width   Output: total scrollable width.
 * @param scroll_height  Output: total scrollable height.
 */
void r8e_ui_layout_get_scroll_size(const R8EUIDOMNode *node,
                                 float *scroll_width,
                                 float *scroll_height) {
    if (scroll_width) *scroll_width = node->scroll_width;
    if (scroll_height) *scroll_height = node->scroll_height;
}

/*
 * Check if a point is inside the border box of a node.
 *
 * @param node   The node to test.
 * @param px     Point x coordinate (absolute).
 * @param py     Point y coordinate (absolute).
 * @return       true if the point is inside the node's border box.
 */
bool r8e_ui_layout_hit_test(const R8EUIDOMNode *node, float px, float py) {
    float bx, by, bw, bh;
    r8e_ui_layout_get_border_box(node, &bx, &by, &bw, &bh);

    return px >= bx && px < bx + bw && py >= by && py < by + bh;
}

/*
 * Check if a node is a containing block for positioned descendants.
 * A node is a containing block if it is positioned (relative, absolute, fixed)
 * or has overflow != visible.
 */
bool r8e_ui_layout_is_containing_block(const R8EUIDOMNode *node) {
    if (!node) return false;
    return node->style.position != R8E_UI_POSITION_STATIC ||
           node->style.overflow != R8E_UI_OVERFLOW_VISIBLE;
}

/*
 * Find the nearest positioned ancestor of a node.
 * For absolute positioning, this is the containing block.
 */
R8EUIDOMNode *r8e_ui_layout_find_containing_block(R8EUIDOMNode *node) {
    if (!node) return NULL;

    R8EUIDOMNode *parent = node->parent;
    while (parent) {
        if (r8e_ui_layout_is_containing_block(parent)) {
            return parent;
        }
        parent = parent->parent;
    }

    return NULL; /* viewport is the containing block */
}

/*
 * Apply overflow clipping to a node's layout.
 * Sets the clip rectangle that children must be clipped to.
 *
 * @param node    The node with overflow set.
 * @param clip_x  Output: clip rectangle x (absolute).
 * @param clip_y  Output: clip rectangle y (absolute).
 * @param clip_w  Output: clip rectangle width.
 * @param clip_h  Output: clip rectangle height.
 */
void r8e_ui_layout_get_clip_rect(const R8EUIDOMNode *node,
                               float *clip_x, float *clip_y,
                               float *clip_w, float *clip_h) {
    if (node->style.overflow == R8E_UI_OVERFLOW_VISIBLE) {
        /* No clipping */
        if (clip_x) *clip_x = -FLT_MAX;
        if (clip_y) *clip_y = -FLT_MAX;
        if (clip_w) *clip_w = FLT_MAX;
        if (clip_h) *clip_h = FLT_MAX;
        return;
    }

    /* Clip to the padding box */
    float abs_x, abs_y;
    r8e_ui_layout_get_absolute_pos(node, &abs_x, &abs_y);

    const R8EUILayoutBox *box = &node->layout;
    if (clip_x) *clip_x = abs_x - box->padding[EDGE_LEFT];
    if (clip_y) *clip_y = abs_y - box->padding[EDGE_TOP];
    if (clip_w) *clip_w = box->width + r8e_ui_padding_h(&node->style);
    if (clip_h) *clip_h = box->height + r8e_ui_padding_v(&node->style);
}


/* =========================================================================
 * Debug / Diagnostic Utilities
 * ========================================================================= */

/*
 * Print layout tree for debugging (recursive).
 * Writes to a buffer in a human-readable indented format.
 *
 * @param node    The node to dump.
 * @param buf     Output buffer.
 * @param bufsz   Buffer size.
 * @param depth   Current indentation depth.
 * @return        Number of characters written.
 */
static int r8e_ui_layout_dump_recursive(const R8EUIDOMNode *node,
                                      char *buf, int bufsz, int depth) {
    if (!node || bufsz <= 0) return 0;

    int written = 0;
    int remain = bufsz;

    /* Indentation */
    for (int i = 0; i < depth && remain > 1; i++) {
        buf[written++] = ' ';
        buf[written++] = ' ';
        remain -= 2;
    }

    /* Node info */
    const R8EUILayoutBox *b = &node->layout;
    int n;

    if (node->type == R8E_UI_NODE_TEXT) {
        n = snprintf(buf + written, (size_t)remain,
                     "#text x=%.1f y=%.1f w=%.1f h=%.1f\n",
                     b->x, b->y, b->width, b->height);
    } else {
        const char *display_str = "block";
        switch (node->style.display) {
        case R8E_UI_DISPLAY_FLEX:       display_str = "flex"; break;
        case R8E_UI_DISPLAY_INLINE:     display_str = "inline"; break;
        case R8E_UI_DISPLAY_NONE:       display_str = "none"; break;
        case R8E_UI_DISPLAY_TABLE:      display_str = "table"; break;
        case R8E_UI_DISPLAY_TABLE_ROW:  display_str = "table-row"; break;
        case R8E_UI_DISPLAY_TABLE_CELL: display_str = "table-cell"; break;
        default: break;
        }

        n = snprintf(buf + written, (size_t)remain,
                     "<%u> display=%s x=%.1f y=%.1f w=%.1f h=%.1f "
                     "p=[%.0f,%.0f,%.0f,%.0f] m=[%.0f,%.0f,%.0f,%.0f]\n",
                     node->tag_atom, display_str,
                     b->x, b->y, b->width, b->height,
                     b->padding[0], b->padding[1],
                     b->padding[2], b->padding[3],
                     b->margin[0], b->margin[1],
                     b->margin[2], b->margin[3]);
    }

    if (n > 0) {
        if (n >= remain) n = remain - 1;
        written += n;
        remain -= n;
    }

    /* Recurse children */
    const R8EUIDOMNode *child = node->first_child;
    while (child && remain > 1) {
        n = r8e_ui_layout_dump_recursive(child, buf + written, remain, depth + 1);
        written += n;
        remain -= n;
        child = child->next_sibling;
    }

    return written;
}

/*
 * Dump the layout tree to a buffer for debugging.
 *
 * @param root   Root node.
 * @param buf    Output buffer.
 * @param bufsz  Buffer size.
 * @return       Number of characters written (excluding null terminator).
 */
int r8e_ui_layout_dump(const R8EUIDOMNode *root, char *buf, int bufsz) {
    if (!root || !buf || bufsz <= 0) return 0;

    int written = r8e_ui_layout_dump_recursive(root, buf, bufsz - 1, 0);
    buf[written] = '\0';
    return written;
}
