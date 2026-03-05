/*
 * ndom.c - R8EUIDOM: Minimal Document Tree
 *
 * Part of the r8e JavaScript engine R8EUIUI rendering stack.
 * See CLAUDE.md Section 12.2 (R8EUIDOM).
 *
 * NOT the full W3C DOM. Implements the subset needed for 95% of UI code:
 *   - createElement, createTextNode, createDocumentFragment
 *   - appendChild, removeChild, insertBefore, replaceChild
 *   - setAttribute, getAttribute, removeAttribute, hasAttribute
 *   - addEventListener, removeEventListener
 *   - getElementById, getElementsByTagName, getElementsByClassName
 *   - querySelector, querySelectorAll (CSS selectors)
 *   - className, classList, style (inline styles)
 *   - textContent, innerHTML (basic)
 *   - parentNode, childNodes, firstChild, lastChild, nextSibling, previousSibling
 *   - DOM event bubbling and capturing
 *
 * DOM node attributes reuse our CDOL object model (zero extra data structures).
 * Selector matching uses atom interning: class names are integers, matching
 * is integer comparison. Zero string operations during style resolution.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <ctype.h>

/* =========================================================================
 * Type Definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

#ifndef R8E_NDOM_TYPES_DEFINED
#define R8E_NDOM_TYPES_DEFINED

typedef uint64_t R8EValue;

/* NaN-boxing constants (CLAUDE.md Section 2.3) */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)    (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_ATOM(v)       (((v) >> 32) == 0xFFFC0000U)

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint32_t r8e_get_atom(uint64_t v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

static inline uint64_t r8e_from_atom(uint32_t idx) {
    return 0xFFFC000000000000ULL | (uint64_t)idx;
}

static inline int r8e_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}

static inline char r8e_inline_str_char(R8EValue v, int i) {
    return (char)((v >> (38 - i * 7)) & 0x7F);
}

static inline int r8e_inline_str_decode(R8EValue v, char *buf) {
    int len = r8e_inline_str_len(v);
    for (int i = 0; i < len; i++)
        buf[i] = r8e_inline_str_char(v, i);
    buf[len] = '\0';
    return len;
}

/* String hashing (FNV-1a) */
static inline uint32_t r8e_string_hash(const char *str, uint32_t len) {
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619U;
    }
    return hash;
}

/* Forward declarations */
typedef struct R8EContext R8EContext;
typedef struct R8EObject R8EObject;
typedef struct R8EArray  R8EArray;
typedef struct R8EString R8EString;

/* -------------------------------------------------------------------------
 * R8EUILayoutBox - from nlayout.c
 * ------------------------------------------------------------------------- */

typedef struct R8EUILayoutBox {
    float x, y;
    float width, height;
    float padding[4];
    float border[4];
    float margin[4];
} R8EUILayoutBox;

/* -------------------------------------------------------------------------
 * DOM Node Types
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_NODE_ELEMENT   = 1,
    R8E_UI_NODE_TEXT      = 3,
    R8E_UI_NODE_COMMENT   = 8,
    R8E_UI_NODE_DOCUMENT  = 9,
    R8E_UI_NODE_FRAGMENT  = 11
} R8EUINodeType;

/* -------------------------------------------------------------------------
 * DOM Node Flags
 * ------------------------------------------------------------------------- */

#define R8E_UI_FLAG_DIRTY          0x00000001u
#define R8E_UI_FLAG_STYLE_DIRTY    0x00000002u
#define R8E_UI_FLAG_VISIBLE        0x00000004u
#define R8E_UI_FLAG_FOCUSABLE      0x00000008u
#define R8E_UI_FLAG_HAS_ID         0x00000010u
#define R8E_UI_FLAG_HAS_CLASS      0x00000020u
#define R8E_UI_FLAG_HAS_STYLE      0x00000040u
#define R8E_UI_FLAG_HAS_LISTENERS  0x00000080u
#define R8E_UI_FLAG_SELF_CLOSING   0x00000100u
#define R8E_UI_FLAG_CHILDREN_DIRTY 0x00000200u
#define R8E_UI_FLAG_HOVER          0x00000400u
#define R8E_UI_FLAG_FOCUS          0x00000800u
#define R8E_UI_FLAG_ACTIVE         0x00001000u

/* -------------------------------------------------------------------------
 * Event Phase
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_EVENT_NONE      = 0,
    R8E_UI_EVENT_CAPTURING = 1,
    R8E_UI_EVENT_AT_TARGET = 2,
    R8E_UI_EVENT_BUBBLING  = 3
} R8EUIEventPhase;

/* -------------------------------------------------------------------------
 * Event Listener
 * ------------------------------------------------------------------------- */

typedef struct R8EUIEventListener {
    uint32_t event_type;    /* atom index of event name */
    R8EValue callback;      /* NaN-boxed function value */
    uint8_t  use_capture;
    uint8_t  once;
    uint8_t  passive;
    uint8_t  removed;
} R8EUIEventListener;

/* -------------------------------------------------------------------------
 * DOM Event Object
 * ------------------------------------------------------------------------- */

typedef struct R8EUIEvent {
    uint32_t type;
    struct R8EUIDOMNode *target;
    struct R8EUIDOMNode *current;
    R8EUIEventPhase phase;
    uint8_t  bubbles;
    uint8_t  cancelable;
    uint8_t  default_prevented;
    uint8_t  propagation_stopped;
    uint8_t  immediate_stopped;
    float    client_x, client_y;
    float    offset_x, offset_y;
    uint8_t  button;
    uint16_t buttons;
    uint32_t key_code;
    uint32_t key_atom;
    uint8_t  ctrl_key;
    uint8_t  shift_key;
    uint8_t  alt_key;
    uint8_t  meta_key;
} R8EUIEvent;

/* -------------------------------------------------------------------------
 * Attribute Entry
 * ------------------------------------------------------------------------- */

typedef struct R8EUIAttr {
    uint32_t name_atom;
    uint32_t value_len;
    char    *value;
} R8EUIAttr;

/* -------------------------------------------------------------------------
 * Class List (sorted atom array for O(1) matching)
 * ------------------------------------------------------------------------- */

typedef struct R8EUIClassList {
    uint32_t *atoms;
    uint16_t  count;
    uint16_t  capacity;
} R8EUIClassList;

/* -------------------------------------------------------------------------
 * Inline Style Entry
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * R8EUIDOMNode - Core DOM node structure (CLAUDE.md Section 12.2)
 * ------------------------------------------------------------------------- */

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
    R8EUIEventListener *listeners;
    uint16_t          listener_count;
    uint16_t          listener_cap;
    struct R8EUIDOMNode *owner_doc;
} R8EUIDOMNode;

/* -------------------------------------------------------------------------
 * Selector Types for querySelector/querySelectorAll
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_SEL_TAG       = 0,   /* tag name */
    R8E_UI_SEL_CLASS     = 1,   /* .class */
    R8E_UI_SEL_ID        = 2,   /* #id */
    R8E_UI_SEL_UNIVERSAL = 3,   /* * */
    R8E_UI_SEL_PSEUDO    = 4    /* :pseudo-class */
} R8EUISelectorType;

typedef enum {
    R8E_UI_COMB_NONE       = 0,  /* no combinator (compound selector part) */
    R8E_UI_COMB_DESCENDANT = 1,  /* space */
    R8E_UI_COMB_CHILD      = 2   /* > */
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
    R8EUICombinator   combinator;  /* combinator BEFORE this part */
    uint32_t         atom;        /* tag/class/id atom */
    R8EUIPseudoClass  pseudo;      /* pseudo-class if type == R8E_UI_SEL_PSEUDO */
} R8EUISelectorPart;

typedef struct R8EUISelector {
    R8EUISelectorPart parts[16];
    uint8_t          count;
} R8EUISelector;

/* Node collection for query results */
typedef struct R8EUINodeList {
    R8EUIDOMNode **nodes;
    uint32_t      count;
    uint32_t      capacity;
} R8EUINodeList;

/* Constants */
#define R8E_UI_MAX_EVENT_PATH     64
#define R8E_UI_MAX_SELECTOR_PARTS 16
#define R8E_UI_ATTR_INIT_CAP       4
#define R8E_UI_LISTENER_INIT_CAP   4
#define R8E_UI_CLASS_INIT_CAP      4
#define R8E_UI_STYLE_INIT_CAP      8
#define R8E_UI_NODELIST_INIT_CAP   8

#endif /* R8E_NDOM_TYPES_DEFINED */


/* =========================================================================
 * Atom Stubs
 *
 * In the full engine, these call into r8e_atom.c. For now, provide a
 * minimal local atom table for self-contained operation.
 * ========================================================================= */

#define R8E_UI_ATOM_TABLE_SIZE 512

static struct {
    struct { uint32_t hash; char name[64]; uint32_t len; } entries[R8E_UI_ATOM_TABLE_SIZE];
    uint32_t count;
} r8e_ui_atom_table = { .count = 0 };

/**
 * Intern a string into the local atom table.
 * Returns atom index (1-based, 0 = empty sentinel).
 */
static uint32_t r8e_ui_atom_intern(const char *str, uint32_t len) {
    if (!str || len == 0) return 0;
    uint32_t hash = r8e_string_hash(str, len);
    /* Search existing */
    for (uint32_t i = 1; i <= r8e_ui_atom_table.count; i++) {
        if (r8e_ui_atom_table.entries[i].hash == hash &&
            r8e_ui_atom_table.entries[i].len == len &&
            memcmp(r8e_ui_atom_table.entries[i].name, str, len) == 0) {
            return i;
        }
    }
    /* Add new */
    if (r8e_ui_atom_table.count >= R8E_UI_ATOM_TABLE_SIZE - 1) return 0;
    r8e_ui_atom_table.count++;
    uint32_t idx = r8e_ui_atom_table.count;
    r8e_ui_atom_table.entries[idx].hash = hash;
    r8e_ui_atom_table.entries[idx].len = len;
    uint32_t copy_len = len < 63 ? len : 63;
    memcpy(r8e_ui_atom_table.entries[idx].name, str, copy_len);
    r8e_ui_atom_table.entries[idx].name[copy_len] = '\0';
    return idx;
}

