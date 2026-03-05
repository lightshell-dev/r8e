/*
 * npaint.c - R8EUIPaint: Display List Generation and Rendering
 *
 * Part of the r8e JavaScript engine R8EUIUI rendering stack.
 * See CLAUDE.md Section 12.5 (R8EUIPaint).
 *
 * Walks the DOM/layout tree and generates a flat display list of draw
 * commands. Supports display list caching via hashing to skip GPU render
 * when unchanged. Includes a software rasterizer fallback for headless
 * and embedded environments.
 *
 * Draw commands:
 *   - rect, rounded_rect, text, border, clip_push, clip_pop, opacity, image
 *
 * Color handling:
 *   - Hex (#fff, #ffffff, #ffffffff)
 *   - Named colors (basic CSS 16)
 *   - rgba(r, g, b, a)
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* =========================================================================
 * Type Definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

#ifndef R8E_NPAINT_TYPES_DEFINED
#define R8E_NPAINT_TYPES_DEFINED

/* -------------------------------------------------------------------------
 * Color: 32-bit RGBA (0xRRGGBBAA)
 * ------------------------------------------------------------------------- */

typedef uint32_t R8EUIColor;

#define R8E_UI_COLOR_RGBA(r, g, b, a) \
    (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | \
     ((uint32_t)(b) << 8)  | ((uint32_t)(a)))
#define R8E_UI_COLOR_RGB(r, g, b) R8E_UI_COLOR_RGBA(r, g, b, 255)

#define R8E_UI_COLOR_R(c) ((uint8_t)((c) >> 24))
#define R8E_UI_COLOR_G(c) ((uint8_t)((c) >> 16))
#define R8E_UI_COLOR_B(c) ((uint8_t)((c) >> 8))
#define R8E_UI_COLOR_A(c) ((uint8_t)((c)))

/* Predefined colors */
#define R8E_UI_COLOR_TRANSPARENT  R8E_UI_COLOR_RGBA(0, 0, 0, 0)
#define R8E_UI_COLOR_BLACK        R8E_UI_COLOR_RGB(0, 0, 0)
#define R8E_UI_COLOR_WHITE        R8E_UI_COLOR_RGB(255, 255, 255)
#define R8E_UI_COLOR_RED          R8E_UI_COLOR_RGB(255, 0, 0)
#define R8E_UI_COLOR_GREEN        R8E_UI_COLOR_RGB(0, 128, 0)
#define R8E_UI_COLOR_BLUE         R8E_UI_COLOR_RGB(0, 0, 255)
#define R8E_UI_COLOR_YELLOW       R8E_UI_COLOR_RGB(255, 255, 0)

/* -------------------------------------------------------------------------
 * Paint command types
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Paint operation: a single draw command in the display list
 * ------------------------------------------------------------------------- */

typedef struct {
    R8EUIPaintCmd  cmd;
    float         x, y, w, h;
    R8EUIColor     color;
    float         radius[4];     /* corner radii: TL, TR, BR, BL */
    const char   *text;          /* for R8E_UI_CMD_TEXT */
    uint32_t      text_len;
    float         font_size;
    R8EUIColor     text_color;
    float         opacity;       /* for R8E_UI_CMD_OPACITY */
    float         border_widths[4]; /* TRBL for R8E_UI_CMD_BORDER */
    R8EUIColor     border_colors[4]; /* TRBL for R8E_UI_CMD_BORDER */
    uint32_t      image_id;      /* for R8E_UI_CMD_IMAGE */
} R8EUIPaintOp;

/* -------------------------------------------------------------------------
 * Display list: dynamic array of paint operations
 * ------------------------------------------------------------------------- */

typedef struct {
    R8EUIPaintOp *ops;
    uint32_t     count;
    uint32_t     capacity;
    uint64_t     hash;           /* FNV-1a hash of the display list */
    bool         hash_valid;     /* whether hash has been computed */
} R8EUIDisplayList;

/* -------------------------------------------------------------------------
 * Software framebuffer for software rasterizer
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t *pixels;   /* RGBA pixels, row-major, top-to-bottom */
    int       width;
    int       height;
    int       stride;   /* bytes per row (may include padding) */
} R8EUIFramebuffer;

/* -------------------------------------------------------------------------
 * Layout box (must match nlayout.c definition)
 * ------------------------------------------------------------------------- */

typedef struct {
    float x, y;
    float width, height;
    float padding[4];
    float border[4];
    float margin[4];
} R8EUILayoutBox;

/* -------------------------------------------------------------------------
 * Computed style subset needed for painting
 * ------------------------------------------------------------------------- */

typedef struct {
    R8EUIColor     background_color;
    R8EUIColor     color;          /* text color */
    R8EUIColor     border_color[4]; /* TRBL */
    float         border_width[4]; /* TRBL */
    float         border_radius[4]; /* TL, TR, BR, BL */
    float         opacity;
    float         font_size;
    float         line_height;
    uint8_t       overflow;       /* 0=visible, 1=hidden, 2=scroll */
    uint8_t       visibility;     /* 0=visible, 1=hidden */
    /* box-shadow: simplified single shadow */
    float         shadow_offset_x;
    float         shadow_offset_y;
    float         shadow_blur;
    R8EUIColor     shadow_color;
    bool          has_shadow;
} R8EUIPaintStyle;

/* -------------------------------------------------------------------------
 * DOM node type (minimal, matches ndom.c/nlayout.c)
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_NODE_ELEMENT_P  = 1,
    R8E_UI_NODE_TEXT_P     = 3,
} R8EUINodeType_Paint;

typedef struct R8EUIDOMNode_Paint R8EUIDOMNode_Paint;

struct R8EUIDOMNode_Paint {
    uint8_t             type;
    uint32_t            flags;
    R8EUIPaintStyle      paint_style;
    R8EUILayoutBox       layout;
    const char         *text;
    uint32_t            text_len;
    R8EUIDOMNode_Paint  *parent;
    R8EUIDOMNode_Paint  *first_child;
    R8EUIDOMNode_Paint  *last_child;
    R8EUIDOMNode_Paint  *next_sibling;
    float               scroll_x;
    float               scroll_y;
};

#endif /* R8E_NPAINT_TYPES_DEFINED */


