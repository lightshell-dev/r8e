/*
 * r8e_interp.c - Bytecode Interpreter with Computed Goto Dispatch
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 7 (Interpreter and Dispatch).
 *
 * Architecture:
 *   - Computed goto dispatch (GCC/Clang) with switch fallback (MSVC)
 *   - Int32 fast paths for arithmetic (zero function calls on hot path)
 *   - Chain superinstructions fuse 3-5 ops per dispatch (Section 6.2)
 *   - RC elision: 70-75% of RC ops eliminated via classification + chains
 *   - Try/catch/finally stack per call frame for exception handling
 *   - Resource limit checks at loop back-edges (CHECK_LIMITS opcode)
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
#include <assert.h>
#include <stdio.h>
#include <float.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* NaN-boxing constants */
#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL
#define R8E_NAN_VAL    0x7FF8000000000000ULL

/* NaN-boxing type checks (16-bit tag in bits 48-63, 48-bit payload) */
#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)
#define R8E_IS_UNDEFINED(v)  ((v) == R8E_UNDEFINED)
#define R8E_IS_NULL(v)       ((v) == R8E_NULL)
#define R8E_IS_TRUE(v)       ((v) == R8E_TRUE)
#define R8E_IS_FALSE(v)      ((v) == R8E_FALSE)
#define R8E_IS_BOOLEAN(v)    ((v) == R8E_TRUE || (v) == R8E_FALSE)
#define R8E_IS_SYMBOL(v)     (((v) >> 48) == 0xFFFBU)
#define R8E_IS_ATOM(v)       (((v) >> 48) == 0xFFFCU)
#define R8E_IS_NULLISH(v)    (R8E_IS_UNDEFINED(v) || R8E_IS_NULL(v))
#define R8E_IS_NUMBER(v)     (R8E_IS_DOUBLE(v) || R8E_IS_INT32(v))

/* NaN-boxing extraction */
static inline double r8e_interp_get_double(R8EValue v) {
    double d; memcpy(&d, &v, 8); return d;
}
static inline int32_t r8e_interp_get_int32(R8EValue v) {
    return (int32_t)(v & 0xFFFFFFFFULL);
}
static inline void *r8e_interp_get_pointer(R8EValue v) {
    return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* NaN-boxing encoding */
static inline R8EValue r8e_interp_from_double(double d) {
    R8EValue v; memcpy(&v, &d, 8);
    if (v >= 0xFFF8000000000000ULL) v = R8E_NAN_VAL;
    return v;
}
static inline R8EValue r8e_interp_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint64_t)(uint32_t)i;
}
static inline R8EValue r8e_interp_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p;
}
static inline R8EValue r8e_interp_from_bool(bool b) {
    return b ? R8E_TRUE : R8E_FALSE;
}

/* Inline string extraction */
static inline int r8e_interp_inline_str_len(R8EValue v) {
    return (int)((v >> 45) & 0x7);
}

/* =========================================================================
 * GC header and kind constants
 * ========================================================================= */

#define R8E_GC_KIND_MASK     0x000000E0u
#define R8E_GC_KIND_SHIFT    5
#define R8E_GC_KIND_OBJECT   0u
#define R8E_GC_KIND_STRING   1u
#define R8E_GC_KIND_ARRAY    2u
#define R8E_GC_KIND_CLOSURE  3u
#define R8E_GC_KIND_ENVFRAME 4u
#define R8E_GC_KIND_FUNCTION 5u
#define R8E_GC_KIND_PROMISE  6u

/* Minimal Promise struct for async function support.
 * Layout must match r8e_promise.c R8EPromise. */
#define R8E_PROMISE_PENDING   0
#define R8E_PROMISE_FULFILLED 1
#define R8E_PROMISE_REJECTED  2

#define R8E_PROTO_PROMISE    20

typedef struct R8EPromiseInterp {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  state;       /* R8E_PROMISE_{PENDING,FULFILLED,REJECTED} */
    uint8_t  pad_[7];
    R8EValue result;
    /* remaining fields (reactions etc.) not needed for resolved promises */
    void    *reactions_head;
    void    *reactions_tail;
    uint32_t reaction_count;
    uint8_t  is_handled;
    uint8_t  is_resolving;
    uint8_t  already_resolved;
    uint8_t  pad2;
} R8EPromiseInterp;

/* Create a new fulfilled/rejected Promise wrapping a value. */
static R8EValue r8e_interp_make_resolved_promise(R8EValue value, bool rejected)
{
    R8EPromiseInterp *p = (R8EPromiseInterp *)calloc(1, sizeof(R8EPromiseInterp));
    if (!p) return R8E_UNDEFINED;
    p->flags = (R8E_GC_KIND_PROMISE << R8E_GC_KIND_SHIFT)
             | (1u << 16); /* initial refcount = 1 */
    p->proto_id = R8E_PROTO_PROMISE;
    p->state = rejected ? R8E_PROMISE_REJECTED : R8E_PROMISE_FULFILLED;
    p->result = value;
    return r8e_interp_from_pointer(p);
}

#define R8E_GC_GET_KIND(flags) \
    (((flags) & R8E_GC_KIND_MASK) >> R8E_GC_KIND_SHIFT)

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
} R8EGCHeader;

/* =========================================================================
 * String header (matches r8e_value.c)
 * ========================================================================= */

typedef struct R8EStringInterp {
    uint32_t flags;
    uint32_t hash;
    uint32_t byte_length;
    uint32_t char_length;
    void    *offset_table; /* matches R8EString layout in r8e_types.h */
    /* char data[] follows (flexible array member) */
} R8EStringInterp;

static inline const char *r8e_interp_string_data(const R8EStringInterp *s) {
    return (const char *)(s + 1);
}

/* =========================================================================
 * Function structure (matches r8e_closure.c / r8e_types.h)
 * ========================================================================= */

typedef struct R8EFunctionInterp {
    uint32_t  flags;
    uint32_t  proto_id;
    uint8_t  *bytecode;
    uint32_t  bytecode_len;
    uint16_t  param_count;
    uint16_t  local_count;
    uint16_t  capture_count;
    uint16_t  stack_size;
    uint32_t  name_atom;
    R8EValue *constants;
    uint16_t  const_count;
    uint8_t   is_strict;
    uint8_t   is_arrow;
    uint8_t   is_generator;
    uint8_t   is_async;
    uint16_t  source_line;
} R8EFunctionInterp;

/* =========================================================================
 * Closure structure (matches r8e_closure.c)
 * ========================================================================= */

#define R8E_CAPTURE_MODE_INLINE 0
#define R8E_CAPTURE_MODE_ARRAY  1
#define R8E_CAPTURE_MODE_FRAME  2

#define R8E_CAP_META_DIRECT   0
#define R8E_CAP_META_INDIRECT 1

typedef struct {
    uint8_t  kind;
    uint8_t  index;
    uint16_t pad;
} R8ECaptureMetaInterp;

typedef struct R8EEnvFrameInterp {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  refcount;
    uint16_t  slot_count;
    uint16_t  reserved;
    R8EValue *slots;
} R8EEnvFrameInterp;

typedef struct R8EClosureInterp {
    uint32_t             flags;
    uint32_t             proto_id;
    R8EFunctionInterp   *func;
    uint8_t              capture_count;
    uint8_t              capture_mode;
    uint16_t             reserved;
    union {
        struct { R8EValue inline_captures[2]; } inl;
        struct {
            R8EValue              *values;
            R8ECaptureMetaInterp  *meta;
            R8EEnvFrameInterp     *env;
        } arr;
        struct { R8EEnvFrameInterp *env_frame; } frm;
    } storage;
} R8EClosureInterp;

/* =========================================================================
 * Native function callback
 * ========================================================================= */

typedef struct R8EInterpContext R8EInterpContext;

typedef R8EValue (*R8ENativeFuncPtr)(R8EInterpContext *ctx, R8EValue this_val,
                                     int argc, const R8EValue *argv);

typedef struct {
    uint32_t         flags;
    uint32_t         proto_id;
    R8ENativeFuncPtr callback;
    uint32_t         name_atom;
    int16_t          expected_argc;
    uint16_t         pad;
} R8ENativeFuncObj;

/* =========================================================================
 * Object model (minimal, for property access)
 * ========================================================================= */

typedef struct { R8EValue key; R8EValue val; } R8EPropPairInterp;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    R8EValue key0;
    R8EValue val0;
} R8EObjTier0Interp;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  count;
    uint8_t  pad[7];
    R8EPropPairInterp props[4];
} R8EObjTier1Interp;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  count;
    uint8_t  capacity;
    uint8_t  pad[6];
    R8EPropPairInterp *props;
} R8EObjTier2Interp;

typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint16_t count;
    uint16_t capacity;
    uint32_t pad;
    R8EPropPairInterp *buckets;
} R8EObjTier3Interp;

typedef struct {
    uint32_t  flags;
    uint32_t  proto_id;
    uint32_t  length;
    uint32_t  capacity;
    R8EValue *elements;
    void     *named;
} R8EArrayInterp;

/* Map/Set entry structures (must match r8e_weakref.c layout) */
#define R8E_GC_KIND_MAP_INTERP  0x0Cu
#define R8E_GC_KIND_SET_INTERP  0x0Du

typedef struct R8EMapEntryInterp {
    R8EValue key;
    R8EValue value;
    uint32_t hash;
    struct R8EMapEntryInterp *hash_next;
    struct R8EMapEntryInterp *order_next;
    struct R8EMapEntryInterp *order_prev;
} R8EMapEntryInterp;

typedef struct {
    uint32_t              flags;
    uint32_t              proto_id;
    void                **buckets;
    uint32_t              capacity;
    uint32_t              count;
    R8EMapEntryInterp    *order_first;
    R8EMapEntryInterp    *order_last;
} R8EMapInterp;

typedef struct R8ESetEntryInterp {
    R8EValue key;
    uint32_t hash;
    struct R8ESetEntryInterp *hash_next;
    struct R8ESetEntryInterp *order_next;
    struct R8ESetEntryInterp *order_prev;
} R8ESetEntryInterp;

typedef struct {
    uint32_t              flags;
    uint32_t              proto_id;
    void                **buckets;
    uint32_t              capacity;
    uint32_t              count;
    R8ESetEntryInterp    *order_first;
    R8ESetEntryInterp    *order_last;
} R8ESetInterp;

/* Map/Set iterator structures (must match r8e_weakref.c) */
#define R8E_GC_KIND_MAP_ITER_INTERP  0x0Eu
#define R8E_GC_KIND_SET_ITER_INTERP  0x0Fu

typedef struct {
    uint32_t              flags;
    uint32_t              proto_id;
    R8EMapInterp         *map;
    R8EMapEntryInterp    *current;
    uint8_t               mode; /* 0=keys, 1=values, 2=entries */
} R8EMapIteratorInterp;

typedef struct {
    uint32_t              flags;
    uint32_t              proto_id;
    R8ESetInterp         *set;
    R8ESetEntryInterp    *current;
    uint8_t               mode; /* 0=values, 1=entries */
} R8ESetIteratorInterp;

/* FOR_OF_INIT tag values for iterator kind */
#define FO_TAG_ARRAY_STRING  0xFFFC00000FFF0002ULL
#define FO_TAG_MAP           0xFFFC00000FFF0003ULL
#define FO_TAG_SET           0xFFFC00000FFF0004ULL
#define FO_TAG_MAP_KEYS      0xFFFC00000FFF0005ULL
#define FO_TAG_MAP_VALUES    0xFFFC00000FFF0006ULL
#define FO_TAG_MAP_ENTRIES   0xFFFC00000FFF0007ULL
#define FO_TAG_SET_VALUES    0xFFFC00000FFF0008ULL
#define FO_TAG_SET_ENTRIES   0xFFFC00000FFF0009ULL

/* =========================================================================
 * Opcode enum (must match r8e_opcodes.h exactly)
 * ========================================================================= */

enum {
    OP_LOAD_LOCAL       = 0x00, OP_STORE_LOCAL      = 0x01,
    OP_LOAD_GLOBAL      = 0x02, OP_STORE_GLOBAL     = 0x03,
    OP_LOAD_CONST       = 0x04, OP_PUSH_INT8        = 0x05,
    OP_PUSH_INT16       = 0x06, OP_PUSH_INT32       = 0x07,
    OP_PUSH_DOUBLE      = 0x08, OP_PUSH_UNDEFINED   = 0x09,
    OP_PUSH_NULL        = 0x0A, OP_PUSH_TRUE        = 0x0B,
    OP_PUSH_FALSE       = 0x0C, OP_PUSH_ZERO        = 0x0D,
    OP_PUSH_ONE         = 0x0E, OP_PUSH_STRING      = 0x0F,

    OP_DUP              = 0x10, OP_DROP             = 0x11,
    OP_SWAP             = 0x12, OP_ROT3             = 0x13,

    OP_GET_PROP         = 0x14, OP_SET_PROP         = 0x15,
    OP_GET_ELEM         = 0x16, OP_SET_ELEM         = 0x17,
    OP_DELETE_PROP      = 0x18, OP_DELETE_ELEM      = 0x19,
    OP_GET_PROP_2       = 0x1A, OP_DEFINE_PROP      = 0x1B,
    OP_GET_SUPER        = 0x1C, OP_SET_SUPER        = 0x1D,

    OP_ADD              = 0x20, OP_SUB              = 0x21,
    OP_MUL              = 0x22, OP_DIV              = 0x23,
    OP_MOD              = 0x24, OP_POW              = 0x25,
    OP_NEG              = 0x26, OP_POS              = 0x27,
    OP_INC              = 0x28, OP_DEC              = 0x29,
    OP_POST_INC         = 0x2A, OP_POST_DEC         = 0x2B,

    OP_BITAND           = 0x2C, OP_BITOR            = 0x2D,
    OP_BITXOR           = 0x2E, OP_BITNOT           = 0x2F,
    OP_SHL              = 0x30, OP_SHR              = 0x31,
    OP_USHR             = 0x32,

    OP_EQ               = 0x34, OP_SEQ              = 0x35,
    OP_NE               = 0x36, OP_SNE              = 0x37,
    OP_LT               = 0x38, OP_LE               = 0x39,
    OP_GT               = 0x3A, OP_GE               = 0x3B,
    OP_NOT              = 0x3C, OP_TYPEOF           = 0x3D,
    OP_INSTANCEOF       = 0x3E, OP_IN               = 0x3F,

    OP_JUMP             = 0x40, OP_JUMP8            = 0x41,
    OP_JUMP16           = 0x42, OP_JUMP_IF_FALSE    = 0x43,
    OP_JUMP_IF_TRUE     = 0x44, OP_JUMP_IF_FALSE8   = 0x45,
    OP_JUMP_IF_TRUE8    = 0x46, OP_JUMP_IF_FALSE16  = 0x47,
    OP_JUMP_IF_TRUE16   = 0x48, OP_JUMP_IF_NULLISH  = 0x49,

    OP_CALL             = 0x4C, OP_CALL_METHOD      = 0x4D,
    OP_NEW              = 0x4E, OP_RETURN           = 0x4F,
    OP_RETURN_UNDEFINED = 0x50, OP_TAIL_CALL        = 0x51,

    OP_THROW            = 0x54, OP_TRY_PUSH         = 0x55,
    OP_TRY_POP          = 0x56, OP_CATCH            = 0x57,
    OP_FINALLY          = 0x58,

    OP_NEW_OBJECT       = 0x5C, OP_NEW_ARRAY        = 0x5D,
    OP_NEW_FUNCTION     = 0x5E, OP_NEW_REGEXP       = 0x5F,
    OP_INIT_PROP        = 0x60, OP_INIT_ELEM        = 0x61,
    OP_SPREAD           = 0x62,

    OP_LOAD_CLOSURE     = 0x64, OP_STORE_CLOSURE    = 0x65,
    OP_LOAD_ARG         = 0x66, OP_LOAD_REST_ARGS   = 0x67,
    OP_ENTER_SCOPE      = 0x68, OP_EXIT_SCOPE       = 0x69,
    OP_LOAD_THIS        = 0x6A, OP_LOAD_NEW_TARGET  = 0x6B,
    OP_DESTRUCTURE_ARRAY  = 0x6C, OP_DESTRUCTURE_OBJECT = 0x6D,
    OP_SET_DEFAULT        = 0x6E,

    OP_GET_ITERATOR     = 0x70, OP_ITERATOR_NEXT    = 0x71,
    OP_ITERATOR_CLOSE   = 0x72, OP_YIELD            = 0x73,
    OP_YIELD_STAR       = 0x74, OP_AWAIT            = 0x75,
    OP_ASYNC_FUNC_START = 0x76, OP_ASYNC_FUNC_RESOLVE = 0x77,
    OP_ASYNC_FUNC_REJECT = 0x78,

    OP_CLASS_CREATE     = 0x7C, OP_CLASS_EXTENDS    = 0x7D,
    OP_CLASS_METHOD     = 0x7E, OP_CLASS_FIELD      = 0x7F,
    OP_CLASS_STATIC     = 0x80, OP_CLASS_PRIVATE    = 0x81,

    OP_IMPORT           = 0x84, OP_EXPORT           = 0x85,
    OP_IMPORT_DYNAMIC   = 0x86, OP_IMPORT_META      = 0x87,
    OP_OPTIONAL_CHAIN   = 0x88, OP_NULLISH_COALESCE = 0x89,

    OP_VOID             = 0x8C, OP_DEBUGGER         = 0x8D,
    OP_WITH_ENTER       = 0x8E, OP_WITH_EXIT        = 0x8F,
    OP_FOR_IN_INIT      = 0x90, OP_FOR_IN_NEXT      = 0x91,
    OP_FOR_OF_INIT      = 0x92, OP_FOR_OF_NEXT      = 0x93,

    OP_TEMPLATE_HEAD    = 0x94, OP_TEMPLATE_MIDDLE  = 0x95,
    OP_TEMPLATE_TAIL    = 0x96, OP_TAGGED_TEMPLATE  = 0x97,

    OP_CALL_METHOD_FAST = 0xE0, OP_FOR_INC_LOOP     = 0xE1,
    OP_LOOP_CMP_BRANCH  = 0xE2, OP_LOAD_FIELD_CHECK = 0xE3,
    OP_BRANCH_LOAD      = 0xE4, OP_STORE_LOCAL_POP  = 0xE5,
    OP_LOAD_LOCAL2      = 0xE6,

    OP_EVAL_TRAP        = 0xF0, OP_CHECK_LIMITS     = 0xF1,
    OP_NOP              = 0xFE, OP_HALT             = 0xFF
};

/* =========================================================================
 * Interpreter constants
 * ========================================================================= */

#define R8E_MAX_CALL_DEPTH     512
#define R8E_MAX_TRY_DEPTH      32
#define R8E_DEFAULT_STACK_SIZE 256
#define R8E_STACK_CANARY       0xDEADBEEF42424242ULL
#define R8E_FUEL_DEFAULT       10000000

/* =========================================================================
 * Try/catch handler entry
 * ========================================================================= */

typedef struct {
    const uint8_t *catch_pc;    /* PC for catch block (NULL if no catch) */
    const uint8_t *finally_pc;  /* PC for finally block (NULL if none) */
    R8EValue      *saved_sp;    /* stack pointer at try entry */
    uint16_t       scope_depth; /* scope depth at try entry */
    uint16_t       pad;
} R8ETryEntry;

/* =========================================================================
 * Call frame
 * ========================================================================= */

typedef struct R8ECallFrame {
    /* Function being executed (closure or native) */
    R8EClosureInterp   *closure;       /* NULL for native calls */
    R8EFunctionInterp  *func;          /* bytecode + metadata */

    /* Bytecode state */
    const uint8_t      *pc;            /* program counter */
    const uint8_t      *bytecode_end;  /* end of bytecode (for bounds check) */

    /* Register file and operand stack */
    R8EValue           *locals;        /* local register array */
    R8EValue           *stack_base;    /* base of operand stack for this frame */
    R8EValue           *sp;            /* current stack pointer */

    /* this binding */
    R8EValue            this_val;

    /* new.target for constructor calls */
    R8EValue            new_target;

    /* Arguments */
    const R8EValue     *argv;
    int                 argc;

    /* Constant pool */
    R8EValue           *constants;
    uint16_t            const_count;

    /* Try/catch stack */
    R8ETryEntry         try_stack[R8E_MAX_TRY_DEPTH];
    int                 try_depth;

    /* Scope depth (for block scoping) */
    uint16_t            scope_depth;

    /* Is this a constructor call? */
    bool                is_construct;

    /* Stack canary position */
    R8EValue           *canary_ptr;
} R8ECallFrame;

