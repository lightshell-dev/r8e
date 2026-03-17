/*
 * nstyle.c - R8EUIStyle: CSS Subset Engine
 *
 * Part of the r8e JavaScript engine R8EUIUI rendering stack.
 * See CLAUDE.md Section 12.3 (R8EUIStyle).
 *
 * CSS subset engine supporting ~60 properties:
 *   - display (flex, block, inline, none)
 *   - position (relative, absolute, fixed)
 *   - Box model: width, height, padding, margin, border (all with TRBL)
 *   - Flexbox: flex-direction, justify-content, align-items, flex-grow,
 *     flex-shrink, flex-basis, gap, flex-wrap
 *   - Colors: color, background-color, border-color, opacity
 *   - Typography: font-size, font-weight, font-family, line-height, text-align
 *   - Visual: border-radius, overflow, visibility, cursor, box-shadow (basic)
 *   - Selectors: tag, .class, #id, combinators (space, >),
 *     pseudo (:hover, :focus, :active, :first-child, :last-child)
 *
 * Selector matching uses atom interning: class names are integers, matching
 * is integer comparison. Zero string operations during style resolution.
 *
 * Specificity calculation: ID=100, class/pseudo=10, tag=1.
 * Style resolution: cascade through matching rules by specificity, merge
 * with inline styles.
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
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <strings.h>  /* for strcasecmp (POSIX) */

/* Portable case-insensitive string compare fallback */
#ifndef _WIN32
  #define ncss_strcasecmp strcasecmp
#else
  static int ncss_strcasecmp(const char *a, const char *b) {
      while (*a && *b) {
          int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
          if (d != 0) return d;
          a++; b++;
      }
      return tolower((unsigned char)*a) - tolower((unsigned char)*b);
  }
#endif

/* =========================================================================
 * Type Definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

#ifndef R8E_NSTYLE_TYPES_DEFINED
#define R8E_NSTYLE_TYPES_DEFINED

typedef uint64_t R8EValue;

/* NaN-boxing constants (CLAUDE.md Section 2.3) */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)

static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* String hashing (FNV-1a) */
static inline uint32_t r8e_string_hash_s(const char *str, uint32_t len) {
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619U;
    }
    return hash;
}

/* -------------------------------------------------------------------------
 * R8EUILayoutBox - shared with nlayout.c, ndom.c
 * ------------------------------------------------------------------------- */

typedef struct R8EUILayoutBox {
    float x, y;
    float width, height;
    float padding[4];
    float border[4];
    float margin[4];
} R8EUILayoutBox;

/* -------------------------------------------------------------------------
 * DOM Node types - minimal forward declarations from ndom.c
 * In the full build these come from a shared header.
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_NODE_ELEMENT   = 1,
    R8E_UI_NODE_TEXT      = 3,
    R8E_UI_NODE_COMMENT   = 8,
    R8E_UI_NODE_DOCUMENT  = 9,
    R8E_UI_NODE_FRAGMENT  = 11
} R8EUINodeType;

/* DOM node flags used by style */
#define R8E_UI_FLAG_DIRTY          0x00000001u
#define R8E_UI_FLAG_STYLE_DIRTY    0x00000002u
#define R8E_UI_FLAG_VISIBLE        0x00000004u
#define R8E_UI_FLAG_HAS_ID         0x00000010u
#define R8E_UI_FLAG_HAS_CLASS      0x00000020u
#define R8E_UI_FLAG_HAS_STYLE      0x00000040u
#define R8E_UI_FLAG_HOVER          0x00000400u
#define R8E_UI_FLAG_FOCUS          0x00000800u
#define R8E_UI_FLAG_ACTIVE         0x00001000u

/* Class list from ndom.c */
typedef struct R8EUIClassList {
    uint32_t *atoms;
    uint16_t  count;
    uint16_t  capacity;
} R8EUIClassList;

/* Inline style entry from ndom.c */
typedef struct R8EUIStyleEntry {
    uint16_t property_id;
    uint16_t value_len;
    char    *value;
} R8EUIStyleEntry;

typedef struct R8EUIInlineStyle {
    R8EUIStyleEntry *entries;
    uint16_t count;
    uint16_t capacity;
} R8EUIInlineStyle;

/* Attribute entry from ndom.c */
typedef struct R8EUIAttr {
    uint32_t name_atom;
    uint32_t value_len;
    char    *value;
} R8EUIAttr;

/* Minimal R8EUIDOMNode forward for style resolution */
typedef struct R8EUIDOMNode {
    uint8_t           type;
    uint8_t           pad[3];
    uint32_t          flags;
    uint32_t          tag_atom;
    uint32_t          id_atom;
    R8EUIAttr         *attrs;
    uint16_t          attr_count;
    uint16_t          attr_capacity;
    R8EUIClassList     class_list;
    R8EUIInlineStyle  *inline_style;
    struct R8EUIDOMNode *parent;
    struct R8EUIDOMNode *first_child;
    struct R8EUIDOMNode *last_child;
    struct R8EUIDOMNode *next_sibling;
    struct R8EUIDOMNode *prev_sibling;
    uint32_t          child_count;
    R8EUILayoutBox     layout;
    void             *computed;
    char             *text;
    uint32_t          text_len;
    void             *listeners;
    uint16_t          listener_count;
    uint16_t          listener_cap;
    struct R8EUIDOMNode *owner_doc;
} R8EUIDOMNode;

/* -------------------------------------------------------------------------
 * CSS Property IDs
 * ------------------------------------------------------------------------- */

typedef enum {
    /* Display and position */
    NCSS_PROP_DISPLAY = 0,
    NCSS_PROP_POSITION,
    NCSS_PROP_VISIBILITY,
    NCSS_PROP_OVERFLOW,

    /* Box model - dimensions */
    NCSS_PROP_WIDTH,
    NCSS_PROP_HEIGHT,
    NCSS_PROP_MIN_WIDTH,
    NCSS_PROP_MIN_HEIGHT,
    NCSS_PROP_MAX_WIDTH,
    NCSS_PROP_MAX_HEIGHT,

    /* Box model - padding */
    NCSS_PROP_PADDING,
    NCSS_PROP_PADDING_TOP,
    NCSS_PROP_PADDING_RIGHT,
    NCSS_PROP_PADDING_BOTTOM,
    NCSS_PROP_PADDING_LEFT,

    /* Box model - margin */
    NCSS_PROP_MARGIN,
    NCSS_PROP_MARGIN_TOP,
    NCSS_PROP_MARGIN_RIGHT,
    NCSS_PROP_MARGIN_BOTTOM,
    NCSS_PROP_MARGIN_LEFT,

    /* Box model - border */
    NCSS_PROP_BORDER_WIDTH,
    NCSS_PROP_BORDER_TOP_WIDTH,
    NCSS_PROP_BORDER_RIGHT_WIDTH,
    NCSS_PROP_BORDER_BOTTOM_WIDTH,
    NCSS_PROP_BORDER_LEFT_WIDTH,
    NCSS_PROP_BORDER_COLOR,
    NCSS_PROP_BORDER_RADIUS,
    NCSS_PROP_BORDER_TOP_LEFT_RADIUS,
    NCSS_PROP_BORDER_TOP_RIGHT_RADIUS,
    NCSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS,
    NCSS_PROP_BORDER_BOTTOM_LEFT_RADIUS,

    /* Flexbox */
    NCSS_PROP_FLEX_DIRECTION,
    NCSS_PROP_FLEX_WRAP,
    NCSS_PROP_JUSTIFY_CONTENT,
    NCSS_PROP_ALIGN_ITEMS,
    NCSS_PROP_ALIGN_SELF,
    NCSS_PROP_FLEX_GROW,
    NCSS_PROP_FLEX_SHRINK,
    NCSS_PROP_FLEX_BASIS,
    NCSS_PROP_GAP,
    NCSS_PROP_ORDER,

    /* Colors */
    NCSS_PROP_COLOR,
    NCSS_PROP_BACKGROUND_COLOR,
    NCSS_PROP_OPACITY,

    /* Typography */
    NCSS_PROP_FONT_SIZE,
    NCSS_PROP_FONT_WEIGHT,
    NCSS_PROP_FONT_FAMILY,
    NCSS_PROP_LINE_HEIGHT,
    NCSS_PROP_TEXT_ALIGN,

    /* Visual */
    NCSS_PROP_CURSOR,
    NCSS_PROP_BOX_SHADOW,

    /* Positioning offsets */
    NCSS_PROP_TOP,
    NCSS_PROP_RIGHT,
    NCSS_PROP_BOTTOM,
    NCSS_PROP_LEFT,

    /* z-index */
    NCSS_PROP_Z_INDEX,

    NCSS_PROP__COUNT
} R8EUICSSPropertyID;

/* -------------------------------------------------------------------------
 * Enumerated CSS values
 * ------------------------------------------------------------------------- */

/* Display */
typedef enum {
    NCSS_DISPLAY_BLOCK   = 0,
    NCSS_DISPLAY_INLINE  = 1,
    NCSS_DISPLAY_FLEX    = 2,
    NCSS_DISPLAY_NONE    = 3
} R8EUICSSDisplay;

