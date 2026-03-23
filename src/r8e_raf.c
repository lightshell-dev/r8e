/*
 * r8e_raf.c - requestAnimationFrame / cancelAnimationFrame
 *
 * Part of the r8e JavaScript engine.
 *
 * Implements:
 *   - requestAnimationFrame(callback) -> numeric ID
 *   - cancelAnimationFrame(id) -> void
 *   - r8e_raf_tick(ctx) -> fire all pending callbacks (called by event loop)
 *
 * The rAF queue is a static array. Callbacks fire once per tick.
 * Callbacks registered during firing go into the NEXT frame
 * (we snapshot the current queue, clear, then fire the snapshot).
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/r8e_types.h"
#include "../include/r8e_api.h"

/* =========================================================================
 * rAF Queue (static, per-process)
 * ========================================================================= */

#define RAF_MAX 256

typedef struct {
    R8EValue callback;
    uint32_t id;
    bool     cancelled;
} RAFEntry;

static RAFEntry  g_raf_queue[RAF_MAX];
static uint32_t  g_raf_count   = 0;
static uint32_t  g_raf_next_id = 1;

/* =========================================================================
 * C API
 * ========================================================================= */

uint32_t r8e_raf_request(R8EContext *ctx, R8EValue callback) {
    (void)ctx;
    if (g_raf_count >= RAF_MAX) return 0;

    uint32_t id = g_raf_next_id++;
    g_raf_queue[g_raf_count].callback  = callback;
    g_raf_queue[g_raf_count].id        = id;
    g_raf_queue[g_raf_count].cancelled = false;
    g_raf_count++;

    return id;
}

void r8e_raf_cancel(R8EContext *ctx, uint32_t id) {
    (void)ctx;
    for (uint32_t i = 0; i < g_raf_count; i++) {
        if (g_raf_queue[i].id == id) {
            g_raf_queue[i].cancelled = true;
            return;
        }
    }
}

void r8e_raf_tick(R8EContext *ctx) {
    if (g_raf_count == 0) return;

    /* Snapshot current queue */
    uint32_t  snap_count = g_raf_count;
    RAFEntry  snap[RAF_MAX];
    memcpy(snap, g_raf_queue, snap_count * sizeof(RAFEntry));

    /* Clear the queue so callbacks registered during firing go to next frame */
    g_raf_count = 0;

    /* Fire each non-cancelled callback */
    for (uint32_t i = 0; i < snap_count; i++) {
        if (!snap[i].cancelled) {
            /* Pass a timestamp-like argument (the ID as a number for now) */
            R8EValue arg = r8e_make_number(0.0);
            r8e_call(ctx, snap[i].callback, R8E_UNDEFINED, 1, &arg);
        }
    }
}

/* Reset state (for tests) */
void r8e_raf_reset(void) {
    g_raf_count   = 0;
    g_raf_next_id = 1;
}

/* =========================================================================
 * JS-facing native functions
 * ========================================================================= */

static R8EValue js_requestAnimationFrame(R8EContext *ctx, R8EValue this_val,
                                          int argc, const R8EValue *argv) {
    (void)this_val;
    if (argc < 1 || !r8e_is_function(argv[0])) {
        return R8E_UNDEFINED;
    }
    uint32_t id = r8e_raf_request(ctx, argv[0]);
    return r8e_make_number((double)id);
}

static R8EValue js_cancelAnimationFrame(R8EContext *ctx, R8EValue this_val,
                                         int argc, const R8EValue *argv) {
    (void)this_val;
    if (argc < 1) return R8E_UNDEFINED;
    int32_t id = r8e_to_int32(argv[0]);
    if (id > 0) {
        r8e_raf_cancel(ctx, (uint32_t)id);
    }
    return R8E_UNDEFINED;
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void r8e_raf_init(R8EContext *ctx) {
    r8e_set_global_func(ctx, "requestAnimationFrame",
                         js_requestAnimationFrame, 1);
    r8e_set_global_func(ctx, "cancelAnimationFrame",
                         js_cancelAnimationFrame, 1);
}