/* =========================================================================
 * Interpreter context (wraps the engine context for interpreter state)
 * ========================================================================= */

struct R8EInterpContext {
    /* Call stack */
    R8ECallFrame  frames[R8E_MAX_CALL_DEPTH];
    int           frame_count;

    /* Global object (pointer to heap object) */
    R8EValue      global_obj;

    /* Exception state */
    R8EValue      exception;
    bool          has_exception;

    /* Fuel counter for CPU time limiting */
    int64_t       fuel;

    /* Resource limits */
    size_t        max_memory;
    uint64_t      max_cpu_time_us;
    uint32_t      max_stack_depth;

    /* Memory usage tracking */
    size_t        memory_used;

    /* Last property atom accessed (for builtin method dispatch) */
    uint32_t      last_prop_atom;

    /* Allocation (simplified - uses malloc/free) */
    void         *user_data;
};

/* =========================================================================
 * Forward declarations for external functions
 * These would normally come from other modules. Declared extern here.
 * ========================================================================= */

/* Object property access (from r8e_object.c) */
extern R8EValue r8e_obj_get_prop_atom(void *obj, uint32_t atom);
extern bool     r8e_obj_set_prop_atom(void *obj, uint32_t atom, R8EValue val);
extern bool     r8e_obj_delete_prop_atom(void *obj, uint32_t atom);
extern bool     r8e_obj_has_prop_atom(void *obj, uint32_t atom);
extern R8EValue r8e_obj_get_elem(void *obj, R8EValue key);
extern bool     r8e_obj_set_elem(void *obj, R8EValue key, R8EValue val);
extern bool     r8e_obj_delete_elem(void *obj, R8EValue key);

/* Object creation (from r8e_object.c) */
extern void *r8e_obj_new(void *ctx);
extern void *r8e_array_new(void *ctx, uint32_t capacity);

/* Closure access (from r8e_closure.c) */
extern R8EValue r8e_closure_get_capture(const void *closure, uint8_t index);
extern bool     r8e_closure_set_capture(void *ctx, void *closure,
                                         uint8_t index, R8EValue value);

/* GC (from r8e_gc.c) */
extern void r8e_retain(void *ctx, R8EValue val);
extern void r8e_release(void *ctx, R8EValue val);

/* Value conversion (from r8e_value.c) */
extern R8EValue r8e_to_number(void *ctx, R8EValue val);
extern R8EValue r8e_to_boolean(R8EValue val);
extern R8EValue r8e_to_string(void *ctx, R8EValue val);
extern R8EValue r8e_abstract_eq(void *ctx, R8EValue a, R8EValue b);
extern R8EValue r8e_strict_eq(R8EValue a, R8EValue b);
extern R8EValue r8e_typeof(R8EValue val);
extern const char *r8e_typeof_cstring(R8EValue val);

/* =========================================================================
 * Internal helper: read operands from bytecode stream
 * ========================================================================= */

static inline uint8_t read_u8(const uint8_t **pc) {
    uint8_t v = **pc; (*pc)++; return v;
}

static inline int8_t read_i8(const uint8_t **pc) {
    int8_t v = (int8_t)**pc; (*pc)++; return v;
}

static inline uint16_t read_u16(const uint8_t **pc) {
    uint16_t v = (uint16_t)((*pc)[0] | ((*pc)[1] << 8));
    *pc += 2; return v;
}

static inline int16_t read_i16(const uint8_t **pc) {
    uint16_t u = read_u16(pc);
    return (int16_t)u;
}

static inline uint32_t read_u32(const uint8_t **pc) {
    uint32_t v = (uint32_t)((*pc)[0] | ((*pc)[1] << 8) |
                             ((*pc)[2] << 16) | ((*pc)[3] << 24));
    *pc += 4; return v;
}

static inline int32_t read_i32(const uint8_t **pc) {
    uint32_t u = read_u32(pc);
    return (int32_t)u;
}

/* =========================================================================
 * Internal helper: truthiness test (JS ToBoolean)
 * ========================================================================= */

static inline bool r8e_is_truthy(R8EValue v) {
    if (v == R8E_FALSE || v == R8E_UNDEFINED || v == R8E_NULL) return false;
    if (v == R8E_TRUE) return true;
    if (R8E_IS_INT32(v)) return r8e_interp_get_int32(v) != 0;
    if (R8E_IS_DOUBLE(v)) {
        double d = r8e_interp_get_double(v);
        return d != 0.0 && !isnan(d);
    }
    if (R8E_IS_INLINE_STR(v)) return r8e_interp_inline_str_len(v) != 0;
    if (R8E_IS_POINTER(v)) {
        const R8EGCHeader *h = (const R8EGCHeader *)r8e_interp_get_pointer(v);
        if (h && R8E_GC_GET_KIND(h->flags) == R8E_GC_KIND_STRING) {
            const R8EStringInterp *s = (const R8EStringInterp *)h;
            return s->byte_length > 0;
        }
        return true; /* objects are always truthy */
    }
    return false;
}

/* =========================================================================
 * Internal helper: convert value to double for arithmetic
 * ========================================================================= */

static inline double r8e_to_double_fast(R8EValue v) {
    if (R8E_IS_INT32(v)) return (double)r8e_interp_get_int32(v);
    if (R8E_IS_DOUBLE(v)) return r8e_interp_get_double(v);
    return NAN;
}

/* =========================================================================
 * Internal helper: create number, preferring int32
 * ========================================================================= */

static inline R8EValue r8e_number_from_double(double d) {
    if (isfinite(d)) {
        int32_t i = (int32_t)d;
        if ((double)i == d && (i != 0 || !signbit(d))) {
            return r8e_interp_from_int32(i);
        }
    }
    return r8e_interp_from_double(d);
}

/* =========================================================================
 * Internal helper: ToInt32 for bitwise ops
 * ========================================================================= */

static inline int32_t r8e_val_to_int32(R8EValue v) {
    if (R8E_IS_INT32(v)) return r8e_interp_get_int32(v);
    if (R8E_IS_DOUBLE(v)) {
        double d = r8e_interp_get_double(v);
        if (!isfinite(d) || d == 0.0) return 0;
        double t = trunc(d);
        double m = fmod(t, 4294967296.0);
        if (m < 0) m += 4294967296.0;
        if (m >= 2147483648.0) return (int32_t)(m - 4294967296.0);
        return (int32_t)m;
    }
    return 0;
}

static inline uint32_t r8e_val_to_uint32(R8EValue v) {
    return (uint32_t)r8e_val_to_int32(v);
}

/* =========================================================================
 * Internal helper: check if a value is callable
 * ========================================================================= */

static inline bool r8e_is_callable(R8EValue v) {
    if (!R8E_IS_POINTER(v)) return false;
    const R8EGCHeader *h = (const R8EGCHeader *)r8e_interp_get_pointer(v);
    if (!h) return false;
    uint32_t kind = R8E_GC_GET_KIND(h->flags);
    return kind == R8E_GC_KIND_CLOSURE || kind == R8E_GC_KIND_FUNCTION;
}

/* =========================================================================
 * Internal helper: get string data from any string value
 * ========================================================================= */

static const char *r8e_interp_get_string(R8EValue v, char *buf,
                                          uint32_t *out_len) {
    if (R8E_IS_INLINE_STR(v)) {
        int len = r8e_interp_inline_str_len(v);
        for (int i = 0; i < len; i++) {
            buf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
        }
        buf[len] = '\0';
        *out_len = (uint32_t)len;
        return buf;
    }
    if (R8E_IS_POINTER(v)) {
        const R8EGCHeader *h = (const R8EGCHeader *)r8e_interp_get_pointer(v);
        if (h && R8E_GC_GET_KIND(h->flags) == R8E_GC_KIND_STRING) {
            const R8EStringInterp *s = (const R8EStringInterp *)h;
            *out_len = s->byte_length;
            return r8e_interp_string_data(s);
        }
    }
    *out_len = 0;
    return "";
}

/* =========================================================================
 * Internal helper: check if value is a string (inline or heap)
 * ========================================================================= */

static inline bool r8e_is_string_val(R8EValue v) {
    if (R8E_IS_INLINE_STR(v)) return true;
    if (!R8E_IS_POINTER(v)) return false;
    const R8EGCHeader *h = (const R8EGCHeader *)r8e_interp_get_pointer(v);
    return h && R8E_GC_GET_KIND(h->flags) == R8E_GC_KIND_STRING;
}

/* =========================================================================
 * Internal helper: create an interp-compatible string value
 * ========================================================================= */

static R8EValue r8e_interp_make_string(const char *data, uint32_t len) {
    /* Try inline (0-6 ASCII chars) */
    if (len <= 6) {
        bool all_ascii = true;
        for (uint32_t i = 0; i < len; i++) {
            if ((uint8_t)data[i] > 127) { all_ascii = false; break; }
        }
        if (all_ascii) {
            R8EValue v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)len << 45);
            for (uint32_t i = 0; i < len; i++) {
                v |= ((uint64_t)(uint8_t)data[i] << (38 - i * 7));
            }
            return v;
        }
    }
    /* Heap-allocate */
    size_t alloc_sz = sizeof(R8EStringInterp) + len + 1;
    R8EStringInterp *s = (R8EStringInterp *)malloc(alloc_sz);
    if (!s) return R8E_UNDEFINED;
    s->flags = (R8E_GC_KIND_STRING << R8E_GC_KIND_SHIFT) | 0x01;
    s->hash = 0;
    s->byte_length = len;
    s->char_length = len;
    s->offset_table = NULL;
    char *dst = (char *)(s + 1);
    memcpy(dst, data, len);
    dst[len] = '\0';
    return r8e_interp_from_pointer(s);
}

/* =========================================================================
 * Internal helper: string concatenation (simplified)
 * ========================================================================= */

static R8EValue r8e_string_concat(R8EInterpContext *ctx,
                                   R8EValue a, R8EValue b) {
    char buf_a[8], buf_b[8];
    uint32_t len_a, len_b;
    const char *sa = r8e_interp_get_string(a, buf_a, &len_a);
    const char *sb = r8e_interp_get_string(b, buf_b, &len_b);

    uint32_t total = len_a + len_b;

    /* Try inline if result fits */
    if (total <= 6) {
        char tmp[8];
        memcpy(tmp, sa, len_a);
        memcpy(tmp + len_a, sb, len_b);
        tmp[total] = '\0';
        bool all_ascii = true;
        for (uint32_t i = 0; i < total; i++) {
            if ((uint8_t)tmp[i] > 127) { all_ascii = false; break; }
        }
        if (all_ascii) {
            R8EValue v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)total << 45);
            for (uint32_t i = 0; i < total; i++) {
                v |= ((uint64_t)(uint8_t)tmp[i] << (38 - i * 7));
            }
            return v;
        }
    }

    /* Heap-allocate concatenated string */
    size_t alloc_sz = sizeof(R8EStringInterp) + total + 1;
    R8EStringInterp *s = (R8EStringInterp *)malloc(alloc_sz);
    if (!s) return R8E_UNDEFINED;

    s->flags = (R8E_GC_KIND_STRING << R8E_GC_KIND_SHIFT) | 0x01;
    s->hash = 0;
    s->byte_length = total;
    s->char_length = total;
    s->offset_table = NULL;

    char *data = (char *)(s + 1);
    memcpy(data, sa, len_a);
    memcpy(data + len_a, sb, len_b);
    data[total] = '\0';

    /* FNV-1a hash */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < total; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    s->hash = h;

    (void)ctx;
    return r8e_interp_from_pointer(s);
}

/* =========================================================================
 * Internal helper: throw an exception
 * ========================================================================= */

static void r8e_interp_throw(R8EInterpContext *ctx, R8EValue exception) {
    ctx->exception = exception;
    ctx->has_exception = true;
}

static void r8e_interp_throw_type_error(R8EInterpContext *ctx,
                                         const char *msg) {
    /* Simplified: store message as inline string or heap string */
    size_t len = strlen(msg);
    if (len <= 6) {
        r8e_interp_throw(ctx, (R8EValue)0xFFFD000000000000ULL); /* empty inline */
    }
    /* Create a heap string for the error message */
    size_t alloc_sz = sizeof(R8EStringInterp) + len + 1;
    R8EStringInterp *s = (R8EStringInterp *)malloc(alloc_sz);
    if (s) {
        s->flags = (R8E_GC_KIND_STRING << R8E_GC_KIND_SHIFT) | 0x01;
        s->hash = 0;
        s->byte_length = (uint32_t)len;
        s->char_length = (uint32_t)len;
        s->offset_table = NULL;
        memcpy((char *)(s + 1), msg, len + 1);
        r8e_interp_throw(ctx, r8e_interp_from_pointer(s));
    } else {
        r8e_interp_throw(ctx, R8E_UNDEFINED);
    }
}

static void r8e_interp_throw_ref_error(R8EInterpContext *ctx,
                                        const char *msg) {
    r8e_interp_throw_type_error(ctx, msg); /* Simplified: same path */
}

static void r8e_interp_throw_range_error(R8EInterpContext *ctx,
                                          const char *msg) {
    r8e_interp_throw_type_error(ctx, msg);
}

/* =========================================================================
 * Internal helper: object property access wrappers
 * These call into r8e_object.c functions, with fallbacks for when
 * the object module is not yet linked.
 * ========================================================================= */

/* Weak symbol fallbacks for property access.
 * When the object module is linked, these are overridden. */
static R8EValue r8e_interp_get_prop(R8EInterpContext *ctx, R8EValue obj,
                                     uint32_t atom) {
    /* Handle inline strings */
    if (R8E_IS_INLINE_STR(obj)) {
        if (atom == 1 /* R8E_ATOM_length */) {
            return r8e_interp_from_int32(r8e_interp_inline_str_len(obj));
        }
        return R8E_UNDEFINED;
    }
    if (!R8E_IS_POINTER(obj)) {
        /* Allow property access on booleans/numbers to return undefined
         * instead of throwing, to match JS semantics for primitives */
        if (R8E_IS_NULL(obj) || R8E_IS_UNDEFINED(obj)) {
            r8e_interp_throw_type_error(ctx, "Cannot read property of null/undefined");
            return R8E_UNDEFINED;
        }
        return R8E_UNDEFINED;
    }
    void *ptr = r8e_interp_get_pointer(obj);
    if (!ptr) {
        r8e_interp_throw_type_error(ctx, "Cannot read property of null");
        return R8E_UNDEFINED;
    }

    R8EGCHeader *h = (R8EGCHeader *)ptr;
    uint32_t kind = R8E_GC_GET_KIND(h->flags);

    /* Array length */
    if (kind == R8E_GC_KIND_ARRAY && atom == 1 /* R8E_ATOM_length */) {
        R8EArrayInterp *arr = (R8EArrayInterp *)ptr;
        return r8e_interp_from_int32((int32_t)arr->length);
    }

    /* String length */
    if (kind == R8E_GC_KIND_STRING && atom == 1 /* R8E_ATOM_length */) {
        R8EStringInterp *s = (R8EStringInterp *)ptr;
        return r8e_interp_from_int32((int32_t)s->char_length);
    }

    /* Object property lookup by tier */
    if (kind == R8E_GC_KIND_OBJECT) {
        uint8_t tier = h->flags & 0x03;
        R8EValue key = 0xFFFC000000000000ULL | (uint64_t)atom; /* atom-tagged */

        if (tier == 0) {
            R8EObjTier0Interp *t0 = (R8EObjTier0Interp *)ptr;
            if (t0->key0 == key) return t0->val0;
        } else if (tier == 1) {
            R8EObjTier1Interp *t1 = (R8EObjTier1Interp *)ptr;
            for (uint8_t i = 0; i < t1->count; i++) {
                if (t1->props[i].key == key) return t1->props[i].val;
            }
        } else if (tier == 2) {
            R8EObjTier2Interp *t2 = (R8EObjTier2Interp *)ptr;
            if (t2->props) {
                for (uint8_t i = 0; i < t2->count; i++) {
                    if (t2->props[i].key == key) return t2->props[i].val;
                }
            }
        }
        /* Tier 3 / prototype chain: fall through to undefined */
    }

    return R8E_UNDEFINED;
}

static bool r8e_interp_set_prop(R8EInterpContext *ctx, R8EValue obj,
                                 uint32_t atom, R8EValue val) {
    if (!R8E_IS_POINTER(obj)) {
        r8e_interp_throw_type_error(ctx, "Cannot set property of non-object");
        return false;
    }
    void *ptr = r8e_interp_get_pointer(obj);
    if (!ptr) {
        r8e_interp_throw_type_error(ctx, "Cannot set property of null");
        return false;
    }

    R8EGCHeader *h = (R8EGCHeader *)ptr;
    uint32_t kind = R8E_GC_GET_KIND(h->flags);

    if (kind == R8E_GC_KIND_ARRAY && atom == 1 /* length */) {
        if (R8E_IS_INT32(val)) {
            R8EArrayInterp *arr = (R8EArrayInterp *)ptr;
            arr->length = (uint32_t)r8e_interp_get_int32(val);
            return true;
        }
    }

    if (kind == R8E_GC_KIND_OBJECT) {
        uint8_t tier = h->flags & 0x03;
        R8EValue key = 0xFFFC000000000000ULL | (uint64_t)atom;

        if (tier == 0) {
            R8EObjTier0Interp *t0 = (R8EObjTier0Interp *)ptr;
            if (t0->key0 == 0 || t0->key0 == key) {
                t0->key0 = key;
                t0->val0 = val;
                return true;
            }
            /* Tier 0 full - would need promotion. Not supported yet. */
        } else if (tier == 1) {
            R8EObjTier1Interp *t1 = (R8EObjTier1Interp *)ptr;
            for (uint8_t i = 0; i < t1->count; i++) {
                if (t1->props[i].key == key) {
                    t1->props[i].val = val;
                    return true;
                }
            }
            if (t1->count < 4) {
                t1->props[t1->count].key = key;
                t1->props[t1->count].val = val;
                t1->count++;
                return true;
            }
        }
    }

    return false;
}

static R8EValue r8e_interp_get_elem(R8EInterpContext *ctx, R8EValue obj,
                                     R8EValue key) {
    if (!R8E_IS_POINTER(obj)) {
        r8e_interp_throw_type_error(ctx,
            "Cannot read property of non-object");
        return R8E_UNDEFINED;
    }
    void *ptr = r8e_interp_get_pointer(obj);
    if (!ptr) return R8E_UNDEFINED;

    R8EGCHeader *h = (R8EGCHeader *)ptr;
    uint32_t kind = R8E_GC_GET_KIND(h->flags);

    /* Array element access */
    if (kind == R8E_GC_KIND_ARRAY && R8E_IS_INT32(key)) {
        int32_t idx = r8e_interp_get_int32(key);
        R8EArrayInterp *arr = (R8EArrayInterp *)ptr;
        if (idx >= 0 && (uint32_t)idx < arr->length &&
            (uint32_t)idx < arr->capacity && arr->elements) {
            return arr->elements[idx];
        }
        return R8E_UNDEFINED;
    }

    /* String char access */
    if (kind == R8E_GC_KIND_STRING && R8E_IS_INT32(key)) {
        int32_t idx = r8e_interp_get_int32(key);
        R8EStringInterp *s = (R8EStringInterp *)ptr;
        if (idx >= 0 && (uint32_t)idx < s->char_length) {
            const char *data = r8e_interp_string_data(s);
            char ch = data[idx]; /* ASCII fast path */
            R8EValue v = 0xFFFD000000000000ULL;
            v |= ((uint64_t)1 << 45);
            v |= ((uint64_t)(uint8_t)ch << 38);
            return v;
        }
        return R8E_UNDEFINED;
    }

    return R8E_UNDEFINED;
}

