/*
 * r8e_closure.c - PVCC (Per-Variable Capture Classification) Closure System
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 9 for design rationale.
 *
 * Architecture:
 *   - 83.1% of captured variables are immutable after capture (EXP-04)
 *   - Immutable captures are COPIED directly into the closure (no upvalue box)
 *   - Mutable captures share a heap-allocated R8EEnvFrame
 *   - Tiered storage: INLINE (1-2 captures), ARRAY (3-8), FRAME (9+)
 *   - 70.4% of closures have ALL captures immutable -> most compact path
 *   - Average captures/closure = 6.2 (higher than expected due to bundling)
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

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

/* NaN-boxing type checks */
#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)    (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

/* NaN-boxing extraction / encoding */
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

/* =========================================================================
 * GC header flags layout (must match r8e_gc.c)
 * ========================================================================= */

#define R8E_GC_TIER_MASK         0x00000003u
#define R8E_GC_MARK              0x00000004u
#define R8E_GC_ON_SUSPECT        0x00000008u
#define R8E_GC_FROZEN            0x00000010u

#define R8E_GC_KIND_MASK         0x000000E0u
#define R8E_GC_KIND_SHIFT        5
#define R8E_GC_KIND_OBJECT       0u
#define R8E_GC_KIND_STRING       1u
#define R8E_GC_KIND_ARRAY        2u
#define R8E_GC_KIND_CLOSURE      3u
#define R8E_GC_KIND_ENVFRAME     4u
#define R8E_GC_KIND_FUNCTION     5u

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)

/* =========================================================================
 * GC header
 * ========================================================================= */

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* =========================================================================
 * Heap object types needed by closure system
 * ========================================================================= */

typedef struct R8EFunction {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t *bytecode;
    uint32_t bytecode_len;
    uint16_t param_count;
    uint16_t local_count;
    uint16_t capture_count;
    uint16_t reserved;
    uint32_t name_atom;
} R8EFunction;

typedef struct R8EEnvFrame {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  refcount;    /* number of closures sharing this frame */
    uint16_t  slot_count;
    uint16_t  reserved;
    R8EValue *slots;
} R8EEnvFrame;

/* =========================================================================
 * Capture classification constants (compile-time decision)
 * ========================================================================= */

#define R8E_CAPTURE_IMMUTABLE  0x00  /* 83.1%: copy value directly */
#define R8E_CAPTURE_MUTABLE    0x01  /* 16.9%: share via R8EEnvFrame */

/* =========================================================================
 * Capture storage mode (Section 9.3)
 * ========================================================================= */

#define R8E_CAPTURE_MODE_INLINE 0  /* 1-2 captures, all immutable (~52%) */
#define R8E_CAPTURE_MODE_ARRAY  1  /* 3-8 captures (~28%) */
#define R8E_CAPTURE_MODE_FRAME  2  /* 9+ captures, or mutable-heavy (~20%) */

/* =========================================================================
 * Capture metadata
 * ========================================================================= */

#define R8E_CAP_META_DIRECT   0  /* value stored directly in closure */
#define R8E_CAP_META_INDIRECT 1  /* value lives in env_frame->slots[index] */

typedef struct {
    uint8_t  kind;    /* R8E_CAP_META_DIRECT or R8E_CAP_META_INDIRECT */
    uint8_t  index;   /* frame slot index (only for INDIRECT) */
    uint16_t pad;
} R8ECaptureMeta;

/* =========================================================================
 * R8EClosure structure (Section 9.3)
 * ========================================================================= */

typedef struct R8EClosure {
    uint32_t      flags;
    uint32_t      proto_id;
    R8EFunction  *func;
    uint8_t       capture_count;
    uint8_t       capture_mode;
    uint16_t      reserved;
    union {
        /* Mode INLINE: 1-2 captures, all immutable */
        struct {
            R8EValue inline_captures[2];
        } inl;

        /* Mode ARRAY: 3-8 captures, mixed mutability */
        struct {
            R8EValue       *values;  /* heap array of capture_count values */
            R8ECaptureMeta *meta;    /* heap array of capture_count metas */
            R8EEnvFrame    *env;     /* shared frame for mutable slots, NULL if none */
        } arr;

        /* Mode FRAME: 9+ captures, all go through the frame */
        struct {
            R8EEnvFrame *env_frame;
        } frm;
    } storage;
} R8EClosure;