/* =========================================================================
 * Display List Management
 * ========================================================================= */

#define R8E_UI_DL_INITIAL_CAPACITY 256

/*
 * Initialize a display list.
 */
void r8e_ui_paint_dl_init(R8EUIDisplayList *dl) {
    if (!dl) return;
    memset(dl, 0, sizeof(R8EUIDisplayList));
}

/*
 * Free a display list's internal resources.
 */
void r8e_ui_paint_dl_free(R8EUIDisplayList *dl) {
    if (!dl) return;
    free(dl->ops);
    memset(dl, 0, sizeof(R8EUIDisplayList));
}

/*
 * Clear all operations from a display list without freeing memory.
 */
void r8e_ui_paint_dl_clear(R8EUIDisplayList *dl) {
    if (!dl) return;
    dl->count = 0;
    dl->hash_valid = false;
}

/*
 * Ensure the display list has room for at least one more operation.
 * Returns false on allocation failure.
 */
static bool r8e_ui_paint_dl_ensure(R8EUIDisplayList *dl) {
    if (dl->count < dl->capacity) return true;

    uint32_t new_cap = dl->capacity == 0 ? R8E_UI_DL_INITIAL_CAPACITY :
                        dl->capacity * 2;
    R8EUIPaintOp *new_ops = (R8EUIPaintOp *)realloc(
        dl->ops, sizeof(R8EUIPaintOp) * new_cap);
    if (!new_ops) return false;

    dl->ops = new_ops;
    dl->capacity = new_cap;
    return true;
}

/*
 * Append a paint operation to the display list.
 * Returns a pointer to the new operation, or NULL on failure.
 */
static R8EUIPaintOp *r8e_ui_paint_dl_push(R8EUIDisplayList *dl) {
    if (!r8e_ui_paint_dl_ensure(dl)) return NULL;

    R8EUIPaintOp *op = &dl->ops[dl->count++];
    memset(op, 0, sizeof(R8EUIPaintOp));
    dl->hash_valid = false;
    return op;
}


/* =========================================================================
 * Display List Hashing (for caching)
 *
 * FNV-1a hash over the display list contents. If the hash matches the
 * previous frame, we can skip GPU re-rendering entirely.
 * ========================================================================= */