static bool r8e_interp_set_elem(R8EInterpContext *ctx, R8EValue obj,
                                 R8EValue key, R8EValue val) {
    if (!R8E_IS_POINTER(obj)) {
        r8e_interp_throw_type_error(ctx,
            "Cannot set property of non-object");
        return false;
    }
    void *ptr = r8e_interp_get_pointer(obj);
    if (!ptr) return false;

    R8EGCHeader *h = (R8EGCHeader *)ptr;
    uint32_t kind = R8E_GC_GET_KIND(h->flags);

    if (kind == R8E_GC_KIND_ARRAY && R8E_IS_INT32(key)) {
        int32_t idx = r8e_interp_get_int32(key);
        R8EArrayInterp *arr = (R8EArrayInterp *)ptr;
        if (idx >= 0) {
            uint32_t uidx = (uint32_t)idx;
            /* Grow if needed */
            if (uidx >= arr->capacity) {
                uint32_t new_cap = arr->capacity ? arr->capacity * 2 : 8;
                while (new_cap <= uidx) new_cap *= 2;
                R8EValue *new_elems = (R8EValue *)realloc(
                    arr->elements, new_cap * sizeof(R8EValue));
                if (!new_elems) return false;
                for (uint32_t i = arr->capacity; i < new_cap; i++) {
                    new_elems[i] = R8E_UNDEFINED;
                }
                arr->elements = new_elems;
                arr->capacity = new_cap;
            }
            arr->elements[uidx] = val;
            if (uidx >= arr->length) arr->length = uidx + 1;
            return true;
        }
    }

    return false;
}

/* =========================================================================
 * Internal helper: closure capture access
 * ========================================================================= */

static R8EValue r8e_interp_closure_get(const R8EClosureInterp *cl,
                                        uint8_t idx) {
    if (!cl || idx >= cl->capture_count) return R8E_UNDEFINED;

    switch (cl->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        return (idx < 2) ? cl->storage.inl.inline_captures[idx] : R8E_UNDEFINED;
    case R8E_CAPTURE_MODE_ARRAY:
        if (cl->storage.arr.meta &&
            cl->storage.arr.meta[idx].kind == R8E_CAP_META_DIRECT) {
            return cl->storage.arr.values[idx];
        } else if (cl->storage.arr.env) {
            uint8_t slot = cl->storage.arr.meta[idx].index;
            return cl->storage.arr.env->slots[slot];
        }
        return R8E_UNDEFINED;
    case R8E_CAPTURE_MODE_FRAME:
        if (cl->storage.frm.env_frame &&
            idx < cl->storage.frm.env_frame->slot_count) {
            return cl->storage.frm.env_frame->slots[idx];
        }
        return R8E_UNDEFINED;
    default:
        return R8E_UNDEFINED;
    }
}