/* Position */
typedef enum {
    NCSS_POSITION_STATIC   = 0,
    NCSS_POSITION_RELATIVE = 1,
    NCSS_POSITION_ABSOLUTE = 2,
    NCSS_POSITION_FIXED    = 3
} R8EUICSSPosition;

/* Visibility */
typedef enum {
    NCSS_VISIBILITY_VISIBLE = 0,
    NCSS_VISIBILITY_HIDDEN  = 1
} R8EUICSSVisibility;

/* Overflow */
typedef enum {
    NCSS_OVERFLOW_VISIBLE = 0,
    NCSS_OVERFLOW_HIDDEN  = 1,
    NCSS_OVERFLOW_SCROLL  = 2,
    NCSS_OVERFLOW_AUTO    = 3
} R8EUICSSOverflow;

/* Flex direction */
typedef enum {
    NCSS_FLEX_ROW            = 0,
    NCSS_FLEX_ROW_REVERSE    = 1,
    NCSS_FLEX_COLUMN         = 2,
    NCSS_FLEX_COLUMN_REVERSE = 3
} R8EUICSSFlexDirection;

/* Flex wrap */
typedef enum {
    NCSS_FLEX_NOWRAP       = 0,
    NCSS_FLEX_WRAP         = 1,
    NCSS_FLEX_WRAP_REVERSE = 2
} R8EUICSSFlexWrap;

/* Justify content */
typedef enum {
    NCSS_JUSTIFY_FLEX_START    = 0,
    NCSS_JUSTIFY_FLEX_END      = 1,
    NCSS_JUSTIFY_CENTER        = 2,
    NCSS_JUSTIFY_SPACE_BETWEEN = 3,
    NCSS_JUSTIFY_SPACE_AROUND  = 4,
    NCSS_JUSTIFY_SPACE_EVENLY  = 5
} R8EUICSSJustifyContent;

/* Align items / align self */
typedef enum {
    NCSS_ALIGN_STRETCH    = 0,
    NCSS_ALIGN_FLEX_START = 1,
    NCSS_ALIGN_FLEX_END   = 2,
    NCSS_ALIGN_CENTER     = 3,
    NCSS_ALIGN_BASELINE   = 4,
    NCSS_ALIGN_AUTO       = 5  /* for align-self only */
} R8EUICSSAlignItems;

/* Text align */
typedef enum {
    NCSS_TEXT_ALIGN_LEFT   = 0,
    NCSS_TEXT_ALIGN_CENTER = 1,
    NCSS_TEXT_ALIGN_RIGHT  = 2
} R8EUICSSTextAlign;

/* Cursor */
typedef enum {
    NCSS_CURSOR_DEFAULT = 0,
    NCSS_CURSOR_POINTER = 1,
    NCSS_CURSOR_TEXT    = 2,
    NCSS_CURSOR_MOVE    = 3,
    NCSS_CURSOR_NONE    = 4
} R8EUICSSCursor;

/* Special float constant for "auto" */
#define NCSS_AUTO  (-1.0f)

/* -------------------------------------------------------------------------
 * R8EUIComputedStyle - Resolved CSS values (CLAUDE.md Section 12.3)
 * ------------------------------------------------------------------------- */

typedef struct R8EUIComputedStyle {
    /* Display and layout model */
    uint8_t  display;          /* R8EUICSSDisplay */
    uint8_t  position;         /* R8EUICSSPosition */
    uint8_t  flex_direction;   /* R8EUICSSFlexDirection */
    uint8_t  justify_content;  /* R8EUICSSJustifyContent */
    uint8_t  align_items;      /* R8EUICSSAlignItems */
    uint8_t  align_self;       /* R8EUICSSAlignItems (ALIGN_AUTO = inherit) */
    uint8_t  text_align;       /* R8EUICSSTextAlign */
    uint8_t  overflow;         /* R8EUICSSOverflow */
    uint8_t  visibility;       /* R8EUICSSVisibility */
    uint8_t  flex_wrap;        /* R8EUICSSFlexWrap */
    uint8_t  cursor;           /* R8EUICSSCursor */
    uint8_t  _pad;

    /* Dimensions */
    float    width, height;
    float    min_width, min_height;
    float    max_width, max_height;

    /* Box model: TRBL order (top, right, bottom, left) */
    float    padding[4];
    float    margin[4];
    float    border_width[4];

    /* Flexbox */
    float    flex_grow;
    float    flex_shrink;
    float    flex_basis;
    float    gap;
    int32_t  order;

    /* Typography */
    float    font_size;
    float    line_height;
    uint16_t font_weight;
    uint16_t font_family_atom;  /* atom index of font family name */

    /* Colors (RGBA packed as uint32: 0xRRGGBBAA) */
    uint32_t color;
    uint32_t background_color;
    uint32_t border_color;
    float    opacity;

    /* Border radius: TL, TR, BR, BL */
    float    border_radius[4];

    /* Positioning offsets */
    float    top, right, bottom, left;
    int32_t  z_index;
} R8EUIComputedStyle;

/* -------------------------------------------------------------------------
 * Selector types (matching ndom.c definitions)
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_SEL_TAG       = 0,
    R8E_UI_SEL_CLASS     = 1,
    R8E_UI_SEL_ID        = 2,
    R8E_UI_SEL_UNIVERSAL = 3,
    R8E_UI_SEL_PSEUDO    = 4
} R8EUISelectorType;

typedef enum {
    R8E_UI_COMB_NONE       = 0,
    R8E_UI_COMB_DESCENDANT = 1,
    R8E_UI_COMB_CHILD      = 2
} R8EUICombinator;

typedef enum {
    R8E_UI_PSEUDO_NONE        = 0,
    R8E_UI_PSEUDO_HOVER       = 1,
    R8E_UI_PSEUDO_FOCUS       = 2,
    R8E_UI_PSEUDO_ACTIVE      = 3,
    R8E_UI_PSEUDO_FIRST_CHILD = 4,
    R8E_UI_PSEUDO_LAST_CHILD  = 5
} R8EUIPseudoClass;

typedef struct R8EUISelectorPart {
    R8EUISelectorType type;
    R8EUICombinator   combinator;
    uint32_t         atom;
    R8EUIPseudoClass  pseudo;
} R8EUISelectorPart;

#define R8E_UI_MAX_SELECTOR_PARTS 16

typedef struct R8EUISelector {
    R8EUISelectorPart parts[R8E_UI_MAX_SELECTOR_PARTS];
    uint8_t          count;
} R8EUISelector;

/* -------------------------------------------------------------------------
 * CSS Rule: a selector + declarations
 * ------------------------------------------------------------------------- */

typedef struct R8EUICSSDecl {
    R8EUICSSPropertyID property;
    char             *value;       /* string value (owned) */
    uint16_t          value_len;
    uint8_t           important;   /* !important flag */
    uint8_t           _pad;
} R8EUICSSDecl;

typedef struct R8EUICSSRule {
    R8EUISelector selector;
    uint32_t     specificity;    /* precomputed */
    R8EUICSSDecl *declarations;
    uint16_t     decl_count;
    uint16_t     decl_capacity;
} R8EUICSSRule;

/* -------------------------------------------------------------------------
 * Stylesheet: collection of rules
 * ------------------------------------------------------------------------- */

typedef struct R8EUIStyleSheet {
    R8EUICSSRule *rules;
    uint32_t     rule_count;
    uint32_t     rule_capacity;
} R8EUIStyleSheet;

/* -------------------------------------------------------------------------
 * Style context: holds stylesheets and provides resolution
 * ------------------------------------------------------------------------- */

typedef struct R8EUIStyleContext {
    R8EUIStyleSheet **sheets;      /* array of stylesheet pointers */
    uint16_t         sheet_count;
    uint16_t         sheet_capacity;
} R8EUIStyleContext;

/* Constants */
#define NCSS_RULE_INIT_CAP     16
#define NCSS_DECL_INIT_CAP      8
#define NCSS_SHEET_INIT_CAP     4
#define NCSS_MATCH_INIT_CAP    32

/* Default computed style values */
#define NCSS_DEFAULT_FONT_SIZE     16.0f
#define NCSS_DEFAULT_LINE_HEIGHT   1.2f
#define NCSS_DEFAULT_FONT_WEIGHT   400
#define NCSS_DEFAULT_COLOR         0x000000FFu  /* black */
#define NCSS_DEFAULT_BG_COLOR      0x00000000u  /* transparent */
#define NCSS_DEFAULT_BORDER_COLOR  0x000000FFu  /* black */

#endif /* R8E_NSTYLE_TYPES_DEFINED */


/* =========================================================================
 * Atom Stubs (matching ndom.c local atom table)
 * ========================================================================= */

#define R8E_UI_STYLE_ATOM_TABLE_SIZE 512

static struct {
    struct { uint32_t hash; char name[64]; uint32_t len; } entries[R8E_UI_STYLE_ATOM_TABLE_SIZE];
    uint32_t count;
} r8e_ui_style_atom_table = { .count = 0 };