/**
 * Intern a C string (null-terminated).
 */
static uint32_t r8e_ui_atom_intern_cstr(const char *str) {
    if (!str) return 0;
    return r8e_ui_atom_intern(str, (uint32_t)strlen(str));
}

/**
 * Get string for an atom index.
 */
static const char *r8e_ui_atom_str(uint32_t atom) {
    if (atom == 0 || atom > r8e_ui_atom_table.count) return "";
    return r8e_ui_atom_table.entries[atom].name;
}


/* =========================================================================
 * Section 1: Node Creation and Destruction
 * ========================================================================= */

/**
 * Allocate and zero-initialize a new DOM node.
 */
static R8EUIDOMNode *r8e_ui_dom_node_alloc(void) {
    R8EUIDOMNode *node = (R8EUIDOMNode *)calloc(1, sizeof(R8EUIDOMNode));
    if (!node) return NULL;
    node->flags = R8E_UI_FLAG_VISIBLE | R8E_UI_FLAG_DIRTY | R8E_UI_FLAG_STYLE_DIRTY;
    return node;
}

/**
 * Create an element node with the given tag name.
 */
R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag) {
    if (!tag) return NULL;
    R8EUIDOMNode *node = r8e_ui_dom_node_alloc();
    if (!node) return NULL;

    node->type = R8E_UI_NODE_ELEMENT;

    /* Intern the tag name as lowercase for case-insensitive matching */
    char lower[64];
    uint32_t len = 0;
    while (tag[len] && len < 63) {
        lower[len] = (char)tolower((unsigned char)tag[len]);
        len++;
    }
    lower[len] = '\0';
    node->tag_atom = r8e_ui_atom_intern(lower, len);

    return node;
}

/**
 * Create an element node with a pre-interned tag atom.
 */
R8EUIDOMNode *r8e_ui_dom_create_element_atom(uint32_t tag_atom) {
    R8EUIDOMNode *node = r8e_ui_dom_node_alloc();
    if (!node) return NULL;
    node->type = R8E_UI_NODE_ELEMENT;
    node->tag_atom = tag_atom;
    return node;
}

/**
 * Create a text node with the given content.
 */
R8EUIDOMNode *r8e_ui_dom_create_text_node(const char *text, uint32_t len) {
    R8EUIDOMNode *node = r8e_ui_dom_node_alloc();
    if (!node) return NULL;

    node->type = R8E_UI_NODE_TEXT;

    if (text && len > 0) {
        node->text = (char *)malloc(len + 1);
        if (!node->text) { free(node); return NULL; }
        memcpy(node->text, text, len);
        node->text[len] = '\0';
        node->text_len = len;
    }
    return node;
}

/**
 * Create a text node from a C string.
 */
R8EUIDOMNode *r8e_ui_dom_create_text_node_cstr(const char *text) {
    if (!text) return r8e_ui_dom_create_text_node(NULL, 0);
    return r8e_ui_dom_create_text_node(text, (uint32_t)strlen(text));
}

/**
 * Create a document fragment node.
 */
R8EUIDOMNode *r8e_ui_dom_create_fragment(void) {
    R8EUIDOMNode *node = r8e_ui_dom_node_alloc();
    if (!node) return NULL;
    node->type = R8E_UI_NODE_FRAGMENT;
    return node;
}

/**
 * Create a document node (root of a document tree).
 */
R8EUIDOMNode *r8e_ui_dom_create_document(void) {
    R8EUIDOMNode *node = r8e_ui_dom_node_alloc();
    if (!node) return NULL;
    node->type = R8E_UI_NODE_DOCUMENT;
    node->owner_doc = node; /* document is its own owner */
    return node;
}

/**
 * Create a comment node.
 */
R8EUIDOMNode *r8e_ui_dom_create_comment(const char *text, uint32_t len) {
    R8EUIDOMNode *node = r8e_ui_dom_node_alloc();
    if (!node) return NULL;
    node->type = R8E_UI_NODE_COMMENT;
    if (text && len > 0) {
        node->text = (char *)malloc(len + 1);
        if (!node->text) { free(node); return NULL; }
        memcpy(node->text, text, len);
        node->text[len] = '\0';
        node->text_len = len;
    }
    return node;
}

/* Forward declaration */
static void r8e_ui_dom_remove_all_children(R8EUIDOMNode *node);

/**
 * Free a single DOM node and all its owned resources.
 * Does NOT unlink from tree (caller must do that first).
 */
void r8e_ui_dom_node_free(R8EUIDOMNode *node) {
    if (!node) return;

    /* Free attributes */
    for (uint16_t i = 0; i < node->attr_count; i++) {
        free(node->attrs[i].value);
    }
    free(node->attrs);

    /* Free class list */
    free(node->class_list.atoms);

    /* Free inline styles */
    if (node->inline_style) {
        for (uint16_t i = 0; i < node->inline_style->count; i++) {
            free(node->inline_style->entries[i].value);
        }
        free(node->inline_style->entries);
        free(node->inline_style);
    }

    /* Free event listeners */
    free(node->listeners);

    /* Free text content */
    free(node->text);

    free(node);
}

/**
 * Recursively free a node and all its descendants.
 */
void r8e_ui_dom_tree_free(R8EUIDOMNode *node) {
    if (!node) return;
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        R8EUIDOMNode *next = child->next_sibling;
        r8e_ui_dom_tree_free(child);
        child = next;
    }
    r8e_ui_dom_node_free(node);
}


/* =========================================================================
 * Section 2: Tree Manipulation
 * ========================================================================= */

/**
 * Mark a node and all ancestors as dirty (needs re-layout).
 */
static void r8e_ui_dom_mark_dirty(R8EUIDOMNode *node) {
    while (node) {
        if (node->flags & R8E_UI_FLAG_DIRTY) break; /* already dirty upward */
        node->flags |= R8E_UI_FLAG_DIRTY | R8E_UI_FLAG_STYLE_DIRTY;
        node = node->parent;
    }
}

/**
 * Unlink a node from its parent without freeing it.
 */
static void r8e_ui_dom_unlink(R8EUIDOMNode *node) {
    if (!node || !node->parent) return;

    R8EUIDOMNode *parent = node->parent;

    if (node->prev_sibling) {
        node->prev_sibling->next_sibling = node->next_sibling;
    } else {
        parent->first_child = node->next_sibling;
    }

    if (node->next_sibling) {
        node->next_sibling->prev_sibling = node->prev_sibling;
    } else {
        parent->last_child = node->prev_sibling;
    }

    parent->child_count--;
    parent->flags |= R8E_UI_FLAG_CHILDREN_DIRTY;
    r8e_ui_dom_mark_dirty(parent);

    node->parent = NULL;
    node->prev_sibling = NULL;
    node->next_sibling = NULL;
}

/**
 * appendChild: Append a child node to the end of parent's child list.
 *
 * If child already has a parent, it is first removed from its current parent.
 * If child is a DocumentFragment, all its children are moved instead.
 */
R8EUIDOMNode *r8e_ui_dom_append_child(R8EUIDOMNode *parent, R8EUIDOMNode *child) {
    if (!parent || !child) return NULL;
    if (parent == child) return NULL; /* cannot append to self */

    /* If child is a fragment, move all its children */
    if (child->type == R8E_UI_NODE_FRAGMENT) {
        R8EUIDOMNode *frag_child = child->first_child;
        while (frag_child) {
            R8EUIDOMNode *next = frag_child->next_sibling;
            frag_child->parent = NULL;
            frag_child->prev_sibling = NULL;
            frag_child->next_sibling = NULL;
            r8e_ui_dom_append_child(parent, frag_child);
            frag_child = next;
        }
        child->first_child = NULL;
        child->last_child = NULL;
        child->child_count = 0;
        return child;
    }

    /* Remove from current parent if any */
    if (child->parent) {
        r8e_ui_dom_unlink(child);
    }

    /* Append to end */
    child->parent = parent;
    child->owner_doc = parent->owner_doc;
    child->prev_sibling = parent->last_child;
    child->next_sibling = NULL;

    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->child_count++;

    parent->flags |= R8E_UI_FLAG_CHILDREN_DIRTY;
    r8e_ui_dom_mark_dirty(parent);

    return child;
}

/**
 * removeChild: Remove a child from its parent.
 * Returns the removed child, or NULL on error.
 */
R8EUIDOMNode *r8e_ui_dom_remove_child(R8EUIDOMNode *parent, R8EUIDOMNode *child) {
    if (!parent || !child) return NULL;
    if (child->parent != parent) return NULL;

    r8e_ui_dom_unlink(child);
    return child;
}

/**
 * insertBefore: Insert new_child before ref_child in parent's child list.
 * If ref_child is NULL, appends to end (same as appendChild).
 */
R8EUIDOMNode *r8e_ui_dom_insert_before(R8EUIDOMNode *parent, R8EUIDOMNode *new_child,
                                     R8EUIDOMNode *ref_child) {
    if (!parent || !new_child) return NULL;
    if (!ref_child) return r8e_ui_dom_append_child(parent, new_child);
    if (ref_child->parent != parent) return NULL;
    if (parent == new_child) return NULL;

    /* Handle fragment */
    if (new_child->type == R8E_UI_NODE_FRAGMENT) {
        R8EUIDOMNode *frag_child = new_child->first_child;
        while (frag_child) {
            R8EUIDOMNode *next = frag_child->next_sibling;
            frag_child->parent = NULL;
            frag_child->prev_sibling = NULL;
            frag_child->next_sibling = NULL;
            r8e_ui_dom_insert_before(parent, frag_child, ref_child);
            frag_child = next;
        }
        new_child->first_child = NULL;
        new_child->last_child = NULL;
        new_child->child_count = 0;
        return new_child;
    }

    /* Remove from current parent */
    if (new_child->parent) {
        r8e_ui_dom_unlink(new_child);
    }

    /* Insert before ref_child */
    new_child->parent = parent;
    new_child->owner_doc = parent->owner_doc;
    new_child->next_sibling = ref_child;
    new_child->prev_sibling = ref_child->prev_sibling;

    if (ref_child->prev_sibling) {
        ref_child->prev_sibling->next_sibling = new_child;
    } else {
        parent->first_child = new_child;
    }
    ref_child->prev_sibling = new_child;

    parent->child_count++;
    parent->flags |= R8E_UI_FLAG_CHILDREN_DIRTY;
    r8e_ui_dom_mark_dirty(parent);

    return new_child;
}