static bool r8e_interp_closure_set(R8EClosureInterp *cl, uint8_t idx,
                                    R8EValue val) {
    if (!cl || idx >= cl->capture_count) return false;

    switch (cl->capture_mode) {
    case R8E_CAPTURE_MODE_INLINE:
        return false; /* inline captures are immutable */
    case R8E_CAPTURE_MODE_ARRAY:
        if (cl->storage.arr.meta &&
            cl->storage.arr.meta[idx].kind == R8E_CAP_META_INDIRECT &&
            cl->storage.arr.env) {
            uint8_t slot = cl->storage.arr.meta[idx].index;
            cl->storage.arr.env->slots[slot] = val;
            return true;
        }
        return false;
    case R8E_CAPTURE_MODE_FRAME:
        if (cl->storage.frm.env_frame &&
            idx < cl->storage.frm.env_frame->slot_count) {
            cl->storage.frm.env_frame->slots[idx] = val;
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* =========================================================================
 * ADD slow path: handles string concatenation and type coercion
 * ========================================================================= */

static R8EValue r8e_add_slow(R8EInterpContext *ctx, R8EValue a, R8EValue b) {
    /* If either operand is a string, convert both to strings and concat */
    if (r8e_is_string_val(a) || r8e_is_string_val(b)) {
        R8EValue sa = r8e_is_string_val(a) ? a : r8e_to_string(NULL, a);
        R8EValue sb = r8e_is_string_val(b) ? b : r8e_to_string(NULL, b);
        return r8e_string_concat(ctx, sa, sb);
    }

    /* Numeric addition */
    double da = r8e_to_double_fast(a);
    double db = r8e_to_double_fast(b);
    return r8e_number_from_double(da + db);
}

/* =========================================================================
 * Comparison helpers
 * ========================================================================= */

static R8EValue r8e_compare_lt(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        return r8e_interp_get_int32(a) < r8e_interp_get_int32(b)
               ? R8E_TRUE : R8E_FALSE;
    }
    double da = r8e_to_double_fast(a);
    double db = r8e_to_double_fast(b);
    if (isnan(da) || isnan(db)) return R8E_FALSE;
    return da < db ? R8E_TRUE : R8E_FALSE;
}

static R8EValue r8e_compare_le(R8EValue a, R8EValue b) {
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        return r8e_interp_get_int32(a) <= r8e_interp_get_int32(b)
               ? R8E_TRUE : R8E_FALSE;
    }
    double da = r8e_to_double_fast(a);
    double db = r8e_to_double_fast(b);
    if (isnan(da) || isnan(db)) return R8E_FALSE;
    return da <= db ? R8E_TRUE : R8E_FALSE;
}

static R8EValue r8e_compare_gt(R8EValue a, R8EValue b) {
    return r8e_compare_lt(b, a);
}

static R8EValue r8e_compare_ge(R8EValue a, R8EValue b) {
    return r8e_compare_le(b, a);
}

/* =========================================================================
 * Global property access helpers
 * ========================================================================= */

static R8EValue r8e_interp_load_global(R8EInterpContext *ctx, uint32_t atom) {
    if (R8E_IS_POINTER(ctx->global_obj)) {
        R8EValue v = r8e_interp_get_prop(ctx, ctx->global_obj, atom);
        if (R8E_IS_UNDEFINED(v) && !ctx->has_exception) {
            /* Could be a reference error, but for now return undefined */
        }
        return v;
    }
    return R8E_UNDEFINED;
}

static bool r8e_interp_store_global(R8EInterpContext *ctx, uint32_t atom,
                                     R8EValue val) {
    if (R8E_IS_POINTER(ctx->global_obj)) {
        return r8e_interp_set_prop(ctx, ctx->global_obj, atom, val);
    }
    return false;
}

/* =========================================================================
 * Exception unwinding: find nearest try handler
 * Returns true if a handler was found and pc/sp were adjusted.
 * ========================================================================= */

static bool r8e_interp_unwind(R8EInterpContext *ctx, R8ECallFrame *frame) {
    while (frame->try_depth > 0) {
        R8ETryEntry *entry = &frame->try_stack[frame->try_depth - 1];

        /* Restore stack pointer to the try entry point */
        frame->sp = entry->saved_sp;

        if (entry->catch_pc) {
            /* Jump to catch block: push exception value */
            frame->pc = entry->catch_pc;
            frame->try_depth--;
            return true;
        }

        if (entry->finally_pc) {
            /* Jump to finally block */
            frame->pc = entry->finally_pc;
            /* Keep the try entry for finally cleanup */
            entry->finally_pc = NULL;
            entry->catch_pc = NULL;
            return true;
        }

        frame->try_depth--;
    }
    return false;
}

/* =========================================================================
 * r8e_interp_call_internal - push a new call frame and execute
 *
 * This is the core function call mechanism. It sets up a new frame,
 * copies arguments, and returns control to the main dispatch loop.
 * ========================================================================= */

static R8EValue r8e_interp_execute(R8EInterpContext *ctx, R8ECallFrame *frame);
static R8EValue r8e_interp_call_internal(R8EInterpContext *ctx,
                                          R8EValue callee,
                                          R8EValue this_val,
                                          const R8EValue *argv, int argc,
                                          bool is_construct);

/* =========================================================================
 * Builtin method dispatch for strings and arrays
 *
 * Called when OP_CALL_METHOD's callee is not a function (UNDEFINED) and the
 * receiver is a string or array. Uses ctx->last_prop_atom to determine
 * which method was requested.
 * ========================================================================= */

static bool r8e_interp_builtin_method(R8EInterpContext *ctx,
                                       R8EValue this_val,
                                       uint32_t method_atom,
                                       const R8EValue *argv, int argc,
                                       R8EValue *out_result) {
    /* --- String methods --- */
    if (r8e_is_string_val(this_val)) {
        char sbuf[8];
        uint32_t slen;
        const char *sdata = r8e_interp_get_string(this_val, sbuf, &slen);

        if (method_atom == 126 /* R8E_ATOM_includes */) {
            if (argc < 1) { *out_result = R8E_FALSE; return true; }
            char nbuf[8]; uint32_t nlen;
            const char *needle = r8e_interp_get_string(argv[0], nbuf, &nlen);
            if (nlen == 0) { *out_result = R8E_TRUE; return true; }
            if (nlen > slen) { *out_result = R8E_FALSE; return true; }
            bool found = false;
            for (uint32_t i = 0; i <= slen - nlen; i++) {
                if (memcmp(sdata + i, needle, nlen) == 0) {
                    found = true; break;
                }
            }
            *out_result = found ? R8E_TRUE : R8E_FALSE;
            return true;
        }

        if (method_atom == 124 /* R8E_ATOM_indexOf */) {
            if (argc < 1) { *out_result = r8e_interp_from_int32(-1); return true; }
            char nbuf[8]; uint32_t nlen;
            const char *needle = r8e_interp_get_string(argv[0], nbuf, &nlen);
            int32_t pos = -1;
            if (nlen <= slen) {
                for (uint32_t i = 0; i <= slen - nlen; i++) {
                    if (memcmp(sdata + i, needle, nlen) == 0) {
                        pos = (int32_t)i; break;
                    }
                }
            }
            *out_result = r8e_interp_from_int32(pos);
            return true;
        }

        if (method_atom == 159 /* R8E_ATOM_split */) {
            if (argc < 1) {
                /* No separator: return array with whole string */
                R8EArrayInterp *arr = (R8EArrayInterp *)calloc(1, sizeof(R8EArrayInterp));
                if (!arr) { *out_result = R8E_UNDEFINED; return true; }
                arr->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
                arr->proto_id = 2;
                arr->length = 1; arr->capacity = 4;
                arr->elements = (R8EValue *)calloc(4, sizeof(R8EValue));
                arr->elements[0] = this_val;
                *out_result = r8e_interp_from_pointer(arr);
                return true;
            }
            char dbuf[8]; uint32_t dlen;
            const char *delim = r8e_interp_get_string(argv[0], dbuf, &dlen);
            /* Split string by delimiter */
            R8EArrayInterp *arr = (R8EArrayInterp *)calloc(1, sizeof(R8EArrayInterp));
            if (!arr) { *out_result = R8E_UNDEFINED; return true; }
            arr->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
            arr->proto_id = 2;
            arr->capacity = 8;
            arr->elements = (R8EValue *)calloc(8, sizeof(R8EValue));
            arr->length = 0;
            uint32_t start = 0;
            for (uint32_t i = 0; i <= slen; i++) {
                bool at_delim = (dlen == 0) ? (i > start) :
                    (i + dlen <= slen && memcmp(sdata + i, delim, dlen) == 0);
                if (at_delim || i == slen) {
                    if (i == slen && dlen > 0) at_delim = false;
                    if (at_delim || i == slen) {
                        uint32_t part_len = i - start;
                        R8EValue part = r8e_interp_make_string(sdata + start, part_len);
                        if (arr->length >= arr->capacity) {
                            arr->capacity *= 2;
                            arr->elements = (R8EValue *)realloc(arr->elements,
                                arr->capacity * sizeof(R8EValue));
                        }
                        arr->elements[arr->length++] = part;
                        start = i + dlen;
                        if (dlen > 0) i += dlen - 1; /* skip delimiter */
                    }
                }
            }
            *out_result = r8e_interp_from_pointer(arr);
            return true;
        }

        return false; /* Not a known string method */
    }

    /* --- Array methods --- */
    if (R8E_IS_POINTER(this_val)) {
        void *ptr = r8e_interp_get_pointer(this_val);
        if (!ptr) return false;
        R8EGCHeader *h = (R8EGCHeader *)ptr;
        if (R8E_GC_GET_KIND(h->flags) != R8E_GC_KIND_ARRAY) return false;
        R8EArrayInterp *arr = (R8EArrayInterp *)ptr;

        if (method_atom == 114 /* R8E_ATOM_push */) {
            for (int i = 0; i < argc; i++) {
                if (arr->length >= arr->capacity) {
                    uint32_t new_cap = arr->capacity ? arr->capacity * 2 : 8;
                    R8EValue *new_el = (R8EValue *)realloc(arr->elements,
                        new_cap * sizeof(R8EValue));
                    if (!new_el) break;
                    for (uint32_t j = arr->capacity; j < new_cap; j++)
                        new_el[j] = R8E_UNDEFINED;
                    arr->elements = new_el;
                    arr->capacity = new_cap;
                }
                arr->elements[arr->length++] = argv[i];
            }
            *out_result = r8e_interp_from_int32((int32_t)arr->length);
            return true;
        }

        if (method_atom == 115 /* R8E_ATOM_pop */) {
            if (arr->length == 0) {
                *out_result = R8E_UNDEFINED;
            } else {
                arr->length--;
                *out_result = arr->elements[arr->length];
            }
            return true;
        }

        if (method_atom == 132 /* R8E_ATOM_map */) {
            if (argc < 1 || !r8e_is_callable(argv[0])) {
                r8e_interp_throw_type_error(ctx, "map callback is not a function");
                *out_result = R8E_UNDEFINED;
                return true;
            }
            R8EArrayInterp *res = (R8EArrayInterp *)calloc(1, sizeof(R8EArrayInterp));
            if (!res) { *out_result = R8E_UNDEFINED; return true; }
            res->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
            res->proto_id = 2;
            res->length = arr->length;
            res->capacity = arr->length > 0 ? arr->length : 1;
            res->elements = (R8EValue *)calloc(res->capacity, sizeof(R8EValue));
            for (uint32_t i = 0; i < arr->length; i++) {
                R8EValue cb_args[3];
                cb_args[0] = arr->elements[i];
                cb_args[1] = r8e_interp_from_int32((int32_t)i);
                cb_args[2] = this_val;
                R8EValue ret = r8e_interp_call_internal(ctx, argv[0],
                    R8E_UNDEFINED, cb_args, 3, false);
                if (ctx->has_exception) { *out_result = R8E_UNDEFINED; return true; }
                res->elements[i] = ret;
            }
            *out_result = r8e_interp_from_pointer(res);
            return true;
        }

        if (method_atom == 131 /* R8E_ATOM_filter */) {
            if (argc < 1 || !r8e_is_callable(argv[0])) {
                r8e_interp_throw_type_error(ctx, "filter callback is not a function");
                *out_result = R8E_UNDEFINED;
                return true;
            }
            R8EArrayInterp *res = (R8EArrayInterp *)calloc(1, sizeof(R8EArrayInterp));
            if (!res) { *out_result = R8E_UNDEFINED; return true; }
            res->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
            res->proto_id = 2;
            res->capacity = arr->length > 0 ? arr->length : 1;
            res->elements = (R8EValue *)calloc(res->capacity, sizeof(R8EValue));
            res->length = 0;
            for (uint32_t i = 0; i < arr->length; i++) {
                R8EValue cb_args[3];
                cb_args[0] = arr->elements[i];
                cb_args[1] = r8e_interp_from_int32((int32_t)i);
                cb_args[2] = this_val;
                R8EValue ret = r8e_interp_call_internal(ctx, argv[0],
                    R8E_UNDEFINED, cb_args, 3, false);
                if (ctx->has_exception) { *out_result = R8E_UNDEFINED; return true; }
                if (r8e_is_truthy(ret)) {
                    res->elements[res->length++] = arr->elements[i];
                }
            }
            *out_result = r8e_interp_from_pointer(res);
            return true;
        }

        if (method_atom == 121 /* R8E_ATOM_join */) {
            char sep_buf[8]; uint32_t sep_len;
            const char *sep = ","; sep_len = 1;
            if (argc >= 1 && r8e_is_string_val(argv[0])) {
                sep = r8e_interp_get_string(argv[0], sep_buf, &sep_len);
            }
            /* Build joined string */
            size_t total = 0;
            for (uint32_t i = 0; i < arr->length; i++) {
                char ebuf[8]; uint32_t elen;
                r8e_interp_get_string(arr->elements[i], ebuf, &elen);
                total += elen;
                if (i > 0) total += sep_len;
            }
            char *joined = (char *)malloc(total + 1);
            if (!joined) { *out_result = R8E_UNDEFINED; return true; }
            size_t pos = 0;
            for (uint32_t i = 0; i < arr->length; i++) {
                if (i > 0) { memcpy(joined + pos, sep, sep_len); pos += sep_len; }
                char ebuf[8]; uint32_t elen;
                const char *edata = r8e_interp_get_string(arr->elements[i], ebuf, &elen);
                memcpy(joined + pos, edata, elen); pos += elen;
            }
            joined[total] = '\0';
            *out_result = r8e_interp_make_string(joined, (uint32_t)total);
            free(joined);
            return true;
        }

        return false;
    }

    return false;
}

static R8EValue r8e_interp_call_internal(R8EInterpContext *ctx,
                                          R8EValue callee,
                                          R8EValue this_val,
                                          const R8EValue *argv, int argc,
                                          bool is_construct) {
    if (!R8E_IS_POINTER(callee)) {
        r8e_interp_throw_type_error(ctx, "not a function");
        return R8E_UNDEFINED;
    }

    void *ptr = r8e_interp_get_pointer(callee);
    if (!ptr) {
        r8e_interp_throw_type_error(ctx, "not a function");
        return R8E_UNDEFINED;
    }

    R8EGCHeader *h = (R8EGCHeader *)ptr;
    uint32_t kind = R8E_GC_GET_KIND(h->flags);

    /* Native function call (shifted encoding from r8e_interp.c) */
    if (kind == R8E_GC_KIND_FUNCTION) {
        R8ENativeFuncObj *nf = (R8ENativeFuncObj *)ptr;
        if (nf->callback) {
            return nf->callback(ctx, this_val, argc, argv);
        }
        r8e_interp_throw_type_error(ctx, "native function has no callback");
        return R8E_UNDEFINED;
    }

    /* Native function call (bottom-nibble encoding from r8e_builtin.c/r8e_weakref.c)
     * R8E_GC_KIND_FUNC = 0x04 stored directly in flags low nibble */
    if ((h->flags & 0x0Fu) == 0x04u && kind == R8E_GC_KIND_OBJECT) {
        R8ENativeFuncObj *nf = (R8ENativeFuncObj *)ptr;
        if (nf->callback) {
            return nf->callback(ctx, this_val, argc, argv);
        }
        r8e_interp_throw_type_error(ctx, "native function has no callback");
        return R8E_UNDEFINED;
    }

    /* Closure (bytecode function) call */
    if (kind != R8E_GC_KIND_CLOSURE) {
        r8e_interp_throw_type_error(ctx, "not a function");
        return R8E_UNDEFINED;
    }

    R8EClosureInterp *cl = (R8EClosureInterp *)ptr;
    R8EFunctionInterp *func = cl->func;
    if (!func || !func->bytecode) {
        r8e_interp_throw_type_error(ctx, "function has no bytecode");
        return R8E_UNDEFINED;
    }

    /* Check stack depth */
    if (ctx->frame_count >= R8E_MAX_CALL_DEPTH) {
        r8e_interp_throw_range_error(ctx, "Maximum call stack size exceeded");
        return R8E_UNDEFINED;
    }

    /* Set up new call frame */
    R8ECallFrame *frame = &ctx->frames[ctx->frame_count];
    memset(frame, 0, sizeof(R8ECallFrame));

    frame->closure = cl;
    frame->func = func;
    frame->pc = func->bytecode;
    frame->bytecode_end = func->bytecode + func->bytecode_len;
    frame->this_val = this_val;
    frame->new_target = is_construct ? callee : R8E_UNDEFINED;
    frame->argv = argv;
    frame->argc = argc;
    frame->constants = func->constants;
    frame->const_count = func->const_count;
    frame->is_construct = is_construct;
    frame->try_depth = 0;
    frame->scope_depth = 0;

    /* Allocate locals + operand stack in one contiguous block */
    uint32_t locals_needed = func->local_count;
    uint32_t stack_needed = func->stack_size ? func->stack_size
                                             : R8E_DEFAULT_STACK_SIZE;
    uint32_t total_slots = locals_needed + stack_needed + 1; /* +1 for canary */

    R8EValue *slot_buf = (R8EValue *)calloc(total_slots, sizeof(R8EValue));
    if (!slot_buf) {
        r8e_interp_throw_type_error(ctx, "out of memory");
        return R8E_UNDEFINED;
    }

    frame->locals = slot_buf;
    frame->stack_base = slot_buf + locals_needed;
    frame->sp = frame->stack_base;
    frame->canary_ptr = slot_buf + total_slots - 1;
    *frame->canary_ptr = R8E_STACK_CANARY;

    /* Copy arguments to local registers */
    for (uint16_t i = 0; i < func->param_count && i < (uint16_t)argc; i++) {
        frame->locals[i] = argv[i];
    }
    /* Fill remaining params with undefined */
    for (uint16_t i = (uint16_t)argc; i < func->param_count; i++) {
        frame->locals[i] = R8E_UNDEFINED;
    }

    ctx->frame_count++;

    /* Execute the frame */
    R8EValue result = r8e_interp_execute(ctx, frame);

    ctx->frame_count--;

    /* Verify canary */
    assert(*frame->canary_ptr == R8E_STACK_CANARY);

    /* Free slot buffer */
    free(slot_buf);

    /* Async function: wrap result in a resolved/rejected Promise */
    if (func->is_async && !is_construct) {
        if (ctx->has_exception) {
            /* Wrap exception in a rejected promise instead of propagating */
            R8EValue exc = ctx->exception;
            ctx->has_exception = false;
            ctx->exception = R8E_UNDEFINED;
            return r8e_interp_make_resolved_promise(exc, true);
        }
        return r8e_interp_make_resolved_promise(result, false);
    }

    return result;
}

/* =========================================================================
 * MAIN INTERPRETER DISPATCH LOOP
 *
 * This is the performance-critical hot loop. Design principles:
 *   - Computed goto (GCC/Clang) for direct threading (~15-20% faster)
 *   - Int32 fast paths first for arithmetic (zero function calls)
 *   - Chain superinstructions keep intermediates in CPU registers
 *   - Minimal stack manipulation in common cases
 *
 * Stack convention:
 *   sp always points to the NEXT free slot (past the top).
 *   Push: *sp++ = val;   Pop: val = *--sp;   Peek: val = sp[-1];
 * ========================================================================= */

/* Stack operation macros */
#define PUSH(v)    (*sp++ = (v))
#define POP()      (*--sp)
#define PEEK()     (sp[-1])
#define PEEK2()    (sp[-2])
#define DROP()     (--sp)

/* Computed goto vs switch dispatch */
#if defined(__GNUC__) || defined(__clang__)
  #define R8E_USE_COMPUTED_GOTO 1
#endif

/*
 * Dispatch mechanism:
 * - Computed goto (GCC/Clang): Each handler is a labeled block.
 *   The dispatch table maps opcode bytes to label addresses.
 *   DISPATCH() reads the next opcode and jumps directly to the handler.
 * - Switch fallback (MSVC): Standard switch-case dispatch.
 *   ~15-20% slower due to indirect branch prediction.
 *
 * TARGET(op) generates a label "op_<name>" for computed goto, or a
 * "case <value>:" for switch dispatch. The dispatch table addresses
 * must match the labels generated by TARGET.
 */
#ifdef R8E_USE_COMPUTED_GOTO
  #define DISPATCH()       goto *dispatch_table[*pc++]
  #define TARGET(name)     op_##name:
#else
  #define DISPATCH()       continue
  #define TARGET(name)     case name:
#endif

static R8EValue r8e_interp_execute(R8EInterpContext *ctx,
                                    R8ECallFrame *frame) {
    /* Local copies of frequently accessed frame fields for performance.
     * These are the interpreter's "register file" from the CPU's perspective.
     * Keeping them in locals lets the compiler assign them to CPU registers. */
    const uint8_t *pc = frame->pc;
    R8EValue      *sp = frame->sp;
    R8EValue      *locals = frame->locals;
    R8EValue      *constants = frame->constants;
    R8EClosureInterp *closure = frame->closure;

    /* Temporary variables used across handlers */
    R8EValue a, b, result;
    int32_t ia, ib;
    int64_t i64;
    double da, db;
    uint32_t atom;
    uint8_t reg, argc_op;
    int8_t off8;
    int16_t off16;
    int32_t off32;

#ifdef R8E_USE_COMPUTED_GOTO
    /* =====================================================================
     * Dispatch table: 256 entries, one for each possible opcode byte.
     * Unused opcodes jump to op_invalid.
     * ===================================================================== */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    /* Dispatch table uses &&op_<OPNAME> labels to match TARGET(OPNAME) */
    #define L(name) &&op_##name
    #define INV     &&op_INVALID
    static const void *dispatch_table[256] = {
        /* 0x00 */ L(OP_LOAD_LOCAL),     L(OP_STORE_LOCAL),
        /* 0x02 */ L(OP_LOAD_GLOBAL),    L(OP_STORE_GLOBAL),
        /* 0x04 */ L(OP_LOAD_CONST),     L(OP_PUSH_INT8),
        /* 0x06 */ L(OP_PUSH_INT16),     L(OP_PUSH_INT32),
        /* 0x08 */ L(OP_PUSH_DOUBLE),    L(OP_PUSH_UNDEFINED),
        /* 0x0A */ L(OP_PUSH_NULL),      L(OP_PUSH_TRUE),
        /* 0x0C */ L(OP_PUSH_FALSE),     L(OP_PUSH_ZERO),
        /* 0x0E */ L(OP_PUSH_ONE),       L(OP_PUSH_STRING),

        /* 0x10 */ L(OP_DUP),            L(OP_DROP),
        /* 0x12 */ L(OP_SWAP),           L(OP_ROT3),
        /* 0x14 */ L(OP_GET_PROP),       L(OP_SET_PROP),
        /* 0x16 */ L(OP_GET_ELEM),       L(OP_SET_ELEM),
        /* 0x18 */ L(OP_DELETE_PROP),    L(OP_DELETE_ELEM),
        /* 0x1A */ L(OP_GET_PROP_2),     L(OP_DEFINE_PROP),
        /* 0x1C */ L(OP_GET_SUPER),      L(OP_SET_SUPER),
        /* 0x1E */ INV,                  INV,

        /* 0x20 */ L(OP_ADD),            L(OP_SUB),
        /* 0x22 */ L(OP_MUL),            L(OP_DIV),
        /* 0x24 */ L(OP_MOD),            L(OP_POW),
        /* 0x26 */ L(OP_NEG),            L(OP_POS),
        /* 0x28 */ L(OP_INC),            L(OP_DEC),
        /* 0x2A */ L(OP_POST_INC),       L(OP_POST_DEC),
        /* 0x2C */ L(OP_BITAND),         L(OP_BITOR),
        /* 0x2E */ L(OP_BITXOR),         L(OP_BITNOT),

        /* 0x30 */ L(OP_SHL),            L(OP_SHR),
        /* 0x32 */ L(OP_USHR),           INV,
        /* 0x34 */ L(OP_EQ),             L(OP_SEQ),
        /* 0x36 */ L(OP_NE),             L(OP_SNE),
        /* 0x38 */ L(OP_LT),             L(OP_LE),
        /* 0x3A */ L(OP_GT),             L(OP_GE),
        /* 0x3C */ L(OP_NOT),            L(OP_TYPEOF),
        /* 0x3E */ L(OP_INSTANCEOF),     L(OP_IN),

        /* 0x40 */ L(OP_JUMP),           L(OP_JUMP8),
        /* 0x42 */ L(OP_JUMP16),         L(OP_JUMP_IF_FALSE),
        /* 0x44 */ L(OP_JUMP_IF_TRUE),   L(OP_JUMP_IF_FALSE8),
        /* 0x46 */ L(OP_JUMP_IF_TRUE8),  L(OP_JUMP_IF_FALSE16),
        /* 0x48 */ L(OP_JUMP_IF_TRUE16), L(OP_JUMP_IF_NULLISH),
        /* 0x4A */ INV,                  INV,
        /* 0x4C */ L(OP_CALL),           L(OP_CALL_METHOD),
        /* 0x4E */ L(OP_NEW),            L(OP_RETURN),

        /* 0x50 */ L(OP_RETURN_UNDEFINED), L(OP_TAIL_CALL),
        /* 0x52 */ INV,                  INV,
        /* 0x54 */ L(OP_THROW),          L(OP_TRY_PUSH),
        /* 0x56 */ L(OP_TRY_POP),        L(OP_CATCH),
        /* 0x58 */ L(OP_FINALLY),        INV,
        /* 0x5A */ INV,                  INV,
        /* 0x5C */ L(OP_NEW_OBJECT),     L(OP_NEW_ARRAY),
        /* 0x5E */ L(OP_NEW_FUNCTION),   L(OP_NEW_REGEXP),

        /* 0x60 */ L(OP_INIT_PROP),      L(OP_INIT_ELEM),
        /* 0x62 */ L(OP_SPREAD),         INV,
        /* 0x64 */ L(OP_LOAD_CLOSURE),   L(OP_STORE_CLOSURE),
        /* 0x66 */ L(OP_LOAD_ARG),       L(OP_LOAD_REST_ARGS),
        /* 0x68 */ L(OP_ENTER_SCOPE),    L(OP_EXIT_SCOPE),
        /* 0x6A */ L(OP_LOAD_THIS),      L(OP_LOAD_NEW_TARGET),
        /* 0x6C */ L(OP_DESTRUCTURE_ARRAY), L(OP_DESTRUCTURE_OBJECT),
        /* 0x6E */ L(OP_SET_DEFAULT),    INV,

        /* 0x70 */ L(OP_GET_ITERATOR),   L(OP_ITERATOR_NEXT),
        /* 0x72 */ L(OP_ITERATOR_CLOSE), L(OP_YIELD),
        /* 0x74 */ L(OP_YIELD_STAR),     L(OP_AWAIT),
        /* 0x76 */ L(OP_ASYNC_FUNC_START), L(OP_ASYNC_FUNC_RESOLVE),
        /* 0x78 */ L(OP_ASYNC_FUNC_REJECT), INV,
        /* 0x7A */ INV,                  INV,
        /* 0x7C */ L(OP_CLASS_CREATE),   L(OP_CLASS_EXTENDS),
        /* 0x7E */ L(OP_CLASS_METHOD),   L(OP_CLASS_FIELD),

        /* 0x80 */ L(OP_CLASS_STATIC),   L(OP_CLASS_PRIVATE),
        /* 0x82 */ INV,                  INV,
        /* 0x84 */ L(OP_IMPORT),         L(OP_EXPORT),
        /* 0x86 */ L(OP_IMPORT_DYNAMIC), L(OP_IMPORT_META),
        /* 0x88 */ L(OP_OPTIONAL_CHAIN), L(OP_NULLISH_COALESCE),
        /* 0x8A */ INV,                  INV,
        /* 0x8C */ L(OP_VOID),           L(OP_DEBUGGER),
        /* 0x8E */ L(OP_WITH_ENTER),     L(OP_WITH_EXIT),

        /* 0x90 */ L(OP_FOR_IN_INIT),    L(OP_FOR_IN_NEXT),
        /* 0x92 */ L(OP_FOR_OF_INIT),    L(OP_FOR_OF_NEXT),
        /* 0x94 */ L(OP_TEMPLATE_HEAD),  L(OP_TEMPLATE_MIDDLE),
        /* 0x96 */ L(OP_TEMPLATE_TAIL),  L(OP_TAGGED_TEMPLATE),
        /* 0x98-0xDF: invalid */
        [0x98 ... 0xDF] = INV,

        /* 0xE0 */ L(OP_CALL_METHOD_FAST),
        /* 0xE1 */ L(OP_FOR_INC_LOOP),
        /* 0xE2 */ L(OP_LOOP_CMP_BRANCH),
        /* 0xE3 */ L(OP_LOAD_FIELD_CHECK),
        /* 0xE4 */ L(OP_BRANCH_LOAD),
        /* 0xE5 */ L(OP_STORE_LOCAL_POP),
        /* 0xE6 */ L(OP_LOAD_LOCAL2),
        /* 0xE7-0xEF: invalid */
        [0xE7 ... 0xEF] = INV,

        /* 0xF0 */ L(OP_EVAL_TRAP),
        /* 0xF1 */ L(OP_CHECK_LIMITS),
        /* 0xF2-0xFD: invalid */
        [0xF2 ... 0xFD] = INV,
        /* 0xFE */ L(OP_NOP),
        /* 0xFF */ L(OP_HALT),
    };
    #undef L
    #undef INV

#pragma GCC diagnostic pop

    /* Begin execution */
    DISPATCH();
#else
    /* Switch-based dispatch fallback (MSVC / portable) */
    uint8_t op;
    for (;;) {
        op = *pc++;
        switch (op) {
#endif

    /* =================================================================
     * LOAD/STORE OPCODES
     * ================================================================= */

    TARGET(OP_LOAD_LOCAL) {
        reg = read_u8(&pc);
        PUSH(locals[reg]);
        DISPATCH();
    }

    TARGET(OP_STORE_LOCAL) {
        reg = read_u8(&pc);
        locals[reg] = POP();
        DISPATCH();
    }

    TARGET(OP_LOAD_GLOBAL) {
        atom = read_u32(&pc);
        PUSH(r8e_interp_load_global(ctx, atom));
        if (ctx->has_exception) goto exception;
        DISPATCH();
    }

    TARGET(OP_STORE_GLOBAL) {
        atom = read_u32(&pc);
        a = POP();
        r8e_interp_store_global(ctx, atom, a);
        if (ctx->has_exception) goto exception;
        DISPATCH();
    }

    TARGET(OP_LOAD_CONST) {
        uint16_t idx = read_u16(&pc);
        PUSH(constants[idx]);
        DISPATCH();
    }

    TARGET(OP_PUSH_INT8) {
        ia = read_i8(&pc);
        PUSH(r8e_interp_from_int32(ia));
        DISPATCH();
    }

    TARGET(OP_PUSH_INT16) {
        ia = read_i16(&pc);
        PUSH(r8e_interp_from_int32(ia));
        DISPATCH();
    }

    TARGET(OP_PUSH_INT32) {
        ia = read_i32(&pc);
        PUSH(r8e_interp_from_int32(ia));
        DISPATCH();
    }

    TARGET(OP_PUSH_DOUBLE) {
        uint16_t idx = read_u16(&pc);
        PUSH(constants[idx]);
        DISPATCH();
    }

    TARGET(OP_PUSH_UNDEFINED) { PUSH(R8E_UNDEFINED); DISPATCH(); }
    TARGET(OP_PUSH_NULL)      { PUSH(R8E_NULL);      DISPATCH(); }
    TARGET(OP_PUSH_TRUE)      { PUSH(R8E_TRUE);      DISPATCH(); }
    TARGET(OP_PUSH_FALSE)     { PUSH(R8E_FALSE);     DISPATCH(); }
    TARGET(OP_PUSH_ZERO)      { PUSH(r8e_interp_from_int32(0)); DISPATCH(); }
    TARGET(OP_PUSH_ONE)       { PUSH(r8e_interp_from_int32(1)); DISPATCH(); }

    TARGET(OP_PUSH_STRING) {
        uint16_t idx = read_u16(&pc);
        PUSH(constants[idx]);
        DISPATCH();
    }

    /* =================================================================
     * STACK MANIPULATION
     * ================================================================= */

    TARGET(OP_DUP) {
        a = PEEK();
        PUSH(a);
        DISPATCH();
    }

    TARGET(OP_DROP) {
        DROP();
        DISPATCH();
    }

    TARGET(OP_SWAP) {
        a = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = a;
        DISPATCH();
    }

    TARGET(OP_ROT3) {
        /* Rotate top 3: [a b c] -> [c a b] */
        a = sp[-3];
        sp[-3] = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = a;
        DISPATCH();
    }

    /* =================================================================
     * PROPERTY ACCESS
     * ================================================================= */

    TARGET(OP_GET_PROP) {
        atom = read_u32(&pc);
        ctx->last_prop_atom = atom;
        a = POP(); /* object */
        result = r8e_interp_get_prop(ctx, a, atom);
        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_SET_PROP) {
        atom = read_u32(&pc);
        b = POP(); /* value */
        a = POP(); /* object */
        r8e_interp_set_prop(ctx, a, atom, b);
        if (ctx->has_exception) goto exception;
        DISPATCH();
    }

    TARGET(OP_GET_ELEM) {
        b = POP(); /* key */
        a = POP(); /* object */
        result = r8e_interp_get_elem(ctx, a, b);
        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_SET_ELEM) {
        R8EValue val = POP();
        b = POP(); /* key */
        a = POP(); /* object */
        r8e_interp_set_elem(ctx, a, b, val);
        if (ctx->has_exception) goto exception;
        DISPATCH();
    }

    TARGET(OP_DELETE_PROP) {
        atom = read_u32(&pc);
        a = POP(); /* object */
        /* Simplified: always return true */
        PUSH(R8E_TRUE);
        (void)atom;
        DISPATCH();
    }

    TARGET(OP_DELETE_ELEM) {
        b = POP(); /* key */
        a = POP(); /* object */
        PUSH(R8E_TRUE);
        (void)a; (void)b;
        DISPATCH();
    }

    TARGET(OP_GET_PROP_2) {
        /* Pop obj, push obj AND obj[atom] (for method calls) */
        atom = read_u32(&pc);
        ctx->last_prop_atom = atom;
        a = PEEK(); /* keep obj on stack */
        result = r8e_interp_get_prop(ctx, a, atom);
        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_DEFINE_PROP) {
        atom = read_u32(&pc);
        uint8_t desc_flags = read_u8(&pc);
        b = POP(); /* value */
        a = PEEK(); /* object (keep on stack) */
        r8e_interp_set_prop(ctx, a, atom, b);
        (void)desc_flags;
        DISPATCH();
    }

    TARGET(OP_GET_SUPER) {
        atom = read_u32(&pc);
        a = POP();
        /*
         * super.prop: look up property on the prototype of the
         * home object's prototype.  The value on stack is 'this'.
         * We walk up the prototype chain one level from 'this' and
         * do a normal property lookup there.
         */
        if (R8E_IS_POINTER(a)) {
            void *ptr = r8e_interp_get_pointer(a);
            R8EGCHeader *h = (R8EGCHeader *)ptr;
            if (h) {
                /* Get proto_id and look up property on the prototype object.
                 * For now, use the standard property lookup which will search
                 * the prototype chain. A full implementation would start
                 * the search one level up from the home object. */
                result = r8e_interp_get_prop(ctx, a, atom);
                if (ctx->has_exception) goto exception;
                PUSH(result);
            } else {
                PUSH(R8E_UNDEFINED);
            }
        } else {
            r8e_interp_throw_type_error(ctx,
                "Cannot read super property of non-object");
            goto exception;
        }
        DISPATCH();
    }

    TARGET(OP_SET_SUPER) {
        atom = read_u32(&pc);
        b = POP(); /* value */
        a = POP(); /* this / super receiver */
        /*
         * super.prop = val: set property through the prototype chain.
         * The receiver is 'this'.
         */
        if (R8E_IS_POINTER(a)) {
            r8e_interp_set_prop(ctx, a, atom, b);
            if (ctx->has_exception) goto exception;
        } else {
            r8e_interp_throw_type_error(ctx,
                "Cannot set super property of non-object");
            goto exception;
        }
        DISPATCH();
    }

    /* =================================================================
     * ARITHMETIC (with int32 fast paths - Section 7.1)
     * ================================================================= */

    TARGET(OP_ADD) {
        b = POP();
        a = POP();
        /* Int32 fast path: most common in loops and counters */
        if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
            i64 = (int64_t)r8e_interp_get_int32(a) +
                  (int64_t)r8e_interp_get_int32(b);
            if (i64 >= INT32_MIN && i64 <= INT32_MAX) {
                PUSH(r8e_interp_from_int32((int32_t)i64));
            } else {
                PUSH(r8e_interp_from_double((double)i64));
            }
        }
        /* Double fast path */
        else if (R8E_IS_DOUBLE(a) && R8E_IS_DOUBLE(b)) {
            PUSH(r8e_number_from_double(
                r8e_interp_get_double(a) + r8e_interp_get_double(b)));
        }
        /* Mixed int/double */
        else if (R8E_IS_NUMBER(a) && R8E_IS_NUMBER(b)) {
            PUSH(r8e_number_from_double(
                r8e_to_double_fast(a) + r8e_to_double_fast(b)));
        }
        /* Slow path: string concat, type coercion */
        else {
            PUSH(r8e_add_slow(ctx, a, b));
        }
        DISPATCH();
    }

    TARGET(OP_SUB) {
        b = POP(); a = POP();
        if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
            i64 = (int64_t)r8e_interp_get_int32(a) -
                  (int64_t)r8e_interp_get_int32(b);
            if (i64 >= INT32_MIN && i64 <= INT32_MAX)
                PUSH(r8e_interp_from_int32((int32_t)i64));
            else
                PUSH(r8e_interp_from_double((double)i64));
        } else {
            PUSH(r8e_number_from_double(
                r8e_to_double_fast(a) - r8e_to_double_fast(b)));
        }
        DISPATCH();
    }

    TARGET(OP_MUL) {
        b = POP(); a = POP();
        if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
            i64 = (int64_t)r8e_interp_get_int32(a) *
                  (int64_t)r8e_interp_get_int32(b);
            if (i64 >= INT32_MIN && i64 <= INT32_MAX)
                PUSH(r8e_interp_from_int32((int32_t)i64));
            else
                PUSH(r8e_interp_from_double((double)i64));
        } else {
            PUSH(r8e_number_from_double(
                r8e_to_double_fast(a) * r8e_to_double_fast(b)));
        }
        DISPATCH();
    }

    TARGET(OP_DIV) {
        b = POP(); a = POP();
        /* Division always produces a double (even 6/3 -> 2.0 -> int32(2)) */
        da = r8e_to_double_fast(a);
        db = r8e_to_double_fast(b);
        PUSH(r8e_number_from_double(da / db));
        DISPATCH();
    }

    TARGET(OP_MOD) {
        b = POP(); a = POP();
        if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
            ib = r8e_interp_get_int32(b);
            if (ib != 0) {
                ia = r8e_interp_get_int32(a);
                /* C99 remainder has same sign as dividend */
                PUSH(r8e_interp_from_int32(ia % ib));
            } else {
                PUSH(r8e_interp_from_double(NAN));
            }
        } else {
            PUSH(r8e_number_from_double(
                fmod(r8e_to_double_fast(a), r8e_to_double_fast(b))));
        }
        DISPATCH();
    }

    TARGET(OP_POW) {
        b = POP(); a = POP();
        PUSH(r8e_number_from_double(
            pow(r8e_to_double_fast(a), r8e_to_double_fast(b))));
        DISPATCH();
    }

    TARGET(OP_NEG) {
        a = POP();
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia == 0) {
                PUSH(r8e_interp_from_double(-0.0));
            } else if (ia == INT32_MIN) {
                PUSH(r8e_interp_from_double(-(double)INT32_MIN));
            } else {
                PUSH(r8e_interp_from_int32(-ia));
            }
        } else {
            PUSH(r8e_number_from_double(-r8e_to_double_fast(a)));
        }
        DISPATCH();
    }

    TARGET(OP_POS) {
        a = POP();
        if (R8E_IS_NUMBER(a)) {
            PUSH(a);
        } else {
            PUSH(r8e_number_from_double(r8e_to_double_fast(a)));
        }
        DISPATCH();
    }

    TARGET(OP_INC) {
        a = POP();
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia < INT32_MAX)
                PUSH(r8e_interp_from_int32(ia + 1));
            else
                PUSH(r8e_interp_from_double((double)ia + 1.0));
        } else {
            PUSH(r8e_number_from_double(r8e_to_double_fast(a) + 1.0));
        }
        DISPATCH();
    }

    TARGET(OP_DEC) {
        a = POP();
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia > INT32_MIN)
                PUSH(r8e_interp_from_int32(ia - 1));
            else
                PUSH(r8e_interp_from_double((double)ia - 1.0));
        } else {
            PUSH(r8e_number_from_double(r8e_to_double_fast(a) - 1.0));
        }
        DISPATCH();
    }

    TARGET(OP_POST_INC) {
        reg = read_u8(&pc);
        a = locals[reg];
        PUSH(a); /* push old value */
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia < INT32_MAX)
                locals[reg] = r8e_interp_from_int32(ia + 1);
            else
                locals[reg] = r8e_interp_from_double((double)ia + 1.0);
        } else {
            locals[reg] = r8e_number_from_double(
                r8e_to_double_fast(a) + 1.0);
        }
        DISPATCH();
    }

    TARGET(OP_POST_DEC) {
        reg = read_u8(&pc);
        a = locals[reg];
        PUSH(a); /* push old value */
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia > INT32_MIN)
                locals[reg] = r8e_interp_from_int32(ia - 1);
            else
                locals[reg] = r8e_interp_from_double((double)ia - 1.0);
        } else {
            locals[reg] = r8e_number_from_double(
                r8e_to_double_fast(a) - 1.0);
        }
        DISPATCH();
    }

    /* =================================================================
     * BITWISE OPERATIONS
     * ================================================================= */

    TARGET(OP_BITAND) {
        b = POP(); a = POP();
        PUSH(r8e_interp_from_int32(r8e_val_to_int32(a) &
                                    r8e_val_to_int32(b)));
        DISPATCH();
    }

    TARGET(OP_BITOR) {
        b = POP(); a = POP();
        PUSH(r8e_interp_from_int32(r8e_val_to_int32(a) |
                                    r8e_val_to_int32(b)));
        DISPATCH();
    }

    TARGET(OP_BITXOR) {
        b = POP(); a = POP();
        PUSH(r8e_interp_from_int32(r8e_val_to_int32(a) ^
                                    r8e_val_to_int32(b)));
        DISPATCH();
    }

    TARGET(OP_BITNOT) {
        a = POP();
        PUSH(r8e_interp_from_int32(~r8e_val_to_int32(a)));
        DISPATCH();
    }

    TARGET(OP_SHL) {
        b = POP(); a = POP();
        PUSH(r8e_interp_from_int32(
            r8e_val_to_int32(a) << (r8e_val_to_uint32(b) & 0x1F)));
        DISPATCH();
    }

    TARGET(OP_SHR) {
        b = POP(); a = POP();
        PUSH(r8e_interp_from_int32(
            r8e_val_to_int32(a) >> (r8e_val_to_uint32(b) & 0x1F)));
        DISPATCH();
    }

    TARGET(OP_USHR) {
        b = POP(); a = POP();
        uint32_t ur = r8e_val_to_uint32(a) >> (r8e_val_to_uint32(b) & 0x1F);
        if (ur > (uint32_t)INT32_MAX)
            PUSH(r8e_interp_from_double((double)ur));
        else
            PUSH(r8e_interp_from_int32((int32_t)ur));
        DISPATCH();
    }

    /* =================================================================
     * COMPARISON AND LOGIC
     * ================================================================= */

    TARGET(OP_EQ) {
        b = POP(); a = POP();
        PUSH(r8e_abstract_eq(NULL, a, b));
        DISPATCH();
    }

    TARGET(OP_SEQ) {
        b = POP(); a = POP();
        PUSH(r8e_strict_eq(a, b));
        DISPATCH();
    }

    TARGET(OP_NE) {
        b = POP(); a = POP();
        PUSH(r8e_abstract_eq(NULL, a, b) == R8E_TRUE ? R8E_FALSE : R8E_TRUE);
        DISPATCH();
    }

    TARGET(OP_SNE) {
        b = POP(); a = POP();
        PUSH(r8e_strict_eq(a, b) == R8E_TRUE ? R8E_FALSE : R8E_TRUE);
        DISPATCH();
    }

    TARGET(OP_LT) {
        b = POP(); a = POP();
        PUSH(r8e_compare_lt(a, b));
        DISPATCH();
    }

    TARGET(OP_LE) {
        b = POP(); a = POP();
        PUSH(r8e_compare_le(a, b));
        DISPATCH();
    }

    TARGET(OP_GT) {
        b = POP(); a = POP();
        PUSH(r8e_compare_gt(a, b));
        DISPATCH();
    }

    TARGET(OP_GE) {
        b = POP(); a = POP();
        PUSH(r8e_compare_ge(a, b));
        DISPATCH();
    }

    TARGET(OP_NOT) {
        a = POP();
        PUSH(r8e_is_truthy(a) ? R8E_FALSE : R8E_TRUE);
        DISPATCH();
    }

    TARGET(OP_TYPEOF) {
        a = POP();
        /* Implement typeof inline to produce interpreter-compatible strings. */
        if (R8E_IS_UNDEFINED(a)) {
            PUSH(r8e_interp_make_string("undefined", 9));
        } else if (R8E_IS_NULL(a)) {
            PUSH(r8e_interp_make_string("object", 6));
        } else if (R8E_IS_BOOLEAN(a)) {
            PUSH(r8e_interp_make_string("boolean", 7));
        } else if (R8E_IS_INT32(a) || R8E_IS_DOUBLE(a)) {
            PUSH(r8e_interp_make_string("number", 6));
        } else if (r8e_is_string_val(a)) {
            PUSH(r8e_interp_make_string("string", 6));
        } else if (R8E_IS_SYMBOL(a)) {
            PUSH(r8e_interp_make_string("symbol", 6));
        } else if (R8E_IS_POINTER(a)) {
            const R8EGCHeader *th = (const R8EGCHeader *)r8e_interp_get_pointer(a);
            uint32_t tkind = th ? R8E_GC_GET_KIND(th->flags) : 0;
            if (tkind == R8E_GC_KIND_CLOSURE || tkind == R8E_GC_KIND_FUNCTION) {
                PUSH(r8e_interp_make_string("function", 8));
            } else {
                PUSH(r8e_interp_make_string("object", 6));
            }
        } else {
            PUSH(r8e_interp_make_string("undefined", 9));
        }
        DISPATCH();
    }

    TARGET(OP_INSTANCEOF) {
        b = POP(); /* constructor (RHS) */
        a = POP(); /* instance (LHS) */
        /*
         * ES2023 instanceof: Check if constructor.prototype appears
         * anywhere in the prototype chain of the instance.
         * Also checks Symbol.hasInstance if present on the constructor.
         */
        {
            bool is_instance = false;

            /* First: check for Symbol.hasInstance on the RHS */
            /* (Symbol.hasInstance check would go here in a full impl;
             *  for now, fall through to the standard OrdinaryHasInstance) */

            /* OrdinaryHasInstance: b must be callable */
            if (!r8e_is_callable(b)) {
                PUSH(R8E_FALSE);
                DISPATCH();
            }

            /* a must be an object */
            if (!R8E_IS_POINTER(a)) {
                PUSH(R8E_FALSE);
                DISPATCH();
            }

            /* Get constructor.prototype */
            R8EValue ctor_proto = r8e_interp_get_prop(ctx, b,
                2 /* R8E_ATOM_prototype */);
            if (ctx->has_exception) goto exception;

            if (!R8E_IS_POINTER(ctor_proto)) {
                PUSH(R8E_FALSE);
                DISPATCH();
            }

            /* Walk the prototype chain of 'a' */
            R8EValue current = a;
            int chain_limit = 128; /* safety limit */
            while (R8E_IS_POINTER(current) && chain_limit-- > 0) {
                void *cptr = r8e_interp_get_pointer(current);
                if (!cptr) break;
                R8EGCHeader *ch = (R8EGCHeader *)cptr;
                uint32_t pid = ch->proto_id;
                /* Compare proto_id: if ctor_proto has the same identity,
                 * the object is an instance. For pointer-based comparison,
                 * check if the prototype objects are the same pointer. */
                void *proto_ptr = r8e_interp_get_pointer(ctor_proto);
                /* Walk up: for now, use proto_id matching.
                 * A full implementation would follow actual prototype pointers. */
                if (pid != 0 && cptr != NULL) {
                    /* Direct pointer comparison of prototype objects */
                    if (cptr == proto_ptr) {
                        is_instance = true;
                        break;
                    }
                }
                /* Move up the prototype chain - simplified: break after first level
                 * since we don't have actual prototype object pointers */
                break;
            }

            PUSH(is_instance ? R8E_TRUE : R8E_FALSE);
        }
        DISPATCH();
    }

    TARGET(OP_IN) {
        b = POP(); /* object (RHS) */
        a = POP(); /* key (LHS) */
        /* Check if property exists in object */
        if (R8E_IS_POINTER(b)) {
            bool in_found = false;
            void *in_ptr = r8e_interp_get_pointer(b);
            R8EGCHeader *in_h = in_ptr ? (R8EGCHeader *)in_ptr : NULL;
            uint32_t in_kind = in_h ? R8E_GC_GET_KIND(in_h->flags) : 99;

            if (R8E_IS_ATOM(a)) {
                atom = (uint32_t)(a & 0xFFFFFFFFULL);
                result = r8e_interp_get_prop(ctx, b, atom);
                in_found = !R8E_IS_UNDEFINED(result);
            } else if (r8e_is_string_val(a) && in_kind == R8E_GC_KIND_OBJECT) {
                /* String key: compare against object property keys */
                char in_buf[8]; uint32_t in_len;
                const char *in_key = r8e_interp_get_string(a, in_buf, &in_len);
                uint8_t in_tier = in_h->flags & 0x03;
                if (in_tier == 1) {
                    R8EObjTier1Interp *in_t1 = (R8EObjTier1Interp *)in_ptr;
                    for (uint8_t ii = 0; ii < in_t1->count; ii++) {
                        R8EValue pk = in_t1->props[ii].key;
                        if (R8E_IS_ATOM(pk)) {
                            /* Convert atom to string and compare */
                            extern const char *r8e_atom_get(void *, uint32_t);
                            uint32_t aid = (uint32_t)(pk & 0xFFFFFFFFULL);
                            const char *aname = r8e_atom_get(NULL, aid);
                            if (aname && strlen(aname) == in_len &&
                                memcmp(aname, in_key, in_len) == 0) {
                                in_found = true; break;
                            }
                        }
                    }
                }
            } else if (R8E_IS_INT32(a) && in_kind == R8E_GC_KIND_ARRAY) {
                int32_t in_idx = r8e_interp_get_int32(a);
                R8EArrayInterp *in_arr = (R8EArrayInterp *)in_ptr;
                in_found = in_idx >= 0 && (uint32_t)in_idx < in_arr->length;
            }
            PUSH(in_found ? R8E_TRUE : R8E_FALSE);
        } else {
            r8e_interp_throw_type_error(ctx, "Cannot use 'in' operator on non-object");
            goto exception;
        }
        DISPATCH();
    }

    /* =================================================================
     * CONTROL FLOW
     * ================================================================= */

    TARGET(OP_JUMP) {
        off32 = read_i32(&pc);
        pc += off32; /* offset is relative to position after operand */
        DISPATCH();
    }

    TARGET(OP_JUMP8) {
        off8 = read_i8(&pc);
        pc += off8;
        DISPATCH();
    }

    TARGET(OP_JUMP16) {
        off16 = read_i16(&pc);
        pc += off16;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_FALSE) {
        off32 = read_i32(&pc);
        a = POP();
        if (!r8e_is_truthy(a)) pc += off32;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_TRUE) {
        off32 = read_i32(&pc);
        a = POP();
        if (r8e_is_truthy(a)) pc += off32;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_FALSE8) {
        off8 = read_i8(&pc);
        a = POP();
        if (!r8e_is_truthy(a)) pc += off8;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_TRUE8) {
        off8 = read_i8(&pc);
        a = POP();
        if (r8e_is_truthy(a)) pc += off8;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_FALSE16) {
        off16 = read_i16(&pc);
        a = POP();
        if (!r8e_is_truthy(a)) pc += off16;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_TRUE16) {
        off16 = read_i16(&pc);
        a = POP();
        if (r8e_is_truthy(a)) pc += off16;
        DISPATCH();
    }

    TARGET(OP_JUMP_IF_NULLISH) {
        off32 = read_i32(&pc);
        a = POP();
        if (R8E_IS_NULLISH(a)) pc += off32;
        DISPATCH();
    }

    /* =================================================================
     * FUNCTION CALLS
     * ================================================================= */

    TARGET(OP_CALL) {
        argc_op = read_u8(&pc);
        /* Stack: [func, arg0, arg1, ..., argN-1] */
        sp -= argc_op;
        const R8EValue *call_args = sp;
        R8EValue func_val = POP();

        /* Save frame state */
        frame->pc = pc;
        frame->sp = sp;

        result = r8e_interp_call_internal(ctx, func_val, R8E_UNDEFINED,
                                           call_args, argc_op, false);
        /* Restore frame state */
        pc = frame->pc;
        sp = frame->sp;
        locals = frame->locals;
        constants = frame->constants;
        closure = frame->closure;

        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_CALL_METHOD) {
        argc_op = read_u8(&pc);
        /* Stack: [this, func, arg0, ..., argN-1] */
        sp -= argc_op;
        const R8EValue *call_args = sp;
        R8EValue func_val = POP();
        R8EValue this_obj = POP();

        frame->pc = pc;
        frame->sp = sp;

        /* Try builtin method dispatch if callee is not callable */
        if (!r8e_is_callable(func_val)) {
            R8EValue builtin_result;
            if (r8e_interp_builtin_method(ctx, this_obj,
                    ctx->last_prop_atom, call_args, argc_op,
                    &builtin_result)) {
                pc = frame->pc;
                sp = frame->sp;
                locals = frame->locals;
                constants = frame->constants;
                closure = frame->closure;
                if (ctx->has_exception) goto exception;
                PUSH(builtin_result);
                DISPATCH();
            }
        }

        result = r8e_interp_call_internal(ctx, func_val, this_obj,
                                           call_args, argc_op, false);
        pc = frame->pc;
        sp = frame->sp;
        locals = frame->locals;
        constants = frame->constants;
        closure = frame->closure;

        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_NEW) {
        argc_op = read_u8(&pc);
        sp -= argc_op;
        const R8EValue *call_args = sp;
        R8EValue ctor = POP();

        frame->pc = pc;
        frame->sp = sp;

        result = r8e_interp_call_internal(ctx, ctor, R8E_UNDEFINED,
                                           call_args, argc_op, true);
        pc = frame->pc;
        sp = frame->sp;
        locals = frame->locals;
        constants = frame->constants;
        closure = frame->closure;

        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_RETURN) {
        result = POP();
        goto do_return;
    }

    TARGET(OP_RETURN_UNDEFINED) {
        result = R8E_UNDEFINED;
        goto do_return;
    }

    TARGET(OP_TAIL_CALL) {
        /* Treat as regular call for now */
        argc_op = read_u8(&pc);
        sp -= argc_op;
        const R8EValue *call_args = sp;
        R8EValue func_val = POP();

        frame->pc = pc;
        frame->sp = sp;

        result = r8e_interp_call_internal(ctx, func_val, R8E_UNDEFINED,
                                           call_args, argc_op, false);
        if (ctx->has_exception) goto exception;
        goto do_return_with_result;
    }

    /* =================================================================
     * EXCEPTION HANDLING
     * ================================================================= */

    TARGET(OP_THROW) {
        a = POP();
        r8e_interp_throw(ctx, a);
        goto exception;
    }

    TARGET(OP_TRY_PUSH) {
        off32 = read_i32(&pc);
        if (frame->try_depth >= R8E_MAX_TRY_DEPTH) {
            r8e_interp_throw_range_error(ctx, "try stack overflow");
            goto exception;
        }
        R8ETryEntry *entry = &frame->try_stack[frame->try_depth];
        entry->catch_pc = pc + off32; /* catch block address */
        entry->finally_pc = NULL;
        entry->saved_sp = sp;
        entry->scope_depth = frame->scope_depth;
        frame->try_depth++;
        DISPATCH();
    }

    TARGET(OP_TRY_POP) {
        if (frame->try_depth > 0) frame->try_depth--;
        DISPATCH();
    }

    TARGET(OP_CATCH) {
        /* Push the caught exception onto the stack */
        PUSH(ctx->exception);
        ctx->has_exception = false;
        ctx->exception = R8E_UNDEFINED;
        DISPATCH();
    }

    TARGET(OP_FINALLY) {
        /* Finally block runs regardless; exception re-thrown after */
        DISPATCH();
    }

    /* =================================================================
     * OBJECT/ARRAY CREATION
     * ================================================================= */

    TARGET(OP_NEW_OBJECT) {
        /* Allocate a tier-1 object (supports up to 4 properties).
         * Most object literals have 2-4 properties, so tier 1 is ideal. */
        R8EObjTier1Interp *t1 = (R8EObjTier1Interp *)calloc(
            1, sizeof(R8EObjTier1Interp));
        if (!t1) {
            r8e_interp_throw_type_error(ctx, "out of memory");
            goto exception;
        }
        t1->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1u; /* tier 1 */
        t1->proto_id = 1; /* PROTO_OBJECT */
        t1->count = 0;
        PUSH(r8e_interp_from_pointer(t1));
        DISPATCH();
    }

    TARGET(OP_NEW_ARRAY) {
        uint16_t count = read_u16(&pc);
        R8EArrayInterp *arr = (R8EArrayInterp *)calloc(
            1, sizeof(R8EArrayInterp));
        if (!arr) {
            r8e_interp_throw_type_error(ctx, "out of memory");
            goto exception;
        }
        arr->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
        arr->proto_id = 2; /* PROTO_ARRAY */
        arr->length = count;
        arr->capacity = count > 0 ? count : 4;
        arr->elements = (R8EValue *)calloc(arr->capacity, sizeof(R8EValue));
        arr->named = NULL;
        if (!arr->elements && arr->capacity > 0) {
            free(arr);
            r8e_interp_throw_type_error(ctx, "out of memory");
            goto exception;
        }
        /* Pop elements from stack in reverse order */
        for (int i = (int)count - 1; i >= 0; i--) {
            arr->elements[i] = POP();
        }
        PUSH(r8e_interp_from_pointer(arr));
        DISPATCH();
    }

    TARGET(OP_NEW_FUNCTION) {
        uint16_t idx = read_u16(&pc);
        /* Function closures are stored after the regular constants in
         * the constants array. The original literal constant count is
         * stored in the function's capture_count field (repurposed for
         * the top-level synthetic function). This gives us the offset
         * where function closures begin. */
        uint16_t func_base = frame->func ? frame->func->capture_count : 0;
        uint16_t adjusted_idx = func_base + idx;
        R8EValue func_val;
        if (adjusted_idx < frame->const_count) {
            func_val = constants[adjusted_idx];
        } else if (idx < frame->const_count) {
            func_val = constants[idx];
        } else {
            func_val = R8E_UNDEFINED;
        }
        PUSH(func_val);
        DISPATCH();
    }

    TARGET(OP_NEW_REGEXP) {
        uint16_t idx = read_u16(&pc);
        PUSH(constants[idx]);
        DISPATCH();
    }

    TARGET(OP_INIT_PROP) {
        atom = read_u32(&pc);
        b = POP(); /* value */
        a = PEEK(); /* object (keep on stack) */
        r8e_interp_set_prop(ctx, a, atom, b);
        DISPATCH();
    }

    TARGET(OP_INIT_ELEM) {
        b = POP(); /* value */
        a = POP(); /* key */
        R8EValue obj = PEEK(); /* object (keep on stack) */
        r8e_interp_set_elem(ctx, obj, a, b);
        DISPATCH();
    }

    TARGET(OP_SPREAD) {
        /*
         * Spread: iterate the value on the stack and push each element.
         * For arrays, directly expand elements onto the stack.
         * For other iterables, would call Symbol.iterator (future).
         *
         * The spread opcode is used by NEW_ARRAY and CALL to expand
         * [...arr] or f(...arr) arguments.
         *
         * Convention: stack has [target_array, iterable]
         * We pop the iterable, iterate it, and push elements into the
         * target array on the stack.
         */
        a = POP(); /* iterable */
        if (R8E_IS_POINTER(a)) {
            R8EGCHeader *sh = (R8EGCHeader *)r8e_interp_get_pointer(a);
            if (sh && R8E_GC_GET_KIND(sh->flags) == R8E_GC_KIND_ARRAY) {
                /* Array: spread each element onto the stack */
                R8EArrayInterp *src = (R8EArrayInterp *)sh;
                for (uint32_t si = 0; si < src->length; si++) {
                    R8EValue elem = (si < src->capacity && src->elements)
                                    ? src->elements[si] : R8E_UNDEFINED;
                    PUSH(elem);
                }
            } else {
                /* Non-array object: push as single value for now */
                PUSH(a);
            }
        } else {
            /* Strings: spread each character */
            if (R8E_IS_INLINE_STR(a) || r8e_is_string_val(a)) {
                char sbuf[8];
                uint32_t slen;
                const char *sdata = r8e_interp_get_string(a, sbuf, &slen);
                for (uint32_t si = 0; si < slen; si++) {
                    R8EValue ch = 0xFFFD000000000000ULL;
                    ch |= ((uint64_t)1 << 45);
                    ch |= ((uint64_t)(uint8_t)sdata[si] << 38);
                    PUSH(ch);
                }
            } else {
                /* Not iterable: push as-is */
                PUSH(a);
            }
        }
        DISPATCH();
    }

    /* =================================================================
     * VARIABLE SCOPE OPERATIONS
     * ================================================================= */

    TARGET(OP_LOAD_CLOSURE) {
        uint8_t idx = read_u8(&pc);
        PUSH(r8e_interp_closure_get(closure, idx));
        DISPATCH();
    }

    TARGET(OP_STORE_CLOSURE) {
        uint8_t idx = read_u8(&pc);
        a = POP();
        r8e_interp_closure_set(closure, idx, a);
        DISPATCH();
    }

    TARGET(OP_LOAD_ARG) {
        uint8_t idx = read_u8(&pc);
        if (idx < frame->argc) {
            PUSH(frame->argv[idx]);
        } else {
            PUSH(R8E_UNDEFINED);
        }
        DISPATCH();
    }

    TARGET(OP_LOAD_REST_ARGS) {
        uint8_t start = read_u8(&pc);
        /* Create an array from arguments[start..] */
        int rest_count = frame->argc - (int)start;
        if (rest_count < 0) rest_count = 0;

        R8EArrayInterp *arr = (R8EArrayInterp *)calloc(
            1, sizeof(R8EArrayInterp));
        if (arr) {
            arr->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
            arr->proto_id = 2;
            arr->length = (uint32_t)rest_count;
            arr->capacity = (uint32_t)rest_count;
            if (rest_count > 0) {
                arr->elements = (R8EValue *)malloc(
                    rest_count * sizeof(R8EValue));
                if (arr->elements) {
                    for (int i = 0; i < rest_count; i++) {
                        arr->elements[i] = frame->argv[start + i];
                    }
                }
            }
            PUSH(r8e_interp_from_pointer(arr));
        } else {
            PUSH(R8E_UNDEFINED);
        }
        DISPATCH();
    }

    TARGET(OP_ENTER_SCOPE) {
        frame->scope_depth++;
        DISPATCH();
    }

    TARGET(OP_EXIT_SCOPE) {
        if (frame->scope_depth > 0) frame->scope_depth--;
        DISPATCH();
    }

    TARGET(OP_LOAD_THIS) {
        PUSH(frame->this_val);
        DISPATCH();
    }

    TARGET(OP_LOAD_NEW_TARGET) {
        PUSH(frame->new_target);
        DISPATCH();
    }

    TARGET(OP_DESTRUCTURE_ARRAY) {
        /*
         * Array destructuring: pop the source array/iterable from the stack.
         * Push 'count_d' elements extracted by index (0, 1, 2, ...).
         * If the source is shorter, push undefined for missing positions.
         * Elements are pushed in order so that STORE_LOCAL can bind them.
         */
        {
            uint8_t count_d = read_u8(&pc);
            a = POP(); /* source value */

            if (R8E_IS_POINTER(a)) {
                R8EGCHeader *dh = (R8EGCHeader *)r8e_interp_get_pointer(a);
                if (dh && R8E_GC_GET_KIND(dh->flags) == R8E_GC_KIND_ARRAY) {
                    R8EArrayInterp *darr = (R8EArrayInterp *)dh;
                    for (uint8_t di = 0; di < count_d; di++) {
                        if (di < darr->length && di < darr->capacity &&
                            darr->elements) {
                            PUSH(darr->elements[di]);
                        } else {
                            PUSH(R8E_UNDEFINED);
                        }
                    }
                } else {
                    /* Non-array object: try to use it as iterable (simplified) */
                    for (uint8_t di = 0; di < count_d; di++) {
                        R8EValue idx_val = r8e_interp_from_int32((int32_t)di);
                        result = r8e_interp_get_elem(ctx, a, idx_val);
                        if (ctx->has_exception) goto exception;
                        PUSH(result);
                    }
                }
            } else {
                /* Not an object: push undefined for each binding */
                for (uint8_t di = 0; di < count_d; di++) {
                    PUSH(R8E_UNDEFINED);
                }
            }
        }
        DISPATCH();
    }

    TARGET(OP_DESTRUCTURE_OBJECT) {
        /*
         * Object destructuring: pop the source object from the stack.
         * Read 'count_d' atom keys from the bytecode stream, then
         * push the value of each property from the source object.
         * If a property does not exist, push undefined.
         *
         * Bytecode format: OP_DESTRUCTURE_OBJECT count_d
         *   followed by count_d atom operands (each 4 bytes).
         */
        {
            uint8_t count_d = read_u8(&pc);
            a = POP(); /* source object */

            for (uint8_t di = 0; di < count_d; di++) {
                uint32_t prop_atom = read_u32(&pc);
                if (R8E_IS_POINTER(a)) {
                    result = r8e_interp_get_prop(ctx, a, prop_atom);
                    if (ctx->has_exception) goto exception;
                    PUSH(result);
                } else {
                    PUSH(R8E_UNDEFINED);
                }
            }
        }
        DISPATCH();
    }

    TARGET(OP_SET_DEFAULT) {
        off8 = read_i8(&pc);
        a = PEEK();
        if (R8E_IS_UNDEFINED(a)) {
            pc += off8;
        }
        DISPATCH();
    }

    /* =================================================================
     * ITERATOR / GENERATOR / ASYNC (stubs)
     * ================================================================= */

    TARGET(OP_GET_ITERATOR) {
        a = POP();
        /*
         * Get iterator from an iterable value.
         * For arrays: create a simple array iterator object.
         * For strings: create a string character iterator.
         * For objects with Symbol.iterator: call it (future).
         *
         * We create a minimal iterator object:
         *   { __iter_source: <source>, __iter_index: 0 }
         * stored as a Tier1 object with two properties.
         */
        {
            R8EObjTier1Interp *iter_obj = (R8EObjTier1Interp *)calloc(
                1, sizeof(R8EObjTier1Interp));
            if (!iter_obj) {
                r8e_interp_throw_type_error(ctx, "out of memory");
                goto exception;
            }
            iter_obj->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1;
            iter_obj->proto_id = 0;
            iter_obj->count = 2;
            /* Use high atom IDs unlikely to collide with user atoms */
            iter_obj->props[0].key = 0xFFFC00000FFF0001ULL; /* __iter_source */
            iter_obj->props[0].val = a;
            iter_obj->props[1].key = 0xFFFC00000FFF0002ULL; /* __iter_index */
            iter_obj->props[1].val = r8e_interp_from_int32(0);
            PUSH(r8e_interp_from_pointer(iter_obj));
        }
        DISPATCH();
    }

    TARGET(OP_ITERATOR_NEXT) {
        /*
         * Advance iterator: peek at the iterator object on the stack
         * (do NOT pop it - the for-of loop needs it).
         * Push: value, done (boolean).
         *
         * Read the __iter_source and __iter_index from the iterator object.
         * If source is an array, return elements[index] and increment.
         * If source is a string, return chars[index].
         */
        {
            R8EValue iter = PEEK(); /* iterator object stays on stack */
            R8EValue iter_val = R8E_UNDEFINED;
            bool iter_done = true;

            if (R8E_IS_POINTER(iter)) {
                R8EObjTier1Interp *iobj = (R8EObjTier1Interp *)
                    r8e_interp_get_pointer(iter);
                if (iobj && iobj->count >= 2) {
                    R8EValue source = iobj->props[0].val;
                    int32_t idx = r8e_interp_get_int32(iobj->props[1].val);

                    if (R8E_IS_POINTER(source)) {
                        R8EGCHeader *srch = (R8EGCHeader *)
                            r8e_interp_get_pointer(source);
                        if (srch && R8E_GC_GET_KIND(srch->flags) ==
                                    R8E_GC_KIND_ARRAY) {
                            R8EArrayInterp *sarr = (R8EArrayInterp *)srch;
                            if ((uint32_t)idx < sarr->length) {
                                iter_val = (sarr->elements &&
                                           (uint32_t)idx < sarr->capacity)
                                    ? sarr->elements[idx] : R8E_UNDEFINED;
                                iter_done = false;
                                iobj->props[1].val =
                                    r8e_interp_from_int32(idx + 1);
                            }
                        } else if (srch && R8E_GC_GET_KIND(srch->flags) ==
                                           R8E_GC_KIND_STRING) {
                            R8EStringInterp *ss = (R8EStringInterp *)srch;
                            if ((uint32_t)idx < ss->char_length) {
                                const char *sd = r8e_interp_string_data(ss);
                                R8EValue ch = 0xFFFD000000000000ULL;
                                ch |= ((uint64_t)1 << 45);
                                ch |= ((uint64_t)(uint8_t)sd[idx] << 38);
                                iter_val = ch;
                                iter_done = false;
                                iobj->props[1].val =
                                    r8e_interp_from_int32(idx + 1);
                            }
                        }
                    } else if (R8E_IS_INLINE_STR(source) ||
                               r8e_is_string_val(source)) {
                        char sbuf[8];
                        uint32_t slen;
                        const char *sd = r8e_interp_get_string(
                            source, sbuf, &slen);
                        if ((uint32_t)idx < slen) {
                            R8EValue ch = 0xFFFD000000000000ULL;
                            ch |= ((uint64_t)1 << 45);
                            ch |= ((uint64_t)(uint8_t)sd[idx] << 38);
                            iter_val = ch;
                            iter_done = false;
                            iobj->props[1].val =
                                r8e_interp_from_int32(idx + 1);
                        }
                    }
                }
            }

            PUSH(iter_val);
            PUSH(iter_done ? R8E_TRUE : R8E_FALSE);
        }
        DISPATCH();
    }

    TARGET(OP_ITERATOR_CLOSE) {
        a = POP(); /* iterator object - just discard it */
        (void)a;
        DISPATCH();
    }

    TARGET(OP_YIELD) {
        /*
         * Generator yield: save the current value and suspend execution.
         *
         * In a full coroutine implementation, this would save the entire
         * frame state and return control to the caller. For now, we
         * implement a simplified version: yield returns the value to
         * the caller by storing it in the frame and returning.
         *
         * The value to yield is on the stack.
         */
        a = POP(); /* yield value */
        /* Save frame state for resumption */
        frame->pc = pc;
        frame->sp = sp;
        /* Return the yielded value - the generator wrapper handles
         * creating the {value, done} result object */
        return a;
    }

    TARGET(OP_YIELD_STAR) {
        /*
         * yield* delegate: iterate the operand and yield each value.
         * Simplified: if operand is an array, yield each element.
         * A full implementation would forward .next()/.throw()/.return().
         */
        a = POP(); /* iterable to delegate to */
        if (R8E_IS_POINTER(a)) {
            R8EGCHeader *yh = (R8EGCHeader *)r8e_interp_get_pointer(a);
            if (yh && R8E_GC_GET_KIND(yh->flags) == R8E_GC_KIND_ARRAY) {
                R8EArrayInterp *yarr = (R8EArrayInterp *)yh;
                /* For simplicity, yield the last element and continue.
                 * A real implementation needs coroutine state. */
                if (yarr->length > 0 && yarr->elements) {
                    PUSH(yarr->elements[yarr->length - 1]);
                } else {
                    PUSH(R8E_UNDEFINED);
                }
            } else {
                PUSH(a);
            }
        } else {
            PUSH(a);
        }
        DISPATCH();
    }

    TARGET(OP_AWAIT) {
        /*
         * Await: suspend async function until promise resolves.
         *
         * Phase B implementation: if the value is an already-resolved
         * Promise, extract its result. If it's a non-promise value,
         * pass through. Pending promises would require coroutine
         * suspension (Phase C, not yet implemented).
         */
        a = POP(); /* value or promise */
        if (R8E_IS_POINTER(a)) {
            R8EGCHeader *ah = (R8EGCHeader *)r8e_interp_get_pointer(a);
            if (ah && R8E_GC_GET_KIND(ah->flags) == R8E_GC_KIND_PROMISE) {
                R8EPromiseInterp *pr = (R8EPromiseInterp *)ah;
                if (pr->state == R8E_PROMISE_FULFILLED) {
                    PUSH(pr->result);
                } else if (pr->state == R8E_PROMISE_REJECTED) {
                    /* Re-throw the rejection reason */
                    r8e_interp_throw(ctx, pr->result);
                    goto exception;
                } else {
                    /* Pending promise: cannot suspend, pass through */
                    PUSH(a);
                }
                DISPATCH();
            }
        }
        /* Non-promise value: pass through (await 42 === 42) */
        PUSH(a);
        DISPATCH();
    }

    TARGET(OP_ASYNC_FUNC_START) { DISPATCH(); }
    TARGET(OP_ASYNC_FUNC_RESOLVE) { DISPATCH(); }
    TARGET(OP_ASYNC_FUNC_REJECT) { DISPATCH(); }

    /* =================================================================
     * CLASS (stubs)
     * ================================================================= */

    TARGET(OP_CLASS_CREATE) {
        atom = read_u32(&pc);
        /*
         * Create a class: pop the superclass (or null) from stack.
         * Create a constructor function object and a prototype object.
         * The constructor is a Tier1 object with a 'prototype' property.
         * The prototype has a 'constructor' back-reference.
         *
         * Stack before: [superclass_or_null]
         * Stack after:  [constructor_function]
         */
        {
            R8EValue super_val = POP(); /* superclass or null */

            /* Create the prototype object (Tier1, initially empty) */
            R8EObjTier1Interp *proto = (R8EObjTier1Interp *)calloc(
                1, sizeof(R8EObjTier1Interp));
            if (!proto) {
                r8e_interp_throw_type_error(ctx, "out of memory");
                goto exception;
            }
            proto->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1;
            proto->proto_id = 1; /* PROTO_OBJECT */
            proto->count = 0;

            /* Create the constructor object (Tier1) */
            R8EObjTier1Interp *ctor = (R8EObjTier1Interp *)calloc(
                1, sizeof(R8EObjTier1Interp));
            if (!ctor) {
                free(proto);
                r8e_interp_throw_type_error(ctx, "out of memory");
                goto exception;
            }
            ctor->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1;
            ctor->proto_id = 6; /* PROTO_FUNCTION */
            ctor->count = 1;
            /* Set constructor.prototype = proto */
            ctor->props[0].key = 0xFFFC000000000000ULL | 2ULL; /* atom 2 = prototype */
            ctor->props[0].val = r8e_interp_from_pointer(proto);

            /* Set proto.constructor = ctor */
            proto->count = 1;
            proto->props[0].key = 0xFFFC000000000000ULL | 3ULL; /* atom 3 = constructor */
            proto->props[0].val = r8e_interp_from_pointer(ctor);

            /* If there is a superclass, set up inheritance */
            if (!R8E_IS_NULL(super_val) && R8E_IS_POINTER(super_val)) {
                /* Get superclass.prototype */
                R8EValue super_proto = r8e_interp_get_prop(ctx, super_val,
                    2 /* prototype */);
                if (!ctx->has_exception && R8E_IS_POINTER(super_proto)) {
                    /* Set proto's proto_id based on the super prototype */
                    R8EGCHeader *sp_h = (R8EGCHeader *)
                        r8e_interp_get_pointer(super_proto);
                    if (sp_h) {
                        proto->proto_id = sp_h->proto_id;
                    }
                }
            }

            PUSH(r8e_interp_from_pointer(ctor));
        }
        (void)atom;
        DISPATCH();
    }

    TARGET(OP_CLASS_EXTENDS) {
        b = POP(); /* superclass */
        a = POP(); /* class being defined (constructor) */
        /* Set up prototype chain: class.prototype.__proto__ = super.prototype */
        if (R8E_IS_POINTER(a) && R8E_IS_POINTER(b)) {
            R8EValue class_proto = r8e_interp_get_prop(ctx, a,
                2 /* prototype */);
            R8EValue super_proto = r8e_interp_get_prop(ctx, b,
                2 /* prototype */);
            if (R8E_IS_POINTER(class_proto) && R8E_IS_POINTER(super_proto)) {
                R8EGCHeader *cp_h = (R8EGCHeader *)
                    r8e_interp_get_pointer(class_proto);
                R8EGCHeader *sp_h = (R8EGCHeader *)
                    r8e_interp_get_pointer(super_proto);
                if (cp_h && sp_h) {
                    cp_h->proto_id = sp_h->proto_id;
                }
            }
        }
        PUSH(a);
        DISPATCH();
    }

    TARGET(OP_CLASS_METHOD) {
        atom = read_u32(&pc);
        uint8_t mflags = read_u8(&pc);
        a = POP(); /* method function */
        b = POP(); /* class constructor (DUP'd) */
        /*
         * Install method on the class prototype.
         * mflags: bit 0 = getter, bit 1 = setter
         * Get constructor.prototype, then set the method on it.
         */
        {
            R8EValue cls_proto = r8e_interp_get_prop(ctx, b,
                2 /* prototype */);
            if (R8E_IS_POINTER(cls_proto)) {
                r8e_interp_set_prop(ctx, cls_proto, atom, a);
            }
        }
        (void)mflags;
        DISPATCH();
    }

    TARGET(OP_CLASS_FIELD) {
        atom = read_u32(&pc);
        a = POP(); /* field initializer value */
        /* Fields are set on 'this' during construction.
         * For now, this is handled by the constructor body. */
        (void)atom; (void)a;
        DISPATCH();
    }

    TARGET(OP_CLASS_STATIC) {
        atom = read_u32(&pc);
        uint8_t sflags = read_u8(&pc);
        a = POP(); /* static method/property value */
        b = POP(); /* class constructor (DUP'd) */
        /*
         * Install static method/property directly on the constructor.
         */
        if (R8E_IS_POINTER(b)) {
            r8e_interp_set_prop(ctx, b, atom, a);
        }
        (void)sflags;
        DISPATCH();
    }

    TARGET(OP_CLASS_PRIVATE) {
        atom = read_u32(&pc);
        a = POP();
        (void)atom; (void)a;
        DISPATCH();
    }

    /* =================================================================
     * MODULE (stubs)
     * ================================================================= */

    TARGET(OP_IMPORT) {
        uint16_t idx = read_u16(&pc);
        PUSH(R8E_UNDEFINED);
        (void)idx;
        DISPATCH();
    }

    TARGET(OP_EXPORT) {
        atom = read_u32(&pc);
        a = POP();
        (void)atom; (void)a;
        DISPATCH();
    }

    TARGET(OP_IMPORT_DYNAMIC) {
        a = POP();
        PUSH(R8E_UNDEFINED);
        (void)a;
        DISPATCH();
    }

    TARGET(OP_IMPORT_META) {
        PUSH(R8E_UNDEFINED);
        DISPATCH();
    }

    /* =================================================================
     * OPTIONAL CHAINING / NULLISH COALESCING
     * ================================================================= */

    TARGET(OP_OPTIONAL_CHAIN) {
        off32 = read_i32(&pc);
        a = PEEK();
        if (R8E_IS_NULLISH(a)) {
            DROP();
            PUSH(R8E_UNDEFINED);
            pc += off32;
        }
        DISPATCH();
    }

    TARGET(OP_NULLISH_COALESCE) {
        off32 = read_i32(&pc);
        a = PEEK();
        if (!R8E_IS_NULLISH(a)) {
            pc += off32; /* keep value, skip to end */
        } else {
            DROP(); /* discard nullish value, evaluate RHS */
        }
        DISPATCH();
    }

    /* =================================================================
     * MISCELLANEOUS
     * ================================================================= */

    TARGET(OP_VOID) {
        DROP();
        PUSH(R8E_UNDEFINED);
        DISPATCH();
    }

    TARGET(OP_DEBUGGER) {
        /* No-op in release builds */
        DISPATCH();
    }

    TARGET(OP_WITH_ENTER) {
        a = POP();
        (void)a;
        DISPATCH();
    }

    TARGET(OP_WITH_EXIT) {
        DISPATCH();
    }

    TARGET(OP_FOR_IN_INIT) {
        a = POP();
        /*
         * Enumerate object properties for for-in.
         * Create an array of property keys from the object, then
         * create an iterator over that array.
         *
         * For null/undefined, create an empty iterator.
         */
        {
            if (R8E_IS_NULL(a) || R8E_IS_UNDEFINED(a)) {
                /* Empty iterator */
                R8EObjTier1Interp *eiter = (R8EObjTier1Interp *)calloc(
                    1, sizeof(R8EObjTier1Interp));
                if (eiter) {
                    eiter->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1;
                    eiter->count = 2;
                    eiter->props[0].key = 0xFFFC00000FFF0001ULL;
                    eiter->props[0].val = R8E_NULL; /* empty source */
                    eiter->props[1].key = 0xFFFC00000FFF0002ULL;
                    eiter->props[1].val = r8e_interp_from_int32(0);
                }
                PUSH(eiter ? r8e_interp_from_pointer(eiter) : R8E_UNDEFINED);
            } else if (R8E_IS_POINTER(a)) {
                R8EGCHeader *fh = (R8EGCHeader *)r8e_interp_get_pointer(a);
                uint32_t fkind = fh ? R8E_GC_GET_KIND(fh->flags) : 99;

                /* Collect property keys into an array */
                R8EArrayInterp *keys = (R8EArrayInterp *)calloc(
                    1, sizeof(R8EArrayInterp));
                if (!keys) {
                    r8e_interp_throw_type_error(ctx, "out of memory");
                    goto exception;
                }
                keys->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
                keys->proto_id = 2;
                keys->length = 0;
                keys->capacity = 8;
                keys->elements = (R8EValue *)calloc(8, sizeof(R8EValue));

                if (fkind == R8E_GC_KIND_OBJECT) {
                    uint8_t tier = fh->flags & 0x03;
                    if (tier == 0) {
                        R8EObjTier0Interp *t0 = (R8EObjTier0Interp *)fh;
                        if (t0->key0 != 0) {
                            keys->elements[0] = t0->key0;
                            keys->length = 1;
                        }
                    } else if (tier == 1) {
                        R8EObjTier1Interp *t1 = (R8EObjTier1Interp *)fh;
                        for (uint8_t ki = 0; ki < t1->count && ki < 4; ki++) {
                            keys->elements[ki] = t1->props[ki].key;
                        }
                        keys->length = t1->count;
                    } else if (tier == 2) {
                        R8EObjTier2Interp *t2 = (R8EObjTier2Interp *)fh;
                        if (t2->props) {
                            uint8_t kc = t2->count < 8 ? t2->count : 8;
                            for (uint8_t ki = 0; ki < kc; ki++) {
                                keys->elements[ki] = t2->props[ki].key;
                            }
                            keys->length = kc;
                        }
                    }
                } else if (fkind == R8E_GC_KIND_ARRAY) {
                    R8EArrayInterp *farr = (R8EArrayInterp *)fh;
                    /* For arrays, enumerate indices as strings */
                    uint32_t alen = farr->length;
                    if (alen > 8) {
                        free(keys->elements);
                        keys->capacity = alen;
                        keys->elements = (R8EValue *)calloc(
                            alen, sizeof(R8EValue));
                    }
                    for (uint32_t ki = 0; ki < alen; ki++) {
                        /* Store index as int for now;
                         * for-in will convert to string */
                        keys->elements[ki] = r8e_interp_from_int32((int32_t)ki);
                    }
                    keys->length = alen;
                }

                /* Create iterator over the keys array */
                R8EObjTier1Interp *fiter = (R8EObjTier1Interp *)calloc(
                    1, sizeof(R8EObjTier1Interp));
                if (fiter) {
                    fiter->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1;
                    fiter->count = 2;
                    fiter->props[0].key = 0xFFFC00000FFF0001ULL;
                    fiter->props[0].val = r8e_interp_from_pointer(keys);
                    fiter->props[1].key = 0xFFFC00000FFF0002ULL;
                    fiter->props[1].val = r8e_interp_from_int32(0);
                }
                PUSH(fiter ? r8e_interp_from_pointer(fiter) : R8E_UNDEFINED);
            } else {
                PUSH(R8E_UNDEFINED);
            }
        }
        DISPATCH();
    }

    TARGET(OP_FOR_IN_NEXT) {
        off32 = read_i32(&pc);
        /*
         * Get next property key from the for-in iterator.
         * The iterator object is on the stack (peek it).
         * Push the key string. If done, jump to the offset.
         */
        {
            R8EValue fi_iter = PEEK(); /* iterator stays on stack */
            R8EValue fi_key = R8E_UNDEFINED;
            bool fi_done = true;

            if (R8E_IS_POINTER(fi_iter)) {
                R8EObjTier1Interp *fi_obj = (R8EObjTier1Interp *)
                    r8e_interp_get_pointer(fi_iter);
                if (fi_obj && fi_obj->count >= 2) {
                    R8EValue fi_source = fi_obj->props[0].val;
                    int32_t fi_idx = r8e_interp_get_int32(fi_obj->props[1].val);

                    if (R8E_IS_POINTER(fi_source)) {
                        R8EGCHeader *fsh = (R8EGCHeader *)
                            r8e_interp_get_pointer(fi_source);
                        if (fsh && R8E_GC_GET_KIND(fsh->flags) ==
                                   R8E_GC_KIND_ARRAY) {
                            R8EArrayInterp *fi_arr = (R8EArrayInterp *)fsh;
                            if ((uint32_t)fi_idx < fi_arr->length) {
                                fi_key = fi_arr->elements[fi_idx];
                                /* Convert atom keys to strings for for-in */
                                if (R8E_IS_ATOM(fi_key)) {
                                    fi_key = r8e_to_string(NULL, fi_key);
                                } else if (R8E_IS_INT32(fi_key)) {
                                    fi_key = r8e_to_string(NULL, fi_key);
                                }
                                fi_done = false;
                                fi_obj->props[1].val =
                                    r8e_interp_from_int32(fi_idx + 1);
                            }
                        }
                    }
                }
            }

            PUSH(fi_key);
            if (fi_done) {
                pc += off32; /* jump to loop end */
            }
        }
        DISPATCH();
    }

    TARGET(OP_FOR_OF_INIT) {
        a = POP();
        /*
         * Initialize for-of iterator. Same as GET_ITERATOR but
         * used specifically in for-of context.
         *
         * props[0].val = source object
         * props[1].val = index (int32) for Array/String,
         *                entry pointer for Map/Set
         * props[1].key = tag: 0xFFFC00000FFF0002 for Array/String,
         *                     0xFFFC00000FFF0003 for Map,
         *                     0xFFFC00000FFF0004 for Set
         */
        {
            R8EObjTier1Interp *fo_iter = (R8EObjTier1Interp *)calloc(
                1, sizeof(R8EObjTier1Interp));
            if (!fo_iter) {
                r8e_interp_throw_type_error(ctx, "out of memory");
                goto exception;
            }
            fo_iter->flags = (R8E_GC_KIND_OBJECT << R8E_GC_KIND_SHIFT) | 1;
            fo_iter->count = 2;
            fo_iter->props[0].key = 0xFFFC00000FFF0001ULL;
            fo_iter->props[0].val = a;

            /* Detect Map/Set and store first entry pointer.
             * Note: Map/Set GC kinds use bottom nibble (r8e_weakref.c convention),
             * not the R8E_GC_GET_KIND shifted field. */
            if (R8E_IS_POINTER(a)) {
                R8EGCHeader *init_h = (R8EGCHeader *)r8e_interp_get_pointer(a);
                uint32_t init_kind_lo = init_h ? (init_h->flags & 0x0Fu) : 0;
                if (init_kind_lo == R8E_GC_KIND_MAP_INTERP) {
                    R8EMapInterp *init_map = (R8EMapInterp *)init_h;
                    fo_iter->props[1].key = FO_TAG_MAP; /* Map: entries */
                    fo_iter->props[1].val = init_map->order_first
                        ? r8e_interp_from_pointer(init_map->order_first)
                        : R8E_UNDEFINED;
                } else if (init_kind_lo == R8E_GC_KIND_SET_INTERP) {
                    R8ESetInterp *init_set = (R8ESetInterp *)init_h;
                    fo_iter->props[1].key = FO_TAG_SET; /* Set: values */
                    fo_iter->props[1].val = init_set->order_first
                        ? r8e_interp_from_pointer(init_set->order_first)
                        : R8E_UNDEFINED;
                } else if (init_kind_lo == R8E_GC_KIND_MAP_ITER_INTERP) {
                    /* Map iterator from .keys()/.values()/.entries() */
                    R8EMapIteratorInterp *mi = (R8EMapIteratorInterp *)init_h;
                    uint64_t tag;
                    switch (mi->mode) {
                    case 0: tag = FO_TAG_MAP_KEYS; break;
                    case 1: tag = FO_TAG_MAP_VALUES; break;
                    default: tag = FO_TAG_MAP_ENTRIES; break;
                    }
                    fo_iter->props[1].key = tag;
                    fo_iter->props[1].val = mi->current
                        ? r8e_interp_from_pointer(mi->current)
                        : R8E_UNDEFINED;
                } else if (init_kind_lo == R8E_GC_KIND_SET_ITER_INTERP) {
                    /* Set iterator from .values()/.entries()/.keys() */
                    R8ESetIteratorInterp *si = (R8ESetIteratorInterp *)init_h;
                    uint64_t tag = (si->mode == 1) ? FO_TAG_SET_ENTRIES : FO_TAG_SET_VALUES;
                    fo_iter->props[1].key = tag;
                    fo_iter->props[1].val = si->current
                        ? r8e_interp_from_pointer(si->current)
                        : R8E_UNDEFINED;
                } else {
                    fo_iter->props[1].key = FO_TAG_ARRAY_STRING;
                    fo_iter->props[1].val = r8e_interp_from_int32(0);
                }
            } else {
                fo_iter->props[1].key = FO_TAG_ARRAY_STRING;
                fo_iter->props[1].val = r8e_interp_from_int32(0);
            }
            PUSH(r8e_interp_from_pointer(fo_iter));
        }
        DISPATCH();
    }

    TARGET(OP_FOR_OF_NEXT) {
        off32 = read_i32(&pc);
        /*
         * Get next value from the for-of iterator.
         * Reuses the same iterator protocol as OP_ITERATOR_NEXT.
         */
        {
            R8EValue fo_it = PEEK(); /* iterator stays on stack */
            R8EValue fo_val = R8E_UNDEFINED;
            bool fo_done = true;

            if (R8E_IS_POINTER(fo_it)) {
                R8EObjTier1Interp *fo_obj = (R8EObjTier1Interp *)
                    r8e_interp_get_pointer(fo_it);
                if (fo_obj && fo_obj->count >= 2) {
                    R8EValue fo_source = fo_obj->props[0].val;
                    R8EValue fo_tag = fo_obj->props[1].key;

                    /* Map iteration: produce [key, value] pairs */
                    if (fo_tag == FO_TAG_MAP || fo_tag == FO_TAG_MAP_ENTRIES) {
                        R8EValue fo_cur = fo_obj->props[1].val;
                        if (R8E_IS_POINTER(fo_cur)) {
                            R8EMapEntryInterp *me = (R8EMapEntryInterp *)
                                r8e_interp_get_pointer(fo_cur);
                            if (me) {
                                /* Create [key, value] pair array */
                                R8EArrayInterp *pair = (R8EArrayInterp *)
                                    calloc(1, sizeof(R8EArrayInterp));
                                if (pair) {
                                    pair->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
                                    pair->proto_id = 2; /* R8E_PROTO_ARRAY */
                                    pair->length = 2;
                                    pair->capacity = 2;
                                    pair->elements = (R8EValue *)calloc(2, sizeof(R8EValue));
                                    if (pair->elements) {
                                        pair->elements[0] = me->key;
                                        pair->elements[1] = me->value;
                                    }
                                    fo_val = r8e_interp_from_pointer(pair);
                                }
                                fo_done = false;
                                fo_obj->props[1].val = me->order_next
                                    ? r8e_interp_from_pointer(me->order_next)
                                    : R8E_UNDEFINED;
                            }
                        }
                    }
                    /* Map keys iteration: produce keys only */
                    else if (fo_tag == FO_TAG_MAP_KEYS) {
                        R8EValue fo_cur = fo_obj->props[1].val;
                        if (R8E_IS_POINTER(fo_cur)) {
                            R8EMapEntryInterp *me = (R8EMapEntryInterp *)
                                r8e_interp_get_pointer(fo_cur);
                            if (me) {
                                fo_val = me->key;
                                fo_done = false;
                                fo_obj->props[1].val = me->order_next
                                    ? r8e_interp_from_pointer(me->order_next)
                                    : R8E_UNDEFINED;
                            }
                        }
                    }
                    /* Map values iteration: produce values only */
                    else if (fo_tag == FO_TAG_MAP_VALUES) {
                        R8EValue fo_cur = fo_obj->props[1].val;
                        if (R8E_IS_POINTER(fo_cur)) {
                            R8EMapEntryInterp *me = (R8EMapEntryInterp *)
                                r8e_interp_get_pointer(fo_cur);
                            if (me) {
                                fo_val = me->value;
                                fo_done = false;
                                fo_obj->props[1].val = me->order_next
                                    ? r8e_interp_from_pointer(me->order_next)
                                    : R8E_UNDEFINED;
                            }
                        }
                    }
                    /* Set iteration: produce values */
                    else if (fo_tag == FO_TAG_SET || fo_tag == FO_TAG_SET_VALUES) {
                        R8EValue fo_cur = fo_obj->props[1].val;
                        if (R8E_IS_POINTER(fo_cur)) {
                            R8ESetEntryInterp *se = (R8ESetEntryInterp *)
                                r8e_interp_get_pointer(fo_cur);
                            if (se) {
                                fo_val = se->key;
                                fo_done = false;
                                fo_obj->props[1].val = se->order_next
                                    ? r8e_interp_from_pointer(se->order_next)
                                    : R8E_UNDEFINED;
                            }
                        }
                    }
                    /* Set entries iteration: produce [value, value] pairs */
                    else if (fo_tag == FO_TAG_SET_ENTRIES) {
                        R8EValue fo_cur = fo_obj->props[1].val;
                        if (R8E_IS_POINTER(fo_cur)) {
                            R8ESetEntryInterp *se = (R8ESetEntryInterp *)
                                r8e_interp_get_pointer(fo_cur);
                            if (se) {
                                R8EArrayInterp *pair = (R8EArrayInterp *)
                                    calloc(1, sizeof(R8EArrayInterp));
                                if (pair) {
                                    pair->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
                                    pair->proto_id = 2;
                                    pair->length = 2;
                                    pair->capacity = 2;
                                    pair->elements = (R8EValue *)calloc(2, sizeof(R8EValue));
                                    if (pair->elements) {
                                        pair->elements[0] = se->key;
                                        pair->elements[1] = se->key;
                                    }
                                    fo_val = r8e_interp_from_pointer(pair);
                                }
                                fo_done = false;
                                fo_obj->props[1].val = se->order_next
                                    ? r8e_interp_from_pointer(se->order_next)
                                    : R8E_UNDEFINED;
                            }
                        }
                    }
                    /* Array/String iteration (original code) */
                    else {
                    int32_t fo_idx = r8e_interp_get_int32(fo_obj->props[1].val);

                    if (R8E_IS_POINTER(fo_source)) {
                        R8EGCHeader *foh = (R8EGCHeader *)
                            r8e_interp_get_pointer(fo_source);
                        if (foh && R8E_GC_GET_KIND(foh->flags) ==
                                   R8E_GC_KIND_ARRAY) {
                            R8EArrayInterp *fo_arr = (R8EArrayInterp *)foh;
                            if ((uint32_t)fo_idx < fo_arr->length) {
                                fo_val = (fo_arr->elements &&
                                         (uint32_t)fo_idx < fo_arr->capacity)
                                    ? fo_arr->elements[fo_idx] : R8E_UNDEFINED;
                                fo_done = false;
                                fo_obj->props[1].val =
                                    r8e_interp_from_int32(fo_idx + 1);
                            }
                        } else if (foh && R8E_GC_GET_KIND(foh->flags) ==
                                          R8E_GC_KIND_STRING) {
                            R8EStringInterp *fos = (R8EStringInterp *)foh;
                            if ((uint32_t)fo_idx < fos->char_length) {
                                const char *fod = r8e_interp_string_data(fos);
                                R8EValue fch = 0xFFFD000000000000ULL;
                                fch |= ((uint64_t)1 << 45);
                                fch |= ((uint64_t)(uint8_t)fod[fo_idx] << 38);
                                fo_val = fch;
                                fo_done = false;
                                fo_obj->props[1].val =
                                    r8e_interp_from_int32(fo_idx + 1);
                            }
                        }
                    } else if (R8E_IS_INLINE_STR(fo_source)) {
                        char fsbuf[8];
                        uint32_t fslen;
                        const char *fsd = r8e_interp_get_string(
                            fo_source, fsbuf, &fslen);
                        if ((uint32_t)fo_idx < fslen) {
                            R8EValue fch = 0xFFFD000000000000ULL;
                            fch |= ((uint64_t)1 << 45);
                            fch |= ((uint64_t)(uint8_t)fsd[fo_idx] << 38);
                            fo_val = fch;
                            fo_done = false;
                            fo_obj->props[1].val =
                                r8e_interp_from_int32(fo_idx + 1);
                        }
                    }
                    } /* end else (Array/String iteration) */
                }
            }

            PUSH(fo_val);
            if (fo_done) {
                pc += off32; /* jump to loop end */
            }
        }
        DISPATCH();
    }

    TARGET(OP_TEMPLATE_HEAD) {
        uint16_t idx = read_u16(&pc);
        PUSH(constants[idx]);
        DISPATCH();
    }

    TARGET(OP_TEMPLATE_MIDDLE) {
        uint16_t idx = read_u16(&pc);
        b = constants[idx];
        a = POP();
        /* Stringify the expression and concat */
        R8EValue sa = r8e_to_string(NULL, a);
        result = r8e_string_concat(ctx, sa, b);
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_TEMPLATE_TAIL) {
        uint16_t idx = read_u16(&pc);
        b = constants[idx];
        a = POP();
        R8EValue sa = r8e_to_string(NULL, a);
        result = r8e_string_concat(ctx, sa, b);
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_TAGGED_TEMPLATE) {
        argc_op = read_u8(&pc);
        /*
         * Tagged template: tag`str0${expr0}str1${expr1}str2`
         *
         * Stack layout (from bottom to top), interleaved:
         *   tag_func, str0, expr0, str1, expr1, ..., strN
         *
         * argc_op = number of expressions (interpolated values).
         * string_count = argc_op + 1.
         * Total stack items = 1 (tag) + string_count + argc_op
         *                   = 1 + 2*argc_op + 1 = 2 + 2*argc_op.
         *
         * We build a strings array at runtime, then call:
         *   tag(strings_array, expr0, expr1, ...)
         */
        {
            int tt_str_count = (int)argc_op + 1;
            int tt_total_items = tt_str_count + (int)argc_op; /* strings + exprs */
            R8EValue *tt_base = sp - tt_total_items; /* points to str0 */
            R8EValue tt_tag = tt_base[-1]; /* tag function below the items */

            /* Build the strings array */
            R8EArrayInterp *tt_arr = (R8EArrayInterp *)calloc(
                1, sizeof(R8EArrayInterp));
            if (!tt_arr) {
                sp -= tt_total_items + 1;
                PUSH(R8E_UNDEFINED);
                DISPATCH();
            }
            tt_arr->flags = (R8E_GC_KIND_ARRAY << R8E_GC_KIND_SHIFT);
            tt_arr->proto_id = 2; /* PROTO_ARRAY */
            tt_arr->length = tt_str_count;
            tt_arr->capacity = tt_str_count;
            tt_arr->elements = (R8EValue *)calloc(tt_str_count, sizeof(R8EValue));
            tt_arr->named = NULL;
            if (!tt_arr->elements) {
                free(tt_arr);
                sp -= tt_total_items + 1;
                PUSH(R8E_UNDEFINED);
                DISPATCH();
            }

            /* Extract strings (at even positions: 0, 2, 4, ...) */
            for (int ti = 0; ti < tt_str_count; ti++) {
                tt_arr->elements[ti] = tt_base[ti * 2];
            }
            R8EValue tt_strings_val = r8e_interp_from_pointer(tt_arr);

            /* Build argument list: [strings_array, expr0, expr1, ...] */
            int tt_total_argc = 1 + (int)argc_op;
            R8EValue *tt_args = (R8EValue *)malloc(
                tt_total_argc * sizeof(R8EValue));
            if (!tt_args) {
                sp -= tt_total_items + 1;
                PUSH(R8E_UNDEFINED);
                DISPATCH();
            }
            tt_args[0] = tt_strings_val;
            /* Extract expressions (at odd positions: 1, 3, 5, ...) */
            for (int ti = 0; ti < (int)argc_op; ti++) {
                tt_args[1 + ti] = tt_base[ti * 2 + 1];
            }

            /* Pop everything: all items + tag */
            sp -= tt_total_items + 1;

            /* Call the tag function */
            frame->pc = pc;
            frame->sp = sp;

            result = r8e_interp_call_internal(ctx, tt_tag, R8E_UNDEFINED,
                                               tt_args, tt_total_argc, false);
            free(tt_args);

            pc = frame->pc;
            sp = frame->sp;
            locals = frame->locals;
            constants = frame->constants;
            closure = frame->closure;

            if (ctx->has_exception) goto exception;
            PUSH(result);
        }
        DISPATCH();
    }

    /* =================================================================
     * CHAIN SUPERINSTRUCTIONS (Section 7.2)
     *
     * Each fuses 3-5 instructions into one dispatch, keeping
     * intermediates in CPU registers. Zero RC operations on fast paths.
     * ================================================================= */

    TARGET(OP_CALL_METHOD_FAST) {
        /*
         * Fused: get_var + get_field2 + get_loc_check + call_method
         * Operands: atom (4 bytes) + argc (1 byte)
         * Covers 17.8% of all dispatches.
         */
        atom = read_u32(&pc);
        argc_op = read_u8(&pc);

        /* Pop the object from stack */
        R8EValue obj = POP();

        /* Get the method from the object */
        R8EValue method = r8e_interp_get_prop(ctx, obj, atom);
        if (ctx->has_exception) goto exception;

        /* Pop arguments */
        sp -= argc_op;
        const R8EValue *margs = sp;

        /* Call the method with obj as this */
        frame->pc = pc;
        frame->sp = sp;

        result = r8e_interp_call_internal(ctx, method, obj,
                                           margs, argc_op, false);
        pc = frame->pc;
        sp = frame->sp;
        locals = frame->locals;
        constants = frame->constants;
        closure = frame->closure;

        if (ctx->has_exception) goto exception;
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_FOR_INC_LOOP) {
        /*
         * Fused: get_loc(reg) + post_inc + put_loc(reg) + drop + goto8(off)
         * Operands: reg (1 byte) + off8 (1 byte)
         * Covers 18.0% of dispatches. Zero RC on fast path.
         *
         * This is the i++ at the end of a for loop.
         * Fast path: int32 that does not overflow -> 3-4 machine instructions.
         */
        reg = read_u8(&pc);
        off8 = read_i8(&pc);

        a = locals[reg];

        /* Int32 fast path - vast majority of loop counters */
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia < INT32_MAX) {
                locals[reg] = r8e_interp_from_int32(ia + 1);
                /* ZERO RC operations - value never leaves registers */
                pc += off8 - 2;
                DISPATCH();
            }
        }
        /* Slow path: double, overflow, non-numeric */
        locals[reg] = r8e_number_from_double(r8e_to_double_fast(a) + 1.0);
        pc += off8 - 2;
        DISPATCH();
    }

    TARGET(OP_LOOP_CMP_BRANCH) {
        /*
         * Fused: get_loc(reg) + push_i32(imm) + lt + if_false8(off)
         * Operands: reg (1 byte) + imm32 (4 bytes) + off8 (1 byte)
         * Covers ~7% of dispatches.
         *
         * This is the i < n condition at the top of a for loop.
         */
        reg = read_u8(&pc);
        int32_t limit = read_i32(&pc);
        off8 = read_i8(&pc);

        a = locals[reg];

        /* Int32 fast path */
        if (R8E_IS_INT32(a)) {
            ia = r8e_interp_get_int32(a);
            if (ia < limit) {
                /* Condition is true: continue loop body */
                DISPATCH();
            }
            /* Condition is false: jump past loop body */
            pc += off8 - 6;
            DISPATCH();
        }

        /* Slow path: double comparison */
        da = r8e_to_double_fast(a);
        if (!(da < (double)limit)) { /* handles NaN correctly */
            pc += off8 - 6;
        }
        DISPATCH();
    }

    TARGET(OP_LOAD_FIELD_CHECK) {
        /*
         * Fused: get_field2 + get_loc_check
         * Operands: atom (4 bytes)
         * Covers ~6% of dispatches.
         */
        atom = read_u32(&pc);
        a = PEEK(); /* object stays on stack */
        result = r8e_interp_get_prop(ctx, a, atom);
        if (ctx->has_exception) goto exception;
        if (R8E_IS_UNDEFINED(result)) {
            /* Property not found - could be reference error */
        }
        PUSH(result);
        DISPATCH();
    }

    TARGET(OP_BRANCH_LOAD) {
        /*
         * Fused: if_false8(off) + get_loc_check(reg)
         * Operands: off8 (1 byte) + reg (1 byte)
         * Covers ~3% of dispatches.
         */
        off8 = read_i8(&pc);
        reg = read_u8(&pc);

        a = POP();
        if (!r8e_is_truthy(a)) {
            pc += off8 - 2;
        } else {
            PUSH(locals[reg]);
        }
        DISPATCH();
    }

    TARGET(OP_STORE_LOCAL_POP) {
        /*
         * Fused: store_local(reg) + drop
         * Operands: reg (1 byte)
         * Common in assignment statements.
         */
        reg = read_u8(&pc);
        locals[reg] = POP();
        DISPATCH();
    }

    TARGET(OP_LOAD_LOCAL2) {
        /*
         * Fused: load_local(reg1) + load_local(reg2)
         * Operands: reg1 (1 byte) + reg2 (1 byte)
         * Common in binary expressions (a + b, a < b, etc.)
         */
        uint8_t reg1 = read_u8(&pc);
        uint8_t reg2 = read_u8(&pc);
        PUSH(locals[reg1]);
        PUSH(locals[reg2]);
        DISPATCH();
    }

    /* =================================================================
     * SPECIAL / SECURITY
     * ================================================================= */

    TARGET(OP_EVAL_TRAP) {
        /*
         * Deferred eval handling (Section 5.7).
         * At runtime: parse eval string, examine scope usage,
         * materialize only needed variables.
         * For now: no-op placeholder.
         */
        DISPATCH();
    }

    TARGET(OP_CHECK_LIMITS) {
        /*
         * Resource limit check (Section 11.6).
         * Inserted at loop back-edges and function calls.
         * Checks fuel counter (CPU time proxy).
         */
        ctx->fuel--;
        if (ctx->fuel <= 0) {
            r8e_interp_throw_range_error(ctx,
                "CPU time limit exceeded");
            goto exception;
        }
        DISPATCH();
    }

    TARGET(OP_NOP) {
        DISPATCH();
    }

    TARGET(OP_HALT) {
        /* End of top-level script */
        if (sp > frame->stack_base) {
            result = POP();
        } else {
            result = R8E_UNDEFINED;
        }
        goto do_return;
    }