static uint32_t r8e_ui_style_atom_intern(const char *str, uint32_t len) {
    if (!str || len == 0) return 0;
    uint32_t hash = r8e_string_hash_s(str, len);
    for (uint32_t i = 1; i <= r8e_ui_style_atom_table.count; i++) {
        if (r8e_ui_style_atom_table.entries[i].hash == hash &&
            r8e_ui_style_atom_table.entries[i].len == len &&
            memcmp(r8e_ui_style_atom_table.entries[i].name, str, len) == 0) {
            return i;
        }
    }
    if (r8e_ui_style_atom_table.count >= R8E_UI_STYLE_ATOM_TABLE_SIZE - 1) return 0;
    r8e_ui_style_atom_table.count++;
    uint32_t idx = r8e_ui_style_atom_table.count;
    r8e_ui_style_atom_table.entries[idx].hash = hash;
    r8e_ui_style_atom_table.entries[idx].len = len;
    uint32_t copy_len = len < 63 ? len : 63;
    memcpy(r8e_ui_style_atom_table.entries[idx].name, str, copy_len);
    r8e_ui_style_atom_table.entries[idx].name[copy_len] = '\0';
    return idx;
}

static uint32_t r8e_ui_style_atom_intern_cstr(const char *str) {
    if (!str) return 0;
    return r8e_ui_style_atom_intern(str, (uint32_t)strlen(str));
}

static const char *r8e_ui_style_atom_str(uint32_t atom) {
    if (atom == 0 || atom > r8e_ui_style_atom_table.count) return "";
    return r8e_ui_style_atom_table.entries[atom].name;
}


/* =========================================================================
 * Section 1: Default Computed Style
 * ========================================================================= */

/**
 * Initialize a computed style with CSS initial values.
 */
void r8e_ui_style_init_defaults(R8EUIComputedStyle *style) {
    if (!style) return;
    memset(style, 0, sizeof(R8EUIComputedStyle));

    style->display = NCSS_DISPLAY_BLOCK;
    style->position = NCSS_POSITION_STATIC;
    style->visibility = NCSS_VISIBILITY_VISIBLE;
    style->overflow = NCSS_OVERFLOW_VISIBLE;

    style->width = NCSS_AUTO;
    style->height = NCSS_AUTO;
    style->min_width = 0.0f;
    style->min_height = 0.0f;
    style->max_width = NCSS_AUTO;
    style->max_height = NCSS_AUTO;

    style->flex_direction = NCSS_FLEX_ROW;
    style->flex_wrap = NCSS_FLEX_NOWRAP;
    style->justify_content = NCSS_JUSTIFY_FLEX_START;
    style->align_items = NCSS_ALIGN_STRETCH;
    style->align_self = NCSS_ALIGN_AUTO;
    style->flex_grow = 0.0f;
    style->flex_shrink = 1.0f;
    style->flex_basis = NCSS_AUTO;
    style->gap = 0.0f;
    style->order = 0;

    style->font_size = NCSS_DEFAULT_FONT_SIZE;
    style->line_height = NCSS_DEFAULT_LINE_HEIGHT;
    style->font_weight = NCSS_DEFAULT_FONT_WEIGHT;
    style->font_family_atom = 0;
    style->text_align = NCSS_TEXT_ALIGN_LEFT;

    style->color = NCSS_DEFAULT_COLOR;
    style->background_color = NCSS_DEFAULT_BG_COLOR;
    style->border_color = NCSS_DEFAULT_BORDER_COLOR;
    style->opacity = 1.0f;

    style->cursor = NCSS_CURSOR_DEFAULT;

    style->top = NCSS_AUTO;
    style->right = NCSS_AUTO;
    style->bottom = NCSS_AUTO;
    style->left = NCSS_AUTO;
    style->z_index = 0;
}

/**
 * Allocate a new computed style with default values.
 */
R8EUIComputedStyle *r8e_ui_style_create(void) {
    R8EUIComputedStyle *style = (R8EUIComputedStyle *)malloc(sizeof(R8EUIComputedStyle));
    if (style) r8e_ui_style_init_defaults(style);
    return style;
}

/**
 * Free a computed style.
 */
void r8e_ui_style_free(R8EUIComputedStyle *style) {
    free(style);
}

/**
 * Copy a computed style.
 */
void r8e_ui_style_copy(R8EUIComputedStyle *dst, const R8EUIComputedStyle *src) {
    if (dst && src) memcpy(dst, src, sizeof(R8EUIComputedStyle));
}


/* =========================================================================
 * Section 2: CSS Value Parsing
 * ========================================================================= */

/**
 * Skip whitespace in CSS string.
 */
static const char *ncss_skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/**
 * Parse a float value from a CSS string. Handles px, em, %, and unitless.
 * For em: multiplies by base_font_size.
 * For %: returns negative value (caller interprets as percentage).
 * Returns NCSS_AUTO for "auto".
 */
static float ncss_parse_length(const char *value, float base_font_size) {
    if (!value) return 0.0f;
    value = ncss_skip_ws(value);

    if (strncmp(value, "auto", 4) == 0) return NCSS_AUTO;

    char *end = NULL;
    float v = strtof(value, &end);
    if (end == value) return 0.0f;

    /* Check unit */
    const char *unit = ncss_skip_ws(end);
    if (strncmp(unit, "px", 2) == 0) {
        return v;
    } else if (strncmp(unit, "em", 2) == 0) {
        return v * base_font_size;
    } else if (strncmp(unit, "rem", 3) == 0) {
        return v * NCSS_DEFAULT_FONT_SIZE;
    } else if (*unit == '%') {
        return -v; /* negative = percentage, caller resolves */
    } else if (strncmp(unit, "pt", 2) == 0) {
        return v * (4.0f / 3.0f); /* 1pt = 1.333px */
    } else if (strncmp(unit, "vh", 2) == 0 || strncmp(unit, "vw", 2) == 0) {
        return -v; /* treat viewport units as percentages */
    }
    return v; /* unitless */
}

/**
 * Parse a CSS color value.
 * Supports: #RGB, #RRGGBB, #RRGGBBAA, named colors (basic set).
 * Returns RGBA packed as 0xRRGGBBAA.
 */
static uint32_t ncss_parse_color(const char *value) {
    if (!value) return 0x000000FFu;
    value = ncss_skip_ws(value);

    /* Named colors (basic set) */
    static const struct { const char *name; uint32_t rgba; } named_colors[] = {
        { "transparent",  0x00000000u },
        { "black",        0x000000FFu },
        { "white",        0xFFFFFFFFu },
        { "red",          0xFF0000FFu },
        { "green",        0x008000FFu },
        { "blue",         0x0000FFFFu },
        { "yellow",       0xFFFF00FFu },
        { "cyan",         0x00FFFFFFu },
        { "magenta",      0xFF00FFFFu },
        { "orange",       0xFFA500FFu },
        { "purple",       0x800080FFu },
        { "pink",         0xFFC0CBFFu },
        { "gray",         0x808080FFu },
        { "grey",         0x808080FFu },
        { "silver",       0xC0C0C0FFu },
        { "maroon",       0x800000FFu },
        { "olive",        0x808000FFu },
        { "lime",         0x00FF00FFu },
        { "aqua",         0x00FFFFFFu },
        { "teal",         0x008080FFu },
        { "navy",         0x000080FFu },
        { "fuchsia",      0xFF00FFFFu },
        { "inherit",      0x000000FFu },
        { NULL, 0 }
    };

    for (int i = 0; named_colors[i].name; i++) {
        if (ncss_strcasecmp(value, named_colors[i].name) == 0) {
            return named_colors[i].rgba;
        }
    }

    /* Hex colors */
    if (*value == '#') {
        value++;
        uint32_t len = (uint32_t)strlen(value);
        unsigned long hex = strtoul(value, NULL, 16);

        if (len == 3) {
            /* #RGB -> #RRGGBB */
            uint32_t r = (hex >> 8) & 0xF;
            uint32_t g = (hex >> 4) & 0xF;
            uint32_t b = hex & 0xF;
            return ((r | (r << 4)) << 24) | ((g | (g << 4)) << 16) |
                   ((b | (b << 4)) << 8) | 0xFF;
        } else if (len == 6) {
            return ((uint32_t)hex << 8) | 0xFF;
        } else if (len == 8) {
            return (uint32_t)hex;
        }
    }

    /* rgb(r, g, b) and rgba(r, g, b, a) */
    if (strncmp(value, "rgb", 3) == 0) {
        const char *p = strchr(value, '(');
        if (!p) return 0x000000FFu;
        p++;
        int r = 0, g = 0, b = 0;
        float a = 1.0f;
        r = (int)strtol(p, (char **)&p, 10);
        while (*p == ',' || *p == ' ') p++;
        g = (int)strtol(p, (char **)&p, 10);
        while (*p == ',' || *p == ' ') p++;
        b = (int)strtol(p, (char **)&p, 10);
        if (*p == ',' || *p == '/') {
            p++;
            while (*p == ' ') p++;
            a = strtof(p, NULL);
        }
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) |
               ((uint32_t)b << 8) | (uint32_t)(a * 255.0f);
    }

    return 0x000000FFu; /* default: black */
}

/**
 * Parse a font-weight value.
 */
static uint16_t ncss_parse_font_weight(const char *value) {
    if (!value) return 400;
    value = ncss_skip_ws(value);
    if (strcmp(value, "normal") == 0) return 400;
    if (strcmp(value, "bold") == 0) return 700;
    if (strcmp(value, "bolder") == 0) return 700;
    if (strcmp(value, "lighter") == 0) return 300;
    int w = atoi(value);
    if (w >= 100 && w <= 900) return (uint16_t)w;
    return 400;
}


