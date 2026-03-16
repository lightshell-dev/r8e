/*
 * nevent.c - R8EUIEvent: Event Dispatch and Hit Testing
 *
 * Part of the r8e JavaScript engine R8EUIUI rendering stack.
 * See CLAUDE.md Section 12.5 (R8EUIPaint + Events).
 *
 * Implements:
 *   - Hit testing: walk tree in reverse paint order, find node at (x, y)
 *   - Event types: click, mousedown/up/move, mouseenter/leave,
 *     keydown/up/keypress, focus/blur, input, change, scroll, wheel
 *   - Event bubbling: target -> parent -> ... -> document
 *   - Event capturing: document -> ... -> parent -> target
 *   - stopPropagation(), preventDefault()
 *   - Focus management: track focused element, tab order
 *   - Hover state tracking for :hover pseudo-class
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

/* =========================================================================
 * Type Definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

#ifndef R8E_NEVENT_TYPES_DEFINED
#define R8E_NEVENT_TYPES_DEFINED

/* -------------------------------------------------------------------------
 * Event types
 * ------------------------------------------------------------------------- */

typedef enum {
    /* Mouse events */
    R8E_UI_EVENT_CLICK       = 0,
    R8E_UI_EVENT_DBLCLICK    = 1,
    R8E_UI_EVENT_MOUSEDOWN   = 2,
    R8E_UI_EVENT_MOUSEUP     = 3,
    R8E_UI_EVENT_MOUSEMOVE   = 4,
    R8E_UI_EVENT_MOUSEENTER  = 5,
    R8E_UI_EVENT_MOUSELEAVE  = 6,
    R8E_UI_EVENT_CONTEXTMENU = 7,

    /* Keyboard events */
    R8E_UI_EVENT_KEYDOWN     = 10,
    R8E_UI_EVENT_KEYUP       = 11,
    R8E_UI_EVENT_KEYPRESS    = 12,

    /* Focus events */
    R8E_UI_EVENT_FOCUS       = 20,
    R8E_UI_EVENT_BLUR        = 21,
    R8E_UI_EVENT_FOCUSIN     = 22,
    R8E_UI_EVENT_FOCUSOUT    = 23,

    /* Form events */
    R8E_UI_EVENT_INPUT       = 30,
    R8E_UI_EVENT_CHANGE      = 31,
    R8E_UI_EVENT_SUBMIT      = 32,

    /* Scroll/wheel events */
    R8E_UI_EVENT_SCROLL      = 40,
    R8E_UI_EVENT_WHEEL       = 41,

    /* Touch events (basic) */
    R8E_UI_EVENT_TOUCHSTART  = 50,
    R8E_UI_EVENT_TOUCHEND    = 51,
    R8E_UI_EVENT_TOUCHMOVE   = 52,
    R8E_UI_EVENT_TOUCHCANCEL = 53,

    R8E_UI_EVENT_TYPE_COUNT  = 54,
} R8EUIEventType;

/* -------------------------------------------------------------------------
 * Event phase
 * ------------------------------------------------------------------------- */

typedef enum {
    R8E_UI_PHASE_NONE     = 0,
    R8E_UI_PHASE_CAPTURE  = 1,
    R8E_UI_PHASE_TARGET   = 2,
    R8E_UI_PHASE_BUBBLE   = 3,
} R8EUIEventPhase;

/* -------------------------------------------------------------------------
 * Modifier keys
 * ------------------------------------------------------------------------- */

#define R8E_UI_MOD_SHIFT   0x01
#define R8E_UI_MOD_CTRL    0x02
#define R8E_UI_MOD_ALT     0x04
#define R8E_UI_MOD_META    0x08
#define R8E_UI_MOD_CAPS    0x10

/* -------------------------------------------------------------------------
 * Mouse buttons
 * ------------------------------------------------------------------------- */

#define R8E_UI_BUTTON_LEFT    0
#define R8E_UI_BUTTON_MIDDLE  1
#define R8E_UI_BUTTON_RIGHT   2

/* -------------------------------------------------------------------------
 * Event flags
 * ------------------------------------------------------------------------- */

#define R8E_UI_EVENT_FLAG_STOPPED          0x0001
#define R8E_UI_EVENT_FLAG_IMMEDIATE_STOP   0x0002
#define R8E_UI_EVENT_FLAG_PREVENTED        0x0004
#define R8E_UI_EVENT_FLAG_BUBBLES          0x0008
#define R8E_UI_EVENT_FLAG_CANCELABLE       0x0010
#define R8E_UI_EVENT_FLAG_COMPOSED         0x0020
#define R8E_UI_EVENT_FLAG_TRUSTED          0x0040

/* -------------------------------------------------------------------------
 * Event structure
 * ------------------------------------------------------------------------- */

typedef struct R8EUIDOMNode_Event R8EUIDOMNode_Event;

typedef struct R8EUIEvent {
    uint8_t         type;            /* R8EUIEventType */
    uint8_t         phase;           /* R8EUIEventPhase */
    uint16_t        flags;           /* R8E_UI_EVENT_FLAG_* */

    /* Mouse data */
    float           x, y;            /* client coordinates */
    float           page_x, page_y;  /* page coordinates (with scroll) */
    float           offset_x;        /* relative to target */
    float           offset_y;
    uint8_t         button;          /* mouse button (0=left, 1=middle, 2=right) */
    uint8_t         buttons;         /* buttons currently pressed (bitmask) */
    uint8_t         detail;          /* click count */

    /* Keyboard data */
    uint32_t        key_code;        /* virtual key code */
    uint32_t        char_code;       /* unicode character (for keypress) */
    uint32_t        modifiers;       /* R8E_UI_MOD_* bitmask */
    const char     *key;             /* key name string (e.g., "Enter") */

    /* Wheel data */
    float           delta_x;
    float           delta_y;
    float           delta_z;
    uint8_t         delta_mode;      /* 0=pixels, 1=lines, 2=pages */

    /* Target tracking */
    R8EUIDOMNode_Event *target;         /* event target node */
    R8EUIDOMNode_Event *current_target; /* current handler node */
    R8EUIDOMNode_Event *related_target; /* for mouseenter/leave, focus/blur */

    /* Timestamp (milliseconds since epoch, or relative) */
    double          timestamp;
} R8EUIEvent;

/* -------------------------------------------------------------------------
 * Event listener
 * ------------------------------------------------------------------------- */

typedef void (*R8EUIEventCallback)(R8EUIEvent *event, void *user_data);