/* =========================================================================
 * R8ECaptureDesc - compile-time info about one captured variable
 * Passed to r8e_closure_new() and consumed during creation.
 * ========================================================================= */

typedef struct {
    R8EValue value;       /* current value to capture */
    uint8_t  mutability;  /* R8E_CAPTURE_IMMUTABLE or R8E_CAPTURE_MUTABLE */
    uint8_t  frame_slot;  /* slot index in env frame (for mutable captures) */
    uint16_t reserved;
} R8ECaptureDesc;

/* =========================================================================
 * GC state types (minimal, needed for context)
 * ========================================================================= */

typedef struct {
    R8EGCHeader **items;
    uint32_t      count;
    uint32_t      capacity;
} R8ESuspectList;

typedef struct {
    void    *entries;
    uint32_t count;
    uint32_t capacity;
} R8EExtRCTable;

typedef struct R8EGCState {
    R8ESuspectList  suspects;
    R8EExtRCTable   ext_rc;
    uint32_t        epoch_threshold;
    uint32_t        total_allocated;
    uint32_t        total_freed;
    uint32_t        cycles_collected;
} R8EGCState;

typedef struct R8EContext {
    R8EGCState gc;
} R8EContext;

/* =========================================================================
 * GC visitor callback type
 * ========================================================================= */

typedef void (*R8EGCVisitorFn)(R8EGCHeader *ref, void *user_data);

/* =========================================================================
 * External GC functions (defined in r8e_gc.c)
 * ========================================================================= */

extern void r8e_retain(R8EContext *ctx, R8EValue val);
extern void r8e_release(R8EContext *ctx, R8EValue val);
extern void r8e_gc_header_init(R8EGCHeader *hdr, uint32_t kind,
                                uint32_t proto_id);

/* =========================================================================
 * Prototype IDs
 * ========================================================================= */

#define R8E_PROTO_FUNCTION  2

/* =========================================================================
 * R8EEnvFrame - shared mutable upvalue storage
 * ========================================================================= */

/**
 * r8e_env_frame_new - create a new environment frame.
 *
 * All slots are initialized to undefined. Initial refcount = 1.
 * The frame is GC-tracked with kind = R8E_GC_KIND_ENVFRAME.
 */
R8EEnvFrame *r8e_env_frame_new(R8EContext *ctx, uint16_t slot_count) {
    R8EEnvFrame *frame = (R8EEnvFrame *)calloc(1, sizeof(R8EEnvFrame));
    if (!frame) return NULL;

    r8e_gc_header_init((R8EGCHeader *)frame, R8E_GC_KIND_ENVFRAME, 0);

    frame->refcount = 1;
    frame->slot_count = slot_count;
    frame->reserved = 0;

    if (slot_count > 0) {
        frame->slots = (R8EValue *)malloc(slot_count * sizeof(R8EValue));
        if (!frame->slots) {
            free(frame);
            return NULL;
        }
        /* Initialize all slots to undefined */
        for (uint16_t i = 0; i < slot_count; i++) {
            frame->slots[i] = R8E_UNDEFINED;
        }
    } else {
        frame->slots = NULL;
    }

    ctx->gc.total_allocated++;
    return frame;
}

/**
 * r8e_env_frame_retain - increment the env frame's refcount.
 *
 * This is a separate refcount from the GC header refcount. The env frame
 * refcount tracks how many closures share this frame. When it reaches zero,
 * the frame can be freed.
 */
void r8e_env_frame_retain(R8EEnvFrame *frame) {
    if (!frame) return;
    frame->refcount++;
}

/**
 * r8e_env_frame_release - decrement the env frame's refcount.
 *
 * When refcount reaches zero, release all slot values and free the frame.
 */
void r8e_env_frame_release(R8EContext *ctx, R8EEnvFrame *frame) {
    if (!frame) return;

    assert(frame->refcount > 0);
    frame->refcount--;

    if (frame->refcount == 0) {
        /* Release all values stored in slots */
        if (frame->slots) {
            for (uint16_t i = 0; i < frame->slot_count; i++) {
                r8e_release(ctx, frame->slots[i]);
            }
            free(frame->slots);
            frame->slots = NULL;
        }
        ctx->gc.total_freed++;
        free(frame);
    }
}

/**
 * r8e_env_frame_get - read a slot from an environment frame.
 */
R8EValue r8e_env_frame_get(const R8EEnvFrame *frame, uint16_t index) {
    assert(frame != NULL);
    assert(index < frame->slot_count);
    return frame->slots[index];
}