#ifdef R8E_USE_COMPUTED_GOTO
    op_INVALID: {
        r8e_interp_throw_type_error(ctx, "invalid opcode");
        goto exception;
    }
#else
        default:
            r8e_interp_throw_type_error(ctx, "invalid opcode");
            goto exception;
        } /* end switch */
    } /* end for */
#endif

    /* =================================================================
     * EXCEPTION HANDLING
     * ================================================================= */

exception:
    /* Save stack pointer back to frame */
    frame->sp = sp;

    /* Try to find a handler in the current frame */
    if (r8e_interp_unwind(ctx, frame)) {
        /* Handler found: restore interpreter state from frame */
        pc = frame->pc;
        sp = frame->sp;
        /* Continue execution at the handler */
        DISPATCH();
    }

    /* No handler in this frame: propagate exception to caller */
    return R8E_UNDEFINED;

    /* =================================================================
     * RETURN HANDLING
     * ================================================================= */

do_return:
    return result;

do_return_with_result:
    return result;
}

/* =========================================================================
 * PUBLIC API: r8e_interpret
 *
 * Execute raw bytecode with a constant pool. This is the entry point
 * for running compiled bytecode from the single-pass compiler.
 *
 * @param ctx           Interpreter context.
 * @param bytecode      Compiled bytecode array.
 * @param bytecode_len  Length of bytecode in bytes.
 * @param constants     Constant pool (NaN-boxed values).
 * @param num_constants Number of constants.
 * @return              Result value, or R8E_UNDEFINED on error.
 * ========================================================================= */

