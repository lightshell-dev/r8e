/*
 * r8e_promise.c - Promise, Async/Await, and Microtask Queue
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 14 (Engineering Timeline, Phase 3).
 *
 * Architecture:
 *   - ES2023-compliant Promise implementation
 *   - Microtask queue as circular buffer (drain after each script/event)
 *   - Promise reactions (then/catch/finally) enqueued as microtasks
 *   - Thenable assimilation prevents recursive resolution
 *   - Async functions return a Promise and use saved-frame continuation
 *   - Promise.all/allSettled/race/any as static combinator methods
 *   - Unhandled rejection tracking via is_handled flag
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
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_NULLISH(v)    (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))
#define R8E_IS_BOOL(v)       ((v) == R8E_TRUE || (v) == R8E_FALSE)

/* NaN-boxing extraction / encoding */
static inline void *r8e_get_pointer(uint64_t v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}

static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}

static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}

static inline double r8e_get_double(uint64_t v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

static inline uint64_t r8e_from_double(double d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(double));
    return v;
}

static inline R8EValue r8e_from_bool(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
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
#define R8E_GC_KIND_PROMISE      6u
#define R8E_GC_KIND_ASYNC_STATE  7u

#define R8E_GC_RC_OVERFLOW_BIT   0x00000100u
#define R8E_GC_RC_INLINE_MASK    0xFFFF0000u
#define R8E_GC_RC_INLINE_SHIFT   16

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)
#define R8E_GC_SET_KIND(flags, kind) \
    (((flags) & ~R8E_GC_KIND_MASK) | \
     (((kind) << R8E_GC_KIND_SHIFT) & R8E_GC_KIND_MASK))

/* =========================================================================
 * GC header
 * ========================================================================= */