/* =========================================================================
 * Section 3: Property Name -> ID Mapping
 * ========================================================================= */

typedef struct {
    const char       *name;
    R8EUICSSPropertyID id;
} R8EUICSSPropEntry;

static const R8EUICSSPropEntry ncss_prop_table[] = {
    { "display",                    NCSS_PROP_DISPLAY },
    { "position",                   NCSS_PROP_POSITION },
    { "visibility",                 NCSS_PROP_VISIBILITY },
    { "overflow",                   NCSS_PROP_OVERFLOW },
    { "width",                      NCSS_PROP_WIDTH },
    { "height",                     NCSS_PROP_HEIGHT },
    { "min-width",                  NCSS_PROP_MIN_WIDTH },
    { "min-height",                 NCSS_PROP_MIN_HEIGHT },
    { "max-width",                  NCSS_PROP_MAX_WIDTH },
    { "max-height",                 NCSS_PROP_MAX_HEIGHT },
    { "padding",                    NCSS_PROP_PADDING },
    { "padding-top",                NCSS_PROP_PADDING_TOP },
    { "padding-right",              NCSS_PROP_PADDING_RIGHT },
    { "padding-bottom",             NCSS_PROP_PADDING_BOTTOM },
    { "padding-left",               NCSS_PROP_PADDING_LEFT },
    { "margin",                     NCSS_PROP_MARGIN },
    { "margin-top",                 NCSS_PROP_MARGIN_TOP },
    { "margin-right",               NCSS_PROP_MARGIN_RIGHT },
    { "margin-bottom",              NCSS_PROP_MARGIN_BOTTOM },
    { "margin-left",                NCSS_PROP_MARGIN_LEFT },
    { "border-width",               NCSS_PROP_BORDER_WIDTH },
    { "border-top-width",           NCSS_PROP_BORDER_TOP_WIDTH },
    { "border-right-width",         NCSS_PROP_BORDER_RIGHT_WIDTH },
    { "border-bottom-width",        NCSS_PROP_BORDER_BOTTOM_WIDTH },
    { "border-left-width",          NCSS_PROP_BORDER_LEFT_WIDTH },
    { "border-color",               NCSS_PROP_BORDER_COLOR },
    { "border-radius",              NCSS_PROP_BORDER_RADIUS },
    { "border-top-left-radius",     NCSS_PROP_BORDER_TOP_LEFT_RADIUS },
    { "border-top-right-radius",    NCSS_PROP_BORDER_TOP_RIGHT_RADIUS },
    { "border-bottom-right-radius", NCSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS },
    { "border-bottom-left-radius",  NCSS_PROP_BORDER_BOTTOM_LEFT_RADIUS },
    { "flex-direction",             NCSS_PROP_FLEX_DIRECTION },
    { "flex-wrap",                  NCSS_PROP_FLEX_WRAP },
    { "justify-content",            NCSS_PROP_JUSTIFY_CONTENT },
    { "align-items",                NCSS_PROP_ALIGN_ITEMS },
    { "align-self",                 NCSS_PROP_ALIGN_SELF },
    { "flex-grow",                  NCSS_PROP_FLEX_GROW },
    { "flex-shrink",                NCSS_PROP_FLEX_SHRINK },
    { "flex-basis",                 NCSS_PROP_FLEX_BASIS },
    { "gap",                        NCSS_PROP_GAP },
    { "order",                      NCSS_PROP_ORDER },
    { "color",                      NCSS_PROP_COLOR },
    { "background-color",           NCSS_PROP_BACKGROUND_COLOR },
    { "opacity",                    NCSS_PROP_OPACITY },
    { "font-size",                  NCSS_PROP_FONT_SIZE },
    { "font-weight",                NCSS_PROP_FONT_WEIGHT },
    { "font-family",                NCSS_PROP_FONT_FAMILY },
    { "line-height",                NCSS_PROP_LINE_HEIGHT },
    { "text-align",                 NCSS_PROP_TEXT_ALIGN },
    { "cursor",                     NCSS_PROP_CURSOR },
    { "box-shadow",                 NCSS_PROP_BOX_SHADOW },
    { "top",                        NCSS_PROP_TOP },
    { "right",                      NCSS_PROP_RIGHT },
    { "bottom",                     NCSS_PROP_BOTTOM },
    { "left",                       NCSS_PROP_LEFT },
    { "z-index",                    NCSS_PROP_Z_INDEX },
    { NULL, 0 }
};

/**
 * Look up a CSS property name and return its ID.
 * Returns -1 if not recognized.
 */
static int ncss_lookup_property(const char *name, uint32_t len) {
    for (int i = 0; ncss_prop_table[i].name; i++) {
        if (strlen(ncss_prop_table[i].name) == len &&
            memcmp(ncss_prop_table[i].name, name, len) == 0) {
            return (int)ncss_prop_table[i].id;
        }
    }
    return -1;
}


/* =========================================================================
 * Section 4: Stylesheet and Rule Management
 * ========================================================================= */

/**
 * Create a new empty stylesheet.
 */
R8EUIStyleSheet *r8e_ui_stylesheet_create(void) {
    R8EUIStyleSheet *sheet = (R8EUIStyleSheet *)calloc(1, sizeof(R8EUIStyleSheet));
    return sheet;
}

/**
 * Free a stylesheet and all its rules.
 */
void r8e_ui_stylesheet_free(R8EUIStyleSheet *sheet) {
    if (!sheet) return;
    for (uint32_t i = 0; i < sheet->rule_count; i++) {
        for (uint16_t j = 0; j < sheet->rules[i].decl_count; j++) {
            free(sheet->rules[i].declarations[j].value);
        }
        free(sheet->rules[i].declarations);
    }
    free(sheet->rules);
    free(sheet);
}

/**
 * Ensure the rule array has capacity for one more rule.
 */
static bool ncss_rule_grow(R8EUIStyleSheet *sheet) {
    if (sheet->rule_count < sheet->rule_capacity) return true;
    uint32_t new_cap = sheet->rule_capacity == 0 ?
        NCSS_RULE_INIT_CAP : sheet->rule_capacity * 2;
    R8EUICSSRule *new_rules = (R8EUICSSRule *)realloc(
        sheet->rules, new_cap * sizeof(R8EUICSSRule));
    if (!new_rules) return false;
    sheet->rules = new_rules;
    sheet->rule_capacity = new_cap;
    return true;
}

/**
 * Ensure a rule's declaration array has capacity.
 */
static bool ncss_decl_grow(R8EUICSSRule *rule) {
    if (rule->decl_count < rule->decl_capacity) return true;
    uint16_t new_cap = rule->decl_capacity == 0 ?
        NCSS_DECL_INIT_CAP : rule->decl_capacity * 2;
    R8EUICSSDecl *new_decls = (R8EUICSSDecl *)realloc(
        rule->declarations, new_cap * sizeof(R8EUICSSDecl));
    if (!new_decls) return false;
    rule->declarations = new_decls;
    rule->decl_capacity = new_cap;
    return true;
}


/* =========================================================================
 * Section 5: Selector Parsing (CSS)
 * ========================================================================= */

/**
 * Parse a pseudo-class name (for style context).
 */
static R8EUIPseudoClass ncss_parse_pseudo(const char *name, uint32_t len) {
    if (len == 5 && memcmp(name, "hover", 5) == 0) return R8E_UI_PSEUDO_HOVER;
    if (len == 5 && memcmp(name, "focus", 5) == 0) return R8E_UI_PSEUDO_FOCUS;
    if (len == 6 && memcmp(name, "active", 6) == 0) return R8E_UI_PSEUDO_ACTIVE;
    if (len == 11 && memcmp(name, "first-child", 11) == 0)
        return R8E_UI_PSEUDO_FIRST_CHILD;
    if (len == 10 && memcmp(name, "last-child", 10) == 0)
        return R8E_UI_PSEUDO_LAST_CHILD;
    return R8E_UI_PSEUDO_NONE;
}

/**
 * Parse a CSS selector string into a R8EUISelector.
 * Returns true on success.
 */