R8EValue r8e_interpret(R8EInterpContext *ctx, const uint8_t *bytecode,
                        uint32_t bytecode_len, R8EValue *constants,
                        uint16_t num_constants) {
    if (!ctx || !bytecode || bytecode_len == 0) return R8E_UNDEFINED;

    /* Create a synthetic function for the top-level code */
    R8EFunctionInterp func;
    memset(&func, 0, sizeof(func));
    func.flags = (R8E_GC_KIND_FUNCTION << R8E_GC_KIND_SHIFT);
    func.bytecode = (uint8_t *)bytecode;
    func.bytecode_len = bytecode_len;
    func.param_count = 0;
    func.local_count = R8E_DEFAULT_STACK_SIZE;
    func.stack_size = R8E_DEFAULT_STACK_SIZE;
    func.constants = constants;
    func.const_count = num_constants;

    /* Create a synthetic closure wrapping the function */
    R8EClosureInterp cl;
    memset(&cl, 0, sizeof(cl));
    cl.flags = (R8E_GC_KIND_CLOSURE << R8E_GC_KIND_SHIFT);
    cl.func = &func;
    cl.capture_count = 0;
    cl.capture_mode = R8E_CAPTURE_MODE_INLINE;

    /* Reset exception state */
    ctx->has_exception = false;
    ctx->exception = R8E_UNDEFINED;

    /* Initialize fuel if not already set */
    if (ctx->fuel <= 0) ctx->fuel = R8E_FUEL_DEFAULT;

    /* Call the function via the standard path */
    return r8e_interp_call_internal(ctx, r8e_interp_from_pointer(&cl),
                                     R8E_UNDEFINED, NULL, 0, false);
}