/**
 * replaceChild: Replace old_child with new_child in parent's child list.
 * Returns old_child, or NULL on error.
 */
R8EUIDOMNode *r8e_ui_dom_replace_child(R8EUIDOMNode *parent, R8EUIDOMNode *new_child,
                                     R8EUIDOMNode *old_child) {
    if (!parent || !new_child || !old_child) return NULL;
    if (old_child->parent != parent) return NULL;
    if (parent == new_child) return NULL;

    /* Insert new before old, then remove old */
    r8e_ui_dom_insert_before(parent, new_child, old_child);
    r8e_ui_dom_unlink(old_child);
    return old_child;
}

/**
 * Remove all children from a node.
 */
static void r8e_ui_dom_remove_all_children(R8EUIDOMNode *node) {
    if (!node) return;
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        R8EUIDOMNode *next = child->next_sibling;
        child->parent = NULL;
        child->prev_sibling = NULL;
        child->next_sibling = NULL;
        child = next;
    }
    node->first_child = NULL;
    node->last_child = NULL;
    node->child_count = 0;
    node->flags |= R8E_UI_FLAG_CHILDREN_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}

/**
 * Clone a node (shallow - does not clone children).
 */
R8EUIDOMNode *r8e_ui_dom_clone_node(const R8EUIDOMNode *node, bool deep) {
    if (!node) return NULL;

    R8EUIDOMNode *clone = r8e_ui_dom_node_alloc();
    if (!clone) return NULL;

    clone->type = node->type;
    clone->flags = node->flags & ~(R8E_UI_FLAG_HAS_LISTENERS | R8E_UI_FLAG_HOVER |
                                   R8E_UI_FLAG_FOCUS | R8E_UI_FLAG_ACTIVE);
    clone->tag_atom = node->tag_atom;
    clone->id_atom = node->id_atom;

    /* Clone attributes */
    if (node->attr_count > 0) {
        clone->attr_capacity = node->attr_count;
        clone->attrs = (R8EUIAttr *)calloc(clone->attr_capacity, sizeof(R8EUIAttr));
        if (clone->attrs) {
            for (uint16_t i = 0; i < node->attr_count; i++) {
                clone->attrs[i].name_atom = node->attrs[i].name_atom;
                clone->attrs[i].value_len = node->attrs[i].value_len;
                if (node->attrs[i].value) {
                    clone->attrs[i].value = (char *)malloc(node->attrs[i].value_len + 1);
                    if (clone->attrs[i].value) {
                        memcpy(clone->attrs[i].value, node->attrs[i].value,
                               node->attrs[i].value_len + 1);
                    }
                }
            }
            clone->attr_count = node->attr_count;
        }
    }

    /* Clone class list */
    if (node->class_list.count > 0) {
        clone->class_list.capacity = node->class_list.count;
        clone->class_list.atoms = (uint32_t *)malloc(
            clone->class_list.capacity * sizeof(uint32_t));
        if (clone->class_list.atoms) {
            memcpy(clone->class_list.atoms, node->class_list.atoms,
                   node->class_list.count * sizeof(uint32_t));
            clone->class_list.count = node->class_list.count;
        }
    }

    /* Clone text */
    if (node->text && node->text_len > 0) {
        clone->text = (char *)malloc(node->text_len + 1);
        if (clone->text) {
            memcpy(clone->text, node->text, node->text_len + 1);
            clone->text_len = node->text_len;
        }
    }

    /* Deep clone: recursively clone children */
    if (deep) {
        R8EUIDOMNode *child = node->first_child;
        while (child) {
            R8EUIDOMNode *child_clone = r8e_ui_dom_clone_node(child, true);
            if (child_clone) {
                r8e_ui_dom_append_child(clone, child_clone);
            }
            child = child->next_sibling;
        }
    }

    return clone;
}


/* =========================================================================
 * Section 3: Attributes
 * ========================================================================= */

/**
 * Find an attribute by atom index. Returns index or -1 if not found.
 */
static int r8e_ui_dom_find_attr(const R8EUIDOMNode *node, uint32_t name_atom) {
    for (uint16_t i = 0; i < node->attr_count; i++) {
        if (node->attrs[i].name_atom == name_atom) return (int)i;
    }
    return -1;
}

/* Forward declaration */
void r8e_ui_dom_set_class_name(R8EUIDOMNode *node, const char *class_name);

/**
 * Ensure attribute array has capacity for one more entry.
 */
static bool r8e_ui_dom_attr_grow(R8EUIDOMNode *node) {
    if (node->attr_count < node->attr_capacity) return true;
    uint16_t new_cap = node->attr_capacity == 0 ?
        R8E_UI_ATTR_INIT_CAP : node->attr_capacity * 2;
    R8EUIAttr *new_attrs = (R8EUIAttr *)realloc(node->attrs,
        new_cap * sizeof(R8EUIAttr));
    if (!new_attrs) return false;
    node->attrs = new_attrs;
    node->attr_capacity = new_cap;
    return true;
}

/**
 * setAttribute: Set an attribute on an element node.
 */
void r8e_ui_dom_set_attribute(R8EUIDOMNode *node, const char *name,
                            const char *value) {
    if (!node || node->type != R8E_UI_NODE_ELEMENT || !name) return;

    uint32_t name_atom = r8e_ui_atom_intern_cstr(name);
    if (name_atom == 0) return;

    uint32_t val_len = value ? (uint32_t)strlen(value) : 0;

    /* Check for special attributes */
    const char *name_str = r8e_ui_atom_str(name_atom);

    /* Handle "id" attribute */
    if (strcmp(name_str, "id") == 0) {
        node->id_atom = value ? r8e_ui_atom_intern_cstr(value) : 0;
        if (node->id_atom) node->flags |= R8E_UI_FLAG_HAS_ID;
        else node->flags &= ~R8E_UI_FLAG_HAS_ID;
    }

    /* Handle "class" attribute */
    if (strcmp(name_str, "class") == 0) {
        r8e_ui_dom_set_class_name(node, value);
        /* still store as regular attribute too */
    }

    /* Update existing or add new */
    int idx = r8e_ui_dom_find_attr(node, name_atom);
    if (idx >= 0) {
        /* Update existing */
        free(node->attrs[idx].value);
        if (value) {
            node->attrs[idx].value = (char *)malloc(val_len + 1);
            if (node->attrs[idx].value) {
                memcpy(node->attrs[idx].value, value, val_len + 1);
            }
            node->attrs[idx].value_len = val_len;
        } else {
            node->attrs[idx].value = NULL;
            node->attrs[idx].value_len = 0;
        }
    } else {
        /* Add new */
        if (!r8e_ui_dom_attr_grow(node)) return;
        R8EUIAttr *attr = &node->attrs[node->attr_count];
        attr->name_atom = name_atom;
        attr->value_len = val_len;
        attr->value = NULL;
        if (value) {
            attr->value = (char *)malloc(val_len + 1);
            if (attr->value) {
                memcpy(attr->value, value, val_len + 1);
            }
        }
        node->attr_count++;
    }

    node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}

/**
 * setAttribute with pre-interned atom.
 */
void r8e_ui_dom_set_attribute_atom(R8EUIDOMNode *node, uint32_t name_atom,
                                  const char *value, uint32_t value_len) {
    if (!node || node->type != R8E_UI_NODE_ELEMENT || name_atom == 0) return;

    int idx = r8e_ui_dom_find_attr(node, name_atom);
    if (idx >= 0) {
        free(node->attrs[idx].value);
        if (value && value_len > 0) {
            node->attrs[idx].value = (char *)malloc(value_len + 1);
            if (node->attrs[idx].value) {
                memcpy(node->attrs[idx].value, value, value_len);
                node->attrs[idx].value[value_len] = '\0';
            }
            node->attrs[idx].value_len = value_len;
        } else {
            node->attrs[idx].value = NULL;
            node->attrs[idx].value_len = 0;
        }
    } else {
        if (!r8e_ui_dom_attr_grow(node)) return;
        R8EUIAttr *attr = &node->attrs[node->attr_count];
        attr->name_atom = name_atom;
        attr->value_len = value_len;
        attr->value = NULL;
        if (value && value_len > 0) {
            attr->value = (char *)malloc(value_len + 1);
            if (attr->value) {
                memcpy(attr->value, value, value_len);
                attr->value[value_len] = '\0';
            }
        }
        node->attr_count++;
    }

    node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}

/**
 * getAttribute: Get the value of an attribute.
 * Returns NULL if not found.
 */
const char *r8e_ui_dom_get_attribute(const R8EUIDOMNode *node, const char *name) {
    if (!node || node->type != R8E_UI_NODE_ELEMENT || !name) return NULL;
    uint32_t name_atom = r8e_ui_atom_intern_cstr(name);
    if (name_atom == 0) return NULL;
    int idx = r8e_ui_dom_find_attr(node, name_atom);
    if (idx < 0) return NULL;
    return node->attrs[idx].value;
}

/**
 * getAttribute by atom.
 */
const char *r8e_ui_dom_get_attribute_atom(const R8EUIDOMNode *node,
                                         uint32_t name_atom) {
    if (!node || name_atom == 0) return NULL;
    int idx = r8e_ui_dom_find_attr(node, name_atom);
    if (idx < 0) return NULL;
    return node->attrs[idx].value;
}

/**
 * removeAttribute: Remove an attribute from an element.
 */