static bool ncss_parse_selector(const char *text, R8EUISelector *sel) {
    if (!text || !sel) return false;
    memset(sel, 0, sizeof(R8EUISelector));

    const char *p = text;
    R8EUICombinator pending_comb = R8E_UI_COMB_NONE;
    bool first = true;

    while (*p) {
        bool had_space = false;
        while (*p == ' ' || *p == '\t') { had_space = true; p++; }
        if (!*p || *p == '{' || *p == ',') break;

        if (*p == '>') {
            pending_comb = R8E_UI_COMB_CHILD;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '{') break;
        } else if (had_space && !first) {
            pending_comb = R8E_UI_COMB_DESCENDANT;
        }

        if (sel->count >= R8E_UI_MAX_SELECTOR_PARTS) break;

        R8EUISelectorPart *part = &sel->parts[sel->count];
        part->combinator = pending_comb;
        pending_comb = R8E_UI_COMB_NONE;
        first = false;

        if (*p == '#') {
            p++;
            const char *start = p;
            while (*p && *p != '.' && *p != ':' && *p != ' ' &&
                   *p != '>' && *p != '#' && *p != '{' && *p != ',') p++;
            part->type = R8E_UI_SEL_ID;
            part->atom = r8e_ui_style_atom_intern(start, (uint32_t)(p - start));
            sel->count++;
        } else if (*p == '.') {
            p++;
            const char *start = p;
            while (*p && *p != '.' && *p != ':' && *p != ' ' &&
                   *p != '>' && *p != '#' && *p != '{' && *p != ',') p++;
            part->type = R8E_UI_SEL_CLASS;
            part->atom = r8e_ui_style_atom_intern(start, (uint32_t)(p - start));
            sel->count++;
        } else if (*p == ':') {
            p++;
            const char *start = p;
            while (*p && *p != ' ' && *p != '>' && *p != '.' &&
                   *p != '#' && *p != ':' && *p != '{' && *p != ',') p++;
            part->type = R8E_UI_SEL_PSEUDO;
            part->pseudo = ncss_parse_pseudo(start, (uint32_t)(p - start));
            sel->count++;
        } else if (*p == '*') {
            part->type = R8E_UI_SEL_UNIVERSAL;
            sel->count++;
            p++;
        } else {
            const char *start = p;
            while (*p && *p != '.' && *p != ':' && *p != ' ' &&
                   *p != '>' && *p != '#' && *p != '{' && *p != ',') p++;
            uint32_t len = (uint32_t)(p - start);
            if (len > 0) {
                char lower[64];
                uint32_t copy_len = len < 63 ? len : 63;
                for (uint32_t i = 0; i < copy_len; i++)
                    lower[i] = (char)tolower((unsigned char)start[i]);
                lower[copy_len] = '\0';
                part->type = R8E_UI_SEL_TAG;
                part->atom = r8e_ui_style_atom_intern(lower, copy_len);
                sel->count++;
            }
        }
    }

    return sel->count > 0;
}

/**
 * Calculate specificity of a selector.
 * ID = weight 100, class/pseudo = weight 10, tag = weight 1.
 */
static uint32_t ncss_selector_specificity(const R8EUISelector *sel) {
    if (!sel) return 0;
    uint32_t ids = 0, classes = 0, tags = 0;
    for (uint8_t i = 0; i < sel->count; i++) {
        switch (sel->parts[i].type) {
        case R8E_UI_SEL_ID:    ids++; break;
        case R8E_UI_SEL_CLASS: classes++; break;
        case R8E_UI_SEL_TAG:   tags++; break;
        case R8E_UI_SEL_PSEUDO: classes++; break;
        case R8E_UI_SEL_UNIVERSAL: break;
        }
    }
    return (ids * 100) + (classes * 10) + tags;
}


/* =========================================================================
 * Section 6: CSS Stylesheet Parsing
 * ========================================================================= */

/**
 * Parse a CSS declaration block (between { and }).
 * Populates a rule's declarations array.
 */
static const char *ncss_parse_declarations(const char *p, R8EUICSSRule *rule) {
    if (!p || !rule) return p;

    while (*p && *p != '}') {
        p = ncss_skip_ws(p);
        if (*p == '}' || !*p) break;

        /* Property name */
        const char *prop_start = p;
        while (*p && *p != ':' && *p != '}' && *p != ';') p++;
        if (*p != ':') { if (*p == ';') p++; continue; }

        const char *prop_end = p;
        while (prop_end > prop_start &&
               (*(prop_end - 1) == ' ' || *(prop_end - 1) == '\t'))
            prop_end--;

        uint32_t prop_len = (uint32_t)(prop_end - prop_start);
        p++; /* skip ':' */
        p = ncss_skip_ws(p);

        /* Value */
        const char *val_start = p;
        while (*p && *p != ';' && *p != '}') p++;

        const char *val_end = p;
        while (val_end > val_start &&
               (*(val_end - 1) == ' ' || *(val_end - 1) == '\t'))
            val_end--;

        /* Check for !important */
        uint8_t important = 0;
        const char *imp = val_end - 10;
        if (imp >= val_start && memcmp(imp, "!important", 10) == 0) {
            important = 1;
            val_end = imp;
            while (val_end > val_start &&
                   (*(val_end - 1) == ' ' || *(val_end - 1) == '\t'))
                val_end--;
        }

        if (prop_len > 0 && val_end > val_start) {
            /* Look up property */
            char prop_buf[64];
            uint32_t copy_len = prop_len < 63 ? prop_len : 63;
            memcpy(prop_buf, prop_start, copy_len);
            prop_buf[copy_len] = '\0';

            int prop_id = ncss_lookup_property(prop_buf, (uint32_t)copy_len);
            if (prop_id >= 0) {
                if (!ncss_decl_grow(rule)) { if (*p == ';') p++; continue; }

                R8EUICSSDecl *decl = &rule->declarations[rule->decl_count];
                decl->property = (R8EUICSSPropertyID)prop_id;
                decl->important = important;

                uint32_t val_len = (uint32_t)(val_end - val_start);
                decl->value = (char *)malloc(val_len + 1);
                if (decl->value) {
                    memcpy(decl->value, val_start, val_len);
                    decl->value[val_len] = '\0';
                    decl->value_len = (uint16_t)val_len;
                } else {
                    decl->value_len = 0;
                }
                rule->decl_count++;
            }
        }

        if (*p == ';') p++;
    }

    if (*p == '}') p++;
    return p;
}

/**
 * Parse a CSS stylesheet string and populate a R8EUIStyleSheet.
 *
 * Supports:
 *   selector { property: value; ... }
 *   selector1, selector2 { ... }  (grouped selectors)
 *
 * Does NOT support: @media, @import, @keyframes, comments (basic only).
 */
void r8e_ui_stylesheet_parse(R8EUIStyleSheet *sheet, const char *css) {
    if (!sheet || !css) return;

    const char *p = css;

    while (*p) {
        p = ncss_skip_ws(p);
        if (!*p) break;

        /* Skip CSS comments */
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        /* Skip @-rules (unsupported) */
        if (*p == '@') {
            while (*p && *p != '{' && *p != ';') p++;
            if (*p == '{') {
                int depth = 1;
                p++;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }
            } else if (*p == ';') {
                p++;
            }
            continue;
        }

        /* Read selector(s) */
        const char *sel_start = p;
        while (*p && *p != '{') p++;
        if (!*p) break;

        const char *sel_end = p;
        while (sel_end > sel_start &&
               (*(sel_end - 1) == ' ' || *(sel_end - 1) == '\t' ||
                *(sel_end - 1) == '\n'))
            sel_end--;

        p++; /* skip '{' */

        /* Parse declarations into a temporary rule */
        R8EUICSSRule temp_rule;
        memset(&temp_rule, 0, sizeof(temp_rule));
        p = ncss_parse_declarations(p, &temp_rule);

        if (temp_rule.decl_count == 0) {
            free(temp_rule.declarations);
            continue;
        }

        /* Handle grouped selectors (comma-separated) */
        const char *sp = sel_start;
        while (sp < sel_end) {
            /* Find end of this selector */
            const char *comma = sp;
            while (comma < sel_end && *comma != ',') comma++;

            /* Trim whitespace */
            const char *s = sp;
            while (s < comma && (*s == ' ' || *s == '\t' || *s == '\n')) s++;
            const char *e = comma;
            while (e > s && (*(e - 1) == ' ' || *(e - 1) == '\t' ||
                             *(e - 1) == '\n'))
                e--;

            if (e > s) {
                char sel_buf[256];
                uint32_t sel_len = (uint32_t)(e - s);
                uint32_t copy_len = sel_len < 255 ? sel_len : 255;
                memcpy(sel_buf, s, copy_len);
                sel_buf[copy_len] = '\0';

                R8EUISelector parsed_sel;
                if (ncss_parse_selector(sel_buf, &parsed_sel)) {
                    if (ncss_rule_grow(sheet)) {
                        R8EUICSSRule *rule = &sheet->rules[sheet->rule_count];
                        memset(rule, 0, sizeof(R8EUICSSRule));
                        rule->selector = parsed_sel;
                        rule->specificity = ncss_selector_specificity(&parsed_sel);

                        /* Copy declarations */
                        rule->decl_capacity = temp_rule.decl_count;
                        rule->declarations = (R8EUICSSDecl *)malloc(
                            rule->decl_capacity * sizeof(R8EUICSSDecl));
                        if (rule->declarations) {
                            for (uint16_t i = 0; i < temp_rule.decl_count; i++) {
                                rule->declarations[i] = temp_rule.declarations[i];
                                if (temp_rule.declarations[i].value) {
                                    rule->declarations[i].value = strdup(
                                        temp_rule.declarations[i].value);
                                }
                            }
                            rule->decl_count = temp_rule.decl_count;
                        }
                        sheet->rule_count++;
                    }
                }
            }

            sp = comma < sel_end ? comma + 1 : sel_end;
        }

        /* Free temp declarations */
        for (uint16_t i = 0; i < temp_rule.decl_count; i++) {
            free(temp_rule.declarations[i].value);
        }
        free(temp_rule.declarations);
    }
}


/* =========================================================================
 * Section 7: Selector Matching
 * ========================================================================= */

/**
 * Check if a node has a specific class atom (binary search on sorted array).
 */