/**
 * Extended interpret: same as r8e_interpret but with func_base parameter.
 * func_base is the index in the constants array where function closures
 * start (stored in the synthetic function's capture_count for OP_NEW_FUNCTION).
 */
R8EValue r8e_interpret_ex(R8EInterpContext *ctx, const uint8_t *bytecode,
                           uint32_t bytecode_len, R8EValue *constants,
                           uint16_t num_constants, uint16_t func_base) {
    if (!ctx || !bytecode || bytecode_len == 0) return R8E_UNDEFINED;

    R8EFunctionInterp func;
    memset(&func, 0, sizeof(func));
    func.flags = (R8E_GC_KIND_FUNCTION << R8E_GC_KIND_SHIFT);
    func.bytecode = (uint8_t *)bytecode;
    func.bytecode_len = bytecode_len;
    func.param_count = 0;
    func.local_count = R8E_DEFAULT_STACK_SIZE;
    func.stack_size = R8E_DEFAULT_STACK_SIZE;
    func.constants = constants;
    func.const_count = num_constants;
    func.capture_count = func_base; /* Repurpose: offset to func closures */

    R8EClosureInterp cl;
    memset(&cl, 0, sizeof(cl));
    cl.flags = (R8E_GC_KIND_CLOSURE << R8E_GC_KIND_SHIFT);
    cl.func = &func;
    cl.capture_count = 0;
    cl.capture_mode = R8E_CAPTURE_MODE_INLINE;

    ctx->has_exception = false;
    ctx->exception = R8E_UNDEFINED;
    if (ctx->fuel <= 0) ctx->fuel = R8E_FUEL_DEFAULT;

    return r8e_interp_call_internal(ctx, r8e_interp_from_pointer(&cl),
                                     R8E_UNDEFINED, NULL, 0, false);
}