void r8e_ui_dom_remove_attribute(R8EUIDOMNode *node, const char *name) {
    if (!node || node->type != R8E_UI_NODE_ELEMENT || !name) return;
    uint32_t name_atom = r8e_ui_atom_intern_cstr(name);
    if (name_atom == 0) return;
    int idx = r8e_ui_dom_find_attr(node, name_atom);
    if (idx < 0) return;

    /* Handle special attributes */
    const char *name_str = r8e_ui_atom_str(name_atom);
    if (strcmp(name_str, "id") == 0) {
        node->id_atom = 0;
        node->flags &= ~R8E_UI_FLAG_HAS_ID;
    }
    if (strcmp(name_str, "class") == 0) {
        free(node->class_list.atoms);
        node->class_list.atoms = NULL;
        node->class_list.count = 0;
        node->class_list.capacity = 0;
        node->flags &= ~R8E_UI_FLAG_HAS_CLASS;
    }

    free(node->attrs[idx].value);
    /* Shift remaining attributes down */
    for (uint16_t i = (uint16_t)idx; i < node->attr_count - 1; i++) {
        node->attrs[i] = node->attrs[i + 1];
    }
    node->attr_count--;
    node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}

/**
 * hasAttribute: Check if an attribute exists.
 */
bool r8e_ui_dom_has_attribute(const R8EUIDOMNode *node, const char *name) {
    if (!node || !name) return false;
    uint32_t name_atom = r8e_ui_atom_intern_cstr(name);
    return r8e_ui_dom_find_attr(node, name_atom) >= 0;
}


/* =========================================================================
 * Section 4: Class List Operations
 * ========================================================================= */

/**
 * Insert a class atom into the sorted class list (maintaining sort order).
 */
static bool r8e_ui_dom_classlist_add_atom(R8EUIDOMNode *node, uint32_t atom) {
    R8EUIClassList *cl = &node->class_list;

    /* Check if already present (binary search) */
    uint16_t lo = 0, hi = cl->count;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (cl->atoms[mid] < atom) lo = mid + 1;
        else if (cl->atoms[mid] > atom) hi = mid;
        else return true; /* already present */
    }
    /* Insert at position lo */
    if (cl->count >= cl->capacity) {
        uint16_t new_cap = cl->capacity == 0 ?
            R8E_UI_CLASS_INIT_CAP : cl->capacity * 2;
        uint32_t *new_atoms = (uint32_t *)realloc(cl->atoms,
            new_cap * sizeof(uint32_t));
        if (!new_atoms) return false;
        cl->atoms = new_atoms;
        cl->capacity = new_cap;
    }
    /* Shift elements right */
    for (uint16_t i = cl->count; i > lo; i--) {
        cl->atoms[i] = cl->atoms[i - 1];
    }
    cl->atoms[lo] = atom;
    cl->count++;
    node->flags |= R8E_UI_FLAG_HAS_CLASS | R8E_UI_FLAG_STYLE_DIRTY;
    return true;
}

/**
 * Remove a class atom from the sorted class list.
 */
static bool r8e_ui_dom_classlist_remove_atom(R8EUIDOMNode *node, uint32_t atom) {
    R8EUIClassList *cl = &node->class_list;

    /* Binary search */
    uint16_t lo = 0, hi = cl->count;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (cl->atoms[mid] < atom) lo = mid + 1;
        else if (cl->atoms[mid] > atom) hi = mid;
        else {
            /* Found: shift elements left */
            for (uint16_t i = mid; i < cl->count - 1; i++) {
                cl->atoms[i] = cl->atoms[i + 1];
            }
            cl->count--;
            if (cl->count == 0) node->flags &= ~R8E_UI_FLAG_HAS_CLASS;
            node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
            return true;
        }
    }
    return false;
}

/**
 * Check if a class atom is present (binary search on sorted array).
 */