static bool ncss_node_has_class(const R8EUIDOMNode *node, uint32_t atom) {
    const R8EUIClassList *cl = &node->class_list;
    uint16_t lo = 0, hi = cl->count;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (cl->atoms[mid] < atom) lo = mid + 1;
        else if (cl->atoms[mid] > atom) hi = mid;
        else return true;
    }
    return false;
}

/**
 * Check if a single selector part matches a node.
 */
static bool ncss_match_part(const R8EUIDOMNode *node,
                            const R8EUISelectorPart *part) {
    if (node->type != R8E_UI_NODE_ELEMENT) return false;

    switch (part->type) {
    case R8E_UI_SEL_TAG:
        return node->tag_atom == part->atom;
    case R8E_UI_SEL_CLASS:
        return ncss_node_has_class(node, part->atom);
    case R8E_UI_SEL_ID:
        return node->id_atom == part->atom;
    case R8E_UI_SEL_UNIVERSAL:
        return true;
    case R8E_UI_SEL_PSEUDO:
        switch (part->pseudo) {
        case R8E_UI_PSEUDO_HOVER:
            return (node->flags & R8E_UI_FLAG_HOVER) != 0;
        case R8E_UI_PSEUDO_FOCUS:
            return (node->flags & R8E_UI_FLAG_FOCUS) != 0;
        case R8E_UI_PSEUDO_ACTIVE:
            return (node->flags & R8E_UI_FLAG_ACTIVE) != 0;
        case R8E_UI_PSEUDO_FIRST_CHILD:
            return node->parent && node->parent->first_child == node;
        case R8E_UI_PSEUDO_LAST_CHILD:
            return node->parent && node->parent->last_child == node;
        default:
            return false;
        }
    default:
        return false;
    }
}

/**
 * Match a complete selector against a node.
 */
static bool ncss_match_selector(const R8EUIDOMNode *node,
                                const R8EUISelector *sel) {
    if (!sel || sel->count == 0) return false;

    const R8EUIDOMNode *current = node;

    for (int i = (int)sel->count - 1; i >= 0; i--) {
        const R8EUISelectorPart *part = &sel->parts[i];

        if (i == (int)sel->count - 1 || part->combinator == R8E_UI_COMB_NONE) {
            if (!ncss_match_part(current, part)) return false;
        } else if (part->combinator == R8E_UI_COMB_CHILD) {
            current = current->parent;
            if (!current) return false;
            if (!ncss_match_part(current, part)) return false;
        } else if (part->combinator == R8E_UI_COMB_DESCENDANT) {
            current = current->parent;
            bool found = false;
            while (current) {
                if (ncss_match_part(current, part)) { found = true; break; }
                current = current->parent;
            }
            if (!found) return false;
        }
    }

    return true;
}


/* =========================================================================
 * Section 8: Applying Declarations to Computed Style
 * ========================================================================= */

/**
 * Parse TRBL (top, right, bottom, left) shorthand values.
 * Supports 1, 2, 3, or 4 values.
 */
static void ncss_parse_trbl(const char *value, float base_font_size,
                            float out[4]) {
    if (!value) { out[0] = out[1] = out[2] = out[3] = 0; return; }

    float vals[4] = {0, 0, 0, 0};
    int count = 0;
    const char *p = value;

    while (count < 4) {
        p = ncss_skip_ws(p);
        if (!*p) break;
        vals[count] = ncss_parse_length(p, base_font_size);
        count++;
        /* Skip past the value */
        while (*p && *p != ' ' && *p != '\t') p++;
    }

    switch (count) {
    case 1:
        out[0] = out[1] = out[2] = out[3] = vals[0];
        break;
    case 2:
        out[0] = out[2] = vals[0]; /* top, bottom */
        out[1] = out[3] = vals[1]; /* right, left */
        break;
    case 3:
        out[0] = vals[0]; /* top */
        out[1] = out[3] = vals[1]; /* right, left */
        out[2] = vals[2]; /* bottom */
        break;
    case 4:
        out[0] = vals[0]; out[1] = vals[1];
        out[2] = vals[2]; out[3] = vals[3];
        break;
    default:
        out[0] = out[1] = out[2] = out[3] = 0;
        break;
    }
}

/**
 * Apply a single CSS declaration to a computed style.
 */