/**
 * r8e_env_frame_set - write a slot in an environment frame.
 *
 * Releases the old value and retains the new one.
 */
void r8e_env_frame_set(R8EContext *ctx, R8EEnvFrame *frame,
                        uint16_t index, R8EValue value) {
    assert(frame != NULL);
    assert(index < frame->slot_count);

    R8EValue old = frame->slots[index];
    if (old == value) return; /* No-op if same value */

    r8e_retain(ctx, value);
    frame->slots[index] = value;
    r8e_release(ctx, old);
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Check if all captures in a descriptor array are immutable.
 */
static bool r8e_all_captures_immutable(const R8ECaptureDesc *captures,
                                        uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (captures[i].mutability == R8E_CAPTURE_MUTABLE) {
            return false;
        }
    }
    return true;
}

/**
 * Determine the maximum frame slot index among mutable captures.
 * Returns 0 if no mutable captures exist.
 */
static uint16_t r8e_max_mutable_slot(const R8ECaptureDesc *captures,
                                      uint8_t count) {
    uint16_t max_slot = 0;
    bool found = false;
    for (uint8_t i = 0; i < count; i++) {
        if (captures[i].mutability == R8E_CAPTURE_MUTABLE) {
            if (!found || captures[i].frame_slot > max_slot) {
                max_slot = captures[i].frame_slot;
                found = true;
            }
        }
    }
    return found ? (uint16_t)(max_slot + 1) : 0;
}

/* =========================================================================
 * Closure creation: INLINE mode
 *
 * Used when count <= 2 and all captures are immutable.
 * Values are copied directly into inline_captures[].
 * No heap allocation beyond the closure struct itself.
 * ========================================================================= */

static R8EClosure *r8e_closure_new_inline(R8EContext *ctx,
                                           R8EFunction *func,
                                           const R8ECaptureDesc *captures,
                                           uint8_t count) {
    R8EClosure *cl = (R8EClosure *)calloc(1, sizeof(R8EClosure));
    if (!cl) return NULL;

    r8e_gc_header_init((R8EGCHeader *)cl, R8E_GC_KIND_CLOSURE,
                        R8E_PROTO_FUNCTION);

    cl->func = func;
    cl->capture_count = count;
    cl->capture_mode = R8E_CAPTURE_MODE_INLINE;
    cl->reserved = 0;

    /* Copy immutable values directly into inline storage */
    for (uint8_t i = 0; i < count && i < 2; i++) {
        cl->storage.inl.inline_captures[i] = captures[i].value;
        r8e_retain(ctx, captures[i].value);
    }
    /* Zero-fill unused slot */
    for (uint8_t i = count; i < 2; i++) {
        cl->storage.inl.inline_captures[i] = R8E_UNDEFINED;
    }

    /* Retain the function object */
    r8e_retain(ctx, r8e_from_pointer(func));

    ctx->gc.total_allocated++;
    return cl;
}

/* =========================================================================
 * Closure creation: ARRAY mode
 *
 * Used when count is 3-8, or count <= 2 with mutable captures.
 * Values are stored in a heap-allocated array with per-capture metadata.
 * Mutable captures reference slots in a shared R8EEnvFrame.
 * ========================================================================= */