typedef struct R8EUIEventListener {
    uint8_t              type;       /* R8EUIEventType */
    bool                 capture;    /* listening in capture phase */
    bool                 once;       /* remove after first dispatch */
    bool                 passive;    /* cannot preventDefault */
    R8EUIEventCallback    callback;
    void                *user_data;
    struct R8EUIEventListener *next;  /* linked list */
} R8EUIEventListener;

/* -------------------------------------------------------------------------
 * Layout box (must match nlayout.c definition)
 * ------------------------------------------------------------------------- */

typedef struct {
    float x, y;
    float width, height;
    float padding[4];
    float border[4];
    float margin[4];
} R8EUILayoutBox_Event;

/* -------------------------------------------------------------------------
 * DOM node (minimal for event handling)
 * ------------------------------------------------------------------------- */

/* Node flags */
#define R8E_UI_NODE_FLAG_FOCUSABLE  0x0004
#define R8E_UI_NODE_FLAG_HOVERED    0x0100
#define R8E_UI_NODE_FLAG_ACTIVE     0x0200
#define R8E_UI_NODE_FLAG_FOCUSED    0x0400
#define R8E_UI_NODE_FLAG_DISABLED   0x0800

struct R8EUIDOMNode_Event {
    uint8_t               type;       /* ELEMENT=1, TEXT=3, DOCUMENT=9 */
    uint32_t              flags;
    uint32_t              tag_atom;
    R8EUILayoutBox_Event   layout;

    R8EUIDOMNode_Event    *parent;
    R8EUIDOMNode_Event    *first_child;
    R8EUIDOMNode_Event    *last_child;
    R8EUIDOMNode_Event    *next_sibling;
    R8EUIDOMNode_Event    *prev_sibling;

    R8EUIEventListener    *listeners;  /* linked list of event listeners */
    int32_t               tab_index;  /* tab order (-1 = not focusable) */

    /* Overflow/position for hit test */
    uint8_t               overflow;   /* 0=visible, 1=hidden, 2=scroll */
    uint8_t               position;   /* 0=static, 1=relative, 2=absolute, 3=fixed */
    uint8_t               display;    /* 0=block, 1=inline, 2=flex, 3=none */
    uint8_t               visibility; /* 0=visible, 1=hidden */
};

/* -------------------------------------------------------------------------
 * Event manager: global state for event handling
 * ------------------------------------------------------------------------- */

typedef struct {
    R8EUIDOMNode_Event  *document;       /* document root node */
    R8EUIDOMNode_Event  *focused;        /* currently focused element */
    R8EUIDOMNode_Event  *hovered;        /* currently hovered element */
    R8EUIDOMNode_Event  *active;         /* currently active (mousedown) element */
    R8EUIDOMNode_Event  *capture_target; /* mouse capture target */

    /* Mouse state */
    float               mouse_x;
    float               mouse_y;
    uint8_t             mouse_buttons;  /* currently pressed buttons */
    uint8_t             click_count;    /* for double-click detection */
    double              last_click_time;
    float               last_click_x;
    float               last_click_y;

    /* Configuration */
    double              dblclick_time;  /* double-click time threshold (ms) */
    float               dblclick_dist;  /* double-click distance threshold */
} R8EUIEventManager;

#endif /* R8E_NEVENT_TYPES_DEFINED */


/* =========================================================================
 * Internal Constants
 * ========================================================================= */

/* Maximum path length for capture/bubble (tree depth) */
#define R8E_UI_MAX_EVENT_PATH 128

/* Double-click defaults */
#define R8E_UI_DBLCLICK_TIME_MS 500.0
#define R8E_UI_DBLCLICK_DIST    5.0f

/* Edge indices */
#define EDGE_TOP    0
#define EDGE_RIGHT  1
#define EDGE_BOTTOM 2
#define EDGE_LEFT   3


/* =========================================================================
 * Event Manager Initialization
 * ========================================================================= */

/*
 * Initialize the event manager.
 */
void r8e_ui_event_manager_init(R8EUIEventManager *mgr, R8EUIDOMNode_Event *document) {
    if (!mgr) return;
    memset(mgr, 0, sizeof(R8EUIEventManager));
    mgr->document = document;
    mgr->dblclick_time = R8E_UI_DBLCLICK_TIME_MS;
    mgr->dblclick_dist = R8E_UI_DBLCLICK_DIST;
}

/*
 * Clean up the event manager. Does NOT free the DOM tree.
 */
void r8e_ui_event_manager_cleanup(R8EUIEventManager *mgr) {
    if (!mgr) return;
    /* Clear state pointers */
    mgr->focused = NULL;
    mgr->hovered = NULL;
    mgr->active = NULL;
    mgr->capture_target = NULL;
}


/* =========================================================================
 * Event Listener Management
 * ========================================================================= */

/*
 * Add an event listener to a node.
 *
 * @param node      Target node.
 * @param type      Event type.
 * @param callback  Callback function.
 * @param user_data User data passed to callback.
 * @param capture   Listen in capture phase.
 * @param once      Remove after first invocation.
 * @param passive   Cannot call preventDefault.
 * @return          true on success, false on allocation failure.
 */
bool r8e_ui_event_add_listener(R8EUIDOMNode_Event *node, R8EUIEventType type,
                             R8EUIEventCallback callback, void *user_data,
                             bool capture, bool once, bool passive) {
    if (!node || !callback) return false;

    /* Check for duplicate */
    R8EUIEventListener *existing = node->listeners;
    while (existing) {
        if (existing->type == (uint8_t)type &&
            existing->callback == callback &&
            existing->user_data == user_data &&
            existing->capture == capture) {
            return true;  /* already registered */
        }
        existing = existing->next;
    }

    R8EUIEventListener *listener = (R8EUIEventListener *)malloc(
        sizeof(R8EUIEventListener));
    if (!listener) return false;

    listener->type = (uint8_t)type;
    listener->capture = capture;
    listener->once = once;
    listener->passive = passive;
    listener->callback = callback;
    listener->user_data = user_data;
    listener->next = node->listeners;
    node->listeners = listener;

    return true;
}

/*
 * Remove an event listener from a node.
 *
 * @param node      Target node.
 * @param type      Event type.
 * @param callback  Callback function to match.
 * @param user_data User data to match.
 * @param capture   Capture flag to match.
 * @return          true if listener was found and removed.
 */