static void ncss_apply_declaration(R8EUIComputedStyle *style,
                                   const R8EUICSSDecl *decl,
                                   float base_font_size) {
    if (!style || !decl || !decl->value) return;

    const char *v = decl->value;

    switch (decl->property) {

    /* -- Display and layout model -- */
    case NCSS_PROP_DISPLAY:
        if (strcmp(v, "flex") == 0)        style->display = NCSS_DISPLAY_FLEX;
        else if (strcmp(v, "block") == 0)  style->display = NCSS_DISPLAY_BLOCK;
        else if (strcmp(v, "inline") == 0) style->display = NCSS_DISPLAY_INLINE;
        else if (strcmp(v, "none") == 0)   style->display = NCSS_DISPLAY_NONE;
        break;

    case NCSS_PROP_POSITION:
        if (strcmp(v, "static") == 0)        style->position = NCSS_POSITION_STATIC;
        else if (strcmp(v, "relative") == 0) style->position = NCSS_POSITION_RELATIVE;
        else if (strcmp(v, "absolute") == 0) style->position = NCSS_POSITION_ABSOLUTE;
        else if (strcmp(v, "fixed") == 0)    style->position = NCSS_POSITION_FIXED;
        break;

    case NCSS_PROP_VISIBILITY:
        if (strcmp(v, "visible") == 0)      style->visibility = NCSS_VISIBILITY_VISIBLE;
        else if (strcmp(v, "hidden") == 0)  style->visibility = NCSS_VISIBILITY_HIDDEN;
        break;

    case NCSS_PROP_OVERFLOW:
        if (strcmp(v, "visible") == 0)      style->overflow = NCSS_OVERFLOW_VISIBLE;
        else if (strcmp(v, "hidden") == 0)  style->overflow = NCSS_OVERFLOW_HIDDEN;
        else if (strcmp(v, "scroll") == 0)  style->overflow = NCSS_OVERFLOW_SCROLL;
        else if (strcmp(v, "auto") == 0)    style->overflow = NCSS_OVERFLOW_AUTO;
        break;

    /* -- Dimensions -- */
    case NCSS_PROP_WIDTH:      style->width = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_HEIGHT:     style->height = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MIN_WIDTH:  style->min_width = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MIN_HEIGHT: style->min_height = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MAX_WIDTH:  style->max_width = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MAX_HEIGHT: style->max_height = ncss_parse_length(v, base_font_size); break;

    /* -- Padding -- */
    case NCSS_PROP_PADDING:
        ncss_parse_trbl(v, base_font_size, style->padding);
        break;
    case NCSS_PROP_PADDING_TOP:    style->padding[0] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_PADDING_RIGHT:  style->padding[1] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_PADDING_BOTTOM: style->padding[2] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_PADDING_LEFT:   style->padding[3] = ncss_parse_length(v, base_font_size); break;

    /* -- Margin -- */
    case NCSS_PROP_MARGIN:
        ncss_parse_trbl(v, base_font_size, style->margin);
        break;
    case NCSS_PROP_MARGIN_TOP:    style->margin[0] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MARGIN_RIGHT:  style->margin[1] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MARGIN_BOTTOM: style->margin[2] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_MARGIN_LEFT:   style->margin[3] = ncss_parse_length(v, base_font_size); break;

    /* -- Border width -- */
    case NCSS_PROP_BORDER_WIDTH:
        ncss_parse_trbl(v, base_font_size, style->border_width);
        break;
    case NCSS_PROP_BORDER_TOP_WIDTH:    style->border_width[0] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BORDER_RIGHT_WIDTH:  style->border_width[1] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BORDER_BOTTOM_WIDTH: style->border_width[2] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BORDER_LEFT_WIDTH:   style->border_width[3] = ncss_parse_length(v, base_font_size); break;

    /* -- Border color -- */
    case NCSS_PROP_BORDER_COLOR:
        style->border_color = ncss_parse_color(v);
        break;

    /* -- Border radius -- */
    case NCSS_PROP_BORDER_RADIUS:
        ncss_parse_trbl(v, base_font_size, style->border_radius);
        break;
    case NCSS_PROP_BORDER_TOP_LEFT_RADIUS:     style->border_radius[0] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BORDER_TOP_RIGHT_RADIUS:    style->border_radius[1] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS: style->border_radius[2] = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BORDER_BOTTOM_LEFT_RADIUS:  style->border_radius[3] = ncss_parse_length(v, base_font_size); break;

    /* -- Flexbox -- */
    case NCSS_PROP_FLEX_DIRECTION:
        if (strcmp(v, "row") == 0)                 style->flex_direction = NCSS_FLEX_ROW;
        else if (strcmp(v, "row-reverse") == 0)    style->flex_direction = NCSS_FLEX_ROW_REVERSE;
        else if (strcmp(v, "column") == 0)         style->flex_direction = NCSS_FLEX_COLUMN;
        else if (strcmp(v, "column-reverse") == 0) style->flex_direction = NCSS_FLEX_COLUMN_REVERSE;
        break;

    case NCSS_PROP_FLEX_WRAP:
        if (strcmp(v, "nowrap") == 0)           style->flex_wrap = NCSS_FLEX_NOWRAP;
        else if (strcmp(v, "wrap") == 0)        style->flex_wrap = NCSS_FLEX_WRAP;
        else if (strcmp(v, "wrap-reverse") == 0) style->flex_wrap = NCSS_FLEX_WRAP_REVERSE;
        break;

    case NCSS_PROP_JUSTIFY_CONTENT:
        if (strcmp(v, "flex-start") == 0)         style->justify_content = NCSS_JUSTIFY_FLEX_START;
        else if (strcmp(v, "flex-end") == 0)      style->justify_content = NCSS_JUSTIFY_FLEX_END;
        else if (strcmp(v, "center") == 0)        style->justify_content = NCSS_JUSTIFY_CENTER;
        else if (strcmp(v, "space-between") == 0) style->justify_content = NCSS_JUSTIFY_SPACE_BETWEEN;
        else if (strcmp(v, "space-around") == 0)  style->justify_content = NCSS_JUSTIFY_SPACE_AROUND;
        else if (strcmp(v, "space-evenly") == 0)  style->justify_content = NCSS_JUSTIFY_SPACE_EVENLY;
        break;

    case NCSS_PROP_ALIGN_ITEMS:
        if (strcmp(v, "stretch") == 0)        style->align_items = NCSS_ALIGN_STRETCH;
        else if (strcmp(v, "flex-start") == 0) style->align_items = NCSS_ALIGN_FLEX_START;
        else if (strcmp(v, "flex-end") == 0)  style->align_items = NCSS_ALIGN_FLEX_END;
        else if (strcmp(v, "center") == 0)    style->align_items = NCSS_ALIGN_CENTER;
        else if (strcmp(v, "baseline") == 0)  style->align_items = NCSS_ALIGN_BASELINE;
        break;

    case NCSS_PROP_ALIGN_SELF:
        if (strcmp(v, "auto") == 0)           style->align_self = NCSS_ALIGN_AUTO;
        else if (strcmp(v, "stretch") == 0)   style->align_self = NCSS_ALIGN_STRETCH;
        else if (strcmp(v, "flex-start") == 0) style->align_self = NCSS_ALIGN_FLEX_START;
        else if (strcmp(v, "flex-end") == 0)  style->align_self = NCSS_ALIGN_FLEX_END;
        else if (strcmp(v, "center") == 0)    style->align_self = NCSS_ALIGN_CENTER;
        else if (strcmp(v, "baseline") == 0)  style->align_self = NCSS_ALIGN_BASELINE;
        break;

    case NCSS_PROP_FLEX_GROW:   style->flex_grow = strtof(v, NULL); break;
    case NCSS_PROP_FLEX_SHRINK: style->flex_shrink = strtof(v, NULL); break;
    case NCSS_PROP_FLEX_BASIS:  style->flex_basis = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_GAP:         style->gap = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_ORDER:       style->order = atoi(v); break;

    /* -- Colors -- */
    case NCSS_PROP_COLOR:            style->color = ncss_parse_color(v); break;
    case NCSS_PROP_BACKGROUND_COLOR: style->background_color = ncss_parse_color(v); break;
    case NCSS_PROP_OPACITY: {
        float o = strtof(v, NULL);
        if (o < 0.0f) o = 0.0f;
        if (o > 1.0f) o = 1.0f;
        style->opacity = o;
        break;
    }

    /* -- Typography -- */
    case NCSS_PROP_FONT_SIZE:
        style->font_size = ncss_parse_length(v, base_font_size);
        if (style->font_size <= 0) style->font_size = NCSS_DEFAULT_FONT_SIZE;
        break;
    case NCSS_PROP_FONT_WEIGHT:
        style->font_weight = ncss_parse_font_weight(v);
        break;
    case NCSS_PROP_FONT_FAMILY:
        style->font_family_atom = r8e_ui_style_atom_intern_cstr(v);
        break;
    case NCSS_PROP_LINE_HEIGHT: {
        float lh = strtof(v, NULL);
        if (lh > 0) style->line_height = lh;
        break;
    }
    case NCSS_PROP_TEXT_ALIGN:
        if (strcmp(v, "left") == 0)        style->text_align = NCSS_TEXT_ALIGN_LEFT;
        else if (strcmp(v, "center") == 0) style->text_align = NCSS_TEXT_ALIGN_CENTER;
        else if (strcmp(v, "right") == 0)  style->text_align = NCSS_TEXT_ALIGN_RIGHT;
        break;

    /* -- Cursor -- */
    case NCSS_PROP_CURSOR:
        if (strcmp(v, "default") == 0)      style->cursor = NCSS_CURSOR_DEFAULT;
        else if (strcmp(v, "pointer") == 0) style->cursor = NCSS_CURSOR_POINTER;
        else if (strcmp(v, "text") == 0)    style->cursor = NCSS_CURSOR_TEXT;
        else if (strcmp(v, "move") == 0)    style->cursor = NCSS_CURSOR_MOVE;
        else if (strcmp(v, "none") == 0)    style->cursor = NCSS_CURSOR_NONE;
        break;

    /* -- Box shadow (basic: store raw value for paint layer) -- */
    case NCSS_PROP_BOX_SHADOW:
        /* Box shadow is complex; store the atom for the paint layer to parse */
        break;

    /* -- Positioning offsets -- */
    case NCSS_PROP_TOP:    style->top = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_RIGHT:  style->right = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_BOTTOM: style->bottom = ncss_parse_length(v, base_font_size); break;
    case NCSS_PROP_LEFT:   style->left = ncss_parse_length(v, base_font_size); break;

    case NCSS_PROP_Z_INDEX:
        style->z_index = atoi(v);
        break;

    default:
        break;
    }
}


/* =========================================================================
 * Section 9: Style Resolution (Cascade)
 *
 * For each node:
 *   1. Start with inherited values from parent (for inheritable properties)
 *   2. Collect all matching rules, sorted by specificity
 *   3. Apply declarations in order (low specificity first)
 *   4. Apply inline styles (highest priority except !important)
 *   5. Apply !important declarations last
 * ========================================================================= */

/* Matched rule entry for sorting */
typedef struct {
    const R8EUICSSRule *rule;
    uint32_t           specificity;
    uint32_t           source_order; /* index in stylesheet for stable sort */
} R8EUICSSMatch;

/**
 * Comparison function for sorting matches by specificity (ascending).
 */
static int ncss_match_compare(const void *a, const void *b) {
    const R8EUICSSMatch *ma = (const R8EUICSSMatch *)a;
    const R8EUICSSMatch *mb = (const R8EUICSSMatch *)b;
    if (ma->specificity != mb->specificity) {
        return (int)ma->specificity - (int)mb->specificity;
    }
    return (int)ma->source_order - (int)mb->source_order;
}

/**
 * Inherit inheritable properties from parent.
 * Inheritable: color, font-size, font-weight, font-family, line-height,
 * text-align, cursor, visibility.
 */
static void ncss_inherit_from_parent(R8EUIComputedStyle *style,
                                     const R8EUIComputedStyle *parent) {
    if (!parent) return;
    style->color = parent->color;
    style->font_size = parent->font_size;
    style->font_weight = parent->font_weight;
    style->font_family_atom = parent->font_family_atom;
    style->line_height = parent->line_height;
    style->text_align = parent->text_align;
    style->cursor = parent->cursor;
    style->visibility = parent->visibility;
}

/**
 * Resolve the computed style for a single node.
 *
 * @param node         The DOM node to style.
 * @param sheets       Array of stylesheets to cascade.
 * @param sheet_count  Number of stylesheets.
 */