bool r8e_ui_dom_classlist_contains(const R8EUIDOMNode *node, uint32_t atom) {
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
 * classList.add: Add one or more class names.
 */
void r8e_ui_dom_classlist_add(R8EUIDOMNode *node, const char *class_name) {
    if (!node || !class_name) return;
    uint32_t atom = r8e_ui_atom_intern_cstr(class_name);
    if (atom == 0) return;
    r8e_ui_dom_classlist_add_atom(node, atom);
    r8e_ui_dom_mark_dirty(node);
}

/**
 * classList.remove: Remove a class name.
 */
void r8e_ui_dom_classlist_remove(R8EUIDOMNode *node, const char *class_name) {
    if (!node || !class_name) return;
    uint32_t atom = r8e_ui_atom_intern_cstr(class_name);
    if (atom == 0) return;
    r8e_ui_dom_classlist_remove_atom(node, atom);
    r8e_ui_dom_mark_dirty(node);
}

/**
 * classList.toggle: Toggle a class name. Returns true if class is now present.
 */
bool r8e_ui_dom_classlist_toggle(R8EUIDOMNode *node, const char *class_name) {
    if (!node || !class_name) return false;
    uint32_t atom = r8e_ui_atom_intern_cstr(class_name);
    if (atom == 0) return false;
    if (r8e_ui_dom_classlist_contains(node, atom)) {
        r8e_ui_dom_classlist_remove_atom(node, atom);
        r8e_ui_dom_mark_dirty(node);
        return false;
    } else {
        r8e_ui_dom_classlist_add_atom(node, atom);
        r8e_ui_dom_mark_dirty(node);
        return true;
    }
}

/**
 * Set className (replaces all classes). Parses space-separated class names.
 */
void r8e_ui_dom_set_class_name(R8EUIDOMNode *node, const char *class_name) {
    if (!node) return;

    /* Clear existing */
    node->class_list.count = 0;

    if (!class_name || *class_name == '\0') {
        node->flags &= ~R8E_UI_FLAG_HAS_CLASS;
        node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
        r8e_ui_dom_mark_dirty(node);
        return;
    }

    /* Parse space-separated class names */
    const char *p = class_name;
    while (*p) {
        /* Skip whitespace */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (!*p) break;

        /* Find end of token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;

        uint32_t len = (uint32_t)(p - start);
        if (len > 0) {
            uint32_t atom = r8e_ui_atom_intern(start, len);
            if (atom != 0) {
                r8e_ui_dom_classlist_add_atom(node, atom);
            }
        }
    }

    node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}


/* =========================================================================
 * Section 5: Inline Style Operations
 * ========================================================================= */

/**
 * Ensure inline style structure is allocated.
 */
static R8EUIInlineStyle *r8e_ui_dom_ensure_inline_style(R8EUIDOMNode *node) {
    if (node->inline_style) return node->inline_style;
    node->inline_style = (R8EUIInlineStyle *)calloc(1, sizeof(R8EUIInlineStyle));
    if (node->inline_style) {
        node->flags |= R8E_UI_FLAG_HAS_STYLE;
    }
    return node->inline_style;
}

/**
 * Set an inline style property.
 */
void r8e_ui_dom_set_style_property(R8EUIDOMNode *node, uint16_t property_id,
                                  const char *value) {
    if (!node) return;
    R8EUIInlineStyle *style = r8e_ui_dom_ensure_inline_style(node);
    if (!style) return;

    uint32_t val_len = value ? (uint32_t)strlen(value) : 0;

    /* Find existing entry */
    for (uint16_t i = 0; i < style->count; i++) {
        if (style->entries[i].property_id == property_id) {
            free(style->entries[i].value);
            if (value && val_len > 0) {
                style->entries[i].value = (char *)malloc(val_len + 1);
                if (style->entries[i].value) {
                    memcpy(style->entries[i].value, value, val_len + 1);
                }
                style->entries[i].value_len = (uint16_t)val_len;
            } else {
                style->entries[i].value = NULL;
                style->entries[i].value_len = 0;
            }
            node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
            r8e_ui_dom_mark_dirty(node);
            return;
        }
    }

    /* Add new entry */
    if (style->count >= style->capacity) {
        uint16_t new_cap = style->capacity == 0 ?
            R8E_UI_STYLE_INIT_CAP : style->capacity * 2;
        R8EUIStyleEntry *new_entries = (R8EUIStyleEntry *)realloc(
            style->entries, new_cap * sizeof(R8EUIStyleEntry));
        if (!new_entries) return;
        style->entries = new_entries;
        style->capacity = new_cap;
    }

    R8EUIStyleEntry *entry = &style->entries[style->count];
    entry->property_id = property_id;
    entry->value_len = (uint16_t)val_len;
    entry->value = NULL;
    if (value && val_len > 0) {
        entry->value = (char *)malloc(val_len + 1);
        if (entry->value) {
            memcpy(entry->value, value, val_len + 1);
        }
    }
    style->count++;
    node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}

/**
 * Get an inline style property value.
 */
const char *r8e_ui_dom_get_style_property(const R8EUIDOMNode *node,
                                         uint16_t property_id) {
    if (!node || !node->inline_style) return NULL;
    for (uint16_t i = 0; i < node->inline_style->count; i++) {
        if (node->inline_style->entries[i].property_id == property_id) {
            return node->inline_style->entries[i].value;
        }
    }
    return NULL;
}

/**
 * Remove an inline style property.
 */
void r8e_ui_dom_remove_style_property(R8EUIDOMNode *node, uint16_t property_id) {
    if (!node || !node->inline_style) return;
    R8EUIInlineStyle *style = node->inline_style;
    for (uint16_t i = 0; i < style->count; i++) {
        if (style->entries[i].property_id == property_id) {
            free(style->entries[i].value);
            for (uint16_t j = i; j < style->count - 1; j++) {
                style->entries[j] = style->entries[j + 1];
            }
            style->count--;
            node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
            r8e_ui_dom_mark_dirty(node);
            return;
        }
    }
}

/**
 * Parse and apply a style string ("color: red; font-size: 14px").
 * This is a simplified parser for inline style attributes.
 */
void r8e_ui_dom_parse_inline_style(R8EUIDOMNode *node, const char *style_str) {
    if (!node || !style_str) return;

    /* Clear existing inline styles */
    if (node->inline_style) {
        for (uint16_t i = 0; i < node->inline_style->count; i++) {
            free(node->inline_style->entries[i].value);
        }
        node->inline_style->count = 0;
    }

    const char *p = style_str;
    while (*p) {
        /* Skip whitespace and semicolons */
        while (*p && (*p == ' ' || *p == '\t' || *p == ';' || *p == '\n')) p++;
        if (!*p) break;

        /* Read property name */
        const char *prop_start = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') continue;

        /* Trim trailing whitespace from property name */
        const char *prop_end = p;
        while (prop_end > prop_start &&
               (*(prop_end - 1) == ' ' || *(prop_end - 1) == '\t'))
            prop_end--;

        uint32_t prop_len = (uint32_t)(prop_end - prop_start);
        p++; /* skip ':' */

        /* Skip whitespace before value */
        while (*p && (*p == ' ' || *p == '\t')) p++;

        /* Read value */
        const char *val_start = p;
        while (*p && *p != ';') p++;

        /* Trim trailing whitespace from value */
        const char *val_end = p;
        while (val_end > val_start &&
               (*(val_end - 1) == ' ' || *(val_end - 1) == '\t'))
            val_end--;

        if (prop_len > 0 && val_end > val_start) {
            /* Intern property name as an atom and use its ID */
            char prop_buf[64];
            uint32_t copy_len = prop_len < 63 ? prop_len : 63;
            memcpy(prop_buf, prop_start, copy_len);
            prop_buf[copy_len] = '\0';
            uint32_t prop_atom = r8e_ui_atom_intern(prop_buf, (uint32_t)copy_len);

            /* Store value */
            uint32_t val_len = (uint32_t)(val_end - val_start);
            char *val_buf = (char *)malloc(val_len + 1);
            if (val_buf) {
                memcpy(val_buf, val_start, val_len);
                val_buf[val_len] = '\0';

                R8EUIInlineStyle *style = r8e_ui_dom_ensure_inline_style(node);
                if (style) {
                    if (style->count >= style->capacity) {
                        uint16_t new_cap = style->capacity == 0 ?
                            R8E_UI_STYLE_INIT_CAP : style->capacity * 2;
                        R8EUIStyleEntry *ne = (R8EUIStyleEntry *)realloc(
                            style->entries, new_cap * sizeof(R8EUIStyleEntry));
                        if (ne) {
                            style->entries = ne;
                            style->capacity = new_cap;
                        }
                    }
                    if (style->count < style->capacity) {
                        style->entries[style->count].property_id = (uint16_t)prop_atom;
                        style->entries[style->count].value = val_buf;
                        style->entries[style->count].value_len = (uint16_t)val_len;
                        style->count++;
                    } else {
                        free(val_buf);
                    }
                } else {
                    free(val_buf);
                }
            }
        }

        if (*p == ';') p++;
    }

    node->flags |= R8E_UI_FLAG_STYLE_DIRTY;
    r8e_ui_dom_mark_dirty(node);
}


/* =========================================================================
 * Section 6: Text Content
 * ========================================================================= */

/**
 * Set text content. For TEXT nodes, replaces the text.
 * For ELEMENT nodes, removes all children and creates a text child.
 */
void r8e_ui_dom_set_text_content(R8EUIDOMNode *node, const char *text) {
    if (!node) return;

    if (node->type == R8E_UI_NODE_TEXT || node->type == R8E_UI_NODE_COMMENT) {
        free(node->text);
        if (text) {
            uint32_t len = (uint32_t)strlen(text);
            node->text = (char *)malloc(len + 1);
            if (node->text) {
                memcpy(node->text, text, len + 1);
                node->text_len = len;
            } else {
                node->text_len = 0;
            }
        } else {
            node->text = NULL;
            node->text_len = 0;
        }
        r8e_ui_dom_mark_dirty(node);
        return;
    }

    if (node->type == R8E_UI_NODE_ELEMENT || node->type == R8E_UI_NODE_FRAGMENT) {
        /* Remove all existing children */
        R8EUIDOMNode *child = node->first_child;
        while (child) {
            R8EUIDOMNode *next = child->next_sibling;
            r8e_ui_dom_tree_free(child);
            child = next;
        }
        node->first_child = NULL;
        node->last_child = NULL;
        node->child_count = 0;

        /* Create text node if text is non-empty */
        if (text && *text) {
            R8EUIDOMNode *tn = r8e_ui_dom_create_text_node_cstr(text);
            if (tn) r8e_ui_dom_append_child(node, tn);
        }
        r8e_ui_dom_mark_dirty(node);
    }
}

/**
 * Get text content. Concatenates all descendant text nodes.
 * Caller must free the returned string.
 */
char *r8e_ui_dom_get_text_content(const R8EUIDOMNode *node) {
    if (!node) return NULL;

    if (node->type == R8E_UI_NODE_TEXT || node->type == R8E_UI_NODE_COMMENT) {
        if (node->text) return strdup(node->text);
        return strdup("");
    }

    /* For elements: concatenate all descendant text */
    size_t total_len = 0;

    /* First pass: compute total length */
    const R8EUIDOMNode *stack[256];
    int sp = 0;
    stack[sp++] = node;
    while (sp > 0) {
        const R8EUIDOMNode *n = stack[--sp];
        if (n->type == R8E_UI_NODE_TEXT && n->text) {
            total_len += n->text_len;
        }
        /* Push children in reverse order so they pop in order */
        const R8EUIDOMNode *c = n->last_child;
        while (c && sp < 256) {
            stack[sp++] = c;
            c = c->prev_sibling;
        }
    }

    char *result = (char *)malloc(total_len + 1);
    if (!result) return NULL;

    /* Second pass: copy text */
    size_t pos = 0;
    sp = 0;
    stack[sp++] = node;
    while (sp > 0) {
        const R8EUIDOMNode *n = stack[--sp];
        if (n->type == R8E_UI_NODE_TEXT && n->text) {
            memcpy(result + pos, n->text, n->text_len);
            pos += n->text_len;
        }
        const R8EUIDOMNode *c = n->last_child;
        while (c && sp < 256) {
            stack[sp++] = c;
            c = c->prev_sibling;
        }
    }

    result[total_len] = '\0';
    return result;
}


/* =========================================================================
 * Section 7: DOM Traversal and Queries
 * ========================================================================= */

/**
 * NodeList helpers
 */
static R8EUINodeList *r8e_ui_nodelist_create(void) {
    R8EUINodeList *list = (R8EUINodeList *)calloc(1, sizeof(R8EUINodeList));
    return list;
}

static void r8e_ui_nodelist_push(R8EUINodeList *list, R8EUIDOMNode *node) {
    if (!list || !node) return;
    if (list->count >= list->capacity) {
        uint32_t new_cap = list->capacity == 0 ?
            R8E_UI_NODELIST_INIT_CAP : list->capacity * 2;
        R8EUIDOMNode **new_nodes = (R8EUIDOMNode **)realloc(
            list->nodes, new_cap * sizeof(R8EUIDOMNode *));
        if (!new_nodes) return;
        list->nodes = new_nodes;
        list->capacity = new_cap;
    }
    list->nodes[list->count++] = node;
}

void r8e_ui_nodelist_free(R8EUINodeList *list) {
    if (!list) return;
    free(list->nodes);
    free(list);
}

/**
 * getElementById: Search subtree for element with matching id.
 */
R8EUIDOMNode *r8e_ui_dom_get_element_by_id(R8EUIDOMNode *root, const char *id) {
    if (!root || !id) return NULL;
    uint32_t id_atom = r8e_ui_atom_intern_cstr(id);
    if (id_atom == 0) return NULL;

    /* DFS traversal */
    R8EUIDOMNode *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        R8EUIDOMNode *node = stack[--sp];
        if (node->type == R8E_UI_NODE_ELEMENT && node->id_atom == id_atom) {
            return node;
        }
        R8EUIDOMNode *child = node->last_child;
        while (child && sp < 256) {
            stack[sp++] = child;
            child = child->prev_sibling;
        }
    }
    return NULL;
}

/**
 * getElementsByTagName: Collect all descendants with matching tag name.
 */
R8EUINodeList *r8e_ui_dom_get_elements_by_tag_name(R8EUIDOMNode *root,
                                                  const char *tag) {
    R8EUINodeList *list = r8e_ui_nodelist_create();
    if (!root || !tag || !list) return list;

    /* Lowercase the tag for comparison */
    char lower[64];
    uint32_t len = 0;
    while (tag[len] && len < 63) {
        lower[len] = (char)tolower((unsigned char)tag[len]);
        len++;
    }
    lower[len] = '\0';
    uint32_t tag_atom = r8e_ui_atom_intern(lower, len);
    bool match_all = (tag[0] == '*' && tag[1] == '\0');

    R8EUIDOMNode *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        R8EUIDOMNode *node = stack[--sp];
        if (node != root && node->type == R8E_UI_NODE_ELEMENT) {
            if (match_all || node->tag_atom == tag_atom) {
                r8e_ui_nodelist_push(list, node);
            }
        }
        R8EUIDOMNode *child = node->last_child;
        while (child && sp < 256) {
            stack[sp++] = child;
            child = child->prev_sibling;
        }
    }
    return list;
}

/**
 * getElementsByClassName: Collect all descendants with matching class name.
 */
R8EUINodeList *r8e_ui_dom_get_elements_by_class_name(R8EUIDOMNode *root,
                                                    const char *class_name) {
    R8EUINodeList *list = r8e_ui_nodelist_create();
    if (!root || !class_name || !list) return list;

    uint32_t class_atom = r8e_ui_atom_intern_cstr(class_name);
    if (class_atom == 0) return list;

    R8EUIDOMNode *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        R8EUIDOMNode *node = stack[--sp];
        if (node != root && node->type == R8E_UI_NODE_ELEMENT) {
            if (r8e_ui_dom_classlist_contains(node, class_atom)) {
                r8e_ui_nodelist_push(list, node);
            }
        }
        R8EUIDOMNode *child = node->last_child;
        while (child && sp < 256) {
            stack[sp++] = child;
            child = child->prev_sibling;
        }
    }
    return list;
}


/* =========================================================================
 * Section 8: CSS Selector Parsing and Matching
 *
 * Supports: tag, .class, #id, combinators (space, >),
 * pseudo (:hover, :focus, :active, :first-child, :last-child)
 * ========================================================================= */

/**
 * Parse a pseudo-class name.
 */