bool r8e_ui_event_remove_listener(R8EUIDOMNode_Event *node, R8EUIEventType type,
                                R8EUIEventCallback callback, void *user_data,
                                bool capture) {
    if (!node || !callback) return false;

    R8EUIEventListener **prev = &node->listeners;
    R8EUIEventListener *curr = node->listeners;

    while (curr) {
        if (curr->type == (uint8_t)type &&
            curr->callback == callback &&
            curr->user_data == user_data &&
            curr->capture == capture) {
            *prev = curr->next;
            free(curr);
            return true;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    return false;
}

/*
 * Remove all event listeners from a node.
 */
void r8e_ui_event_remove_all_listeners(R8EUIDOMNode_Event *node) {
    if (!node) return;

    R8EUIEventListener *curr = node->listeners;
    while (curr) {
        R8EUIEventListener *next = curr->next;
        free(curr);
        curr = next;
    }
    node->listeners = NULL;
}

/*
 * Remove all listeners of a specific type from a node.
 */
void r8e_ui_event_remove_listeners_by_type(R8EUIDOMNode_Event *node,
                                         R8EUIEventType type) {
    if (!node) return;

    R8EUIEventListener **prev = &node->listeners;
    R8EUIEventListener *curr = node->listeners;

    while (curr) {
        R8EUIEventListener *next = curr->next;
        if (curr->type == (uint8_t)type) {
            *prev = next;
            free(curr);
        } else {
            prev = &curr->next;
        }
        curr = next;
    }
}


/* =========================================================================
 * Event Creation and Manipulation
 * ========================================================================= */

/*
 * Initialize an event structure with defaults.
 */
void r8e_ui_event_init(R8EUIEvent *event, R8EUIEventType type) {
    if (!event) return;
    memset(event, 0, sizeof(R8EUIEvent));
    event->type = (uint8_t)type;
    event->phase = R8E_UI_PHASE_NONE;

    /* Set default flags based on event type */
    switch (type) {
    case R8E_UI_EVENT_CLICK:
    case R8E_UI_EVENT_DBLCLICK:
    case R8E_UI_EVENT_MOUSEDOWN:
    case R8E_UI_EVENT_MOUSEUP:
    case R8E_UI_EVENT_CONTEXTMENU:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES | R8E_UI_EVENT_FLAG_CANCELABLE |
                       R8E_UI_EVENT_FLAG_COMPOSED;
        break;
    case R8E_UI_EVENT_MOUSEMOVE:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES | R8E_UI_EVENT_FLAG_COMPOSED;
        break;
    case R8E_UI_EVENT_MOUSEENTER:
    case R8E_UI_EVENT_MOUSELEAVE:
        event->flags = R8E_UI_EVENT_FLAG_COMPOSED;
        /* mouseenter/leave do NOT bubble */
        break;
    case R8E_UI_EVENT_KEYDOWN:
    case R8E_UI_EVENT_KEYUP:
    case R8E_UI_EVENT_KEYPRESS:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES | R8E_UI_EVENT_FLAG_CANCELABLE |
                       R8E_UI_EVENT_FLAG_COMPOSED;
        break;
    case R8E_UI_EVENT_FOCUS:
    case R8E_UI_EVENT_BLUR:
        event->flags = R8E_UI_EVENT_FLAG_COMPOSED;
        /* focus/blur do NOT bubble */
        break;
    case R8E_UI_EVENT_FOCUSIN:
    case R8E_UI_EVENT_FOCUSOUT:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES | R8E_UI_EVENT_FLAG_COMPOSED;
        break;
    case R8E_UI_EVENT_INPUT:
    case R8E_UI_EVENT_CHANGE:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES;
        break;
    case R8E_UI_EVENT_SCROLL:
        event->flags = 0;
        /* scroll does NOT bubble */
        break;
    case R8E_UI_EVENT_WHEEL:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES | R8E_UI_EVENT_FLAG_CANCELABLE;
        break;
    default:
        event->flags = R8E_UI_EVENT_FLAG_BUBBLES;
        break;
    }
}

/*
 * Create a mouse event.
 */
void r8e_ui_event_init_mouse(R8EUIEvent *event, R8EUIEventType type,
                           float x, float y, uint8_t button,
                           uint32_t modifiers) {
    r8e_ui_event_init(event, type);
    event->x = x;
    event->y = y;
    event->page_x = x;
    event->page_y = y;
    event->button = button;
    event->modifiers = modifiers;
}

/*
 * Create a keyboard event.
 */
void r8e_ui_event_init_keyboard(R8EUIEvent *event, R8EUIEventType type,
                              uint32_t key_code, uint32_t char_code,
                              uint32_t modifiers, const char *key) {
    r8e_ui_event_init(event, type);
    event->key_code = key_code;
    event->char_code = char_code;
    event->modifiers = modifiers;
    event->key = key;
}

/*
 * Create a wheel event.
 */
void r8e_ui_event_init_wheel(R8EUIEvent *event, float x, float y,
                           float delta_x, float delta_y, float delta_z,
                           uint8_t delta_mode, uint32_t modifiers) {
    r8e_ui_event_init(event, R8E_UI_EVENT_WHEEL);
    event->x = x;
    event->y = y;
    event->delta_x = delta_x;
    event->delta_y = delta_y;
    event->delta_z = delta_z;
    event->delta_mode = delta_mode;
    event->modifiers = modifiers;
}

/*
 * stopPropagation()
 */
static inline void r8e_ui_event_stop_propagation(R8EUIEvent *event) {
    if (event) event->flags |= R8E_UI_EVENT_FLAG_STOPPED;
}

/*
 * stopImmediatePropagation()
 */
static inline void r8e_ui_event_stop_immediate(R8EUIEvent *event) {
    if (event) event->flags |= R8E_UI_EVENT_FLAG_STOPPED |
                                R8E_UI_EVENT_FLAG_IMMEDIATE_STOP;
}

/*
 * preventDefault()
 */
static inline void r8e_ui_event_prevent_default(R8EUIEvent *event) {
    if (event && (event->flags & R8E_UI_EVENT_FLAG_CANCELABLE)) {
        event->flags |= R8E_UI_EVENT_FLAG_PREVENTED;
    }
}

/*
 * Check if propagation was stopped.
 */
static inline bool r8e_ui_event_is_stopped(const R8EUIEvent *event) {
    return event && (event->flags & R8E_UI_EVENT_FLAG_STOPPED);
}

/*
 * Check if immediate propagation was stopped.
 */
static inline bool r8e_ui_event_is_immediate_stopped(const R8EUIEvent *event) {
    return event && (event->flags & R8E_UI_EVENT_FLAG_IMMEDIATE_STOP);
}

/*
 * Check if default was prevented.
 */
static inline bool r8e_ui_event_is_prevented(const R8EUIEvent *event) {
    return event && (event->flags & R8E_UI_EVENT_FLAG_PREVENTED);
}


/* =========================================================================
 * Hit Testing
 *
 * Walk the tree in reverse paint order (last painted = topmost visual)
 * to find the deepest node at a given point.
 * ========================================================================= */

/*
 * Get the absolute position of a node by walking up the tree.
 */
static void r8e_ui_event_get_abs_pos(const R8EUIDOMNode_Event *node,
                                   float *abs_x, float *abs_y) {
    float x = 0.0f, y = 0.0f;
    const R8EUIDOMNode_Event *current = node;
    while (current) {
        x += current->layout.x;
        y += current->layout.y;
        current = current->parent;
    }
    if (abs_x) *abs_x = x;
    if (abs_y) *abs_y = y;
}

/*
 * Check if a point is within a node's border box.
 */
static bool r8e_ui_event_point_in_node(const R8EUIDOMNode_Event *node,
                                     float px, float py) {
    if (!node) return false;

    float abs_x, abs_y;
    r8e_ui_event_get_abs_pos(node, &abs_x, &abs_y);

    const R8EUILayoutBox_Event *box = &node->layout;

    /* Border box */
    float bx = abs_x - box->padding[EDGE_LEFT] - box->border[EDGE_LEFT];
    float by = abs_y - box->padding[EDGE_TOP] - box->border[EDGE_TOP];
    float bw = box->width +
               box->padding[EDGE_LEFT] + box->padding[EDGE_RIGHT] +
               box->border[EDGE_LEFT] + box->border[EDGE_RIGHT];
    float bh = box->height +
               box->padding[EDGE_TOP] + box->padding[EDGE_BOTTOM] +
               box->border[EDGE_TOP] + box->border[EDGE_BOTTOM];

    return px >= bx && px < bx + bw && py >= by && py < by + bh;
}

/*
 * Recursive hit test: find the deepest node containing the point.
 * Walks children in reverse order (last child = painted on top).
 *
 * @param node   Current node to test.
 * @param px     Point x (absolute coordinates).
 * @param py     Point y (absolute coordinates).
 * @return       The deepest node at (px, py), or NULL if not found.
 */
R8EUIDOMNode_Event *r8e_ui_event_hit_test(R8EUIDOMNode_Event *node,
                                       float px, float py) {
    if (!node) return NULL;

    /* Skip invisible or display:none nodes */
    if (node->display == 3 || node->visibility != 0) return NULL;

    /* Check if point is in this node */
    if (node->type == 1 && !r8e_ui_event_point_in_node(node, px, py)) {
        /* For overflow:hidden, children outside the box are not hit */
        if (node->overflow == 1) return NULL;
        /* For overflow:visible, children may extend beyond */
    }

    /* Test children in reverse order (topmost first) */
    R8EUIDOMNode_Event *child = node->last_child;
    while (child) {
        R8EUIDOMNode_Event *hit = r8e_ui_event_hit_test(child, px, py);
        if (hit) return hit;
        child = child->prev_sibling;
    }

    /* If no child was hit, check if this node itself is the target */
    if (node->type == 1 && r8e_ui_event_point_in_node(node, px, py)) {
        return node;
    }

    return NULL;
}

/*
 * Build the event path from target to root (for capture and bubble phases).
 *
 * @param target  The event target node.
 * @param path    Output array of node pointers (root first, target last).
 * @param max_len Maximum path length.
 * @return        Number of nodes in the path.
 */
static int r8e_ui_event_build_path(R8EUIDOMNode_Event *target,
                                 R8EUIDOMNode_Event **path, int max_len) {
    /* Build path from target to root */
    R8EUIDOMNode_Event *stack[R8E_UI_MAX_EVENT_PATH];
    int depth = 0;

    R8EUIDOMNode_Event *current = target;
    while (current && depth < R8E_UI_MAX_EVENT_PATH) {
        stack[depth++] = current;
        current = current->parent;
    }

    /* Reverse into output (root first, target last) */
    int count = depth < max_len ? depth : max_len;
    for (int i = 0; i < count; i++) {
        path[i] = stack[depth - 1 - i];
    }

    return count;
}


/* =========================================================================
 * Event Dispatch
 *
 * Three-phase dispatch: capture -> target -> bubble
 * ========================================================================= */

/*
 * Invoke listeners on a single node for a given phase.
 */
static void r8e_ui_event_invoke_listeners(R8EUIDOMNode_Event *node,
                                        R8EUIEvent *event,
                                        R8EUIEventPhase phase) {
    if (!node || !event) return;
    if (r8e_ui_event_is_stopped(event)) return;

    event->current_target = node;
    event->phase = (uint8_t)phase;

    /* Collect matching listeners (snapshot to handle removal during dispatch) */
    R8EUIEventListener *listener = node->listeners;
    R8EUIEventListener *to_remove[32];
    int remove_count = 0;

    while (listener) {
        if (r8e_ui_event_is_immediate_stopped(event)) break;

        if (listener->type == event->type) {
            bool should_fire = false;

            switch (phase) {
            case R8E_UI_PHASE_CAPTURE:
                should_fire = listener->capture;
                break;
            case R8E_UI_PHASE_TARGET:
                /* At target: fire both capture and bubble listeners */
                should_fire = true;
                break;
            case R8E_UI_PHASE_BUBBLE:
                should_fire = !listener->capture;
                break;
            default:
                break;
            }

            if (should_fire) {
                listener->callback(event, listener->user_data);

                if (listener->once && remove_count < 32) {
                    to_remove[remove_count++] = listener;
                }
            }
        }

        listener = listener->next;
    }

    /* Remove once-only listeners */
    for (int i = 0; i < remove_count; i++) {
        r8e_ui_event_remove_listener(node,
                                   (R8EUIEventType)to_remove[i]->type,
                                   to_remove[i]->callback,
                                   to_remove[i]->user_data,
                                   to_remove[i]->capture);
    }
}

/*
 * Dispatch an event through the DOM tree.
 * Implements the full W3C event dispatch algorithm:
 *   1. Build path from target to root
 *   2. Capture phase: root -> ... -> parent (not target)
 *   3. Target phase: target node
 *   4. Bubble phase: parent -> ... -> root (if event bubbles)
 *
 * @param event   The event to dispatch.
 * @param target  The target node.
 * @return        true if preventDefault() was NOT called.
 */
bool r8e_ui_event_dispatch(R8EUIEvent *event, R8EUIDOMNode_Event *target) {
    if (!event || !target) return true;

    event->target = target;

    /* Build event path */
    R8EUIDOMNode_Event *path[R8E_UI_MAX_EVENT_PATH];
    int path_len = r8e_ui_event_build_path(target, path, R8E_UI_MAX_EVENT_PATH);

    if (path_len == 0) return true;

    /* Phase 1: Capture (root to parent, skip target) */
    for (int i = 0; i < path_len - 1; i++) {
        if (r8e_ui_event_is_stopped(event)) break;
        r8e_ui_event_invoke_listeners(path[i], event, R8E_UI_PHASE_CAPTURE);
    }

    /* Phase 2: Target */
    if (!r8e_ui_event_is_stopped(event)) {
        r8e_ui_event_invoke_listeners(target, event, R8E_UI_PHASE_TARGET);
    }

    /* Phase 3: Bubble (parent to root, skip target) */
    if ((event->flags & R8E_UI_EVENT_FLAG_BUBBLES) &&
        !r8e_ui_event_is_stopped(event)) {
        for (int i = path_len - 2; i >= 0; i--) {
            if (r8e_ui_event_is_stopped(event)) break;
            r8e_ui_event_invoke_listeners(path[i], event, R8E_UI_PHASE_BUBBLE);
        }
    }

    return !r8e_ui_event_is_prevented(event);
}


/* =========================================================================
 * Focus Management
 *
 * Track the currently focused element. Handle focus/blur events.
 * Support tab order navigation.
 * ========================================================================= */

/*
 * Check if a node is focusable.
 * Focusable elements: those with tab_index >= 0 or the FOCUSABLE flag.
 */
static bool r8e_ui_event_is_focusable(const R8EUIDOMNode_Event *node) {
    if (!node) return false;
    if (node->flags & R8E_UI_NODE_FLAG_DISABLED) return false;
    if (node->display == 3) return false;  /* display:none */
    if (node->visibility != 0) return false;  /* hidden */
    return (node->flags & R8E_UI_NODE_FLAG_FOCUSABLE) ||
           (node->tab_index >= 0);
}

/*
 * Set focus to a new element. Fires blur on old, focus on new.
 *
 * @param mgr   Event manager.
 * @param node  Node to focus, or NULL to blur current.
 */
void r8e_ui_event_set_focus(R8EUIEventManager *mgr, R8EUIDOMNode_Event *node) {
    if (!mgr) return;

    R8EUIDOMNode_Event *old_focus = mgr->focused;

    /* No change */
    if (old_focus == node) return;

    /* Blur old focused element */
    if (old_focus) {
        old_focus->flags &= ~R8E_UI_NODE_FLAG_FOCUSED;

        R8EUIEvent blur_event;
        r8e_ui_event_init(&blur_event, R8E_UI_EVENT_BLUR);
        blur_event.related_target = node;
        r8e_ui_event_dispatch(&blur_event, old_focus);

        /* focusout bubbles */
        R8EUIEvent focusout_event;
        r8e_ui_event_init(&focusout_event, R8E_UI_EVENT_FOCUSOUT);
        focusout_event.related_target = node;
        r8e_ui_event_dispatch(&focusout_event, old_focus);
    }

    /* Focus new element */
    mgr->focused = node;

    if (node) {
        node->flags |= R8E_UI_NODE_FLAG_FOCUSED;

        R8EUIEvent focus_event;
        r8e_ui_event_init(&focus_event, R8E_UI_EVENT_FOCUS);
        focus_event.related_target = old_focus;
        r8e_ui_event_dispatch(&focus_event, node);

        /* focusin bubbles */
        R8EUIEvent focusin_event;
        r8e_ui_event_init(&focusin_event, R8E_UI_EVENT_FOCUSIN);
        focusin_event.related_target = old_focus;
        r8e_ui_event_dispatch(&focusin_event, node);
    }
}

/*
 * Collect focusable elements in tab order.
 * Tab order: elements with tab_index > 0 first (sorted), then
 * elements with tab_index == 0 in document order.
 */
static void r8e_ui_event_collect_focusable(R8EUIDOMNode_Event *node,
                                         R8EUIDOMNode_Event **out,
                                         int *count, int max) {
    if (!node || *count >= max) return;
    if (node->display == 3) return;

    if (r8e_ui_event_is_focusable(node)) {
        out[*count] = node;
        (*count)++;
    }

    R8EUIDOMNode_Event *child = node->first_child;
    while (child) {
        r8e_ui_event_collect_focusable(child, out, count, max);
        child = child->next_sibling;
    }
}

/*
 * Sort focusable elements by tab order (insertion sort).
 * tab_index > 0: sorted ascending, come first.
 * tab_index == 0: document order, come after.
 */
static void r8e_ui_event_sort_tab_order(R8EUIDOMNode_Event **elems, int count) {
    for (int i = 1; i < count; i++) {
        R8EUIDOMNode_Event *temp = elems[i];
        int j = i - 1;

        /* Elements with tab_index > 0 sort before tab_index == 0 */
        while (j >= 0) {
            int32_t ti_j = elems[j]->tab_index;
            int32_t ti_temp = temp->tab_index;

            /* Both > 0: sort ascending */
            if (ti_j > 0 && ti_temp > 0) {
                if (ti_j > ti_temp) {
                    elems[j + 1] = elems[j];
                    j--;
                    continue;
                }
                break;
            }
            /* j > 0, temp == 0: j comes first (already correct) */
            if (ti_j > 0 && ti_temp == 0) break;
            /* j == 0, temp > 0: temp comes first */
            if (ti_j == 0 && ti_temp > 0) {
                elems[j + 1] = elems[j];
                j--;
                continue;
            }
            /* Both == 0: document order (stable, so break) */
            break;
        }
        elems[j + 1] = temp;
    }
}

/*
 * Move focus to the next/previous focusable element (Tab / Shift+Tab).
 *
 * @param mgr      Event manager.
 * @param forward  true for Tab, false for Shift+Tab.
 */
void r8e_ui_event_focus_next(R8EUIEventManager *mgr, bool forward) {
    if (!mgr || !mgr->document) return;

    #define MAX_FOCUSABLE 256
    R8EUIDOMNode_Event *focusable[MAX_FOCUSABLE];
    int count = 0;

    r8e_ui_event_collect_focusable(mgr->document, focusable, &count,
                                 MAX_FOCUSABLE);
    if (count == 0) return;

    r8e_ui_event_sort_tab_order(focusable, count);

    /* Find current focused element in the list */
    int current_idx = -1;
    for (int i = 0; i < count; i++) {
        if (focusable[i] == mgr->focused) {
            current_idx = i;
            break;
        }
    }

    /* Move to next/previous */
    int new_idx;
    if (forward) {
        new_idx = (current_idx + 1) % count;
    } else {
        new_idx = (current_idx - 1 + count) % count;
    }

    r8e_ui_event_set_focus(mgr, focusable[new_idx]);
    #undef MAX_FOCUSABLE
}


/* =========================================================================
 * Hover State Tracking
 *
 * Tracks the :hover pseudo-class state. Updates when the mouse moves.
 * ========================================================================= */

/*
 * Check if node is an ancestor of another node.
 */
static bool r8e_ui_event_is_ancestor(const R8EUIDOMNode_Event *ancestor,
                                   const R8EUIDOMNode_Event *descendant) {
    const R8EUIDOMNode_Event *current = descendant;
    while (current) {
        if (current == ancestor) return true;
        current = current->parent;
    }
    return false;
}

/*
 * Update hover state for mouse movement.
 * Fires mouseenter/mouseleave events as needed.
 *
 * @param mgr       Event manager.
 * @param new_hover  The new element under the mouse (from hit test).
 */
void r8e_ui_event_update_hover(R8EUIEventManager *mgr,
                             R8EUIDOMNode_Event *new_hover) {
    if (!mgr) return;

    R8EUIDOMNode_Event *old_hover = mgr->hovered;
    if (old_hover == new_hover) return;

    /* Fire mouseleave events from old_hover up to common ancestor */
    if (old_hover) {
        /* Walk up from old_hover; fire mouseleave for nodes NOT in new path */
        R8EUIDOMNode_Event *node = old_hover;
        while (node) {
            bool still_hovered = (new_hover &&
                                  r8e_ui_event_is_ancestor(node, new_hover));
            if (!still_hovered) {
                node->flags &= ~R8E_UI_NODE_FLAG_HOVERED;

                R8EUIEvent leave_event;
                r8e_ui_event_init_mouse(&leave_event, R8E_UI_EVENT_MOUSELEAVE,
                                      mgr->mouse_x, mgr->mouse_y, 0, 0);
                leave_event.related_target = new_hover;
                leave_event.target = node;
                /* mouseleave does not bubble: dispatch directly */
                r8e_ui_event_invoke_listeners(node, &leave_event,
                                            R8E_UI_PHASE_TARGET);
            }
            node = node->parent;
        }
    }

    /* Fire mouseenter events from common ancestor down to new_hover */
    if (new_hover) {
        /* Collect path from new_hover to root */
        R8EUIDOMNode_Event *enter_path[R8E_UI_MAX_EVENT_PATH];
        int enter_count = 0;
        R8EUIDOMNode_Event *node = new_hover;
        while (node && enter_count < R8E_UI_MAX_EVENT_PATH) {
            enter_path[enter_count++] = node;
            node = node->parent;
        }

        /* Walk from root to new_hover, fire enter for newly hovered nodes */
        for (int i = enter_count - 1; i >= 0; i--) {
            node = enter_path[i];
            bool was_hovered = (old_hover &&
                                r8e_ui_event_is_ancestor(node, old_hover));
            if (!was_hovered) {
                node->flags |= R8E_UI_NODE_FLAG_HOVERED;

                R8EUIEvent enter_event;
                r8e_ui_event_init_mouse(&enter_event, R8E_UI_EVENT_MOUSEENTER,
                                      mgr->mouse_x, mgr->mouse_y, 0, 0);
                enter_event.related_target = old_hover;
                enter_event.target = node;
                r8e_ui_event_invoke_listeners(node, &enter_event,
                                            R8E_UI_PHASE_TARGET);
            }
        }
    }

    mgr->hovered = new_hover;
}


/* =========================================================================
 * High-Level Event Handlers
 *
 * Process raw input (mouse, keyboard) and generate DOM events.
 * ========================================================================= */

/*
 * Process a mouse move event.
 *
 * @param mgr   Event manager.
 * @param x     Mouse x coordinate.
 * @param y     Mouse y coordinate.
 * @param mods  Modifier keys.
 */
void r8e_ui_event_mouse_move(R8EUIEventManager *mgr, float x, float y,
                           uint32_t mods) {
    if (!mgr || !mgr->document) return;

    mgr->mouse_x = x;
    mgr->mouse_y = y;

    /* Hit test */
    R8EUIDOMNode_Event *target;
    if (mgr->capture_target) {
        target = mgr->capture_target;
    } else {
        target = r8e_ui_event_hit_test(mgr->document, x, y);
    }

    /* Update hover state */
    r8e_ui_event_update_hover(mgr, target);

    /* Fire mousemove event */
    if (target) {
        R8EUIEvent move_event;
        r8e_ui_event_init_mouse(&move_event, R8E_UI_EVENT_MOUSEMOVE,
                              x, y, 0, mods);

        /* Compute offset relative to target */
        float abs_x, abs_y;
        r8e_ui_event_get_abs_pos(target, &abs_x, &abs_y);
        move_event.offset_x = x - abs_x;
        move_event.offset_y = y - abs_y;
        move_event.buttons = mgr->mouse_buttons;

        r8e_ui_event_dispatch(&move_event, target);
    }
}

/*
 * Process a mouse button down event.
 *
 * @param mgr     Event manager.
 * @param x       Mouse x coordinate.
 * @param y       Mouse y coordinate.
 * @param button  Mouse button (0=left, 1=middle, 2=right).
 * @param mods    Modifier keys.
 */
void r8e_ui_event_mouse_down(R8EUIEventManager *mgr, float x, float y,
                           uint8_t button, uint32_t mods) {
    if (!mgr || !mgr->document) return;

    mgr->mouse_x = x;
    mgr->mouse_y = y;
    mgr->mouse_buttons |= (1 << button);

    R8EUIDOMNode_Event *target;
    if (mgr->capture_target) {
        target = mgr->capture_target;
    } else {
        target = r8e_ui_event_hit_test(mgr->document, x, y);
    }

    /* Set active state */
    if (mgr->active) {
        mgr->active->flags &= ~R8E_UI_NODE_FLAG_ACTIVE;
    }
    mgr->active = target;
    if (target) {
        target->flags |= R8E_UI_NODE_FLAG_ACTIVE;
    }

    /* Focus the target (if focusable) */
    if (target) {
        R8EUIDOMNode_Event *focusable = target;
        while (focusable && !r8e_ui_event_is_focusable(focusable)) {
            focusable = focusable->parent;
        }
        if (focusable) {
            r8e_ui_event_set_focus(mgr, focusable);
        } else {
            /* Click on non-focusable: blur current */
            r8e_ui_event_set_focus(mgr, NULL);
        }
    }

    /* Fire mousedown event */
    if (target) {
        R8EUIEvent down_event;
        r8e_ui_event_init_mouse(&down_event, R8E_UI_EVENT_MOUSEDOWN,
                              x, y, button, mods);

        float abs_x, abs_y;
        r8e_ui_event_get_abs_pos(target, &abs_x, &abs_y);
        down_event.offset_x = x - abs_x;
        down_event.offset_y = y - abs_y;
        down_event.buttons = mgr->mouse_buttons;

        r8e_ui_event_dispatch(&down_event, target);
    }
}

/*
 * Process a mouse button up event.
 *
 * @param mgr     Event manager.
 * @param x       Mouse x coordinate.
 * @param y       Mouse y coordinate.
 * @param button  Mouse button.
 * @param mods    Modifier keys.
 * @param timestamp  Event timestamp in milliseconds.
 */
void r8e_ui_event_mouse_up(R8EUIEventManager *mgr, float x, float y,
                         uint8_t button, uint32_t mods, double timestamp) {
    if (!mgr || !mgr->document) return;

    mgr->mouse_x = x;
    mgr->mouse_y = y;
    mgr->mouse_buttons &= ~(1 << button);

    R8EUIDOMNode_Event *target;
    if (mgr->capture_target) {
        target = mgr->capture_target;
    } else {
        target = r8e_ui_event_hit_test(mgr->document, x, y);
    }

    /* Clear active state */
    if (mgr->active) {
        mgr->active->flags &= ~R8E_UI_NODE_FLAG_ACTIVE;
    }

    /* Fire mouseup event */
    if (target) {
        R8EUIEvent up_event;
        r8e_ui_event_init_mouse(&up_event, R8E_UI_EVENT_MOUSEUP,
                              x, y, button, mods);

        float abs_x, abs_y;
        r8e_ui_event_get_abs_pos(target, &abs_x, &abs_y);
        up_event.offset_x = x - abs_x;
        up_event.offset_y = y - abs_y;
        up_event.buttons = mgr->mouse_buttons;

        r8e_ui_event_dispatch(&up_event, target);

        /* Fire click if mouseup target matches mousedown target */
        if (target == mgr->active && button == R8E_UI_BUTTON_LEFT) {
            /* Double-click detection */
            float dx = x - mgr->last_click_x;
            float dy = y - mgr->last_click_y;
            float dist = (float)sqrt((double)(dx * dx + dy * dy));
            double dt = timestamp - mgr->last_click_time;

            if (dt < mgr->dblclick_time && dist < mgr->dblclick_dist) {
                mgr->click_count++;
            } else {
                mgr->click_count = 1;
            }

            mgr->last_click_time = timestamp;
            mgr->last_click_x = x;
            mgr->last_click_y = y;

            /* Fire click event */
            R8EUIEvent click_event;
            r8e_ui_event_init_mouse(&click_event, R8E_UI_EVENT_CLICK,
                                  x, y, button, mods);
            click_event.detail = mgr->click_count;
            click_event.offset_x = x - abs_x;
            click_event.offset_y = y - abs_y;
            r8e_ui_event_dispatch(&click_event, target);

            /* Fire dblclick if click_count == 2 */
            if (mgr->click_count == 2) {
                R8EUIEvent dblclick_event;
                r8e_ui_event_init_mouse(&dblclick_event, R8E_UI_EVENT_DBLCLICK,
                                      x, y, button, mods);
                dblclick_event.detail = 2;
                r8e_ui_event_dispatch(&dblclick_event, target);
                mgr->click_count = 0;
            }
        }

        /* Context menu on right-click */
        if (button == R8E_UI_BUTTON_RIGHT) {
            R8EUIEvent ctx_event;
            r8e_ui_event_init_mouse(&ctx_event, R8E_UI_EVENT_CONTEXTMENU,
                                  x, y, button, mods);
            r8e_ui_event_dispatch(&ctx_event, target);
        }
    }

    mgr->active = NULL;

    /* Release pointer capture when all buttons released */
    if (mgr->mouse_buttons == 0) {
        mgr->capture_target = NULL;
    }
}

/*
 * Process a keyboard key down event.
 *
 * @param mgr       Event manager.
 * @param key_code  Virtual key code.
 * @param char_code Unicode character (0 if not applicable).
 * @param mods      Modifier keys.
 * @param key       Key name string.
 */
void r8e_ui_event_key_down(R8EUIEventManager *mgr, uint32_t key_code,
                         uint32_t char_code, uint32_t mods,
                         const char *key) {
    if (!mgr) return;

    R8EUIDOMNode_Event *target = mgr->focused ? mgr->focused : mgr->document;
    if (!target) return;

    /* Handle Tab key for focus navigation */
    if (key_code == 9) {  /* Tab key */
        bool shift = (mods & R8E_UI_MOD_SHIFT) != 0;
        r8e_ui_event_focus_next(mgr, !shift);
        return;
    }

    /* Fire keydown */
    R8EUIEvent down_event;
    r8e_ui_event_init_keyboard(&down_event, R8E_UI_EVENT_KEYDOWN,
                             key_code, char_code, mods, key);
    r8e_ui_event_dispatch(&down_event, target);

    /* Fire keypress for printable characters (deprecated but still used) */
    if (char_code > 0 && !r8e_ui_event_is_prevented(&down_event)) {
        R8EUIEvent press_event;
        r8e_ui_event_init_keyboard(&press_event, R8E_UI_EVENT_KEYPRESS,
                                 key_code, char_code, mods, key);
        r8e_ui_event_dispatch(&press_event, target);
    }
}

/*
 * Process a keyboard key up event.
 *
 * @param mgr       Event manager.
 * @param key_code  Virtual key code.
 * @param char_code Unicode character.
 * @param mods      Modifier keys.
 * @param key       Key name string.
 */
void r8e_ui_event_key_up(R8EUIEventManager *mgr, uint32_t key_code,
                       uint32_t char_code, uint32_t mods,
                       const char *key) {
    if (!mgr) return;

    R8EUIDOMNode_Event *target = mgr->focused ? mgr->focused : mgr->document;
    if (!target) return;

    R8EUIEvent up_event;
    r8e_ui_event_init_keyboard(&up_event, R8E_UI_EVENT_KEYUP,
                             key_code, char_code, mods, key);
    r8e_ui_event_dispatch(&up_event, target);
}

/*
 * Process a wheel/scroll event.
 *
 * @param mgr       Event manager.
 * @param x         Mouse x.
 * @param y         Mouse y.
 * @param delta_x   Horizontal scroll amount.
 * @param delta_y   Vertical scroll amount.
 * @param mods      Modifier keys.
 */
void r8e_ui_event_wheel(R8EUIEventManager *mgr, float x, float y,
                      float delta_x, float delta_y, uint32_t mods) {
    if (!mgr || !mgr->document) return;

    R8EUIDOMNode_Event *target = r8e_ui_event_hit_test(mgr->document, x, y);
    if (!target) return;

    /* Fire wheel event */
    R8EUIEvent wheel_event;
    r8e_ui_event_init_wheel(&wheel_event, x, y, delta_x, delta_y, 0.0f,
                          0, mods);
    bool allowed = r8e_ui_event_dispatch(&wheel_event, target);

    /* If not prevented, apply scroll to the nearest scrollable ancestor */
    if (allowed) {
        R8EUIDOMNode_Event *scrollable = target;
        while (scrollable) {
            if (scrollable->overflow == 2) {  /* scroll */
                /* Fire scroll event */
                R8EUIEvent scroll_event;
                r8e_ui_event_init(&scroll_event, R8E_UI_EVENT_SCROLL);
                scroll_event.target = scrollable;
                r8e_ui_event_dispatch(&scroll_event, scrollable);
                break;
            }
            scrollable = scrollable->parent;
        }
    }
}


/* =========================================================================
 * Pointer Capture
 *
 * Lock mouse events to a specific element (e.g., during drag operations).
 * ========================================================================= */

/*
 * Set pointer capture to a specific element.
 * While captured, all mouse events are directed to this element.
 *
 * @param mgr   Event manager.
 * @param node  Node to capture pointer events.
 */
void r8e_ui_event_set_capture(R8EUIEventManager *mgr, R8EUIDOMNode_Event *node) {
    if (!mgr) return;
    mgr->capture_target = node;
}

/*
 * Release pointer capture.
 */
void r8e_ui_event_release_capture(R8EUIEventManager *mgr) {
    if (!mgr) return;
    mgr->capture_target = NULL;
}

/*
 * Check if a specific node has pointer capture.
 */
bool r8e_ui_event_has_capture(const R8EUIEventManager *mgr,
                            const R8EUIDOMNode_Event *node) {
    return mgr && mgr->capture_target == node;
}


/* =========================================================================
 * Utility Functions
 * ========================================================================= */

/*
 * Check if a node is currently hovered.
 */
bool r8e_ui_event_is_hovered(const R8EUIDOMNode_Event *node) {
    return node && (node->flags & R8E_UI_NODE_FLAG_HOVERED);
}

/*
 * Check if a node is currently focused.
 */
bool r8e_ui_event_is_focused(const R8EUIDOMNode_Event *node) {
    return node && (node->flags & R8E_UI_NODE_FLAG_FOCUSED);
}

/*
 * Check if a node is currently active (mouse pressed).
 */
bool r8e_ui_event_is_active(const R8EUIDOMNode_Event *node) {
    return node && (node->flags & R8E_UI_NODE_FLAG_ACTIVE);
}

/*
 * Get the currently focused element.
 */
R8EUIDOMNode_Event *r8e_ui_event_get_focused(const R8EUIEventManager *mgr) {
    return mgr ? mgr->focused : NULL;
}

/*
 * Get the currently hovered element.
 */
R8EUIDOMNode_Event *r8e_ui_event_get_hovered(const R8EUIEventManager *mgr) {
    return mgr ? mgr->hovered : NULL;
}

/*
 * Fire a custom event on a target node.
 *
 * @param target   Target node.
 * @param type     Event type.
 * @return         true if not prevented.
 */
bool r8e_ui_event_fire(R8EUIDOMNode_Event *target, R8EUIEventType type) {
    if (!target) return true;

    R8EUIEvent event;
    r8e_ui_event_init(&event, type);
    return r8e_ui_event_dispatch(&event, target);
}

/*
 * Fire an input event on a target node.
 */
bool r8e_ui_event_fire_input(R8EUIDOMNode_Event *target) {
    return r8e_ui_event_fire(target, R8E_UI_EVENT_INPUT);
}

/*
 * Fire a change event on a target node.
 */
bool r8e_ui_event_fire_change(R8EUIDOMNode_Event *target) {
    return r8e_ui_event_fire(target, R8E_UI_EVENT_CHANGE);
}

/*
 * Get the event type name as a string (for debugging).
 */
const char *r8e_ui_event_type_name(R8EUIEventType type) {
    switch (type) {
    case R8E_UI_EVENT_CLICK:       return "click";
    case R8E_UI_EVENT_DBLCLICK:    return "dblclick";
    case R8E_UI_EVENT_MOUSEDOWN:   return "mousedown";
    case R8E_UI_EVENT_MOUSEUP:     return "mouseup";
    case R8E_UI_EVENT_MOUSEMOVE:   return "mousemove";
    case R8E_UI_EVENT_MOUSEENTER:  return "mouseenter";
    case R8E_UI_EVENT_MOUSELEAVE:  return "mouseleave";
    case R8E_UI_EVENT_CONTEXTMENU: return "contextmenu";
    case R8E_UI_EVENT_KEYDOWN:     return "keydown";
    case R8E_UI_EVENT_KEYUP:       return "keyup";
    case R8E_UI_EVENT_KEYPRESS:    return "keypress";
    case R8E_UI_EVENT_FOCUS:       return "focus";
    case R8E_UI_EVENT_BLUR:        return "blur";
    case R8E_UI_EVENT_FOCUSIN:     return "focusin";
    case R8E_UI_EVENT_FOCUSOUT:    return "focusout";
    case R8E_UI_EVENT_INPUT:       return "input";
    case R8E_UI_EVENT_CHANGE:      return "change";
    case R8E_UI_EVENT_SUBMIT:      return "submit";
    case R8E_UI_EVENT_SCROLL:      return "scroll";
    case R8E_UI_EVENT_WHEEL:       return "wheel";
    case R8E_UI_EVENT_TOUCHSTART:  return "touchstart";
    case R8E_UI_EVENT_TOUCHEND:    return "touchend";
    case R8E_UI_EVENT_TOUCHMOVE:   return "touchmove";
    case R8E_UI_EVENT_TOUCHCANCEL: return "touchcancel";
    default:                     return "unknown";
    }
}