static R8EClosure *r8e_closure_new_array(R8EContext *ctx,
                                          R8EFunction *func,
                                          const R8ECaptureDesc *captures,
                                          uint8_t count,
                                          R8EEnvFrame *env) {
    R8EClosure *cl = (R8EClosure *)calloc(1, sizeof(R8EClosure));
    if (!cl) return NULL;

    R8EValue *values = (R8EValue *)malloc(count * sizeof(R8EValue));
    R8ECaptureMeta *meta = (R8ECaptureMeta *)calloc(count,
                                                     sizeof(R8ECaptureMeta));
    if (!values || !meta) {
        free(values);
        free(meta);
        free(cl);
        return NULL;
    }

    r8e_gc_header_init((R8EGCHeader *)cl, R8E_GC_KIND_CLOSURE,
                        R8E_PROTO_FUNCTION);

    cl->func = func;
    cl->capture_count = count;
    cl->capture_mode = R8E_CAPTURE_MODE_ARRAY;
    cl->reserved = 0;

    /* Determine if we need a shared env frame for mutable captures */
    bool needs_env = false;
    for (uint8_t i = 0; i < count; i++) {
        if (captures[i].mutability == R8E_CAPTURE_MUTABLE) {
            needs_env = true;
            break;
        }
    }

    R8EEnvFrame *shared_env = NULL;
    if (needs_env) {
        if (env) {
            /* Reuse existing env frame */
            shared_env = env;
            r8e_env_frame_retain(shared_env);
        } else {
            /* Create a new env frame sized to hold all mutable slots */
            uint16_t frame_size = r8e_max_mutable_slot(captures, count);
            shared_env = r8e_env_frame_new(ctx, frame_size);
            if (!shared_env) {
                free(values);
                free(meta);
                free(cl);
                return NULL;
            }
        }
    }

    /* Populate values and metadata */
    for (uint8_t i = 0; i < count; i++) {
        if (captures[i].mutability == R8E_CAPTURE_IMMUTABLE) {
            /* Immutable: copy value directly */
            values[i] = captures[i].value;
            meta[i].kind = R8E_CAP_META_DIRECT;
            meta[i].index = 0;
            r8e_retain(ctx, captures[i].value);
        } else {
            /* Mutable: store the value in the env frame, record indirect ref */
            assert(shared_env != NULL);
            uint8_t slot = captures[i].frame_slot;
            assert(slot < shared_env->slot_count);

            /* If this is a new frame we created, populate the slot.
             * If reusing an existing frame, the slot may already have a value
             * from the enclosing scope. Only set if it was still undefined. */
            if (!env) {
                /* New frame: write the current value into the slot */
                r8e_retain(ctx, captures[i].value);
                R8EValue old = shared_env->slots[slot];
                shared_env->slots[slot] = captures[i].value;
                r8e_release(ctx, old);
            }

            values[i] = R8E_UNDEFINED; /* Not used for indirect captures */
            meta[i].kind = R8E_CAP_META_INDIRECT;
            meta[i].index = slot;
        }
    }

    cl->storage.arr.values = values;
    cl->storage.arr.meta = meta;
    cl->storage.arr.env = shared_env;

    /* Retain the function object */
    r8e_retain(ctx, r8e_from_pointer(func));

    ctx->gc.total_allocated++;
    return cl;
}

/* =========================================================================
 * Closure creation: FRAME mode
 *
 * Used when count >= 9, or when the caller explicitly provides a shared
 * env frame. All captures (mutable and immutable) go through the frame.
 * ========================================================================= */

static R8EClosure *r8e_closure_new_frame(R8EContext *ctx,
                                          R8EFunction *func,
                                          const R8ECaptureDesc *captures,
                                          uint8_t count,
                                          R8EEnvFrame *env) {
    R8EClosure *cl = (R8EClosure *)calloc(1, sizeof(R8EClosure));
    if (!cl) return NULL;

    r8e_gc_header_init((R8EGCHeader *)cl, R8E_GC_KIND_CLOSURE,
                        R8E_PROTO_FUNCTION);

    cl->func = func;
    cl->capture_count = count;
    cl->capture_mode = R8E_CAPTURE_MODE_FRAME;
    cl->reserved = 0;

    R8EEnvFrame *frame;
    if (env) {
        /* Reuse existing env frame */
        frame = env;
        r8e_env_frame_retain(frame);
    } else {
        /* Create a new env frame with enough slots for all captures.
         * We need max(frame_slot) + 1 slots for mutable captures, and
         * additional slots for immutable captures. Use count as slot count
         * when no frame slots are specified. */
        uint16_t frame_size = count;

        /* Check if any mutable capture requests a higher slot index */
        uint16_t max_slot = r8e_max_mutable_slot(captures, count);
        if (max_slot > frame_size) {
            frame_size = max_slot;
        }

        frame = r8e_env_frame_new(ctx, frame_size);
        if (!frame) {
            free(cl);
            return NULL;
        }

        /* Populate the frame with all capture values */
        for (uint8_t i = 0; i < count; i++) {
            uint16_t slot;
            if (captures[i].mutability == R8E_CAPTURE_MUTABLE) {
                slot = captures[i].frame_slot;
            } else {
                /* Immutable captures get their own sequential slot */
                slot = i;
            }
            if (slot < frame_size) {
                r8e_retain(ctx, captures[i].value);
                R8EValue old = frame->slots[slot];
                frame->slots[slot] = captures[i].value;
                r8e_release(ctx, old);
            }
        }
    }

    cl->storage.frm.env_frame = frame;

    /* Retain the function object */
    r8e_retain(ctx, r8e_from_pointer(func));

    ctx->gc.total_allocated++;
    return cl;
}