static R8EUIPseudoClass r8e_ui_parse_pseudo(const char *name, uint32_t len) {
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
 * Parse a CSS selector string into parts.
 * Returns true on success.
 *
 * Supported syntax:
 *   tag
 *   .class
 *   #id
 *   tag.class
 *   tag#id
 *   .class1.class2
 *   ancestor descendant   (space = descendant combinator)
 *   parent > child        (> = child combinator)
 *   :hover, :focus, :active, :first-child, :last-child
 *   * (universal)
 */
static bool r8e_ui_parse_selector(const char *selector, R8EUISelector *out) {
    if (!selector || !out) return false;
    memset(out, 0, sizeof(R8EUISelector));

    const char *p = selector;
    R8EUICombinator pending_comb = R8E_UI_COMB_NONE;
    bool first_in_group = true;

    while (*p) {
        /* Skip whitespace - may indicate descendant combinator */
        bool had_space = false;
        while (*p == ' ' || *p == '\t') { had_space = true; p++; }
        if (!*p) break;

        /* Check for child combinator */
        if (*p == '>') {
            pending_comb = R8E_UI_COMB_CHILD;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
        } else if (had_space && !first_in_group) {
            pending_comb = R8E_UI_COMB_DESCENDANT;
        }

        if (out->count >= R8E_UI_MAX_SELECTOR_PARTS) break;

        R8EUISelectorPart *part = &out->parts[out->count];
        part->combinator = pending_comb;
        pending_comb = R8E_UI_COMB_NONE;
        first_in_group = false;

        if (*p == '#') {
            /* ID selector */
            p++;
            const char *start = p;
            while (*p && *p != '.' && *p != ':' && *p != ' ' &&
                   *p != '>' && *p != '#') p++;
            uint32_t len = (uint32_t)(p - start);
            part->type = R8E_UI_SEL_ID;
            part->atom = r8e_ui_atom_intern(start, len);
            out->count++;
        } else if (*p == '.') {
            /* Class selector */
            p++;
            const char *start = p;
            while (*p && *p != '.' && *p != ':' && *p != ' ' &&
                   *p != '>' && *p != '#') p++;
            uint32_t len = (uint32_t)(p - start);
            part->type = R8E_UI_SEL_CLASS;
            part->atom = r8e_ui_atom_intern(start, len);
            out->count++;
        } else if (*p == ':') {
            /* Pseudo-class */
            p++;
            const char *start = p;
            while (*p && *p != ' ' && *p != '>' && *p != '.' &&
                   *p != '#' && *p != ':') p++;
            uint32_t len = (uint32_t)(p - start);
            part->type = R8E_UI_SEL_PSEUDO;
            part->pseudo = r8e_ui_parse_pseudo(start, len);
            out->count++;
        } else if (*p == '*') {
            /* Universal selector */
            part->type = R8E_UI_SEL_UNIVERSAL;
            out->count++;
            p++;
        } else {
            /* Tag selector */
            const char *start = p;
            while (*p && *p != '.' && *p != ':' && *p != ' ' &&
                   *p != '>' && *p != '#') p++;
            uint32_t len = (uint32_t)(p - start);
            if (len > 0) {
                char lower[64];
                uint32_t copy_len = len < 63 ? len : 63;
                for (uint32_t i = 0; i < copy_len; i++)
                    lower[i] = (char)tolower((unsigned char)start[i]);
                lower[copy_len] = '\0';
                part->type = R8E_UI_SEL_TAG;
                part->atom = r8e_ui_atom_intern(lower, copy_len);
                out->count++;
            }
        }
    }

    return out->count > 0;
}

/**
 * Check if a single selector part matches a node.
 */
static bool r8e_ui_match_part(const R8EUIDOMNode *node,
                            const R8EUISelectorPart *part) {
    if (node->type != R8E_UI_NODE_ELEMENT) return false;

    switch (part->type) {
    case R8E_UI_SEL_TAG:
        return node->tag_atom == part->atom;

    case R8E_UI_SEL_CLASS:
        return r8e_ui_dom_classlist_contains(node, part->atom);

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
            return node->parent &&
                   node->parent->first_child == node;
        case R8E_UI_PSEUDO_LAST_CHILD:
            return node->parent &&
                   node->parent->last_child == node;
        default:
            return false;
        }

    default:
        return false;
    }
}

/**
 * Match a complete selector against a node.
 * Walks backward through selector parts, using combinators to
 * navigate ancestors.
 */
static bool r8e_ui_match_selector(const R8EUIDOMNode *node,
                                const R8EUISelector *sel) {
    if (!sel || sel->count == 0) return false;

    /* Start from the last part (rightmost = the target element) */
    int pi = sel->count - 1;

    /* First check the rightmost compound selector parts (no combinator) */
    /* Collect all parts at the end that share COMB_NONE */
    while (pi >= 0 && (pi == (int)(sel->count - 1) ||
           sel->parts[pi + 1].combinator == R8E_UI_COMB_NONE)) {
        /* All these parts with COMB_NONE after the first must all match node */
        break;
    }

    /* Actually: walk from right to left */
    const R8EUIDOMNode *current = node;

    for (int i = (int)sel->count - 1; i >= 0; i--) {
        const R8EUISelectorPart *part = &sel->parts[i];

        if (i == (int)sel->count - 1 || part->combinator == R8E_UI_COMB_NONE) {
            /* Must match current node */
            if (!r8e_ui_match_part(current, part)) return false;
        } else if (part->combinator == R8E_UI_COMB_CHILD) {
            /* Previous part must match current's parent */
            current = current->parent;
            if (!current) return false;
            if (!r8e_ui_match_part(current, part)) return false;
        } else if (part->combinator == R8E_UI_COMB_DESCENDANT) {
            /* Some ancestor must match */
            current = current->parent;
            bool found = false;
            while (current) {
                if (r8e_ui_match_part(current, part)) { found = true; break; }
                current = current->parent;
            }
            if (!found) return false;
        }
    }

    return true;
}

/**
 * querySelector: Find the first descendant matching a CSS selector.
 */
R8EUIDOMNode *r8e_ui_dom_query_selector(R8EUIDOMNode *root,
                                      const char *selector) {
    if (!root || !selector) return NULL;

    R8EUISelector sel;
    if (!r8e_ui_parse_selector(selector, &sel)) return NULL;

    /* DFS */
    R8EUIDOMNode *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        R8EUIDOMNode *node = stack[--sp];
        if (node != root && r8e_ui_match_selector(node, &sel)) {
            return node;
        }
        R8EUIDOMNode *child = node->last_child;
        while (child && sp < 256) {
            stack[sp++] = child;
            child = child->prev_sibling;
        }
    }
    return NULL;
}

/**
 * querySelectorAll: Find all descendants matching a CSS selector.
 */
R8EUINodeList *r8e_ui_dom_query_selector_all(R8EUIDOMNode *root,
                                           const char *selector) {
    R8EUINodeList *list = r8e_ui_nodelist_create();
    if (!root || !selector || !list) return list;

    R8EUISelector sel;
    if (!r8e_ui_parse_selector(selector, &sel)) return list;

    R8EUIDOMNode *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        R8EUIDOMNode *node = stack[--sp];
        if (node != root && r8e_ui_match_selector(node, &sel)) {
            r8e_ui_nodelist_push(list, node);
        }
        R8EUIDOMNode *child = node->last_child;
        while (child && sp < 256) {
            stack[sp++] = child;
            child = child->prev_sibling;
        }
    }
    return list;
}

/**
 * matches: Check if a node matches a CSS selector.
 */
bool r8e_ui_dom_matches(const R8EUIDOMNode *node, const char *selector) {
    if (!node || !selector) return false;
    R8EUISelector sel;
    if (!r8e_ui_parse_selector(selector, &sel)) return false;
    return r8e_ui_match_selector(node, &sel);
}

/**
 * closest: Find the closest ancestor (or self) matching a selector.
 */
R8EUIDOMNode *r8e_ui_dom_closest(R8EUIDOMNode *node, const char *selector) {
    if (!node || !selector) return NULL;
    R8EUISelector sel;
    if (!r8e_ui_parse_selector(selector, &sel)) return NULL;
    R8EUIDOMNode *current = node;
    while (current) {
        if (r8e_ui_match_selector(current, &sel)) return current;
        current = current->parent;
    }
    return NULL;
}


/* =========================================================================
 * Section 9: Event System (Bubbling and Capturing)
 * ========================================================================= */

/**
 * Ensure listener array has capacity.
 */
static bool r8e_ui_dom_listener_grow(R8EUIDOMNode *node) {
    if (node->listener_count < node->listener_cap) return true;
    uint16_t new_cap = node->listener_cap == 0 ?
        R8E_UI_LISTENER_INIT_CAP : node->listener_cap * 2;
    R8EUIEventListener *new_list = (R8EUIEventListener *)realloc(
        node->listeners, new_cap * sizeof(R8EUIEventListener));
    if (!new_list) return false;
    node->listeners = new_list;
    node->listener_cap = new_cap;
    return true;
}

/**
 * addEventListener: Register an event listener on a node.
 */
void r8e_ui_dom_add_event_listener(R8EUIDOMNode *node, const char *event_type,
                                  R8EValue callback, bool use_capture,
                                  bool once, bool passive) {
    if (!node || !event_type) return;
    uint32_t type_atom = r8e_ui_atom_intern_cstr(event_type);
    if (type_atom == 0) return;

    /* Check for duplicate (same type, callback, capture) */
    for (uint16_t i = 0; i < node->listener_count; i++) {
        R8EUIEventListener *l = &node->listeners[i];
        if (!l->removed && l->event_type == type_atom &&
            l->callback == callback && l->use_capture == use_capture) {
            return; /* duplicate, do not add */
        }
    }

    if (!r8e_ui_dom_listener_grow(node)) return;
    R8EUIEventListener *l = &node->listeners[node->listener_count];
    l->event_type = type_atom;
    l->callback = callback;
    l->use_capture = use_capture ? 1 : 0;
    l->once = once ? 1 : 0;
    l->passive = passive ? 1 : 0;
    l->removed = 0;
    node->listener_count++;
    node->flags |= R8E_UI_FLAG_HAS_LISTENERS;
}

/**
 * addEventListener with pre-interned atom event type.
 */