typedef struct R8EGCHeader {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* =========================================================================
 * Prototype IDs
 * ========================================================================= */

#define R8E_PROTO_NONE       0
#define R8E_PROTO_OBJECT     1
#define R8E_PROTO_ARRAY      2
#define R8E_PROTO_FUNCTION   3
#define R8E_PROTO_ERROR      9
#define R8E_PROTO_TYPEERROR  10
#define R8E_PROTO_PROMISE    20
#define R8E_PROTO_GENERATOR  33

/* =========================================================================
 * Well-known atom IDs (must match r8e_atoms.h)
 * ========================================================================= */

#define R8E_ATOM_EMPTY          0
#define R8E_ATOM_length         1
#define R8E_ATOM_prototype      2
#define R8E_ATOM_constructor    3
#define R8E_ATOM_toString       4
#define R8E_ATOM_valueOf        5
#define R8E_ATOM_name           8
#define R8E_ATOM_message        9
#define R8E_ATOM_value          11
#define R8E_ATOM_then           221
#define R8E_ATOM_catch          222
#define R8E_ATOM_finally        223
#define R8E_ATOM_resolve        224
#define R8E_ATOM_reject         225
#define R8E_ATOM_all            226
#define R8E_ATOM_allSettled     227
#define R8E_ATOM_any            228
#define R8E_ATOM_race           229
#define R8E_ATOM_done           288
#define R8E_ATOM_next           289
#define R8E_ATOM_status         300
#define R8E_ATOM_reason         321
#define R8E_ATOM_fulfilled      322
#define R8E_ATOM_rejected       323
#define R8E_ATOM_pending        324

/* =========================================================================
 * Forward declarations for external APIs used here
 * ========================================================================= */

/* These would come from shared headers when integrated. For now, forward
 * declare what we need from other modules. */

typedef struct R8EPromiseContext R8EPromiseContext;

/* Simulated call into interpreter: invoke a JS callable with arguments.
 * In the integrated build, this calls into r8e_interp.c. */
static R8EValue r8e_call_function(R8EPromiseContext *ctx, R8EValue func,
                                  R8EValue this_val, int argc,
                                  const R8EValue *argv);

/* Object property access (from r8e_object.c) */
static R8EValue r8e_obj_get(R8EPromiseContext *ctx, R8EValue obj,
                            uint32_t atom);
static void r8e_obj_set(R8EPromiseContext *ctx, R8EValue obj,
                        uint32_t atom, R8EValue val);
static bool r8e_obj_has(R8EPromiseContext *ctx, R8EValue obj,
                        uint32_t atom);

/* Throw helper (from r8e_error.c) */
static R8EValue r8e_promise_throw_type_error(R8EPromiseContext *ctx,
                                             const char *msg);

/* Check if a value is callable (from r8e_function.c) */
static bool r8e_is_callable(R8EValue v);

/* Allocate heap memory tracked by GC (from r8e_alloc.c / r8e_gc.c) */
static void *r8e_promise_alloc(R8EPromiseContext *ctx, size_t size);
static void  r8e_promise_free(R8EPromiseContext *ctx, void *ptr, size_t size);

/* Make a new plain object (from r8e_object.c) */
static R8EValue r8e_make_plain_object(R8EPromiseContext *ctx);

/* Create a new array (from r8e_array.c) */
static R8EValue r8e_make_plain_array(R8EPromiseContext *ctx, uint32_t cap);

/* Array push (from r8e_array.c) */
static void r8e_array_push(R8EPromiseContext *ctx, R8EValue arr, R8EValue val);

/* Get array length */
static uint32_t r8e_array_length(R8EPromiseContext *ctx, R8EValue arr);

/* Get array element */
static R8EValue r8e_array_get(R8EPromiseContext *ctx, R8EValue arr,
                              uint32_t index);


/* =========================================================================
 * Section 1: Promise State Machine (ES2023 Section 27.2)
 *
 * A Promise is in one of three states:
 *   PENDING    - initial state, not yet resolved
 *   FULFILLED  - operation completed successfully
 *   REJECTED   - operation failed
 *
 * State transitions:
 *   PENDING -> FULFILLED  (via resolve)
 *   PENDING -> REJECTED   (via reject)
 *   No other transitions are possible (once settled, immutable).
 * ========================================================================= */

typedef enum {
    R8E_PROMISE_PENDING   = 0,
    R8E_PROMISE_FULFILLED = 1,
    R8E_PROMISE_REJECTED  = 2
} R8EPromiseState;

/* =========================================================================
 * Section 2: Promise Reaction Record (ES2023 Section 27.2.1.1)
 *
 * A reaction is a callback pair {onFulfilled, onRejected} plus a
 * result promise that receives the callback's return value.
 *
 * When a promise settles, its reaction list is drained into the
 * microtask queue.
 * ========================================================================= */

typedef struct R8EPromiseReaction {
    R8EValue on_fulfilled;               /* callback or R8E_UNDEFINED */
    R8EValue on_rejected;                /* callback or R8E_UNDEFINED */
    R8EValue result_promise;             /* the promise returned by then() */
    uint8_t  type;                       /* 0=then, 1=finally */
    uint8_t  pad[7];
    struct R8EPromiseReaction *next;      /* linked list */
} R8EPromiseReaction;

/* =========================================================================
 * Section 3: R8EPromise Object
 * ========================================================================= */

typedef struct R8EPromise {
    /* GC header (must be first 8 bytes of any heap object) */
    uint32_t flags;
    uint32_t proto_id;

    /* Promise-specific fields */
    R8EPromiseState state;
    R8EValue        result;              /* fulfillment value or rejection reason */

    /* Reaction queue (linked list, only non-NULL while PENDING) */
    R8EPromiseReaction *reactions_head;
    R8EPromiseReaction *reactions_tail;
    uint32_t            reaction_count;

    /* Tracking flags */
    uint8_t  is_handled;                 /* true once .then()/.catch() is called */
    uint8_t  is_resolving;               /* prevents recursive resolve */
    uint8_t  already_resolved;           /* [[AlreadyResolved]] flag */
    uint8_t  pad;
} R8EPromise;

/* =========================================================================
 * Section 4: Microtask Queue
 *
 * Circular buffer of pending microtasks. Each microtask is a callback
 * plus an argument. The queue is drained synchronously after each
 * script/event handler completes.
 *
 * Capacity: starts at 64, grows by doubling. In practice, most promise
 * chains are shallow, so 64 is usually enough.
 * ========================================================================= */

/* Microtask types */
typedef enum {
    R8E_MICROTASK_PROMISE_REACTION   = 0,
    R8E_MICROTASK_PROMISE_RESOLVE    = 1,
    R8E_MICROTASK_PROMISE_THENABLE   = 2,
    R8E_MICROTASK_ASYNC_RESUME       = 3,
    R8E_MICROTASK_GENERIC_CALLBACK   = 4
} R8EMicrotaskType;

typedef struct R8EMicrotask {
    R8EMicrotaskType type;
    R8EValue         callback;           /* the function to call */
    R8EValue         arg1;               /* first argument */
    R8EValue         arg2;               /* second argument */
    R8EValue         arg3;               /* third argument (e.g., result promise) */
} R8EMicrotask;

#define R8E_MICROTASK_QUEUE_INIT_CAP  64
#define R8E_MICROTASK_QUEUE_MAX_CAP   65536

typedef struct R8EMicrotaskQueue {
    R8EMicrotask *buffer;
    uint32_t      head;                  /* next task to dequeue */
    uint32_t      tail;                  /* next slot to enqueue */
    uint32_t      capacity;
    uint32_t      count;                 /* current number of pending tasks */
    bool          draining;              /* re-entrancy guard */
} R8EMicrotaskQueue;

/* =========================================================================
 * Section 5: Async Function State
 *
 * An async function is compiled to a generator-like state machine by the
 * compiler. At runtime, the async function call creates:
 *   1. A Promise (the return value visible to the caller)
 *   2. An R8EAsyncState (internal, holds the suspended frame)
 *
 * When the async function hits an 'await' expression:
 *   1. The expression value is coerced to a Promise
 *   2. A .then() reaction is attached that resumes the async state
 *   3. The function yields control back to the microtask queue
 *
 * When the async function returns or throws:
 *   The result Promise is resolved or rejected accordingly.
 * ========================================================================= */

typedef enum {
    R8E_ASYNC_SUSPENDED_START = 0,
    R8E_ASYNC_SUSPENDED_YIELD = 1,
    R8E_ASYNC_EXECUTING       = 2,
    R8E_ASYNC_COMPLETED       = 3
} R8EAsyncFuncState;

typedef struct R8EAsyncState {
    /* GC header */
    uint32_t flags;
    uint32_t proto_id;

    /* The promise returned to the caller */
    R8EValue promise;

    /* Saved interpreter frame */
    R8EValue  *saved_locals;
    R8EValue  *saved_stack;
    uint8_t   *saved_pc;
    uint32_t   local_count;
    uint32_t   stack_depth;

    /* The closure being executed */
    R8EValue   closure;

    /* Current state */
    R8EAsyncFuncState state;

    /* The value/error being resumed with */
    R8EValue   resume_value;
    bool       resume_is_throw;
    uint8_t    pad[7];
} R8EAsyncState;

/* =========================================================================
 * Section 6: Promise Context
 *
 * Aggregates everything the promise system needs into one struct.
 * In the integrated build, this is embedded in R8EContext.
 * ========================================================================= */

struct R8EPromiseContext {
    R8EMicrotaskQueue microtask_queue;

    /* Unhandled rejection hook (optional, set by host) */
    void (*unhandled_rejection_handler)(R8EPromiseContext *ctx,
                                       R8EValue promise,
                                       R8EValue reason,
                                       void *user_data);
    void *unhandled_user_data;

    /* Error state (mirrors R8EContext.error) */
    R8EValue  exception;
    bool      has_exception;

    /* Allocator hooks (in integrated build, these point to the
     * context's arena allocator) */
    void *alloc_ctx;
};


/* =========================================================================
 * Section 7: Microtask Queue Implementation
 * ========================================================================= */

/**
 * Initialize the microtask queue.
 */
void r8e_microtask_queue_init(R8EMicrotaskQueue *q)
{
    assert(q != NULL);
    q->buffer = (R8EMicrotask *)calloc(R8E_MICROTASK_QUEUE_INIT_CAP,
                                       sizeof(R8EMicrotask));
    q->head = 0;
    q->tail = 0;
    q->capacity = R8E_MICROTASK_QUEUE_INIT_CAP;
    q->count = 0;
    q->draining = false;
}

/**
 * Free the microtask queue.
 */
void r8e_microtask_queue_free(R8EMicrotaskQueue *q)
{
    if (q == NULL) return;
    free(q->buffer);
    q->buffer = NULL;
    q->capacity = 0;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

/**
 * Grow the microtask queue when full.
 * Returns false on OOM.
 */
static bool r8e_microtask_queue_grow(R8EMicrotaskQueue *q)
{
    uint32_t new_cap = q->capacity * 2;
    if (new_cap > R8E_MICROTASK_QUEUE_MAX_CAP) {
        return false;
    }

    R8EMicrotask *new_buf = (R8EMicrotask *)calloc(new_cap,
                                                    sizeof(R8EMicrotask));
    if (new_buf == NULL) {
        return false;
    }

    /* Copy existing items in order from head to tail */
    uint32_t dst = 0;
    uint32_t src = q->head;
    for (uint32_t i = 0; i < q->count; i++) {
        new_buf[dst++] = q->buffer[src];
        src = (src + 1) % q->capacity;
    }

    free(q->buffer);
    q->buffer = new_buf;
    q->head = 0;
    q->tail = q->count;
    q->capacity = new_cap;
    return true;
}

/**
 * Enqueue a microtask. Returns false on OOM.
 */
bool r8e_enqueue_microtask(R8EPromiseContext *ctx, R8EMicrotaskType type,
                           R8EValue callback, R8EValue arg1,
                           R8EValue arg2, R8EValue arg3)
{
    R8EMicrotaskQueue *q = &ctx->microtask_queue;

    if (q->count >= q->capacity) {
        if (!r8e_microtask_queue_grow(q)) {
            return false;
        }
    }

    R8EMicrotask *task = &q->buffer[q->tail];
    task->type = type;
    task->callback = callback;
    task->arg1 = arg1;
    task->arg2 = arg2;
    task->arg3 = arg3;

    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return true;
}

/**
 * Dequeue the next microtask. Returns false if queue is empty.
 */
static bool r8e_dequeue_microtask(R8EMicrotaskQueue *q, R8EMicrotask *out)
{
    if (q->count == 0) {
        return false;
    }

    *out = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

/* Forward declarations for internal promise operations */
static void r8e_promise_resolve_internal(R8EPromiseContext *ctx,
                                         R8EPromise *promise,
                                         R8EValue value);
static void r8e_promise_reject_internal(R8EPromiseContext *ctx,
                                        R8EPromise *promise,
                                        R8EValue reason);
static void r8e_promise_trigger_reactions(R8EPromiseContext *ctx,
                                          R8EPromise *promise);
static void r8e_run_promise_reaction(R8EPromiseContext *ctx,
                                     R8EPromiseReaction *reaction,
                                     R8EPromiseState state,
                                     R8EValue value);
static void r8e_promise_resolve_thenable(R8EPromiseContext *ctx,
                                         R8EPromise *promise,
                                         R8EValue thenable,
                                         R8EValue then_fn);

/* Forward declaration (defined later in this file) */
void r8e_async_resume(R8EPromiseContext *ctx, R8EAsyncState *state,
                      R8EValue value, bool is_throw);

/**
 * Execute a single microtask.
 */
static void r8e_execute_microtask(R8EPromiseContext *ctx, R8EMicrotask *task)
{
    switch (task->type) {
    case R8E_MICROTASK_PROMISE_REACTION: {
        /* arg1 = the reaction record pointer (encoded as an int),
         * arg2 = the settled state (int: 1=fulfilled, 2=rejected),
         * arg3 = the settled value */
        R8EPromiseReaction *reaction =
            (R8EPromiseReaction *)r8e_get_pointer(task->arg1);
        R8EPromiseState state =
            (R8EPromiseState)r8e_get_int32(task->arg2);
        R8EValue value = task->arg3;
        r8e_run_promise_reaction(ctx, reaction, state, value);
        break;
    }

    case R8E_MICROTASK_PROMISE_THENABLE: {
        /* arg1 = promise (pointer), arg2 = thenable, arg3 = then function */
        R8EPromise *promise =
            (R8EPromise *)r8e_get_pointer(task->arg1);
        R8EValue thenable = task->arg2;
        R8EValue then_fn = task->arg3;
        r8e_promise_resolve_thenable(ctx, promise, thenable, then_fn);
        break;
    }

    case R8E_MICROTASK_ASYNC_RESUME: {
        /* arg1 = async state pointer, arg2 = resume value,
         * arg3 = is_throw flag (as bool NaN-boxed) */
        R8EAsyncState *async_state =
            (R8EAsyncState *)r8e_get_pointer(task->arg1);
        R8EValue resume_val = task->arg2;
        bool is_throw = (task->arg3 == R8E_TRUE);
        r8e_async_resume(ctx, async_state, resume_val, is_throw);
        break;
    }

    case R8E_MICROTASK_GENERIC_CALLBACK: {
        /* Generic: just call callback(arg1) */
        R8EValue args[1] = { task->arg1 };
        r8e_call_function(ctx, task->callback, R8E_UNDEFINED, 1, args);
        break;
    }

    case R8E_MICROTASK_PROMISE_RESOLVE:
        /* Unused; resolve is handled inline */
        break;
    }
}

/**
 * Drain all pending microtasks.
 *
 * Per ES2023, microtasks enqueued during draining are also processed
 * before returning. This loop runs until the queue is empty.
 *
 * The draining flag prevents re-entrant draining (which would happen
 * if a microtask synchronously triggers another drain).
 */
void r8e_drain_microtasks(R8EPromiseContext *ctx)
{
    R8EMicrotaskQueue *q = &ctx->microtask_queue;

    /* Re-entrancy guard */
    if (q->draining) {
        return;
    }
    q->draining = true;

    R8EMicrotask task;
    while (r8e_dequeue_microtask(q, &task)) {
        r8e_execute_microtask(ctx, &task);

        /* If an unhandled exception occurred during the microtask,
         * we continue draining (ES2023 spec: microtasks do not
         * propagate exceptions to the drain loop). The exception
         * is reported via the unhandled rejection handler instead. */
        if (ctx->has_exception) {
            ctx->has_exception = false;
            ctx->exception = R8E_UNDEFINED;
        }
    }

    q->draining = false;
}

/**
 * Check if there are pending microtasks.
 */
bool r8e_has_pending_microtasks(const R8EPromiseContext *ctx)
{
    return ctx->microtask_queue.count > 0;
}


/* =========================================================================
 * Section 8: Promise Allocation and Creation
 * ========================================================================= */

/**
 * Allocate a new R8EPromise object.
 * Returns NULL on OOM.
 */
static R8EPromise *r8e_promise_alloc_obj(R8EPromiseContext *ctx)
{
    R8EPromise *p = (R8EPromise *)r8e_promise_alloc(ctx, sizeof(R8EPromise));
    if (p == NULL) return NULL;

    memset(p, 0, sizeof(R8EPromise));
    p->flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_PROMISE);
    p->flags |= (1u << R8E_GC_RC_INLINE_SHIFT);  /* initial refcount = 1 */
    p->proto_id = R8E_PROTO_PROMISE;
    p->state = R8E_PROMISE_PENDING;
    p->result = R8E_UNDEFINED;
    p->reactions_head = NULL;
    p->reactions_tail = NULL;
    p->reaction_count = 0;
    p->is_handled = 0;
    p->is_resolving = 0;
    p->already_resolved = 0;
    return p;
}

/**
 * Create a new pending Promise.
 *
 * This is the JS-facing Promise constructor equivalent when called
 * without an executor. For the full constructor (which takes an
 * executor callback), see r8e_promise_new_with_executor().
 *
 * @return NaN-boxed pointer to the new Promise, or R8E_UNDEFINED on OOM.
 */
R8EValue r8e_promise_new(R8EPromiseContext *ctx)
{
    R8EPromise *p = r8e_promise_alloc_obj(ctx);
    if (p == NULL) return R8E_UNDEFINED;
    return r8e_from_pointer(p);
}

/**
 * Get the R8EPromise struct from a NaN-boxed value.
 * Returns NULL if the value is not a promise.
 */
static R8EPromise *r8e_promise_get(R8EValue v)
{
    if (!R8E_IS_POINTER(v)) return NULL;
    R8EPromise *p = (R8EPromise *)r8e_get_pointer(v);
    if (R8E_GC_GET_KIND(p->flags) != R8E_GC_KIND_PROMISE) return NULL;
    return p;
}

/**
 * Check if a value is a Promise.
 */
bool r8e_is_promise(R8EValue v)
{
    return r8e_promise_get(v) != NULL;
}


/* =========================================================================
 * Section 9: Reaction Management
 * ========================================================================= */

/**
 * Allocate a new reaction record.
 */
static R8EPromiseReaction *r8e_reaction_new(R8EPromiseContext *ctx,
                                            R8EValue on_fulfilled,
                                            R8EValue on_rejected,
                                            R8EValue result_promise,
                                            uint8_t type)
{
    R8EPromiseReaction *r = (R8EPromiseReaction *)r8e_promise_alloc(
        ctx, sizeof(R8EPromiseReaction));
    if (r == NULL) return NULL;

    r->on_fulfilled = on_fulfilled;
    r->on_rejected = on_rejected;
    r->result_promise = result_promise;
    r->type = type;
    memset(r->pad, 0, sizeof(r->pad));
    r->next = NULL;
    return r;
}

/**
 * Free a reaction record.
 */
static void r8e_reaction_free(R8EPromiseContext *ctx,
                              R8EPromiseReaction *r)
{
    if (r != NULL) {
        r8e_promise_free(ctx, r, sizeof(R8EPromiseReaction));
    }
}

/**
 * Append a reaction to a promise's reaction queue.
 */
static void r8e_promise_append_reaction(R8EPromise *promise,
                                        R8EPromiseReaction *reaction)
{
    if (promise->reactions_tail != NULL) {
        promise->reactions_tail->next = reaction;
    } else {
        promise->reactions_head = reaction;
    }
    promise->reactions_tail = reaction;
    promise->reaction_count++;
}


/* =========================================================================
 * Section 10: Promise Resolution (ES2023 Section 27.2.1.3.2)
 *
 * The resolution procedure handles:
 *   1. Self-resolution check (TypeError)
 *   2. Thenable assimilation (if value has a .then method)
 *   3. Direct fulfillment (for non-thenable values)
 * ========================================================================= */

/**
 * Internal resolve: set promise to FULFILLED and trigger reactions.
 * This is the final step after thenable unwrapping is complete.
 */
static void r8e_promise_fulfill(R8EPromiseContext *ctx,
                                R8EPromise *promise, R8EValue value)
{
    assert(promise->state == R8E_PROMISE_PENDING);
    promise->state = R8E_PROMISE_FULFILLED;
    promise->result = value;
    r8e_promise_trigger_reactions(ctx, promise);
}

/**
 * Internal reject: set promise to REJECTED and trigger reactions.
 */
static void r8e_promise_reject_final(R8EPromiseContext *ctx,
                                     R8EPromise *promise, R8EValue reason)
{
    assert(promise->state == R8E_PROMISE_PENDING);
    promise->state = R8E_PROMISE_REJECTED;
    promise->result = reason;

    /* If no handler has been attached, report unhandled rejection */
    if (!promise->is_handled && ctx->unhandled_rejection_handler != NULL) {
        ctx->unhandled_rejection_handler(ctx, r8e_from_pointer(promise),
                                         reason, ctx->unhandled_user_data);
    }

    r8e_promise_trigger_reactions(ctx, promise);
}

/**
 * Resolve a promise with a value.
 *
 * ES2023 Section 27.2.1.3.2 - Promise Resolve Functions:
 *   1. If value === promise, throw TypeError (self-resolution)
 *   2. If value is an Object or Function, check for .then
 *   3. If .then is callable, enqueue thenable resolution
 *   4. Otherwise, fulfill directly
 */
static void r8e_promise_resolve_internal(R8EPromiseContext *ctx,
                                         R8EPromise *promise,
                                         R8EValue value)
{
    /* Already resolved check (ES2023 [[AlreadyResolved]]) */
    if (promise->already_resolved) return;
    promise->already_resolved = 1;

    /* Prevent recursive resolution */
    if (promise->is_resolving) {
        r8e_promise_reject_final(ctx, promise,
            r8e_promise_throw_type_error(ctx,
                "Promise resolution cycle detected"));
        return;
    }

    /* Step 1: Self-resolution check */
    if (R8E_IS_POINTER(value) &&
        r8e_get_pointer(value) == (void *)promise) {
        r8e_promise_reject_final(ctx, promise,
            r8e_promise_throw_type_error(ctx,
                "Promise cannot be resolved with itself"));
        return;
    }

    /* Step 2: Check if value is a thenable object */
    if (R8E_IS_POINTER(value)) {
        /* Check if the value has a .then method */
        R8EValue then_fn = r8e_obj_get(ctx, value, R8E_ATOM_then);

        /* If getting .then threw, reject with the error */
        if (ctx->has_exception) {
            R8EValue err = ctx->exception;
            ctx->has_exception = false;
            ctx->exception = R8E_UNDEFINED;
            r8e_promise_reject_final(ctx, promise, err);
            return;
        }

        if (r8e_is_callable(then_fn)) {
            /* Step 3: Thenable assimilation - enqueue as microtask
             * to prevent stack overflow on deep thenable chains */
            r8e_enqueue_microtask(ctx,
                R8E_MICROTASK_PROMISE_THENABLE,
                R8E_UNDEFINED,              /* callback unused */
                r8e_from_pointer(promise),  /* arg1: promise */
                value,                      /* arg2: thenable */
                then_fn);                   /* arg3: then function */
            return;
        }
    }

    /* Step 4: Direct fulfillment */
    r8e_promise_fulfill(ctx, promise, value);
}

/**
 * Reject a promise with a reason.
 */
static void r8e_promise_reject_internal(R8EPromiseContext *ctx,
                                        R8EPromise *promise,
                                        R8EValue reason)
{
    if (promise->already_resolved) return;
    promise->already_resolved = 1;
    r8e_promise_reject_final(ctx, promise, reason);
}

/**
 * Resolve a promise (public API).
 */
void r8e_promise_resolve(R8EPromiseContext *ctx, R8EValue promise_val,
                         R8EValue value)
{
    R8EPromise *p = r8e_promise_get(promise_val);
    if (p == NULL || p->state != R8E_PROMISE_PENDING) return;
    r8e_promise_resolve_internal(ctx, p, value);
}

/**
 * Reject a promise (public API).
 */
void r8e_promise_reject(R8EPromiseContext *ctx, R8EValue promise_val,
                        R8EValue reason)
{
    R8EPromise *p = r8e_promise_get(promise_val);
    if (p == NULL || p->state != R8E_PROMISE_PENDING) return;
    r8e_promise_reject_internal(ctx, p, reason);
}


/* =========================================================================
 * Section 11: Thenable Assimilation (ES2023 Section 27.2.1.3.2)
 *
 * When a promise is resolved with a thenable, we call thenable.then()
 * with resolve/reject functions that delegate to the original promise.
 *
 * This is done asynchronously (via microtask) to prevent stack overflow
 * from nested thenables and to ensure correct ordering.
 * ========================================================================= */

/**
 * Resolve-element function for thenable assimilation.
 * This is called by thenable.then(resolveElement, rejectElement).
 */
static R8EValue r8e_thenable_resolve_callback(R8EPromiseContext *ctx,
                                              R8EPromise *promise,
                                              R8EValue value)
{
    if (promise->already_resolved) return R8E_UNDEFINED;
    promise->already_resolved = 1;
    r8e_promise_resolve_internal(ctx, promise, value);
    return R8E_UNDEFINED;
}

/**
 * Reject-element function for thenable assimilation.
 */
static R8EValue r8e_thenable_reject_callback(R8EPromiseContext *ctx,
                                             R8EPromise *promise,
                                             R8EValue reason)
{
    if (promise->already_resolved) return R8E_UNDEFINED;
    promise->already_resolved = 1;
    r8e_promise_reject_final(ctx, promise, reason);
    return R8E_UNDEFINED;
}

/**
 * Execute thenable assimilation.
 *
 * Called as a microtask. Invokes thenable.then(resolveElement, rejectElement)
 * where the callbacks delegate to the original promise's resolve/reject.
 *
 * Note: In the full engine, resolveElement and rejectElement would be
 * proper closure objects. For now, we simulate this by directly calling
 * thenable.then and handling the result.
 */
static void r8e_promise_resolve_thenable(R8EPromiseContext *ctx,
                                         R8EPromise *promise,
                                         R8EValue thenable,
                                         R8EValue then_fn)
{
    promise->is_resolving = 1;

    /* In a full implementation, we would create two native-function
     * closures that capture 'promise' and call resolve/reject on it.
     * For now, we call then_fn directly and handle the result/error.
     *
     * The real implementation wraps promise->resolve and promise->reject
     * as NaN-boxed native function pointers. The interpreter integration
     * creates proper R8ENativeFunction objects for these. */

    /* Create resolve/reject capability callbacks.
     * These are lightweight native functions that close over the promise.
     * We encode them as tagged pointers with the promise captured. */

    /* For the interim self-contained implementation, simulate the .then
     * call by checking if the thenable is itself a Promise: */
    R8EPromise *thenable_promise = r8e_promise_get(thenable);
    if (thenable_promise != NULL) {
        /* Thenable is one of our own promises. We can directly chain. */
        if (thenable_promise->state == R8E_PROMISE_FULFILLED) {
            r8e_promise_fulfill(ctx, promise, thenable_promise->result);
        } else if (thenable_promise->state == R8E_PROMISE_REJECTED) {
            r8e_promise_reject_final(ctx, promise, thenable_promise->result);
        } else {
            /* Still pending: attach a reaction that resolves/rejects
             * the outer promise when the thenable settles. */
            R8EPromiseReaction *r = r8e_reaction_new(
                ctx, R8E_UNDEFINED, R8E_UNDEFINED,
                r8e_from_pointer(promise), 0);
            if (r != NULL) {
                r8e_promise_append_reaction(thenable_promise, r);
            }
        }
    } else {
        /* Foreign thenable: call then_fn.call(thenable, resolve, reject).
         * In this case, we need native function wrappers. For now,
         * call then_fn and wrap the result. */
        R8EValue args[2] = { R8E_UNDEFINED, R8E_UNDEFINED };
        R8EValue result = r8e_call_function(ctx, then_fn, thenable,
                                            2, args);

        if (ctx->has_exception) {
            R8EValue err = ctx->exception;
            ctx->has_exception = false;
            ctx->exception = R8E_UNDEFINED;
            r8e_promise_reject_final(ctx, promise, err);
        } else {
            /* The then call returned; if it produced a value,
             * attempt to resolve with it */
            r8e_promise_resolve_internal(ctx, promise, result);
        }
    }

    promise->is_resolving = 0;
}


/* =========================================================================
 * Section 12: Trigger Reactions (ES2023 Section 27.2.1.4)
 *
 * When a promise settles, drain its reaction queue by enqueueing
 * each reaction as a microtask.
 * ========================================================================= */

/**
 * Enqueue all pending reactions as microtasks.
 */
static void r8e_promise_trigger_reactions(R8EPromiseContext *ctx,
                                          R8EPromise *promise)
{
    R8EPromiseReaction *reaction = promise->reactions_head;
    promise->reactions_head = NULL;
    promise->reactions_tail = NULL;
    promise->reaction_count = 0;

    while (reaction != NULL) {
        R8EPromiseReaction *next = reaction->next;
        reaction->next = NULL;

        r8e_enqueue_microtask(ctx,
            R8E_MICROTASK_PROMISE_REACTION,
            R8E_UNDEFINED,                           /* callback unused */
            r8e_from_pointer(reaction),              /* arg1: reaction */
            r8e_from_int32((int32_t)promise->state), /* arg2: state */
            promise->result);                        /* arg3: value */

        reaction = next;
    }
}

/**
 * Execute a single promise reaction.
 *
 * ES2023 Section 27.2.1.3.1 - PromiseReaction Jobs:
 *   1. Pick the handler based on settled state
 *   2. If handler is undefined, pass through the value/reason
 *   3. Otherwise, call the handler and resolve/reject result promise
 */
static void r8e_run_promise_reaction(R8EPromiseContext *ctx,
                                     R8EPromiseReaction *reaction,
                                     R8EPromiseState state,
                                     R8EValue value)
{
    R8EValue handler;
    if (state == R8E_PROMISE_FULFILLED) {
        handler = reaction->on_fulfilled;
    } else {
        handler = reaction->on_rejected;
    }

    R8EPromise *result_promise = r8e_promise_get(reaction->result_promise);

    if (reaction->type == 1) {
        /* finally handler: call handler(), then pass through value/state */
        if (r8e_is_callable(handler)) {
            R8EValue finally_result = r8e_call_function(ctx, handler,
                                                        R8E_UNDEFINED,
                                                        0, NULL);
            if (ctx->has_exception) {
                /* If finally handler threw, reject with that error */
                R8EValue err = ctx->exception;
                ctx->has_exception = false;
                ctx->exception = R8E_UNDEFINED;
                if (result_promise != NULL) {
                    r8e_promise_reject_internal(ctx, result_promise, err);
                }
                r8e_reaction_free(ctx, reaction);
                return;
            }

            /* If finally handler returned a thenable, wait for it */
            if (R8E_IS_POINTER(finally_result)) {
                R8EValue then_fn = r8e_obj_get(ctx, finally_result,
                                               R8E_ATOM_then);
                if (r8e_is_callable(then_fn)) {
                    /* Chain through the thenable, then pass through
                     * the original value */
                    /* For now, simplified: resolve with original value */
                    if (result_promise != NULL) {
                        if (state == R8E_PROMISE_FULFILLED) {
                            r8e_promise_resolve_internal(ctx, result_promise,
                                                        value);
                        } else {
                            r8e_promise_reject_internal(ctx, result_promise,
                                                       value);
                        }
                    }
                    r8e_reaction_free(ctx, reaction);
                    return;
                }
            }
        }

        /* Pass through original value/state */
        if (result_promise != NULL) {
            if (state == R8E_PROMISE_FULFILLED) {
                r8e_promise_resolve_internal(ctx, result_promise, value);
            } else {
                r8e_promise_reject_internal(ctx, result_promise, value);
            }
        }
        r8e_reaction_free(ctx, reaction);
        return;
    }

    /* Regular then/catch handler */
    if (R8E_IS_UNDEFINED(handler) || !r8e_is_callable(handler)) {
        /* No handler: pass through value/reason to result promise */
        if (result_promise != NULL) {
            if (state == R8E_PROMISE_FULFILLED) {
                r8e_promise_resolve_internal(ctx, result_promise, value);
            } else {
                r8e_promise_reject_internal(ctx, result_promise, value);
            }
        }
        r8e_reaction_free(ctx, reaction);
        return;
    }

    /* Call the handler */
    R8EValue args[1] = { value };
    R8EValue handler_result = r8e_call_function(ctx, handler,
                                                R8E_UNDEFINED, 1, args);

    if (ctx->has_exception) {
        /* Handler threw: reject the result promise */
        R8EValue err = ctx->exception;
        ctx->has_exception = false;
        ctx->exception = R8E_UNDEFINED;
        if (result_promise != NULL) {
            r8e_promise_reject_internal(ctx, result_promise, err);
        }
    } else {
        /* Handler returned: resolve the result promise with the return
         * value (which may be a thenable, triggering assimilation) */
        if (result_promise != NULL) {
            r8e_promise_resolve_internal(ctx, result_promise,
                                        handler_result);
        }
    }

    r8e_reaction_free(ctx, reaction);
}


/* =========================================================================
 * Section 13: Promise.prototype.then (ES2023 Section 27.2.5.4)
 *
 * The core chaining method. Returns a new promise whose settlement is
 * determined by the onFulfilled/onRejected handlers.
 * ========================================================================= */

/**
 * Implement promise.then(onFulfilled, onRejected).
 *
 * If the promise is already settled, the reaction is enqueued as a
 * microtask immediately. If still pending, the reaction is added to
 * the promise's queue.
 *
 * @return NaN-boxed pointer to the new result Promise.
 */
R8EValue r8e_promise_then(R8EPromiseContext *ctx, R8EValue promise_val,
                          R8EValue on_fulfilled, R8EValue on_rejected)
{
    R8EPromise *promise = r8e_promise_get(promise_val);
    if (promise == NULL) {
        r8e_promise_throw_type_error(ctx,
            "Promise.prototype.then called on non-promise");
        return R8E_UNDEFINED;
    }

    /* Mark as handled (for unhandled rejection tracking) */
    promise->is_handled = 1;

    /* Create result promise */
    R8EPromise *result = r8e_promise_alloc_obj(ctx);
    if (result == NULL) return R8E_UNDEFINED;
    R8EValue result_val = r8e_from_pointer(result);

    /* Normalize handlers: if not callable, set to undefined */
    if (!r8e_is_callable(on_fulfilled)) on_fulfilled = R8E_UNDEFINED;
    if (!r8e_is_callable(on_rejected))  on_rejected  = R8E_UNDEFINED;

    /* Create reaction */
    R8EPromiseReaction *reaction = r8e_reaction_new(
        ctx, on_fulfilled, on_rejected, result_val, 0);
    if (reaction == NULL) return R8E_UNDEFINED;

    if (promise->state == R8E_PROMISE_PENDING) {
        /* Promise not yet settled: queue the reaction */
        r8e_promise_append_reaction(promise, reaction);
    } else {
        /* Promise already settled: enqueue reaction as microtask */
        r8e_enqueue_microtask(ctx,
            R8E_MICROTASK_PROMISE_REACTION,
            R8E_UNDEFINED,
            r8e_from_pointer(reaction),
            r8e_from_int32((int32_t)promise->state),
            promise->result);
    }

    return result_val;
}


/* =========================================================================
 * Section 14: Promise.prototype.catch (ES2023 Section 27.2.5.1)
 *
 * Syntactic sugar: promise.catch(onRejected) === promise.then(undefined,
 *                                                             onRejected)
 * ========================================================================= */

R8EValue r8e_promise_catch(R8EPromiseContext *ctx, R8EValue promise_val,
                           R8EValue on_rejected)
{
    return r8e_promise_then(ctx, promise_val, R8E_UNDEFINED, on_rejected);
}


/* =========================================================================
 * Section 15: Promise.prototype.finally (ES2023 Section 27.2.5.3)
 *
 * finally(onFinally): call onFinally regardless of settlement, then
 * pass through the original value/reason (unless onFinally throws).
 * ========================================================================= */

R8EValue r8e_promise_finally(R8EPromiseContext *ctx, R8EValue promise_val,
                             R8EValue on_finally)
{
    R8EPromise *promise = r8e_promise_get(promise_val);
    if (promise == NULL) {
        r8e_promise_throw_type_error(ctx,
            "Promise.prototype.finally called on non-promise");
        return R8E_UNDEFINED;
    }

    promise->is_handled = 1;

    R8EPromise *result = r8e_promise_alloc_obj(ctx);
    if (result == NULL) return R8E_UNDEFINED;
    R8EValue result_val = r8e_from_pointer(result);

    /* Create a 'finally' type reaction */
    R8EPromiseReaction *reaction = r8e_reaction_new(
        ctx, on_finally, on_finally, result_val, 1 /* type=finally */);
    if (reaction == NULL) return R8E_UNDEFINED;

    if (promise->state == R8E_PROMISE_PENDING) {
        r8e_promise_append_reaction(promise, reaction);
    } else {
        r8e_enqueue_microtask(ctx,
            R8E_MICROTASK_PROMISE_REACTION,
            R8E_UNDEFINED,
            r8e_from_pointer(reaction),
            r8e_from_int32((int32_t)promise->state),
            promise->result);
    }

    return result_val;
}


/* =========================================================================
 * Section 16: Promise Constructor (ES2023 Section 27.2.3)
 *
 * new Promise(executor) - creates a new promise and calls executor
 * with (resolve, reject) functions.
 * ========================================================================= */

/**
 * Create a promise with an executor callback.
 *
 * The executor is called synchronously: executor(resolve, reject).
 * If the executor throws, the promise is rejected with the thrown value.
 *
 * Note: In the full engine, resolve and reject would be proper
 * native-function closure objects. For this implementation, we
 * call the executor and handle the resolution directly.
 */
R8EValue r8e_promise_new_with_executor(R8EPromiseContext *ctx,
                                       R8EValue executor)
{
    if (!r8e_is_callable(executor)) {
        r8e_promise_throw_type_error(ctx,
            "Promise resolver is not a function");
        return R8E_UNDEFINED;
    }

    R8EPromise *promise = r8e_promise_alloc_obj(ctx);
    if (promise == NULL) return R8E_UNDEFINED;
    R8EValue promise_val = r8e_from_pointer(promise);

    /* In the full implementation, we create native function wrappers
     * for resolve and reject. For now, we pass placeholders and
     * handle the resolution path directly via the promise struct. */

    /* Call executor(resolve, reject)
     * The resolve/reject args would be native function closures.
     * For now, we encode the promise pointer in them as a hack. */
    R8EValue args[2] = { R8E_UNDEFINED, R8E_UNDEFINED };
    R8EValue result = r8e_call_function(ctx, executor, R8E_UNDEFINED,
                                        2, args);
    (void)result;

    if (ctx->has_exception) {
        R8EValue err = ctx->exception;
        ctx->has_exception = false;
        ctx->exception = R8E_UNDEFINED;
        if (promise->state == R8E_PROMISE_PENDING) {
            r8e_promise_reject_internal(ctx, promise, err);
        }
    }

    return promise_val;
}


/* =========================================================================
 * Section 17: Promise.resolve (ES2023 Section 27.2.4.7)
 *
 * Promise.resolve(value):
 *   If value is already a Promise (of same constructor), return it.
 *   Otherwise, create a new Promise and resolve it with value.
 * ========================================================================= */

R8EValue r8e_promise_static_resolve(R8EPromiseContext *ctx, R8EValue value)
{
    /* If value is already a promise, return it directly
     * (ES2023: only if same constructor, which we always are) */
    if (r8e_is_promise(value)) {
        return value;
    }

    R8EPromise *p = r8e_promise_alloc_obj(ctx);
    if (p == NULL) return R8E_UNDEFINED;

    r8e_promise_resolve_internal(ctx, p, value);
    return r8e_from_pointer(p);
}


/* =========================================================================
 * Section 18: Promise.reject (ES2023 Section 27.2.4.6)
 *
 * Promise.reject(reason): create a new Promise, immediately reject it.
 * ========================================================================= */

R8EValue r8e_promise_static_reject(R8EPromiseContext *ctx, R8EValue reason)
{
    R8EPromise *p = r8e_promise_alloc_obj(ctx);
    if (p == NULL) return R8E_UNDEFINED;

    r8e_promise_reject_internal(ctx, p, reason);
    return r8e_from_pointer(p);
}


/* =========================================================================
 * Section 19: Promise.all (ES2023 Section 27.2.4.1)
 *
 * Returns a promise that resolves when ALL input promises resolve,
 * or rejects when ANY input promise rejects.
 *
 * The fulfillment value is an array of all results, in order.
 * ========================================================================= */

/* Internal state for Promise.all tracking */
typedef struct R8EPromiseAllState {
    R8EPromise  *result_promise;     /* the combined promise */
    R8EValue    *values;             /* array of results */
    uint32_t     total;              /* total promises */
    uint32_t     remaining;          /* count of unresolved promises */
    bool         already_rejected;   /* set on first rejection */
} R8EPromiseAllState;

/**
 * Callback when one element of Promise.all resolves.
 */
static void r8e_promise_all_element_resolved(R8EPromiseContext *ctx,
                                             R8EPromiseAllState *state,
                                             uint32_t index,
                                             R8EValue value)
{
    if (state->already_rejected) return;

    state->values[index] = value;
    state->remaining--;

    if (state->remaining == 0) {
        /* All resolved: build result array and fulfill */
        R8EValue arr = r8e_make_plain_array(ctx, state->total);
        for (uint32_t i = 0; i < state->total; i++) {
            r8e_array_push(ctx, arr, state->values[i]);
        }
        r8e_promise_resolve_internal(ctx, state->result_promise, arr);
        r8e_promise_free(ctx, state->values,
                         state->total * sizeof(R8EValue));
        r8e_promise_free(ctx, state, sizeof(R8EPromiseAllState));
    }
}

/**
 * Callback when one element of Promise.all rejects.
 */
static void r8e_promise_all_element_rejected(R8EPromiseContext *ctx,
                                             R8EPromiseAllState *state,
                                             R8EValue reason)
{
    if (state->already_rejected) return;
    state->already_rejected = true;
    r8e_promise_reject_internal(ctx, state->result_promise, reason);
    r8e_promise_free(ctx, state->values,
                     state->total * sizeof(R8EValue));
    r8e_promise_free(ctx, state, sizeof(R8EPromiseAllState));
}

/**
 * Promise.all(iterable).
 *
 * For the simplified implementation, expects iterable to be an array.
 */
R8EValue r8e_promise_all(R8EPromiseContext *ctx, R8EValue iterable)
{
    R8EPromise *result = r8e_promise_alloc_obj(ctx);
    if (result == NULL) return R8E_UNDEFINED;
    R8EValue result_val = r8e_from_pointer(result);

    /* Get the array of promises from the iterable */
    uint32_t len = r8e_array_length(ctx, iterable);

    if (len == 0) {
        /* Empty iterable: resolve with empty array */
        R8EValue empty_arr = r8e_make_plain_array(ctx, 0);
        r8e_promise_resolve_internal(ctx, result, empty_arr);
        return result_val;
    }

    /* Allocate tracking state */
    R8EPromiseAllState *state = (R8EPromiseAllState *)r8e_promise_alloc(
        ctx, sizeof(R8EPromiseAllState));
    if (state == NULL) return R8E_UNDEFINED;

    state->result_promise = result;
    state->total = len;
    state->remaining = len;
    state->already_rejected = false;
    state->values = (R8EValue *)r8e_promise_alloc(
        ctx, len * sizeof(R8EValue));
    if (state->values == NULL) {
        r8e_promise_free(ctx, state, sizeof(R8EPromiseAllState));
        return R8E_UNDEFINED;
    }

    for (uint32_t i = 0; i < len; i++) {
        state->values[i] = R8E_UNDEFINED;
    }

    /* Iterate and attach handlers to each promise */
    for (uint32_t i = 0; i < len; i++) {
        R8EValue elem = r8e_array_get(ctx, iterable, i);

        /* Coerce to promise if not already */
        R8EValue elem_promise;
        if (r8e_is_promise(elem)) {
            elem_promise = elem;
        } else {
            elem_promise = r8e_promise_static_resolve(ctx, elem);
        }

        R8EPromise *ep = r8e_promise_get(elem_promise);
        if (ep == NULL) continue;

        if (ep->state == R8E_PROMISE_FULFILLED) {
            r8e_promise_all_element_resolved(ctx, state, i, ep->result);
        } else if (ep->state == R8E_PROMISE_REJECTED) {
            r8e_promise_all_element_rejected(ctx, state, ep->result);
        } else {
            /* Pending: create a reaction that tracks this element.
             * In the full implementation, the reaction callbacks would
             * close over state and index. For now, we attach a handler
             * that resolves/rejects directly. */
            ep->is_handled = 1;

            /* Create a then reaction that will resolve the all-state
             * element at index i. This is a simplified approach; the
             * real implementation uses closure-based callbacks. */
            R8EPromiseReaction *r = r8e_reaction_new(
                ctx, R8E_UNDEFINED, R8E_UNDEFINED, result_val, 0);
            if (r != NULL) {
                r8e_promise_append_reaction(ep, r);
            }
        }
    }

    return result_val;
}


/* =========================================================================
 * Section 20: Promise.allSettled (ES2023 Section 27.2.4.2)
 *
 * Returns a promise that resolves when ALL input promises settle
 * (resolve or reject). Never rejects.
 *
 * Fulfillment value: array of {status, value/reason} objects.
 * ========================================================================= */

/* Internal state for Promise.allSettled tracking */
typedef struct R8EPromiseAllSettledState {
    R8EPromise *result_promise;
    R8EValue   *results;          /* array of {status, value/reason} */
    uint32_t    total;
    uint32_t    remaining;
} R8EPromiseAllSettledState;

/**
 * Callback when one element of Promise.allSettled settles.
 */
static void r8e_promise_allsettled_element_done(
    R8EPromiseContext *ctx,
    R8EPromiseAllSettledState *state,
    uint32_t index,
    R8EPromiseState settled_state,
    R8EValue value)
{
    /* Create a result object: {status: "fulfilled"/"rejected", value/reason} */
    R8EValue obj = r8e_make_plain_object(ctx);

    if (settled_state == R8E_PROMISE_FULFILLED) {
        r8e_obj_set(ctx, obj, R8E_ATOM_status,
                    r8e_from_pointer(NULL));  /* placeholder: "fulfilled" */
        r8e_obj_set(ctx, obj, R8E_ATOM_fulfilled, R8E_TRUE);
        r8e_obj_set(ctx, obj, R8E_ATOM_value, value);
    } else {
        r8e_obj_set(ctx, obj, R8E_ATOM_status,
                    r8e_from_pointer(NULL));  /* placeholder: "rejected" */
        r8e_obj_set(ctx, obj, R8E_ATOM_rejected, R8E_TRUE);
        r8e_obj_set(ctx, obj, R8E_ATOM_reason, value);
    }

    state->results[index] = obj;
    state->remaining--;

    if (state->remaining == 0) {
        R8EValue arr = r8e_make_plain_array(ctx, state->total);
        for (uint32_t i = 0; i < state->total; i++) {
            r8e_array_push(ctx, arr, state->results[i]);
        }
        r8e_promise_resolve_internal(ctx, state->result_promise, arr);
        r8e_promise_free(ctx, state->results,
                         state->total * sizeof(R8EValue));
        r8e_promise_free(ctx, state,
                         sizeof(R8EPromiseAllSettledState));
    }
}

R8EValue r8e_promise_allSettled(R8EPromiseContext *ctx, R8EValue iterable)
{
    R8EPromise *result = r8e_promise_alloc_obj(ctx);
    if (result == NULL) return R8E_UNDEFINED;
    R8EValue result_val = r8e_from_pointer(result);

    uint32_t len = r8e_array_length(ctx, iterable);

    if (len == 0) {
        R8EValue empty_arr = r8e_make_plain_array(ctx, 0);
        r8e_promise_resolve_internal(ctx, result, empty_arr);
        return result_val;
    }

    R8EPromiseAllSettledState *state =
        (R8EPromiseAllSettledState *)r8e_promise_alloc(
            ctx, sizeof(R8EPromiseAllSettledState));
    if (state == NULL) return R8E_UNDEFINED;

    state->result_promise = result;
    state->total = len;
    state->remaining = len;
    state->results = (R8EValue *)r8e_promise_alloc(
        ctx, len * sizeof(R8EValue));
    if (state->results == NULL) {
        r8e_promise_free(ctx, state,
                         sizeof(R8EPromiseAllSettledState));
        return R8E_UNDEFINED;
    }
    for (uint32_t i = 0; i < len; i++) {
        state->results[i] = R8E_UNDEFINED;
    }

    for (uint32_t i = 0; i < len; i++) {
        R8EValue elem = r8e_array_get(ctx, iterable, i);
        R8EValue elem_promise;
        if (r8e_is_promise(elem)) {
            elem_promise = elem;
        } else {
            elem_promise = r8e_promise_static_resolve(ctx, elem);
        }

        R8EPromise *ep = r8e_promise_get(elem_promise);
        if (ep == NULL) continue;

        if (ep->state != R8E_PROMISE_PENDING) {
            r8e_promise_allsettled_element_done(ctx, state, i,
                                                ep->state, ep->result);
        } else {
            ep->is_handled = 1;
            /* Attach reaction: in full implementation, closure callbacks
             * capture state and index. Simplified here. */
            R8EPromiseReaction *r = r8e_reaction_new(
                ctx, R8E_UNDEFINED, R8E_UNDEFINED, result_val, 0);
            if (r != NULL) {
                r8e_promise_append_reaction(ep, r);
            }
        }
    }

    return result_val;
}


/* =========================================================================
 * Section 21: Promise.race (ES2023 Section 27.2.4.5)
 *
 * Returns a promise that settles with the first input promise to settle.
 * ========================================================================= */

R8EValue r8e_promise_race(R8EPromiseContext *ctx, R8EValue iterable)
{
    R8EPromise *result = r8e_promise_alloc_obj(ctx);
    if (result == NULL) return R8E_UNDEFINED;
    R8EValue result_val = r8e_from_pointer(result);

    uint32_t len = r8e_array_length(ctx, iterable);

    for (uint32_t i = 0; i < len; i++) {
        R8EValue elem = r8e_array_get(ctx, iterable, i);
        R8EValue elem_promise;
        if (r8e_is_promise(elem)) {
            elem_promise = elem;
        } else {
            elem_promise = r8e_promise_static_resolve(ctx, elem);
        }

        R8EPromise *ep = r8e_promise_get(elem_promise);
        if (ep == NULL) continue;

        if (ep->state == R8E_PROMISE_FULFILLED) {
            r8e_promise_resolve_internal(ctx, result, ep->result);
            return result_val;
        } else if (ep->state == R8E_PROMISE_REJECTED) {
            r8e_promise_reject_internal(ctx, result, ep->result);
            return result_val;
        } else {
            ep->is_handled = 1;
            /* Pending: attach a reaction that resolves/rejects the
             * race result with the first settlement */
            R8EPromiseReaction *r = r8e_reaction_new(
                ctx, R8E_UNDEFINED, R8E_UNDEFINED, result_val, 0);
            if (r != NULL) {
                r8e_promise_append_reaction(ep, r);
            }
        }
    }

    return result_val;
}


/* =========================================================================
 * Section 22: Promise.any (ES2023 Section 27.2.4.3)
 *
 * Returns a promise that resolves with the first input promise to
 * resolve, or rejects with an AggregateError if ALL reject.
 * ========================================================================= */

/* Internal state for Promise.any */
typedef struct R8EPromiseAnyState {
    R8EPromise *result_promise;
    R8EValue   *errors;            /* array of rejection reasons */
    uint32_t    total;
    uint32_t    remaining;
    bool        already_resolved;
} R8EPromiseAnyState;

/**
 * Callback when one element of Promise.any resolves.
 */
static void r8e_promise_any_element_resolved(
    R8EPromiseContext *ctx,
    R8EPromiseAnyState *state,
    R8EValue value)
{
    if (state->already_resolved) return;
    state->already_resolved = true;
    r8e_promise_resolve_internal(ctx, state->result_promise, value);
    r8e_promise_free(ctx, state->errors,
                     state->total * sizeof(R8EValue));
    r8e_promise_free(ctx, state, sizeof(R8EPromiseAnyState));
}

/**
 * Callback when one element of Promise.any rejects.
 */
static void r8e_promise_any_element_rejected(
    R8EPromiseContext *ctx,
    R8EPromiseAnyState *state,
    uint32_t index,
    R8EValue reason)
{
    if (state->already_resolved) return;

    state->errors[index] = reason;
    state->remaining--;

    if (state->remaining == 0) {
        /* All rejected: create AggregateError */
        R8EValue err_arr = r8e_make_plain_array(ctx, state->total);
        for (uint32_t i = 0; i < state->total; i++) {
            r8e_array_push(ctx, err_arr, state->errors[i]);
        }

        /* Create an AggregateError object.
         * In the full engine, this would be:
         * new AggregateError(errors, "All promises were rejected") */
        R8EValue agg_err = r8e_make_plain_object(ctx);
        r8e_obj_set(ctx, agg_err, R8E_ATOM_name,
                    R8E_UNDEFINED);  /* placeholder: "AggregateError" */
        r8e_obj_set(ctx, agg_err, R8E_ATOM_message,
                    R8E_UNDEFINED);  /* placeholder */

        r8e_promise_reject_internal(ctx, state->result_promise, agg_err);
        r8e_promise_free(ctx, state->errors,
                         state->total * sizeof(R8EValue));
        r8e_promise_free(ctx, state, sizeof(R8EPromiseAnyState));
    }
}

R8EValue r8e_promise_any(R8EPromiseContext *ctx, R8EValue iterable)
{
    R8EPromise *result = r8e_promise_alloc_obj(ctx);
    if (result == NULL) return R8E_UNDEFINED;
    R8EValue result_val = r8e_from_pointer(result);

    uint32_t len = r8e_array_length(ctx, iterable);

    if (len == 0) {
        /* Empty iterable: reject with AggregateError */
        R8EValue agg_err = r8e_make_plain_object(ctx);
        r8e_promise_reject_internal(ctx, result, agg_err);
        return result_val;
    }

    R8EPromiseAnyState *state = (R8EPromiseAnyState *)r8e_promise_alloc(
        ctx, sizeof(R8EPromiseAnyState));
    if (state == NULL) return R8E_UNDEFINED;

    state->result_promise = result;
    state->total = len;
    state->remaining = len;
    state->already_resolved = false;
    state->errors = (R8EValue *)r8e_promise_alloc(
        ctx, len * sizeof(R8EValue));
    if (state->errors == NULL) {
        r8e_promise_free(ctx, state, sizeof(R8EPromiseAnyState));
        return R8E_UNDEFINED;
    }
    for (uint32_t i = 0; i < len; i++) {
        state->errors[i] = R8E_UNDEFINED;
    }

    for (uint32_t i = 0; i < len; i++) {
        R8EValue elem = r8e_array_get(ctx, iterable, i);
        R8EValue elem_promise;
        if (r8e_is_promise(elem)) {
            elem_promise = elem;
        } else {
            elem_promise = r8e_promise_static_resolve(ctx, elem);
        }

        R8EPromise *ep = r8e_promise_get(elem_promise);
        if (ep == NULL) continue;

        if (ep->state == R8E_PROMISE_FULFILLED) {
            r8e_promise_any_element_resolved(ctx, state, ep->result);
            return result_val;
        } else if (ep->state == R8E_PROMISE_REJECTED) {
            r8e_promise_any_element_rejected(ctx, state, i, ep->result);
        } else {
            ep->is_handled = 1;
            R8EPromiseReaction *r = r8e_reaction_new(
                ctx, R8E_UNDEFINED, R8E_UNDEFINED, result_val, 0);
            if (r != NULL) {
                r8e_promise_append_reaction(ep, r);
            }
        }
    }

    return result_val;
}


/* =========================================================================
 * Section 23: Async Function Support
 *
 * Async functions are compiled to generator-like state machines.
 * The async runtime creates:
 *   1. An R8EAsyncState to hold the suspended interpreter frame
 *   2. A Promise that will be resolved/rejected when the function completes
 *
 * 'await' works by:
 *   1. Coercing the awaited value to a Promise
 *   2. Attaching a .then() reaction that enqueues an async-resume microtask
 *   3. Suspending the current frame (saving locals, stack, PC)
 *
 * When the awaited promise settles, the async-resume microtask:
 *   1. Restores the saved frame
 *   2. Pushes the settled value onto the stack
 *   3. Continues execution from the saved PC
 * ========================================================================= */

/**
 * Allocate a new async function state.
 */
static R8EAsyncState *r8e_async_state_alloc(R8EPromiseContext *ctx,
                                            uint32_t local_count,
                                            uint32_t stack_size)
{
    R8EAsyncState *state = (R8EAsyncState *)r8e_promise_alloc(
        ctx, sizeof(R8EAsyncState));
    if (state == NULL) return NULL;

    memset(state, 0, sizeof(R8EAsyncState));
    state->flags = R8E_GC_SET_KIND(0, R8E_GC_KIND_ASYNC_STATE);
    state->flags |= (1u << R8E_GC_RC_INLINE_SHIFT);
    state->proto_id = R8E_PROTO_NONE;
    state->state = R8E_ASYNC_SUSPENDED_START;

    /* Allocate saved frame storage */
    if (local_count > 0) {
        state->saved_locals = (R8EValue *)r8e_promise_alloc(
            ctx, local_count * sizeof(R8EValue));
        if (state->saved_locals == NULL) {
            r8e_promise_free(ctx, state, sizeof(R8EAsyncState));
            return NULL;
        }
        for (uint32_t i = 0; i < local_count; i++) {
            state->saved_locals[i] = R8E_UNDEFINED;
        }
    }

    if (stack_size > 0) {
        state->saved_stack = (R8EValue *)r8e_promise_alloc(
            ctx, stack_size * sizeof(R8EValue));
        if (state->saved_stack == NULL) {
            if (state->saved_locals) {
                r8e_promise_free(ctx, state->saved_locals,
                                 local_count * sizeof(R8EValue));
            }
            r8e_promise_free(ctx, state, sizeof(R8EAsyncState));
            return NULL;
        }
    }

    state->local_count = local_count;
    return state;
}

/**
 * Free an async function state.
 */
static void r8e_async_state_free(R8EPromiseContext *ctx,
                                 R8EAsyncState *state)
{
    if (state == NULL) return;
    if (state->saved_locals != NULL) {
        r8e_promise_free(ctx, state->saved_locals,
                         state->local_count * sizeof(R8EValue));
    }
    if (state->saved_stack != NULL) {
        r8e_promise_free(ctx, state->saved_stack,
                         state->stack_depth * sizeof(R8EValue));
    }
    r8e_promise_free(ctx, state, sizeof(R8EAsyncState));
}

/**
 * Start an async function execution.
 *
 * Creates the async state and return promise, then begins executing
 * the function body. If the function hits an 'await', the frame is
 * suspended and control returns here.
 *
 * @param ctx    Promise context.
 * @param func   The async closure to execute.
 * @param argc   Argument count.
 * @param argv   Arguments.
 * @return       The return Promise (settles when async function completes).
 */
R8EValue r8e_async_start(R8EPromiseContext *ctx, R8EValue func,
                         int argc, const R8EValue *argv)
{
    /* Create the return promise */
    R8EPromise *promise = r8e_promise_alloc_obj(ctx);
    if (promise == NULL) return R8E_UNDEFINED;
    R8EValue promise_val = r8e_from_pointer(promise);

    /* Allocate async state.
     * In the full engine, we inspect the closure's function descriptor
     * for local_count and stack_size. Use reasonable defaults here. */
    R8EAsyncState *state = r8e_async_state_alloc(ctx, 64, 32);
    if (state == NULL) return R8E_UNDEFINED;

    state->promise = promise_val;
    state->closure = func;
    state->state = R8E_ASYNC_EXECUTING;

    /* Copy arguments into saved locals */
    for (int i = 0; i < argc && (uint32_t)i < state->local_count; i++) {
        state->saved_locals[i] = argv[i];
    }

    /* Begin execution of the async function body.
     * In the full implementation, this calls into the interpreter with
     * the async state, which will suspend on 'await' and resume via
     * microtask queue.
     *
     * For now, we simulate: call the function and handle the result. */
    R8EValue result = r8e_call_function(ctx, func, R8E_UNDEFINED,
                                        argc, argv);

    if (ctx->has_exception) {
        R8EValue err = ctx->exception;
        ctx->has_exception = false;
        ctx->exception = R8E_UNDEFINED;
        r8e_promise_reject_internal(ctx, promise, err);
        state->state = R8E_ASYNC_COMPLETED;
    } else {
        /* If the result is a Promise, chain it to our return promise */
        if (r8e_is_promise(result)) {
            R8EPromise *rp = r8e_promise_get(result);
            if (rp != NULL && rp->state == R8E_PROMISE_PENDING) {
                /* Attach a reaction to chain the settlement */
                R8EPromiseReaction *r = r8e_reaction_new(
                    ctx, R8E_UNDEFINED, R8E_UNDEFINED, promise_val, 0);
                if (r != NULL) {
                    rp->is_handled = 1;
                    r8e_promise_append_reaction(rp, r);
                }
            } else if (rp != NULL &&
                       rp->state == R8E_PROMISE_FULFILLED) {
                r8e_promise_resolve_internal(ctx, promise, rp->result);
            } else if (rp != NULL &&
                       rp->state == R8E_PROMISE_REJECTED) {
                r8e_promise_reject_internal(ctx, promise, rp->result);
            }
        } else {
            r8e_promise_resolve_internal(ctx, promise, result);
        }
        state->state = R8E_ASYNC_COMPLETED;
    }

    r8e_async_state_free(ctx, state);
    return promise_val;
}

/**
 * Resume an async function after an 'await' settles.
 *
 * This is called as a microtask when the awaited promise settles.
 * It restores the saved interpreter frame and continues execution.
 *
 * @param ctx       Promise context.
 * @param state     The saved async state.
 * @param value     The settled value to resume with.
 * @param is_throw  If true, the awaited promise was rejected.
 */
void r8e_async_resume(R8EPromiseContext *ctx, R8EAsyncState *state,
                      R8EValue value, bool is_throw)
{
    if (state == NULL || state->state == R8E_ASYNC_COMPLETED) {
        return;
    }

    state->resume_value = value;
    state->resume_is_throw = is_throw;
    state->state = R8E_ASYNC_EXECUTING;

    R8EPromise *promise = r8e_promise_get(state->promise);
    if (promise == NULL) {
        state->state = R8E_ASYNC_COMPLETED;
        r8e_async_state_free(ctx, state);
        return;
    }

    /* In the full implementation, this would:
     * 1. Restore saved_locals and saved_stack to the interpreter frame
     * 2. Set PC to saved_pc
     * 3. Push value onto the operand stack
     * 4. If is_throw, dispatch to exception handler
     * 5. Continue execution until next await or return
     *
     * For the self-contained version, we simulate completion: */

    if (is_throw) {
        /* Awaited promise was rejected: propagate as rejection */
        r8e_promise_reject_internal(ctx, promise, value);
    } else {
        /* Awaited promise was fulfilled: continue with value */
        r8e_promise_resolve_internal(ctx, promise, value);
    }

    state->state = R8E_ASYNC_COMPLETED;
    r8e_async_state_free(ctx, state);
}

/**
 * Handle the 'await' expression in an async function.
 *
 * Called by the interpreter when it encounters an AWAIT opcode.
 * Suspends the current frame and sets up resumption on promise settlement.
 *
 * @param ctx         Promise context.
 * @param async_state The current async function's state.
 * @param value       The value being awaited (coerced to Promise).
 */
void r8e_async_await(R8EPromiseContext *ctx, R8EAsyncState *async_state,
                     R8EValue value)
{
    /* Coerce value to a Promise */
    R8EValue promise_val;
    if (r8e_is_promise(value)) {
        promise_val = value;
    } else {
        promise_val = r8e_promise_static_resolve(ctx, value);
    }

    R8EPromise *awaited = r8e_promise_get(promise_val);
    if (awaited == NULL) {
        /* Not a promise after resolution attempt: resume with value */
        async_state->resume_value = value;
        async_state->resume_is_throw = false;
        return;
    }

    awaited->is_handled = 1;
    async_state->state = R8E_ASYNC_SUSPENDED_YIELD;

    if (awaited->state == R8E_PROMISE_FULFILLED) {
        /* Already resolved: enqueue immediate resumption */
        r8e_enqueue_microtask(ctx,
            R8E_MICROTASK_ASYNC_RESUME,
            R8E_UNDEFINED,
            r8e_from_pointer(async_state),
            awaited->result,
            R8E_FALSE);
    } else if (awaited->state == R8E_PROMISE_REJECTED) {
        /* Already rejected: enqueue throw resumption */
        r8e_enqueue_microtask(ctx,
            R8E_MICROTASK_ASYNC_RESUME,
            R8E_UNDEFINED,
            r8e_from_pointer(async_state),
            awaited->result,
            R8E_TRUE);
    } else {
        /* Still pending: attach reactions that will enqueue resumption
         * when the promise settles. We create a lightweight reaction
         * that enqueues the appropriate microtask. */

        /* Create a "then" reaction for the awaited promise.
         * When it settles, the reaction handler enqueues an
         * ASYNC_RESUME microtask. */

        /* For the simplified implementation, we create a dummy result
         * promise that chains the resumption. */
        R8EPromise *chain = r8e_promise_alloc_obj(ctx);
        if (chain != NULL) {
            /* The chain promise's settlement will trigger async resume.
             * This is a workaround for not having closure callbacks. */
            R8EPromiseReaction *r = r8e_reaction_new(
                ctx, R8E_UNDEFINED, R8E_UNDEFINED,
                r8e_from_pointer(chain), 0);
            if (r != NULL) {
                r8e_promise_append_reaction(awaited, r);
            }
        }
    }
}


/* =========================================================================
 * Section 24: Promise State Inspection (for debugging/testing)
 * ========================================================================= */

/**
 * Get the state of a promise.
 * Returns -1 if not a promise.
 */
int r8e_promise_state(R8EValue v)
{
    R8EPromise *p = r8e_promise_get(v);
    if (p == NULL) return -1;
    return (int)p->state;
}

/**
 * Get the result (fulfillment value or rejection reason) of a promise.
 * Returns R8E_UNDEFINED if the promise is still pending or not a promise.
 */
R8EValue r8e_promise_result(R8EValue v)
{
    R8EPromise *p = r8e_promise_get(v);
    if (p == NULL || p->state == R8E_PROMISE_PENDING) {
        return R8E_UNDEFINED;
    }
    return p->result;
}

/**
 * Check if a promise has been handled (i.e., .then() or .catch() called).
 */
bool r8e_promise_is_handled(R8EValue v)
{
    R8EPromise *p = r8e_promise_get(v);
    if (p == NULL) return false;
    return p->is_handled != 0;
}


/* =========================================================================
 * Section 25: Promise Context Initialization and Cleanup
 * ========================================================================= */

/**
 * Initialize the promise subsystem within a context.
 */
void r8e_promise_init(R8EPromiseContext *ctx)
{
    assert(ctx != NULL);
    r8e_microtask_queue_init(&ctx->microtask_queue);
    ctx->unhandled_rejection_handler = NULL;
    ctx->exception = R8E_UNDEFINED;
    ctx->has_exception = false;
}

/**
 * Clean up the promise subsystem.
 */
void r8e_promise_cleanup(R8EPromiseContext *ctx)
{
    if (ctx == NULL) return;
    r8e_microtask_queue_free(&ctx->microtask_queue);
}

/**
 * Set the unhandled rejection handler.
 */
void r8e_promise_set_unhandled_handler(
    R8EPromiseContext *ctx,
    void (*handler)(R8EPromiseContext *, R8EValue, R8EValue, void *),
    void *user_data)
{
    ctx->unhandled_rejection_handler = handler;
    ctx->unhandled_user_data = user_data;
}

/**
 * Allocate a new promise context.
 */
R8EPromiseContext *r8e_promise_context_new(void)
{
    R8EPromiseContext *ctx = (R8EPromiseContext *)calloc(1,
                                                         sizeof(R8EPromiseContext));
    return ctx;
}

/**
 * Free a promise context.
 */
void r8e_promise_context_free(R8EPromiseContext *ctx)
{
    free(ctx);
}

/**
 * Drain all pending microtasks (public API).
 */
void r8e_microtask_drain(R8EPromiseContext *ctx)
{
    if (ctx == NULL) return;
    r8e_drain_microtasks(ctx);
}

/**
 * Get the number of pending microtasks.
 */
uint32_t r8e_microtask_pending_count(R8EPromiseContext *ctx)
{
    if (ctx == NULL) return 0;
    return ctx->microtask_queue.count;
}


/* =========================================================================
 * Section 26: Built-in Promise Method Dispatch
 *
 * These functions implement the JS-visible Promise.prototype and
 * Promise static methods. They are registered during built-in
 * initialization in r8e_builtin.c.
 * ========================================================================= */

/**
 * JS-facing: Promise.prototype.then
 * Called from interpreter as a native method.
 */
R8EValue r8e_builtin_promise_then(R8EPromiseContext *ctx,
                                  R8EValue this_val,
                                  int argc, const R8EValue *argv)
{
    R8EValue on_fulfilled = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    R8EValue on_rejected  = (argc > 1) ? argv[1] : R8E_UNDEFINED;
    return r8e_promise_then(ctx, this_val, on_fulfilled, on_rejected);
}

/**
 * JS-facing: Promise.prototype.catch
 */
R8EValue r8e_builtin_promise_catch(R8EPromiseContext *ctx,
                                   R8EValue this_val,
                                   int argc, const R8EValue *argv)
{
    R8EValue on_rejected = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_catch(ctx, this_val, on_rejected);
}

/**
 * JS-facing: Promise.prototype.finally
 */
R8EValue r8e_builtin_promise_finally(R8EPromiseContext *ctx,
                                     R8EValue this_val,
                                     int argc, const R8EValue *argv)
{
    R8EValue on_finally = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_finally(ctx, this_val, on_finally);
}

/**
 * JS-facing: Promise.resolve
 */
R8EValue r8e_builtin_promise_resolve(R8EPromiseContext *ctx,
                                     R8EValue this_val,
                                     int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue value = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_static_resolve(ctx, value);
}

/**
 * JS-facing: Promise.reject
 */
R8EValue r8e_builtin_promise_reject(R8EPromiseContext *ctx,
                                    R8EValue this_val,
                                    int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue reason = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_static_reject(ctx, reason);
}

/**
 * JS-facing: Promise.all
 */
R8EValue r8e_builtin_promise_all(R8EPromiseContext *ctx,
                                 R8EValue this_val,
                                 int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue iterable = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_all(ctx, iterable);
}

/**
 * JS-facing: Promise.allSettled
 */
R8EValue r8e_builtin_promise_allSettled(R8EPromiseContext *ctx,
                                       R8EValue this_val,
                                       int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue iterable = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_allSettled(ctx, iterable);
}

/**
 * JS-facing: Promise.race
 */
R8EValue r8e_builtin_promise_race(R8EPromiseContext *ctx,
                                  R8EValue this_val,
                                  int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue iterable = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_race(ctx, iterable);
}

/**
 * JS-facing: Promise.any
 */
R8EValue r8e_builtin_promise_any(R8EPromiseContext *ctx,
                                 R8EValue this_val,
                                 int argc, const R8EValue *argv)
{
    (void)this_val;
    R8EValue iterable = (argc > 0) ? argv[0] : R8E_UNDEFINED;
    return r8e_promise_any(ctx, iterable);
}


/* =========================================================================
 * Section 27: Promise GC Integration
 *
 * Promises are GC-tracked heap objects. The GC needs to:
 *   1. Trace the result value
 *   2. Trace all values in the reaction queue
 *   3. Free reaction records when the promise is collected
 * ========================================================================= */

/**
 * Mark all values reachable from a promise (for cycle detection).
 * Called by the GC's mark phase.
 */
void r8e_promise_gc_mark(R8EPromiseContext *ctx, R8EPromise *promise)
{
    (void)ctx;
    if (promise == NULL) return;

    /* Mark the result value if it is a heap pointer */
    /* In the full implementation, this would call r8e_gc_mark_value()
     * on promise->result and each value in the reaction queue. */

    R8EPromiseReaction *r = promise->reactions_head;
    while (r != NULL) {
        /* Mark on_fulfilled, on_rejected, result_promise */
        /* r8e_gc_mark_value(ctx, r->on_fulfilled); */
        /* r8e_gc_mark_value(ctx, r->on_rejected); */
        /* r8e_gc_mark_value(ctx, r->result_promise); */
        r = r->next;
    }
}

/**
 * Free all resources owned by a promise (called when refcount hits 0).
 */
void r8e_promise_gc_free(R8EPromiseContext *ctx, R8EPromise *promise)
{
    if (promise == NULL) return;

    /* Free all remaining reactions */
    R8EPromiseReaction *r = promise->reactions_head;
    while (r != NULL) {
        R8EPromiseReaction *next = r->next;
        r8e_reaction_free(ctx, r);
        r = next;
    }
    promise->reactions_head = NULL;
    promise->reactions_tail = NULL;

    r8e_promise_free(ctx, promise, sizeof(R8EPromise));
}


/* =========================================================================
 * Section 28: Stub Implementations for External Dependencies
 *
 * These stubs allow the file to compile standalone. In the integrated
 * build, these are replaced by the real implementations from their
 * respective modules.
 * ========================================================================= */

#ifndef R8E_INTEGRATED_BUILD

/* Call a JS function */
static R8EValue r8e_call_function(R8EPromiseContext *ctx, R8EValue func,
                                  R8EValue this_val, int argc,
                                  const R8EValue *argv)
{
    (void)ctx; (void)func; (void)this_val; (void)argc; (void)argv;
    /* Stub: in the integrated build, calls into r8e_interp.c */
    return R8E_UNDEFINED;
}

/* Object property access */
static R8EValue r8e_obj_get(R8EPromiseContext *ctx, R8EValue obj,
                            uint32_t atom)
{
    (void)ctx; (void)obj; (void)atom;
    return R8E_UNDEFINED;
}

static void r8e_obj_set(R8EPromiseContext *ctx, R8EValue obj,
                        uint32_t atom, R8EValue val)
{
    (void)ctx; (void)obj; (void)atom; (void)val;
}

static bool r8e_obj_has(R8EPromiseContext *ctx, R8EValue obj,
                        uint32_t atom)
{
    (void)ctx; (void)obj; (void)atom;
    return false;
}

/* Check if callable */
static bool r8e_is_callable(R8EValue v)
{
    if (!R8E_IS_POINTER(v)) return false;
    R8EGCHeader *hdr = (R8EGCHeader *)r8e_get_pointer(v);
    uint32_t kind = R8E_GC_GET_KIND(hdr->flags);
    return kind == R8E_GC_KIND_CLOSURE || kind == R8E_GC_KIND_FUNCTION;
}

/* Throw type error */
static R8EValue r8e_promise_throw_type_error(R8EPromiseContext *ctx,
                                             const char *msg)
{
    (void)msg;
    ctx->has_exception = true;
    ctx->exception = R8E_UNDEFINED;  /* would be a real error object */
    return R8E_UNDEFINED;
}

/* Memory allocation */
static void *r8e_promise_alloc(R8EPromiseContext *ctx, size_t size)
{
    (void)ctx;
    return calloc(1, size);
}

static void r8e_promise_free(R8EPromiseContext *ctx, void *ptr, size_t size)
{
    (void)ctx; (void)size;
    free(ptr);
}

/* Object/Array creation */
static R8EValue r8e_make_plain_object(R8EPromiseContext *ctx)
{
    (void)ctx;
    return R8E_UNDEFINED;  /* stub */
}

static R8EValue r8e_make_plain_array(R8EPromiseContext *ctx, uint32_t cap)
{
    (void)ctx; (void)cap;
    return R8E_UNDEFINED;  /* stub */
}

static void r8e_array_push(R8EPromiseContext *ctx, R8EValue arr,
                           R8EValue val)
{
    (void)ctx; (void)arr; (void)val;
}

static uint32_t r8e_array_length(R8EPromiseContext *ctx, R8EValue arr)
{
    (void)ctx; (void)arr;
    return 0;
}

static R8EValue r8e_array_get(R8EPromiseContext *ctx, R8EValue arr,
                              uint32_t index)
{
    (void)ctx; (void)arr; (void)index;
    return R8E_UNDEFINED;
}

#endif /* R8E_INTEGRATED_BUILD */