/* =========================================================================
 * r8e_closure_new - create a new closure with auto-selected storage mode
 *
 * @param ctx             Engine context (for allocation and GC)
 * @param func            The function this closure wraps
 * @param captures        Array of capture descriptors (count entries)
 * @param count           Number of captures (0-255)
 * @param mutability_flags Bitmask: bit i set = capture i is mutable.
 *                         If NULL, uses the mutability field in each desc.
 * @return                Newly allocated R8EClosure, or NULL on OOM
 *
 * Storage mode auto-selection:
 *   count == 0          -> INLINE (empty closure, rare but valid)
 *   count <= 2, all imm -> INLINE (~52% of closures)
 *   count <= 8          -> ARRAY  (~28%)
 *   count > 8           -> FRAME  (~20%)
 *   count <= 2 + mutable -> promoted to ARRAY
 * ========================================================================= */

R8EClosure *r8e_closure_new(R8EContext *ctx, R8EFunction *func,
                             const R8ECaptureDesc *captures, uint8_t count,
                             R8EEnvFrame *env) {
    if (!ctx || !func) return NULL;

    /* Zero captures: create an INLINE closure with no captures */
    if (count == 0) {
        return r8e_closure_new_inline(ctx, func, NULL, 0);
    }

    if (!captures) return NULL;

    bool all_immutable = r8e_all_captures_immutable(captures, count);

    /* Mode selection */
    if (count <= 2 && all_immutable && !env) {
        return r8e_closure_new_inline(ctx, func, captures, count);
    } else if (count <= 8) {
        return r8e_closure_new_array(ctx, func, captures, count, env);
    } else {
        return r8e_closure_new_frame(ctx, func, captures, count, env);
    }
}

/* =========================================================================
 * Capture access: r8e_closure_get_capture
 *
 * Dispatches by capture_mode to retrieve the value at the given index.
 * ========================================================================= */

R8EValue r8e_closure_get_capture(const R8EClosure *closure, uint8_t index) {
    assert(closure != NULL);
    assert(index < closure->capture_count);

    switch (closure->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        assert(index < 2);
        return closure->storage.inl.inline_captures[index];

    case R8E_CAPTURE_MODE_ARRAY:
        assert(closure->storage.arr.values != NULL);
        assert(closure->storage.arr.meta != NULL);

        if (closure->storage.arr.meta[index].kind == R8E_CAP_META_DIRECT) {
            /* Immutable: read directly from values array */
            return closure->storage.arr.values[index];
        } else {
            /* Mutable: read from env frame slot */
            assert(closure->storage.arr.env != NULL);
            uint8_t slot = closure->storage.arr.meta[index].index;
            assert(slot < closure->storage.arr.env->slot_count);
            return closure->storage.arr.env->slots[slot];
        }

    case R8E_CAPTURE_MODE_FRAME:
        assert(closure->storage.frm.env_frame != NULL);
        assert(index < closure->storage.frm.env_frame->slot_count);
        return closure->storage.frm.env_frame->slots[index];

    default:
        /* Should never reach here */
        return R8E_UNDEFINED;
    }
}

/* =========================================================================
 * Capture mutation: r8e_closure_set_capture
 *
 * Only valid for mutable captures. Returns true on success, false if the
 * capture at the given index is immutable.
 * ========================================================================= */

bool r8e_closure_set_capture(R8EContext *ctx, R8EClosure *closure,
                              uint8_t index, R8EValue value) {
    assert(closure != NULL);
    assert(index < closure->capture_count);

    switch (closure->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        /* INLINE mode only contains immutable captures */
        return false;

    case R8E_CAPTURE_MODE_ARRAY:
        assert(closure->storage.arr.meta != NULL);

        if (closure->storage.arr.meta[index].kind == R8E_CAP_META_DIRECT) {
            /* Immutable capture: cannot set */
            return false;
        } else {
            /* Mutable: write to env frame slot */
            assert(closure->storage.arr.env != NULL);
            uint8_t slot = closure->storage.arr.meta[index].index;
            r8e_env_frame_set(ctx, closure->storage.arr.env, slot, value);
            return true;
        }

    case R8E_CAPTURE_MODE_FRAME:
        assert(closure->storage.frm.env_frame != NULL);
        assert(index < closure->storage.frm.env_frame->slot_count);
        r8e_env_frame_set(ctx, closure->storage.frm.env_frame, index, value);
        return true;

    default:
        return false;
    }
}