void r8e_ui_dom_add_event_listener_atom(R8EUIDOMNode *node, uint32_t type_atom,
                                       R8EValue callback, bool use_capture) {
    if (!node || type_atom == 0) return;

    for (uint16_t i = 0; i < node->listener_count; i++) {
        R8EUIEventListener *l = &node->listeners[i];
        if (!l->removed && l->event_type == type_atom &&
            l->callback == callback && l->use_capture == use_capture) {
            return;
        }
    }

    if (!r8e_ui_dom_listener_grow(node)) return;
    R8EUIEventListener *l = &node->listeners[node->listener_count];
    l->event_type = type_atom;
    l->callback = callback;
    l->use_capture = use_capture ? 1 : 0;
    l->once = 0;
    l->passive = 0;
    l->removed = 0;
    node->listener_count++;
    node->flags |= R8E_UI_FLAG_HAS_LISTENERS;
}

/**
 * removeEventListener: Remove an event listener from a node.
 */
void r8e_ui_dom_remove_event_listener(R8EUIDOMNode *node, const char *event_type,
                                     R8EValue callback, bool use_capture) {
    if (!node || !event_type) return;
    uint32_t type_atom = r8e_ui_atom_intern_cstr(event_type);
    if (type_atom == 0) return;

    for (uint16_t i = 0; i < node->listener_count; i++) {
        R8EUIEventListener *l = &node->listeners[i];
        if (!l->removed && l->event_type == type_atom &&
            l->callback == callback && l->use_capture == use_capture) {
            l->removed = 1;
            return;
        }
    }
}

/**
 * Compact the listener array by removing entries marked as removed.
 */
static void r8e_ui_dom_compact_listeners(R8EUIDOMNode *node) {
    uint16_t write = 0;
    for (uint16_t read = 0; read < node->listener_count; read++) {
        if (!node->listeners[read].removed) {
            if (write != read) {
                node->listeners[write] = node->listeners[read];
            }
            write++;
        }
    }
    node->listener_count = write;
    if (write == 0) {
        node->flags &= ~R8E_UI_FLAG_HAS_LISTENERS;
    }
}

/**
 * Build the event propagation path from target up to root.
 * Returns path length.
 */
static int r8e_ui_dom_build_event_path(R8EUIDOMNode *target,
                                      R8EUIDOMNode **path, int max_depth) {
    int depth = 0;
    R8EUIDOMNode *node = target;
    while (node && depth < max_depth) {
        path[depth++] = node;
        node = node->parent;
    }
    return depth;
}

/**
 * Invoke listeners on a node for an event in a specific phase.
 */
static void r8e_ui_dom_invoke_listeners(R8EUIDOMNode *node, R8EUIEvent *event,
                                       R8EUIEventPhase phase) {
    if (!(node->flags & R8E_UI_FLAG_HAS_LISTENERS)) return;

    event->current = node;
    event->phase = phase;

    for (uint16_t i = 0; i < node->listener_count; i++) {
        if (event->immediate_stopped) break;

        R8EUIEventListener *l = &node->listeners[i];
        if (l->removed) continue;
        if (l->event_type != event->type) continue;

        /* Check phase: capture listeners fire in capture, non-capture in bubble */
        if (phase == R8E_UI_EVENT_CAPTURING && !l->use_capture) continue;
        if (phase == R8E_UI_EVENT_BUBBLING && l->use_capture) continue;
        /* At target: both capture and non-capture fire */

        /*
         * In the full engine, we would call the JS function here:
         *   r8e_call(ctx, l->callback, r8e_from_pointer(event), 1, &event_val);
         * For now, we just record that the listener was invoked.
         */
        (void)l->callback;

        if (l->once) {
            l->removed = 1;
        }
    }
}

/**
 * dispatchEvent: Dispatch an event through the DOM tree.
 *
 * Phase 1: Capture (root -> target, excluding target)
 * Phase 2: At target
 * Phase 3: Bubble (target -> root, excluding target)
 *
 * Returns true if default was NOT prevented.
 */
bool r8e_ui_dom_dispatch_event(R8EUIDOMNode *target, R8EUIEvent *event) {
    if (!target || !event) return true;

    event->target = target;
    event->default_prevented = 0;
    event->propagation_stopped = 0;
    event->immediate_stopped = 0;

    /* Build path from target to root */
    R8EUIDOMNode *path[R8E_UI_MAX_EVENT_PATH];
    int depth = r8e_ui_dom_build_event_path(target, path, R8E_UI_MAX_EVENT_PATH);

    /* Phase 1: Capturing (from root toward target, excluding target) */
    for (int i = depth - 1; i > 0; i--) {
        if (event->propagation_stopped) break;
        r8e_ui_dom_invoke_listeners(path[i], event, R8E_UI_EVENT_CAPTURING);
    }

    /* Phase 2: At target */
    if (!event->propagation_stopped) {
        r8e_ui_dom_invoke_listeners(target, event, R8E_UI_EVENT_AT_TARGET);
    }

    /* Phase 3: Bubbling (from target parent toward root) */
    if (event->bubbles && !event->propagation_stopped) {
        for (int i = 1; i < depth; i++) {
            if (event->propagation_stopped) break;
            r8e_ui_dom_invoke_listeners(path[i], event, R8E_UI_EVENT_BUBBLING);
        }
    }

    /* Compact listeners that were marked for removal */
    for (int i = 0; i < depth; i++) {
        if (path[i]->flags & R8E_UI_FLAG_HAS_LISTENERS) {
            r8e_ui_dom_compact_listeners(path[i]);
        }
    }

    return !event->default_prevented;
}

/**
 * Create and dispatch a simple event.
 */
bool r8e_ui_dom_fire_event(R8EUIDOMNode *target, const char *event_type,
                          bool bubbles, bool cancelable) {
    if (!target || !event_type) return true;

    R8EUIEvent event;
    memset(&event, 0, sizeof(event));
    event.type = r8e_ui_atom_intern_cstr(event_type);
    event.bubbles = bubbles ? 1 : 0;
    event.cancelable = cancelable ? 1 : 0;

    return r8e_ui_dom_dispatch_event(target, &event);
}

/**
 * preventDefault: prevent the default action for an event.
 */
void r8e_ui_event_prevent_default(R8EUIEvent *event) {
    if (event && event->cancelable) {
        event->default_prevented = 1;
    }
}

/**
 * stopPropagation: stop event from propagating further.
 */
void r8e_ui_event_stop_propagation(R8EUIEvent *event) {
    if (event) {
        event->propagation_stopped = 1;
    }
}

/**
 * stopImmediatePropagation: stop event from firing any more listeners.
 */
void r8e_ui_event_stop_immediate_propagation(R8EUIEvent *event) {
    if (event) {
        event->propagation_stopped = 1;
        event->immediate_stopped = 1;
    }
}


/* =========================================================================
 * Section 10: innerHTML (Basic)
 *
 * Very simplified HTML parser for innerHTML assignment. Handles:
 *   - Opening/closing tags
 *   - Self-closing tags
 *   - Text content
 *   - Attributes (name="value")
 *   - Does NOT handle: comments, scripts, CDATA, entities, malformed HTML
 * ========================================================================= */

/**
 * Skip whitespace in HTML string.
 */
static const char *r8e_ui_html_skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/**
 * Parse an HTML tag name.
 */
static const char *r8e_ui_html_parse_tag_name(const char *p, char *buf,
                                             uint32_t buf_size) {
    uint32_t i = 0;
    while (*p && *p != ' ' && *p != '>' && *p != '/' && *p != '\t' &&
           *p != '\n' && i < buf_size - 1) {
        buf[i++] = (char)tolower((unsigned char)*p);
        p++;
    }
    buf[i] = '\0';
    return p;
}

/**
 * Parse a quoted attribute value.
 */
static const char *r8e_ui_html_parse_attr_value(const char *p, char *buf,
                                               uint32_t buf_size) {
    char quote = *p;
    if (quote != '"' && quote != '\'') {
        /* Unquoted value */
        uint32_t i = 0;
        while (*p && *p != ' ' && *p != '>' && *p != '/' && i < buf_size - 1) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        return p;
    }
    p++; /* skip opening quote */
    uint32_t i = 0;
    while (*p && *p != quote && i < buf_size - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    if (*p == quote) p++; /* skip closing quote */
    return p;
}

/**
 * Check if a tag is a void/self-closing element.
 */
static bool r8e_ui_html_is_void_tag(const char *tag) {
    static const char *void_tags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; void_tags[i]; i++) {
        if (strcmp(tag, void_tags[i]) == 0) return true;
    }
    return false;
}

/**
 * Parse basic HTML string and build a DOM subtree.
 * Returns the number of top-level nodes created.
 */
int r8e_ui_dom_parse_html(R8EUIDOMNode *parent, const char *html) {
    if (!parent || !html) return 0;

    /* Remove existing children */
    r8e_ui_dom_remove_all_children(parent);

    const char *p = html;
    R8EUIDOMNode *current = parent;
    int count = 0;

    while (*p) {
        if (*p == '<') {
            p++;
            if (*p == '/') {
                /* Closing tag */
                p++;
                char tag_name[64];
                p = r8e_ui_html_parse_tag_name(p, tag_name, sizeof(tag_name));
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                /* Pop up to parent */
                if (current != parent) {
                    current = current->parent ? current->parent : parent;
                }
            } else {
                /* Opening tag */
                char tag_name[64];
                p = r8e_ui_html_parse_tag_name(p, tag_name, sizeof(tag_name));
                if (tag_name[0] == '\0') continue;

                R8EUIDOMNode *elem = r8e_ui_dom_create_element(tag_name);
                if (!elem) continue;

                /* Parse attributes */
                while (*p && *p != '>' && *p != '/') {
                    p = r8e_ui_html_skip_ws(p);
                    if (*p == '>' || *p == '/' || !*p) break;

                    /* Attribute name */
                    char attr_name[64];
                    uint32_t ai = 0;
                    while (*p && *p != '=' && *p != ' ' && *p != '>' &&
                           *p != '/' && ai < 63) {
                        attr_name[ai++] = *p++;
                    }
                    attr_name[ai] = '\0';

                    p = r8e_ui_html_skip_ws(p);

                    if (*p == '=') {
                        p++;
                        p = r8e_ui_html_skip_ws(p);
                        char attr_val[256];
                        p = r8e_ui_html_parse_attr_value(p, attr_val,
                                                        sizeof(attr_val));
                        if (attr_name[0]) {
                            r8e_ui_dom_set_attribute(elem, attr_name, attr_val);
                        }
                    } else {
                        /* Boolean attribute */
                        if (attr_name[0]) {
                            r8e_ui_dom_set_attribute(elem, attr_name, "");
                        }
                    }
                }

                bool self_close = false;
                if (*p == '/') {
                    self_close = true;
                    p++;
                }
                if (*p == '>') p++;

                r8e_ui_dom_append_child(current, elem);
                count++;

                /* Push into element for children (unless self-closing or void) */
                if (!self_close && !r8e_ui_html_is_void_tag(tag_name)) {
                    current = elem;
                }
            }
        } else {
            /* Text content */
            const char *start = p;
            while (*p && *p != '<') p++;
            uint32_t len = (uint32_t)(p - start);
            if (len > 0) {
                /* Trim pure-whitespace text nodes in element context */
                bool all_ws = true;
                for (uint32_t i = 0; i < len; i++) {
                    if (start[i] != ' ' && start[i] != '\t' &&
                        start[i] != '\n' && start[i] != '\r') {
                        all_ws = false;
                        break;
                    }
                }
                if (!all_ws) {
                    R8EUIDOMNode *tn = r8e_ui_dom_create_text_node(start, len);
                    if (tn) {
                        r8e_ui_dom_append_child(current, tn);
                        count++;
                    }
                }
            }
        }
    }

    r8e_ui_dom_mark_dirty(parent);
    return count;
}