static uint64_t r8e_ui_fnv1a_64(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/*
 * Compute the hash of the display list.
 * The hash covers command types, positions, sizes, and colors but
 * NOT text pointers (only text content).
 */
uint64_t r8e_ui_paint_dl_hash(R8EUIDisplayList *dl) {
    if (!dl) return 0;
    if (dl->hash_valid) return dl->hash;

    uint64_t hash = 14695981039346656037ULL;

    for (uint32_t i = 0; i < dl->count; i++) {
        const R8EUIPaintOp *op = &dl->ops[i];

        /* Hash the fixed fields */
        hash ^= (uint64_t)op->cmd;
        hash *= 1099511628211ULL;

        /* Hash position and size as raw bytes */
        uint32_t fbits;
        memcpy(&fbits, &op->x, 4); hash ^= fbits; hash *= 1099511628211ULL;
        memcpy(&fbits, &op->y, 4); hash ^= fbits; hash *= 1099511628211ULL;
        memcpy(&fbits, &op->w, 4); hash ^= fbits; hash *= 1099511628211ULL;
        memcpy(&fbits, &op->h, 4); hash ^= fbits; hash *= 1099511628211ULL;

        hash ^= op->color;
        hash *= 1099511628211ULL;

        /* Hash corner radii */
        for (int r = 0; r < 4; r++) {
            memcpy(&fbits, &op->radius[r], 4);
            hash ^= fbits;
            hash *= 1099511628211ULL;
        }

        /* Hash text content if present */
        if (op->text && op->text_len > 0) {
            hash = hash ^ r8e_ui_fnv1a_64(op->text, op->text_len);
            hash *= 1099511628211ULL;
        }

        memcpy(&fbits, &op->font_size, 4);
        hash ^= fbits;
        hash *= 1099511628211ULL;

        memcpy(&fbits, &op->opacity, 4);
        hash ^= fbits;
        hash *= 1099511628211ULL;
    }

    dl->hash = hash;
    dl->hash_valid = true;
    return hash;
}

/*
 * Check if the display list has changed since the last hash.
 * Returns true if the content is the same as the cached hash.
 */
bool r8e_ui_paint_dl_unchanged(R8EUIDisplayList *dl, uint64_t cached_hash) {
    return r8e_ui_paint_dl_hash(dl) == cached_hash;
}


/* =========================================================================
 * Color Parsing
 *
 * Parse CSS color strings into R8EUIColor (RGBA).
 * Supports: hex (#fff, #ffffff, #ffffffff), named colors, rgba().
 * ========================================================================= */

/* Basic CSS 16 named colors */
typedef struct {
    const char *name;
    R8EUIColor   color;
} R8EUINamedColor;

static const R8EUINamedColor r8e_ui_named_colors[] = {
    { "black",   R8E_UI_COLOR_RGB(0, 0, 0) },
    { "silver",  R8E_UI_COLOR_RGB(192, 192, 192) },
    { "gray",    R8E_UI_COLOR_RGB(128, 128, 128) },
    { "grey",    R8E_UI_COLOR_RGB(128, 128, 128) },
    { "white",   R8E_UI_COLOR_RGB(255, 255, 255) },
    { "maroon",  R8E_UI_COLOR_RGB(128, 0, 0) },
    { "red",     R8E_UI_COLOR_RGB(255, 0, 0) },
    { "purple",  R8E_UI_COLOR_RGB(128, 0, 128) },
    { "fuchsia", R8E_UI_COLOR_RGB(255, 0, 255) },
    { "green",   R8E_UI_COLOR_RGB(0, 128, 0) },
    { "lime",    R8E_UI_COLOR_RGB(0, 255, 0) },
    { "olive",   R8E_UI_COLOR_RGB(128, 128, 0) },
    { "yellow",  R8E_UI_COLOR_RGB(255, 255, 0) },
    { "navy",    R8E_UI_COLOR_RGB(0, 0, 128) },
    { "blue",    R8E_UI_COLOR_RGB(0, 0, 255) },
    { "teal",    R8E_UI_COLOR_RGB(0, 128, 128) },
    { "aqua",    R8E_UI_COLOR_RGB(0, 255, 255) },
    { "orange",  R8E_UI_COLOR_RGB(255, 165, 0) },
    { "transparent", R8E_UI_COLOR_TRANSPARENT },
    { NULL, 0 }
};

/*
 * Parse a single hex digit.
 */
static int r8e_ui_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Case-insensitive string comparison.
 */
static bool r8e_ui_streqi(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/*
 * Skip whitespace.
 */
static const char *r8e_ui_skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

/*
 * Parse an integer from a string, advancing the pointer.
 */
static int r8e_ui_parse_int(const char **s) {
    const char *p = r8e_ui_skip_ws(*s);
    int val = 0;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') { p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    *s = p;
    return neg ? -val : val;
}

/*
 * Parse a float from a string, advancing the pointer.
 */
static float r8e_ui_parse_float(const char **s) {
    const char *p = r8e_ui_skip_ws(*s);
    float val = 0.0f;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') { p++; }

    while (*p >= '0' && *p <= '9') {
        val = val * 10.0f + (float)(*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        float frac = 0.1f;
        while (*p >= '0' && *p <= '9') {
            val += (float)(*p - '0') * frac;
            frac *= 0.1f;
            p++;
        }
    }

    *s = p;
    return neg ? -val : val;
}

/*
 * Parse a CSS color string.
 *
 * Supported formats:
 *   #RGB, #RRGGBB, #RRGGBBAA
 *   rgb(r, g, b), rgba(r, g, b, a)
 *   Named colors (black, white, red, etc.)
 *
 * @param str    Color string.
 * @param out    Output color.
 * @return       true on success, false on parse error.
 */
bool r8e_ui_paint_parse_color(const char *str, R8EUIColor *out) {
    if (!str || !out) return false;

    str = r8e_ui_skip_ws(str);

    /* Hex colors */
    if (*str == '#') {
        str++;
        int len = 0;
        const char *p = str;
        while (r8e_ui_hex_digit(*p) >= 0) { p++; len++; }

        if (len == 3) {
            /* #RGB -> #RRGGBB */
            int r = r8e_ui_hex_digit(str[0]);
            int g = r8e_ui_hex_digit(str[1]);
            int b = r8e_ui_hex_digit(str[2]);
            *out = R8E_UI_COLOR_RGB((uint8_t)(r * 17), (uint8_t)(g * 17),
                                  (uint8_t)(b * 17));
            return true;
        }
        if (len == 4) {
            /* #RGBA -> #RRGGBBAA */
            int r = r8e_ui_hex_digit(str[0]);
            int g = r8e_ui_hex_digit(str[1]);
            int b = r8e_ui_hex_digit(str[2]);
            int a = r8e_ui_hex_digit(str[3]);
            *out = R8E_UI_COLOR_RGBA((uint8_t)(r * 17), (uint8_t)(g * 17),
                                   (uint8_t)(b * 17), (uint8_t)(a * 17));
            return true;
        }
        if (len == 6) {
            /* #RRGGBB */
            int r = r8e_ui_hex_digit(str[0]) * 16 + r8e_ui_hex_digit(str[1]);
            int g = r8e_ui_hex_digit(str[2]) * 16 + r8e_ui_hex_digit(str[3]);
            int b = r8e_ui_hex_digit(str[4]) * 16 + r8e_ui_hex_digit(str[5]);
            *out = R8E_UI_COLOR_RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
            return true;
        }
        if (len == 8) {
            /* #RRGGBBAA */
            int r = r8e_ui_hex_digit(str[0]) * 16 + r8e_ui_hex_digit(str[1]);
            int g = r8e_ui_hex_digit(str[2]) * 16 + r8e_ui_hex_digit(str[3]);
            int b = r8e_ui_hex_digit(str[4]) * 16 + r8e_ui_hex_digit(str[5]);
            int a = r8e_ui_hex_digit(str[6]) * 16 + r8e_ui_hex_digit(str[7]);
            *out = R8E_UI_COLOR_RGBA((uint8_t)r, (uint8_t)g, (uint8_t)b,
                                   (uint8_t)a);
            return true;
        }
        return false;
    }

    /* rgb() / rgba() */
    if ((str[0] == 'r' || str[0] == 'R') &&
        (str[1] == 'g' || str[1] == 'G') &&
        (str[2] == 'b' || str[2] == 'B')) {
        const char *p = str + 3;
        bool has_alpha = false;
        if (*p == 'a' || *p == 'A') {
            has_alpha = true;
            p++;
        }
        p = r8e_ui_skip_ws(p);
        if (*p != '(') return false;
        p++;

        int r = r8e_ui_parse_int(&p);
        p = r8e_ui_skip_ws(p);
        if (*p == ',') p++;

        int g = r8e_ui_parse_int(&p);
        p = r8e_ui_skip_ws(p);
        if (*p == ',') p++;

        int b = r8e_ui_parse_int(&p);

        float a = 1.0f;
        if (has_alpha) {
            p = r8e_ui_skip_ws(p);
            if (*p == ',') p++;
            a = r8e_ui_parse_float(&p);
        }

        p = r8e_ui_skip_ws(p);
        if (*p != ')') return false;

        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;

        *out = R8E_UI_COLOR_RGBA((uint8_t)r, (uint8_t)g, (uint8_t)b,
                               (uint8_t)(a * 255.0f));
        return true;
    }

    /* Named colors */
    for (int i = 0; r8e_ui_named_colors[i].name; i++) {
        if (r8e_ui_streqi(str, r8e_ui_named_colors[i].name)) {
            *out = r8e_ui_named_colors[i].color;
            return true;
        }
    }

    return false;
}


/* =========================================================================
 * Color Manipulation Helpers
 * ========================================================================= */

/*
 * Premultiply alpha for a color.
 */
static R8EUIColor r8e_ui_color_premultiply(R8EUIColor c) {
    uint8_t a = R8E_UI_COLOR_A(c);
    if (a == 255) return c;
    if (a == 0) return R8E_UI_COLOR_TRANSPARENT;

    uint8_t r = (uint8_t)((R8E_UI_COLOR_R(c) * a + 127) / 255);
    uint8_t g = (uint8_t)((R8E_UI_COLOR_G(c) * a + 127) / 255);
    uint8_t b = (uint8_t)((R8E_UI_COLOR_B(c) * a + 127) / 255);
    return R8E_UI_COLOR_RGBA(r, g, b, a);
}

/*
 * Apply opacity to a color.
 */
static R8EUIColor r8e_ui_color_with_opacity(R8EUIColor c, float opacity) {
    if (opacity >= 1.0f) return c;
    if (opacity <= 0.0f) return R8E_UI_COLOR_TRANSPARENT;

    uint8_t a = R8E_UI_COLOR_A(c);
    uint8_t new_a = (uint8_t)((float)a * opacity);
    return (c & 0xFFFFFF00U) | new_a;
}

/*
 * Blend source over destination (Porter-Duff src-over).
 * Both colors should be premultiplied.
 */
static uint32_t r8e_ui_blend_over(uint32_t dst, uint32_t src) {
    uint8_t sa = src & 0xFF;
    if (sa == 255) return src;
    if (sa == 0) return dst;

    uint32_t inv_sa = 255 - sa;

    uint8_t dr = (uint8_t)(dst >> 24);
    uint8_t dg = (uint8_t)(dst >> 16);
    uint8_t db = (uint8_t)(dst >> 8);
    uint8_t da = (uint8_t)(dst);

    uint8_t sr = (uint8_t)(src >> 24);
    uint8_t sg = (uint8_t)(src >> 16);
    uint8_t sb = (uint8_t)(src >> 8);

    uint8_t or_ = (uint8_t)(sr + ((dr * inv_sa + 127) / 255));
    uint8_t og  = (uint8_t)(sg + ((dg * inv_sa + 127) / 255));
    uint8_t ob  = (uint8_t)(sb + ((db * inv_sa + 127) / 255));
    uint8_t oa  = (uint8_t)(sa + ((da * inv_sa + 127) / 255));

    return ((uint32_t)or_ << 24) | ((uint32_t)og << 16) |
           ((uint32_t)ob << 8) | oa;
}


/* =========================================================================
 * Display List Generation
 *
 * Walk the DOM/layout tree and emit paint commands.
 * ========================================================================= */

/*
 * Emit a filled rectangle command.
 */
void r8e_ui_paint_emit_rect(R8EUIDisplayList *dl,
                          float x, float y, float w, float h,
                          R8EUIColor color) {
    if (R8E_UI_COLOR_A(color) == 0) return;  /* fully transparent: skip */
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_RECT;
    op->x = x; op->y = y; op->w = w; op->h = h;
    op->color = color;
}

/*
 * Emit a rounded rectangle command.
 */
void r8e_ui_paint_emit_rounded_rect(R8EUIDisplayList *dl,
                                  float x, float y, float w, float h,
                                  R8EUIColor color,
                                  float r_tl, float r_tr,
                                  float r_br, float r_bl) {
    if (R8E_UI_COLOR_A(color) == 0) return;
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_ROUNDED_RECT;
    op->x = x; op->y = y; op->w = w; op->h = h;
    op->color = color;
    op->radius[0] = r_tl;
    op->radius[1] = r_tr;
    op->radius[2] = r_br;
    op->radius[3] = r_bl;
}

/*
 * Emit a text draw command.
 */
void r8e_ui_paint_emit_text(R8EUIDisplayList *dl,
                          float x, float y, float w, float h,
                          const char *text, uint32_t text_len,
                          R8EUIColor color, float font_size) {
    if (!text || text_len == 0) return;
    if (R8E_UI_COLOR_A(color) == 0) return;
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_TEXT;
    op->x = x; op->y = y; op->w = w; op->h = h;
    op->text = text;
    op->text_len = text_len;
    op->text_color = color;
    op->font_size = font_size;
}

/*
 * Emit a border draw command.
 */
void r8e_ui_paint_emit_border(R8EUIDisplayList *dl,
                            float x, float y, float w, float h,
                            const float widths[4],
                            const R8EUIColor colors[4]) {
    /* Check if any border is visible */
    bool any_visible = false;
    for (int i = 0; i < 4; i++) {
        if (widths[i] > 0.0f && R8E_UI_COLOR_A(colors[i]) > 0) {
            any_visible = true;
            break;
        }
    }
    if (!any_visible) return;

    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_BORDER;
    op->x = x; op->y = y; op->w = w; op->h = h;
    memcpy(op->border_widths, widths, sizeof(float) * 4);
    memcpy(op->border_colors, colors, sizeof(R8EUIColor) * 4);
}

/*
 * Emit a clip push command.
 */
void r8e_ui_paint_emit_clip_push(R8EUIDisplayList *dl,
                               float x, float y, float w, float h) {
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_CLIP_PUSH;
    op->x = x; op->y = y; op->w = w; op->h = h;
}

/*
 * Emit a clip pop command.
 */
void r8e_ui_paint_emit_clip_pop(R8EUIDisplayList *dl) {
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_CLIP_POP;
}

/*
 * Emit an opacity push command.
 */
void r8e_ui_paint_emit_opacity(R8EUIDisplayList *dl, float opacity) {
    if (opacity >= 1.0f) return;  /* no-op at full opacity */
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_OPACITY;
    op->opacity = opacity;
}

/*
 * Emit an image draw command.
 */
void r8e_ui_paint_emit_image(R8EUIDisplayList *dl,
                           float x, float y, float w, float h,
                           uint32_t image_id) {
    R8EUIPaintOp *op = r8e_ui_paint_dl_push(dl);
    if (!op) return;
    op->cmd = R8E_UI_CMD_IMAGE;
    op->x = x; op->y = y; op->w = w; op->h = h;
    op->image_id = image_id;
}


/* =========================================================================
 * Tree Painting
 *
 * Walk the DOM tree and generate paint operations for each node.
 * ========================================================================= */

/*
 * Paint a single node (background, border, text) into the display list.
 * Does NOT recurse into children.
 */
static void r8e_ui_paint_node(R8EUIDisplayList *dl,
                            const R8EUIDOMNode_Paint *node,
                            float offset_x, float offset_y) {
    if (!node) return;

    const R8EUILayoutBox *box = &node->layout;
    const R8EUIPaintStyle *ps = &node->paint_style;

    /* Skip hidden nodes */
    if (ps->visibility != 0) return;

    /* Compute absolute position */
    float abs_x = offset_x + box->x;
    float abs_y = offset_y + box->y;

    /* Border box dimensions */
    float border_x = abs_x - box->padding[3] - box->border[3]; /* left */
    float border_y = abs_y - box->padding[0] - box->border[0]; /* top */
    float border_w = box->width + box->padding[1] + box->padding[3] +
                     box->border[1] + box->border[3];
    float border_h = box->height + box->padding[0] + box->padding[2] +
                     box->border[0] + box->border[2];

    /* Box shadow (rendered before background) */
    if (ps->has_shadow && R8E_UI_COLOR_A(ps->shadow_color) > 0) {
        float sx = border_x + ps->shadow_offset_x - ps->shadow_blur;
        float sy = border_y + ps->shadow_offset_y - ps->shadow_blur;
        float sw = border_w + ps->shadow_blur * 2.0f;
        float sh = border_h + ps->shadow_blur * 2.0f;
        r8e_ui_paint_emit_rect(dl, sx, sy, sw, sh, ps->shadow_color);
    }

    /* Background */
    bool has_radius = (ps->border_radius[0] > 0.0f ||
                       ps->border_radius[1] > 0.0f ||
                       ps->border_radius[2] > 0.0f ||
                       ps->border_radius[3] > 0.0f);

    if (R8E_UI_COLOR_A(ps->background_color) > 0) {
        if (has_radius) {
            r8e_ui_paint_emit_rounded_rect(dl, border_x, border_y,
                                         border_w, border_h,
                                         ps->background_color,
                                         ps->border_radius[0],
                                         ps->border_radius[1],
                                         ps->border_radius[2],
                                         ps->border_radius[3]);
        } else {
            r8e_ui_paint_emit_rect(dl, border_x, border_y,
                                 border_w, border_h,
                                 ps->background_color);
        }
    }

    /* Border */
    r8e_ui_paint_emit_border(dl, border_x, border_y, border_w, border_h,
                           ps->border_width, ps->border_color);

    /* Text content */
    if (node->type == R8E_UI_NODE_TEXT_P && node->text && node->text_len > 0) {
        r8e_ui_paint_emit_text(dl, abs_x, abs_y,
                             box->width, box->height,
                             node->text, node->text_len,
                             ps->color, ps->font_size);
    }
}

/*
 * Recursively paint a DOM subtree into the display list.
 *
 * @param dl         Display list to append operations to.
 * @param node       Current node to paint.
 * @param offset_x   Accumulated x offset from ancestors.
 * @param offset_y   Accumulated y offset from ancestors.
 */
void r8e_ui_paint_tree(R8EUIDisplayList *dl, const R8EUIDOMNode_Paint *node,
                     float offset_x, float offset_y) {
    if (!node) return;

    const R8EUIPaintStyle *ps = &node->paint_style;
    const R8EUILayoutBox *box = &node->layout;

    /* Handle opacity */
    bool pushed_opacity = false;
    if (ps->opacity < 1.0f && ps->opacity > 0.0f) {
        r8e_ui_paint_emit_opacity(dl, ps->opacity);
        pushed_opacity = true;
    }
    if (ps->opacity <= 0.0f) return;

    /* Handle overflow clipping */
    bool pushed_clip = false;
    if (ps->overflow == 1 || ps->overflow == 2) {
        float abs_x = offset_x + box->x;
        float abs_y = offset_y + box->y;
        float clip_x = abs_x - box->padding[3];
        float clip_y = abs_y - box->padding[0];
        float clip_w = box->width + box->padding[1] + box->padding[3];
        float clip_h = box->height + box->padding[0] + box->padding[2];
        r8e_ui_paint_emit_clip_push(dl, clip_x, clip_y, clip_w, clip_h);
        pushed_clip = true;
    }

    /* Paint this node */
    r8e_ui_paint_node(dl, node, offset_x, offset_y);

    /* Paint children (account for scroll offset) */
    float child_offset_x = offset_x + box->x - node->scroll_x;
    float child_offset_y = offset_y + box->y - node->scroll_y;

    const R8EUIDOMNode_Paint *child = node->first_child;
    while (child) {
        r8e_ui_paint_tree(dl, child, child_offset_x, child_offset_y);
        child = child->next_sibling;
    }

    /* Pop clip */
    if (pushed_clip) {
        r8e_ui_paint_emit_clip_pop(dl);
    }

    /* Note: opacity pop is handled by the renderer looking at paired ops */
    (void)pushed_opacity;
}

/*
 * Generate a display list for an entire DOM tree.
 *
 * @param dl           Display list to fill (should be cleared first).
 * @param root         Root node of the DOM tree.
 * @param viewport_w   Viewport width (for background clear).
 * @param viewport_h   Viewport height.
 * @param bg_color     Viewport background color.
 */
void r8e_ui_paint_generate(R8EUIDisplayList *dl, const R8EUIDOMNode_Paint *root,
                         float viewport_w, float viewport_h,
                         R8EUIColor bg_color) {
    if (!dl) return;

    r8e_ui_paint_dl_clear(dl);

    /* Viewport background */
    if (R8E_UI_COLOR_A(bg_color) > 0) {
        r8e_ui_paint_emit_rect(dl, 0.0f, 0.0f, viewport_w, viewport_h,
                             bg_color);
    }

    /* Paint the tree */
    if (root) {
        r8e_ui_paint_tree(dl, root, 0.0f, 0.0f);
    }
}


/* =========================================================================
 * Software Rasterizer
 *
 * Direct pixel buffer rendering for headless/embedded environments.
 * Implements basic 2D drawing primitives:
 *   - Filled rectangle
 *   - Bordered rectangle (separate edge colors)
 *   - Text placeholder (draws a colored rectangle)
 *   - Clip stack
 * ========================================================================= */

/* Clip stack for software rasterizer */
#define R8E_UI_SW_MAX_CLIP_STACK 32

typedef struct {
    float x, y, w, h;
} R8EUIClipRect;

typedef struct {
    R8EUIFramebuffer *fb;
    R8EUIClipRect     clip_stack[R8E_UI_SW_MAX_CLIP_STACK];
    int              clip_depth;
    float            opacity_stack[R8E_UI_SW_MAX_CLIP_STACK];
    int              opacity_depth;
} R8EUISWContext;

/*
 * Initialize the software rendering context.
 */
void r8e_ui_paint_sw_init(R8EUISWContext *ctx, R8EUIFramebuffer *fb) {
    if (!ctx || !fb) return;
    memset(ctx, 0, sizeof(R8EUISWContext));
    ctx->fb = fb;

    /* Initial clip is the full framebuffer */
    ctx->clip_stack[0].x = 0.0f;
    ctx->clip_stack[0].y = 0.0f;
    ctx->clip_stack[0].w = (float)fb->width;
    ctx->clip_stack[0].h = (float)fb->height;
    ctx->clip_depth = 1;

    ctx->opacity_stack[0] = 1.0f;
    ctx->opacity_depth = 1;
}

/*
 * Get the current clip rectangle.
 */
static R8EUIClipRect r8e_ui_sw_current_clip(const R8EUISWContext *ctx) {
    if (ctx->clip_depth > 0) {
        return ctx->clip_stack[ctx->clip_depth - 1];
    }
    R8EUIClipRect r = { 0.0f, 0.0f, (float)ctx->fb->width,
                       (float)ctx->fb->height };
    return r;
}

/*
 * Get the current opacity.
 */
static float r8e_ui_sw_current_opacity(const R8EUISWContext *ctx) {
    if (ctx->opacity_depth > 0) {
        return ctx->opacity_stack[ctx->opacity_depth - 1];
    }
    return 1.0f;
}

/*
 * Push a clip rectangle (intersected with current clip).
 */
static void r8e_ui_sw_clip_push(R8EUISWContext *ctx, float x, float y,
                              float w, float h) {
    if (ctx->clip_depth >= R8E_UI_SW_MAX_CLIP_STACK) return;

    R8EUIClipRect current = r8e_ui_sw_current_clip(ctx);

    /* Intersect with current clip */
    float nx = x > current.x ? x : current.x;
    float ny = y > current.y ? y : current.y;
    float nx2 = (x + w) < (current.x + current.w) ?
                (x + w) : (current.x + current.w);
    float ny2 = (y + h) < (current.y + current.h) ?
                (y + h) : (current.y + current.h);

    R8EUIClipRect *r = &ctx->clip_stack[ctx->clip_depth];
    r->x = nx;
    r->y = ny;
    r->w = nx2 > nx ? nx2 - nx : 0.0f;
    r->h = ny2 > ny ? ny2 - ny : 0.0f;
    ctx->clip_depth++;
}

/*
 * Pop the clip rectangle.
 */
static void r8e_ui_sw_clip_pop(R8EUISWContext *ctx) {
    if (ctx->clip_depth > 1) {
        ctx->clip_depth--;
    }
}

/*
 * Push an opacity level (multiplied with current).
 */
static void r8e_ui_sw_opacity_push(R8EUISWContext *ctx, float opacity) {
    if (ctx->opacity_depth >= R8E_UI_SW_MAX_CLIP_STACK) return;
    float current = r8e_ui_sw_current_opacity(ctx);
    ctx->opacity_stack[ctx->opacity_depth] = current * opacity;
    ctx->opacity_depth++;
}

/*
 * Pop an opacity level.
 */
static void r8e_ui_sw_opacity_pop(R8EUISWContext *ctx) {
    if (ctx->opacity_depth > 1) {
        ctx->opacity_depth--;
    }
}

/*
 * Draw a filled rectangle with the current clip and opacity.
 */
static void r8e_ui_sw_fill_rect(R8EUISWContext *ctx, float x, float y,
                              float w, float h, R8EUIColor color) {
    R8EUIFramebuffer *fb = ctx->fb;
    if (!fb || !fb->pixels) return;

    float opacity = r8e_ui_sw_current_opacity(ctx);
    color = r8e_ui_color_with_opacity(color, opacity);
    if (R8E_UI_COLOR_A(color) == 0) return;

    R8EUIClipRect clip = r8e_ui_sw_current_clip(ctx);

    /* Clamp to clip rect */
    float x1 = x > clip.x ? x : clip.x;
    float y1 = y > clip.y ? y : clip.y;
    float x2 = (x + w) < (clip.x + clip.w) ? (x + w) : (clip.x + clip.w);
    float y2 = (y + h) < (clip.y + clip.h) ? (y + h) : (clip.y + clip.h);

    /* Clamp to framebuffer */
    int ix1 = (int)x1; if (ix1 < 0) ix1 = 0;
    int iy1 = (int)y1; if (iy1 < 0) iy1 = 0;
    int ix2 = (int)x2; if (ix2 > fb->width) ix2 = fb->width;
    int iy2 = (int)y2; if (iy2 > fb->height) iy2 = fb->height;

    if (ix1 >= ix2 || iy1 >= iy2) return;

    uint32_t src = r8e_ui_color_premultiply(color);
    int stride_px = fb->stride / 4;

    if (R8E_UI_COLOR_A(color) == 255) {
        /* Opaque: direct write */
        for (int py = iy1; py < iy2; py++) {
            uint32_t *row = fb->pixels + py * stride_px;
            for (int px = ix1; px < ix2; px++) {
                row[px] = src;
            }
        }
    } else {
        /* Alpha blend */
        for (int py = iy1; py < iy2; py++) {
            uint32_t *row = fb->pixels + py * stride_px;
            for (int px = ix1; px < ix2; px++) {
                row[px] = r8e_ui_blend_over(row[px], src);
            }
        }
    }
}

/*
 * Draw a bordered rectangle.
 * Draws four separate edge rectangles.
 */
static void r8e_ui_sw_draw_border(R8EUISWContext *ctx, float x, float y,
                                float w, float h,
                                const float widths[4],
                                const R8EUIColor colors[4]) {
    /* Top border */
    if (widths[0] > 0.0f && R8E_UI_COLOR_A(colors[0]) > 0) {
        r8e_ui_sw_fill_rect(ctx, x, y, w, widths[0], colors[0]);
    }
    /* Right border */
    if (widths[1] > 0.0f && R8E_UI_COLOR_A(colors[1]) > 0) {
        r8e_ui_sw_fill_rect(ctx, x + w - widths[1], y + widths[0],
                          widths[1], h - widths[0] - widths[2], colors[1]);
    }
    /* Bottom border */
    if (widths[2] > 0.0f && R8E_UI_COLOR_A(colors[2]) > 0) {
        r8e_ui_sw_fill_rect(ctx, x, y + h - widths[2], w, widths[2], colors[2]);
    }
    /* Left border */
    if (widths[3] > 0.0f && R8E_UI_COLOR_A(colors[3]) > 0) {
        r8e_ui_sw_fill_rect(ctx, x, y + widths[0],
                          widths[3], h - widths[0] - widths[2], colors[3]);
    }
}

/*
 * Draw text placeholder (draws a colored rectangle where text would be).
 * Real text rendering requires a font rasterizer (e.g., stb_truetype).
 */
static void r8e_ui_sw_draw_text(R8EUISWContext *ctx, float x, float y,
                              float w, float h, R8EUIColor color) {
    /* Placeholder: draw a semi-transparent rectangle to indicate text area */
    R8EUIColor text_bg = R8E_UI_COLOR_RGBA(R8E_UI_COLOR_R(color),
                                        R8E_UI_COLOR_G(color),
                                        R8E_UI_COLOR_B(color), 40);
    r8e_ui_sw_fill_rect(ctx, x, y, w, h, text_bg);
}

/*
 * Execute the display list using the software rasterizer.
 *
 * @param ctx   Software rendering context.
 * @param dl    Display list to render.
 */
void r8e_ui_paint_sw_render(R8EUISWContext *ctx, const R8EUIDisplayList *dl) {
    if (!ctx || !dl) return;

    for (uint32_t i = 0; i < dl->count; i++) {
        const R8EUIPaintOp *op = &dl->ops[i];

        switch (op->cmd) {
        case R8E_UI_CMD_RECT:
            r8e_ui_sw_fill_rect(ctx, op->x, op->y, op->w, op->h, op->color);
            break;

        case R8E_UI_CMD_ROUNDED_RECT:
            /* Simplified: just draw a regular rect for the software path.
             * Real rounded rect needs per-pixel distance checks. */
            r8e_ui_sw_fill_rect(ctx, op->x, op->y, op->w, op->h, op->color);
            break;

        case R8E_UI_CMD_TEXT:
            r8e_ui_sw_draw_text(ctx, op->x, op->y, op->w, op->h,
                              op->text_color);
            break;

        case R8E_UI_CMD_BORDER:
            r8e_ui_sw_draw_border(ctx, op->x, op->y, op->w, op->h,
                                op->border_widths, op->border_colors);
            break;

        case R8E_UI_CMD_CLIP_PUSH:
            r8e_ui_sw_clip_push(ctx, op->x, op->y, op->w, op->h);
            break;

        case R8E_UI_CMD_CLIP_POP:
            r8e_ui_sw_clip_pop(ctx);
            break;

        case R8E_UI_CMD_OPACITY:
            r8e_ui_sw_opacity_push(ctx, op->opacity);
            break;

        case R8E_UI_CMD_IMAGE:
            /* Image rendering requires an image store; placeholder rect */
            r8e_ui_sw_fill_rect(ctx, op->x, op->y, op->w, op->h,
                              R8E_UI_COLOR_RGB(200, 200, 200));
            break;
        }
    }
}


/* =========================================================================
 * Framebuffer Management
 * ========================================================================= */

/*
 * Create a new framebuffer.
 *
 * @param width   Width in pixels.
 * @param height  Height in pixels.
 * @return        New framebuffer, or NULL on allocation failure.
 */
R8EUIFramebuffer *r8e_ui_paint_fb_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;

    R8EUIFramebuffer *fb = (R8EUIFramebuffer *)malloc(sizeof(R8EUIFramebuffer));
    if (!fb) return NULL;

    fb->width = width;
    fb->height = height;
    fb->stride = width * 4;
    fb->pixels = (uint32_t *)calloc((size_t)(width * height),
                                    sizeof(uint32_t));
    if (!fb->pixels) {
        free(fb);
        return NULL;
    }

    return fb;
}

/*
 * Destroy a framebuffer and free its memory.
 */
void r8e_ui_paint_fb_destroy(R8EUIFramebuffer *fb) {
    if (!fb) return;
    free(fb->pixels);
    free(fb);
}

/*
 * Clear a framebuffer to a solid color.
 */
void r8e_ui_paint_fb_clear(R8EUIFramebuffer *fb, R8EUIColor color) {
    if (!fb || !fb->pixels) return;

    uint32_t c = r8e_ui_color_premultiply(color);
    int total = fb->width * fb->height;

    if (c == 0) {
        memset(fb->pixels, 0, (size_t)(total) * sizeof(uint32_t));
    } else {
        for (int i = 0; i < total; i++) {
            fb->pixels[i] = c;
        }
    }
}

/*
 * Resize a framebuffer. Existing content is lost.
 *
 * @param fb      Framebuffer to resize.
 * @param width   New width.
 * @param height  New height.
 * @return        true on success, false on allocation failure.
 */
bool r8e_ui_paint_fb_resize(R8EUIFramebuffer *fb, int width, int height) {
    if (!fb || width <= 0 || height <= 0) return false;

    uint32_t *new_pixels = (uint32_t *)calloc((size_t)(width * height),
                                              sizeof(uint32_t));
    if (!new_pixels) return false;

    free(fb->pixels);
    fb->pixels = new_pixels;
    fb->width = width;
    fb->height = height;
    fb->stride = width * 4;
    return true;
}

/*
 * Get a pixel value from the framebuffer.
 */
uint32_t r8e_ui_paint_fb_get_pixel(const R8EUIFramebuffer *fb, int x, int y) {
    if (!fb || !fb->pixels) return 0;
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return 0;
    return fb->pixels[y * (fb->stride / 4) + x];
}

/*
 * Set a pixel value in the framebuffer.
 */
void r8e_ui_paint_fb_set_pixel(R8EUIFramebuffer *fb, int x, int y, uint32_t color) {
    if (!fb || !fb->pixels) return;
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    fb->pixels[y * (fb->stride / 4) + x] = color;
}


/* =========================================================================
 * Full Render Pipeline
 * ========================================================================= */

/*
 * Full software render pipeline: generate display list, check cache,
 * render to framebuffer.
 *
 * @param dl           Display list (reused across frames).
 * @param fb           Target framebuffer.
 * @param root         DOM tree root.
 * @param bg_color     Background color.
 * @param cached_hash  Previous frame's display list hash (0 for first frame).
 * @return             The new display list hash (for next frame's cache check).
 */
uint64_t r8e_ui_paint_render_frame(R8EUIDisplayList *dl, R8EUIFramebuffer *fb,
                                 const R8EUIDOMNode_Paint *root,
                                 R8EUIColor bg_color, uint64_t cached_hash) {
    if (!dl || !fb) return 0;

    /* Generate display list */
    r8e_ui_paint_generate(dl, root,
                        (float)fb->width, (float)fb->height, bg_color);

    /* Compute hash */
    uint64_t new_hash = r8e_ui_paint_dl_hash(dl);

    /* Check cache: skip render if unchanged */
    if (cached_hash != 0 && new_hash == cached_hash) {
        return new_hash;
    }

    /* Render */
    r8e_ui_paint_fb_clear(fb, R8E_UI_COLOR_TRANSPARENT);

    R8EUISWContext sw_ctx;
    r8e_ui_paint_sw_init(&sw_ctx, fb);
    r8e_ui_paint_sw_render(&sw_ctx, dl);

    return new_hash;
}


/* =========================================================================
 * Paint Style Initialization
 * ========================================================================= */

/*
 * Initialize a paint style to defaults.
 */
void r8e_ui_paint_style_init(R8EUIPaintStyle *style) {
    if (!style) return;
    memset(style, 0, sizeof(R8EUIPaintStyle));
    style->background_color = R8E_UI_COLOR_TRANSPARENT;
    style->color = R8E_UI_COLOR_BLACK;
    style->opacity = 1.0f;
    style->font_size = 16.0f;
    style->line_height = 1.2f;
    style->visibility = 0;  /* visible */
    style->overflow = 0;    /* visible */
    for (int i = 0; i < 4; i++) {
        style->border_color[i] = R8E_UI_COLOR_BLACK;
    }
}


/* =========================================================================
 * Debug: Display List Dump
 * ========================================================================= */

/*
 * Dump the display list to a buffer for debugging.
 *
 * @param dl     Display list.
 * @param buf    Output buffer.
 * @param bufsz  Buffer size.
 * @return       Number of characters written.
 */
int r8e_ui_paint_dl_dump(const R8EUIDisplayList *dl, char *buf, int bufsz) {
    if (!dl || !buf || bufsz <= 0) return 0;

    int written = 0;
    int remain = bufsz - 1;

    static const char *cmd_names[] = {
        "RECT", "ROUNDED_RECT", "TEXT", "BORDER",
        "CLIP_PUSH", "CLIP_POP", "OPACITY", "IMAGE"
    };

    for (uint32_t i = 0; i < dl->count && remain > 0; i++) {
        const R8EUIPaintOp *op = &dl->ops[i];
        int n = 0;

        const char *name = (op->cmd <= R8E_UI_CMD_IMAGE) ?
                           cmd_names[op->cmd] : "UNKNOWN";

        switch (op->cmd) {
        case R8E_UI_CMD_RECT:
        case R8E_UI_CMD_ROUNDED_RECT:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s (%.1f,%.1f %.1fx%.1f) color=#%08X\n",
                         i, name, op->x, op->y, op->w, op->h, op->color);
            break;

        case R8E_UI_CMD_TEXT:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s (%.1f,%.1f %.1fx%.1f) len=%u size=%.1f\n",
                         i, name, op->x, op->y, op->w, op->h,
                         op->text_len, op->font_size);
            break;

        case R8E_UI_CMD_BORDER:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s (%.1f,%.1f %.1fx%.1f) "
                         "w=[%.1f,%.1f,%.1f,%.1f]\n",
                         i, name, op->x, op->y, op->w, op->h,
                         op->border_widths[0], op->border_widths[1],
                         op->border_widths[2], op->border_widths[3]);
            break;

        case R8E_UI_CMD_CLIP_PUSH:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s (%.1f,%.1f %.1fx%.1f)\n",
                         i, name, op->x, op->y, op->w, op->h);
            break;

        case R8E_UI_CMD_CLIP_POP:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s\n", i, name);
            break;

        case R8E_UI_CMD_OPACITY:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s opacity=%.2f\n", i, name, op->opacity);
            break;

        case R8E_UI_CMD_IMAGE:
            n = snprintf(buf + written, (size_t)remain,
                         "[%u] %s (%.1f,%.1f %.1fx%.1f) id=%u\n",
                         i, name, op->x, op->y, op->w, op->h,
                         op->image_id);
            break;
        }

        if (n > 0) {
            if (n > remain) n = remain;
            written += n;
            remain -= n;
        }
    }

    buf[written] = '\0';
    return written;
}