/* =========================================================================
 * Capture mutability query
 * ========================================================================= */

bool r8e_closure_is_capture_mutable(const R8EClosure *closure, uint8_t index) {
    assert(closure != NULL);
    assert(index < closure->capture_count);

    switch (closure->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        /* INLINE captures are always immutable */
        return false;

    case R8E_CAPTURE_MODE_ARRAY:
        assert(closure->storage.arr.meta != NULL);
        return (closure->storage.arr.meta[index].kind == R8E_CAP_META_INDIRECT);

    case R8E_CAPTURE_MODE_FRAME:
        /* FRAME mode: all captures are mutable (they go through the frame) */
        return true;

    default:
        return false;
    }
}

/* =========================================================================
 * Closure GC integration: reference visitor
 *
 * Used by the GC cycle detector to enumerate all heap references held
 * by a closure. This is also defined in r8e_gc.c's r8e_gc_visit_refs()
 * for the R8E_GC_KIND_CLOSURE case, but provided here as a standalone
 * function for direct use by other modules.
 * ========================================================================= */

static void r8e_visit_value_closure(R8EValue val, R8EGCVisitorFn visitor,
                                     void *ud) {
    if (R8E_IS_POINTER(val)) {
        R8EGCHeader *ref = (R8EGCHeader *)r8e_get_pointer(val);
        if (ref) visitor(ref, ud);
    }
}

void r8e_closure_visit_refs(const R8EClosure *closure,
                             R8EGCVisitorFn visitor, void *ud) {
    if (!closure) return;

    /* Visit the function */
    if (closure->func) {
        visitor((R8EGCHeader *)closure->func, ud);
    }

    switch (closure->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        for (uint8_t i = 0; i < closure->capture_count && i < 2; i++) {
            r8e_visit_value_closure(closure->storage.inl.inline_captures[i],
                                    visitor, ud);
        }
        break;

    case R8E_CAPTURE_MODE_ARRAY:
        if (closure->storage.arr.values && closure->storage.arr.meta) {
            for (uint8_t i = 0; i < closure->capture_count; i++) {
                if (closure->storage.arr.meta[i].kind == R8E_CAP_META_DIRECT) {
                    r8e_visit_value_closure(closure->storage.arr.values[i],
                                            visitor, ud);
                }
            }
        }
        if (closure->storage.arr.env) {
            visitor((R8EGCHeader *)closure->storage.arr.env, ud);
        }
        break;

    case R8E_CAPTURE_MODE_FRAME:
        if (closure->storage.frm.env_frame) {
            visitor((R8EGCHeader *)closure->storage.frm.env_frame, ud);
        }
        break;
    }
}

/**
 * r8e_envframe_visit_refs - visit all heap references in an env frame.
 */
void r8e_envframe_visit_refs(const R8EEnvFrame *frame,
                              R8EGCVisitorFn visitor, void *ud) {
    if (!frame || !frame->slots) return;

    for (uint16_t i = 0; i < frame->slot_count; i++) {
        r8e_visit_value_closure(frame->slots[i], visitor, ud);
    }
}

/* =========================================================================
 * Closure info queries (useful for debugging and the interpreter)
 * ========================================================================= */

/**
 * r8e_closure_get_function - return the function associated with a closure.
 */
R8EFunction *r8e_closure_get_function(const R8EClosure *closure) {
    assert(closure != NULL);
    return closure->func;
}

/**
 * r8e_closure_capture_count - return the number of captures.
 */
uint8_t r8e_closure_capture_count(const R8EClosure *closure) {
    assert(closure != NULL);
    return closure->capture_count;
}

/**
 * r8e_closure_capture_mode - return the capture storage mode.
 */
uint8_t r8e_closure_capture_mode(const R8EClosure *closure) {
    assert(closure != NULL);
    return closure->capture_mode;
}

/**
 * r8e_closure_get_env_frame - return the env frame (ARRAY or FRAME mode).
 * Returns NULL for INLINE mode.
 */
R8EEnvFrame *r8e_closure_get_env_frame(const R8EClosure *closure) {
    assert(closure != NULL);
    switch (closure->capture_mode) {
    case R8E_CAPTURE_MODE_ARRAY:
        return closure->storage.arr.env;
    case R8E_CAPTURE_MODE_FRAME:
        return closure->storage.frm.env_frame;
    default:
        return NULL;
    }
}