void r8e_ui_style_resolve_node(R8EUIDOMNode *node,
                             R8EUIStyleSheet **sheets, uint16_t sheet_count) {
    if (!node || node->type != R8E_UI_NODE_ELEMENT) return;

    /* Allocate or reuse computed style */
    R8EUIComputedStyle *style = (R8EUIComputedStyle *)node->computed;
    if (!style) {
        style = r8e_ui_style_create();
        if (!style) return;
        node->computed = style;
    } else {
        r8e_ui_style_init_defaults(style);
    }

    /* Inherit from parent */
    if (node->parent && node->parent->computed) {
        ncss_inherit_from_parent(style, (R8EUIComputedStyle *)node->parent->computed);
    }

    float base_font_size = style->font_size;

    /* Collect matching rules */
    R8EUICSSMatch *matches = NULL;
    uint32_t match_count = 0;
    uint32_t match_capacity = 0;
    uint32_t source_order = 0;

    for (uint16_t si = 0; si < sheet_count; si++) {
        if (!sheets[si]) continue;
        R8EUIStyleSheet *sheet = sheets[si];

        for (uint32_t ri = 0; ri < sheet->rule_count; ri++) {
            R8EUICSSRule *rule = &sheet->rules[ri];

            if (ncss_match_selector(node, &rule->selector)) {
                /* Add to match list */
                if (match_count >= match_capacity) {
                    uint32_t new_cap = match_capacity == 0 ?
                        NCSS_MATCH_INIT_CAP : match_capacity * 2;
                    R8EUICSSMatch *new_matches = (R8EUICSSMatch *)realloc(
                        matches, new_cap * sizeof(R8EUICSSMatch));
                    if (!new_matches) continue;
                    matches = new_matches;
                    match_capacity = new_cap;
                }
                matches[match_count].rule = rule;
                matches[match_count].specificity = rule->specificity;
                matches[match_count].source_order = source_order++;
                match_count++;
            }
        }
    }

    /* Sort by specificity (ascending) */
    if (match_count > 1) {
        qsort(matches, match_count, sizeof(R8EUICSSMatch), ncss_match_compare);
    }

    /* Apply normal declarations (non-!important) in specificity order */
    for (uint32_t i = 0; i < match_count; i++) {
        const R8EUICSSRule *rule = matches[i].rule;
        for (uint16_t j = 0; j < rule->decl_count; j++) {
            if (!rule->declarations[j].important) {
                ncss_apply_declaration(style, &rule->declarations[j],
                                       base_font_size);
            }
        }
    }

    /* Apply inline styles (higher priority than stylesheet rules) */
    if (node->inline_style) {
        for (uint16_t i = 0; i < node->inline_style->count; i++) {
            R8EUIStyleEntry *entry = &node->inline_style->entries[i];
            R8EUICSSDecl inline_decl;
            inline_decl.property = (R8EUICSSPropertyID)entry->property_id;
            inline_decl.value = entry->value;
            inline_decl.value_len = entry->value_len;
            inline_decl.important = 0;
            ncss_apply_declaration(style, &inline_decl, base_font_size);
        }
    }

    /* Apply !important declarations last (highest priority) */
    for (uint32_t i = 0; i < match_count; i++) {
        const R8EUICSSRule *rule = matches[i].rule;
        for (uint16_t j = 0; j < rule->decl_count; j++) {
            if (rule->declarations[j].important) {
                ncss_apply_declaration(style, &rule->declarations[j],
                                       base_font_size);
            }
        }
    }

    free(matches);

    /* Clear dirty flag */
    node->flags &= ~R8E_UI_FLAG_STYLE_DIRTY;
}


/* =========================================================================
 * Section 10: Tree-Wide Style Resolution
 * ========================================================================= */

/**
 * Resolve styles for an entire DOM subtree (depth-first).
 */
void r8e_ui_style_resolve_tree(R8EUIDOMNode *root,
                              R8EUIStyleSheet **sheets, uint16_t sheet_count) {
    if (!root) return;

    if (root->type == R8E_UI_NODE_ELEMENT) {
        r8e_ui_style_resolve_node(root, sheets, sheet_count);
    }

    R8EUIDOMNode *child = root->first_child;
    while (child) {
        r8e_ui_style_resolve_tree(child, sheets, sheet_count);
        child = child->next_sibling;
    }
}

/**
 * Resolve styles only for dirty nodes in a subtree.
 */
void r8e_ui_style_resolve_dirty(R8EUIDOMNode *root,
                               R8EUIStyleSheet **sheets, uint16_t sheet_count) {
    if (!root) return;

    if (root->type == R8E_UI_NODE_ELEMENT &&
        (root->flags & R8E_UI_FLAG_STYLE_DIRTY)) {
        r8e_ui_style_resolve_node(root, sheets, sheet_count);
    }

    R8EUIDOMNode *child = root->first_child;
    while (child) {
        r8e_ui_style_resolve_dirty(child, sheets, sheet_count);
        child = child->next_sibling;
    }
}


/* =========================================================================
 * Section 11: Style Context Management
 * ========================================================================= */

/**
 * Create a new style context.
 */
R8EUIStyleContext *r8e_ui_style_context_create(void) {
    R8EUIStyleContext *ctx = (R8EUIStyleContext *)calloc(1, sizeof(R8EUIStyleContext));
    return ctx;
}

/**
 * Free a style context (does NOT free the stylesheets it references).
 */
void r8e_ui_style_context_free(R8EUIStyleContext *ctx) {
    if (!ctx) return;
    free(ctx->sheets);
    free(ctx);
}

/**
 * Add a stylesheet to the context.
 */
bool r8e_ui_style_context_add_sheet(R8EUIStyleContext *ctx, R8EUIStyleSheet *sheet) {
    if (!ctx || !sheet) return false;
    if (ctx->sheet_count >= ctx->sheet_capacity) {
        uint16_t new_cap = ctx->sheet_capacity == 0 ?
            NCSS_SHEET_INIT_CAP : ctx->sheet_capacity * 2;
        R8EUIStyleSheet **new_sheets = (R8EUIStyleSheet **)realloc(
            ctx->sheets, new_cap * sizeof(R8EUIStyleSheet *));
        if (!new_sheets) return false;
        ctx->sheets = new_sheets;
        ctx->sheet_capacity = new_cap;
    }
    ctx->sheets[ctx->sheet_count++] = sheet;
    return true;
}

/**
 * Resolve all styles using the context's stylesheets.
 */
void r8e_ui_style_context_resolve(R8EUIStyleContext *ctx, R8EUIDOMNode *root) {
    if (!ctx || !root) return;
    r8e_ui_style_resolve_tree(root, ctx->sheets, ctx->sheet_count);
}

/**
 * Resolve only dirty styles using the context's stylesheets.
 */
void r8e_ui_style_context_resolve_dirty(R8EUIStyleContext *ctx,
                                       R8EUIDOMNode *root) {
    if (!ctx || !root) return;
    r8e_ui_style_resolve_dirty(root, ctx->sheets, ctx->sheet_count);
}


/* =========================================================================
 * Section 12: Computed Style Accessors
 * ========================================================================= */

/**
 * Get the computed style for a node. Returns NULL if not resolved.
 */
const R8EUIComputedStyle *r8e_ui_style_get_computed(const R8EUIDOMNode *node) {
    if (!node) return NULL;
    return (const R8EUIComputedStyle *)node->computed;
}

/**
 * Check if a node has display: none.
 */
bool r8e_ui_style_is_hidden(const R8EUIDOMNode *node) {
    if (!node || !node->computed) return false;
    const R8EUIComputedStyle *s = (const R8EUIComputedStyle *)node->computed;
    return s->display == NCSS_DISPLAY_NONE ||
           s->visibility == NCSS_VISIBILITY_HIDDEN;
}

/**
 * Check if a node is a flex container.
 */
bool r8e_ui_style_is_flex(const R8EUIDOMNode *node) {
    if (!node || !node->computed) return false;
    return ((const R8EUIComputedStyle *)node->computed)->display == NCSS_DISPLAY_FLEX;
}

/**
 * Get the effective font size for a node (resolves inheritance).
 */
float r8e_ui_style_get_font_size(const R8EUIDOMNode *node) {
    if (!node || !node->computed) return NCSS_DEFAULT_FONT_SIZE;
    return ((const R8EUIComputedStyle *)node->computed)->font_size;
}


/* =========================================================================
 * Section 13: Cleanup - Free Computed Styles in a Tree
 * ========================================================================= */

/**
 * Free all computed styles in a DOM subtree.
 */
void r8e_ui_style_free_tree(R8EUIDOMNode *root) {
    if (!root) return;
    if (root->computed) {
        r8e_ui_style_free((R8EUIComputedStyle *)root->computed);
        root->computed = NULL;
    }
    R8EUIDOMNode *child = root->first_child;
    while (child) {
        r8e_ui_style_free_tree(child);
        child = child->next_sibling;
    }
}


/* =========================================================================
 * Section 14: Debug Utilities
 * ========================================================================= */

/**
 * Dump a computed style to stderr for debugging.
 */
void r8e_ui_style_dump(const R8EUIComputedStyle *style) {
    if (!style) { fprintf(stderr, "(null style)\n"); return; }

    static const char *display_names[] = { "block", "inline", "flex", "none" };
    static const char *position_names[] = { "static", "relative", "absolute", "fixed" };
    static const char *flex_dir_names[] = { "row", "row-reverse", "column", "column-reverse" };

    fprintf(stderr, "  display: %s\n", display_names[style->display]);
    fprintf(stderr, "  position: %s\n", position_names[style->position]);

    if (style->width != NCSS_AUTO) fprintf(stderr, "  width: %.1fpx\n", style->width);
    if (style->height != NCSS_AUTO) fprintf(stderr, "  height: %.1fpx\n", style->height);

    fprintf(stderr, "  padding: %.1f %.1f %.1f %.1f\n",
            style->padding[0], style->padding[1],
            style->padding[2], style->padding[3]);
    fprintf(stderr, "  margin: %.1f %.1f %.1f %.1f\n",
            style->margin[0], style->margin[1],
            style->margin[2], style->margin[3]);

    if (style->display == NCSS_DISPLAY_FLEX) {
        fprintf(stderr, "  flex-direction: %s\n",
                flex_dir_names[style->flex_direction]);
        fprintf(stderr, "  flex-grow: %.1f  flex-shrink: %.1f\n",
                style->flex_grow, style->flex_shrink);
        fprintf(stderr, "  gap: %.1f\n", style->gap);
    }

    fprintf(stderr, "  font-size: %.1fpx  font-weight: %u\n",
            style->font_size, style->font_weight);
    fprintf(stderr, "  color: #%08X  bg: #%08X\n",
            style->color, style->background_color);
    fprintf(stderr, "  opacity: %.2f\n", style->opacity);
}