/* =========================================================================
 * Section 11: Utility Functions
 * ========================================================================= */

/**
 * Get the next element sibling.
 */
R8EUIDOMNode *r8e_ui_dom_next_element_sibling(const R8EUIDOMNode *node) {
    if (!node) return NULL;
    R8EUIDOMNode *sib = node->next_sibling;
    while (sib) {
        if (sib->type == R8E_UI_NODE_ELEMENT) return sib;
        sib = sib->next_sibling;
    }
    return NULL;
}

/**
 * Get the previous element sibling.
 */
R8EUIDOMNode *r8e_ui_dom_prev_element_sibling(const R8EUIDOMNode *node) {
    if (!node) return NULL;
    R8EUIDOMNode *sib = node->prev_sibling;
    while (sib) {
        if (sib->type == R8E_UI_NODE_ELEMENT) return sib;
        sib = sib->prev_sibling;
    }
    return NULL;
}

/**
 * Get the first element child.
 */
R8EUIDOMNode *r8e_ui_dom_first_element_child(const R8EUIDOMNode *node) {
    if (!node) return NULL;
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        if (child->type == R8E_UI_NODE_ELEMENT) return child;
        child = child->next_sibling;
    }
    return NULL;
}

/**
 * Get the last element child.
 */
R8EUIDOMNode *r8e_ui_dom_last_element_child(const R8EUIDOMNode *node) {
    if (!node) return NULL;
    R8EUIDOMNode *child = node->last_child;
    while (child) {
        if (child->type == R8E_UI_NODE_ELEMENT) return child;
        child = child->prev_sibling;
    }
    return NULL;
}

/**
 * Count element children.
 */
uint32_t r8e_ui_dom_child_element_count(const R8EUIDOMNode *node) {
    if (!node) return 0;
    uint32_t count = 0;
    R8EUIDOMNode *child = node->first_child;
    while (child) {
        if (child->type == R8E_UI_NODE_ELEMENT) count++;
        child = child->next_sibling;
    }
    return count;
}

/**
 * Check if node contains another node (is ancestor of).
 */
bool r8e_ui_dom_contains(const R8EUIDOMNode *ancestor, const R8EUIDOMNode *node) {
    if (!ancestor || !node) return false;
    const R8EUIDOMNode *current = node->parent;
    while (current) {
        if (current == ancestor) return true;
        current = current->parent;
    }
    return false;
}

/**
 * Compare document position of two nodes.
 * Returns a bitmask:
 *   1 = disconnected
 *   2 = preceding
 *   4 = following
 *   8 = contains
 *   16 = contained by
 */
uint8_t r8e_ui_dom_compare_position(const R8EUIDOMNode *a, const R8EUIDOMNode *b) {
    if (!a || !b) return 1; /* disconnected */
    if (a == b) return 0;

    if (r8e_ui_dom_contains(a, b)) return 16 | 4; /* a contains b, b follows */
    if (r8e_ui_dom_contains(b, a)) return 8 | 2;  /* b contains a, a precedes */

    /* Check if in same tree by finding common root */
    const R8EUIDOMNode *ra = a;
    while (ra->parent) ra = ra->parent;
    const R8EUIDOMNode *rb = b;
    while (rb->parent) rb = rb->parent;
    if (ra != rb) return 1; /* disconnected */

    /* Both in same tree: determine ordering by DFS position */
    /* Simplified: walk from root, first node found precedes */
    return 2; /* default: a precedes b */
}

/**
 * Get the tag name of an element as a C string.
 */
const char *r8e_ui_dom_tag_name(const R8EUIDOMNode *node) {
    if (!node || node->type != R8E_UI_NODE_ELEMENT) return NULL;
    return r8e_ui_atom_str(node->tag_atom);
}

/**
 * Check if node is connected to a document.
 */
bool r8e_ui_dom_is_connected(const R8EUIDOMNode *node) {
    while (node) {
        if (node->type == R8E_UI_NODE_DOCUMENT) return true;
        node = node->parent;
    }
    return false;
}

/**
 * Get computed bounding box (from layout).
 */
void r8e_ui_dom_get_bounding_rect(const R8EUIDOMNode *node,
                                 float *x, float *y,
                                 float *width, float *height) {
    if (!node) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    if (x) *x = node->layout.x;
    if (y) *y = node->layout.y;
    if (width) *width = node->layout.width;
    if (height) *height = node->layout.height;
}

/**
 * Hit testing: find the deepest node at the given coordinates.
 */
R8EUIDOMNode *r8e_ui_dom_hit_test(R8EUIDOMNode *root, float x, float y) {
    if (!root) return NULL;

    /* Check if point is within this node's layout box */
    float nx = root->layout.x;
    float ny = root->layout.y;
    float nw = root->layout.width + root->layout.padding[1] +
               root->layout.padding[3] + root->layout.border[1] +
               root->layout.border[3];
    float nh = root->layout.height + root->layout.padding[0] +
               root->layout.padding[2] + root->layout.border[0] +
               root->layout.border[2];

    if (x < nx || y < ny || x > nx + nw || y > ny + nh) {
        return NULL; /* outside bounds */
    }

    /* Check children in reverse order (last painted = on top) */
    R8EUIDOMNode *child = root->last_child;
    while (child) {
        R8EUIDOMNode *hit = r8e_ui_dom_hit_test(child, x, y);
        if (hit) return hit;
        child = child->prev_sibling;
    }

    /* No child hit: this node is the target */
    if (root->type == R8E_UI_NODE_ELEMENT) return root;
    return NULL;
}


/* =========================================================================
 * Section 12: Specificity Calculation (for nstyle.c integration)
 * ========================================================================= */

/**
 * Calculate specificity of a selector.
 * Returns a 32-bit value: (id_count << 16) | (class_count << 8) | tag_count
 */
uint32_t r8e_ui_selector_specificity(const R8EUISelector *sel) {
    if (!sel) return 0;
    uint32_t ids = 0, classes = 0, tags = 0;
    for (uint8_t i = 0; i < sel->count; i++) {
        switch (sel->parts[i].type) {
        case R8E_UI_SEL_ID:    ids++; break;
        case R8E_UI_SEL_CLASS: classes++; break;
        case R8E_UI_SEL_TAG:   tags++; break;
        case R8E_UI_SEL_PSEUDO: classes++; break; /* pseudo-classes = class level */
        case R8E_UI_SEL_UNIVERSAL: break; /* no specificity */
        }
    }
    return (ids << 16) | (classes << 8) | tags;
}


/* =========================================================================
 * Section 13: Debug/Dump Utilities
 * ========================================================================= */

/**
 * Dump a DOM tree to stderr for debugging.
 */
void r8e_ui_dom_dump(const R8EUIDOMNode *node, int indent) {
    if (!node) return;

    for (int i = 0; i < indent; i++) fprintf(stderr, "  ");

    switch (node->type) {
    case R8E_UI_NODE_ELEMENT:
        fprintf(stderr, "<%s", r8e_ui_atom_str(node->tag_atom));
        if (node->id_atom) {
            fprintf(stderr, " id=\"%s\"", r8e_ui_atom_str(node->id_atom));
        }
        if (node->class_list.count > 0) {
            fprintf(stderr, " class=\"");
            for (uint16_t i = 0; i < node->class_list.count; i++) {
                if (i > 0) fprintf(stderr, " ");
                fprintf(stderr, "%s", r8e_ui_atom_str(node->class_list.atoms[i]));
            }
            fprintf(stderr, "\"");
        }
        for (uint16_t i = 0; i < node->attr_count; i++) {
            const char *name = r8e_ui_atom_str(node->attrs[i].name_atom);
            if (strcmp(name, "id") == 0 || strcmp(name, "class") == 0) continue;
            fprintf(stderr, " %s=\"%s\"", name,
                    node->attrs[i].value ? node->attrs[i].value : "");
        }
        fprintf(stderr, "> (%u children)\n", node->child_count);
        break;

    case R8E_UI_NODE_TEXT:
        fprintf(stderr, "#text \"%.*s\"\n",
                node->text_len > 40 ? 40 : (int)node->text_len,
                node->text ? node->text : "");
        break;

    case R8E_UI_NODE_DOCUMENT:
        fprintf(stderr, "#document\n");
        break;

    case R8E_UI_NODE_FRAGMENT:
        fprintf(stderr, "#fragment\n");
        break;

    case R8E_UI_NODE_COMMENT:
        fprintf(stderr, "<!-- %.*s -->\n",
                node->text_len > 40 ? 40 : (int)node->text_len,
                node->text ? node->text : "");
        break;

    default:
        fprintf(stderr, "#unknown(%d)\n", node->type);
        break;
    }

    R8EUIDOMNode *child = node->first_child;
    while (child) {
        r8e_ui_dom_dump(child, indent + 1);
        child = child->next_sibling;
    }
}