/* =========================================================================
 * PUBLIC API: r8e_call_function
 *
 * Call a JavaScript function value with arguments.
 *
 * @param ctx       Interpreter context.
 * @param func      Function or closure value.
 * @param this_val  The 'this' binding.
 * @param args      Argument array.
 * @param argc      Number of arguments.
 * @return          Return value.
 * ========================================================================= */

R8EValue r8e_call_function(R8EInterpContext *ctx, R8EValue func,
                            R8EValue this_val, const R8EValue *args,
                            int argc) {
    if (!ctx) return R8E_UNDEFINED;

    /* Reset exception state */
    ctx->has_exception = false;
    ctx->exception = R8E_UNDEFINED;

    if (ctx->fuel <= 0) ctx->fuel = R8E_FUEL_DEFAULT;

    return r8e_interp_call_internal(ctx, func, this_val, args, argc, false);
}

/* =========================================================================
 * PUBLIC API: r8e_interp_context_new / r8e_interp_context_free
 *
 * Create and destroy interpreter contexts.
 * ========================================================================= */

R8EInterpContext *r8e_interp_context_new(void) {
    R8EInterpContext *ctx = (R8EInterpContext *)calloc(
        1, sizeof(R8EInterpContext));
    if (!ctx) return NULL;

    ctx->frame_count = 0;
    ctx->global_obj = R8E_UNDEFINED;
    ctx->exception = R8E_UNDEFINED;
    ctx->has_exception = false;
    ctx->fuel = R8E_FUEL_DEFAULT;
    ctx->max_stack_depth = R8E_MAX_CALL_DEPTH;
    ctx->max_memory = 16 * 1024 * 1024; /* 16 MB */
    ctx->max_cpu_time_us = 5000000;      /* 5 seconds */

    return ctx;
}

void r8e_interp_context_free(R8EInterpContext *ctx) {
    if (!ctx) return;
    free(ctx);
}

/* =========================================================================
 * PUBLIC API: r8e_interp_set_global
 *
 * Set the global object for the interpreter context.
 * ========================================================================= */

void r8e_interp_set_global(R8EInterpContext *ctx, R8EValue global) {
    if (ctx) ctx->global_obj = global;
}

/* =========================================================================
 * PUBLIC API: r8e_interp_has_exception / r8e_interp_get_exception
 * ========================================================================= */

bool r8e_interp_has_exception(const R8EInterpContext *ctx) {
    return ctx && ctx->has_exception;
}

R8EValue r8e_interp_get_exception(const R8EInterpContext *ctx) {
    if (!ctx || !ctx->has_exception) return R8E_UNDEFINED;
    return ctx->exception;
}

R8EValue r8e_interp_clear_exception(R8EInterpContext *ctx) {
    if (!ctx || !ctx->has_exception) return R8E_UNDEFINED;
    R8EValue exc = ctx->exception;
    ctx->exception = R8E_UNDEFINED;
    ctx->has_exception = false;
    return exc;
}

/* =========================================================================
 * PUBLIC API: r8e_interp_set_fuel / r8e_interp_get_fuel
 *
 * Fuel counter for CPU time limiting. Decremented by CHECK_LIMITS.
 * ========================================================================= */

void r8e_interp_set_fuel(R8EInterpContext *ctx, int64_t fuel) {
    if (ctx) ctx->fuel = fuel;
}

int64_t r8e_interp_get_fuel(const R8EInterpContext *ctx) {
    return ctx ? ctx->fuel : 0;
}
