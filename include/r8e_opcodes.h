/*
 * r8e_opcodes.h - Bytecode Opcode Definitions and Metadata
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 6 (Bytecode Format and Superinstructions).
 *
 * Architecture:
 *   - Variable-width encoding: 4-bit prefix for top-16 opcodes (90% coverage)
 *   - Chain superinstructions fuse 3-5 ops into one dispatch (Section 6.2)
 *   - EXP-06: 4.23 bits entropy, 44% bytecode size savings
 *   - EXP-02: top-30 bigrams cover 78.7%, chains cover 50%+
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_OPCODES_H
#define R8E_OPCODES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Opcode Enumeration (CLAUDE.md Section 6.3)
 *
 * Organized by category. The numeric values here are the full 8-bit
 * opcode codes used in the bytecode stream. The variable-width encoder
 * (4-bit prefix for top-16) is handled at encode/decode time, not here.
 *
 * Operand notation:
 *   reg    = local register index (uint8_t or uint16_t)
 *   atom   = atom table index (uint32_t)
 *   idx    = constant pool index (uint16_t)
 *   imm8   = 8-bit immediate signed integer
 *   imm16  = 16-bit immediate signed integer
 *   imm32  = 32-bit immediate signed integer
 *   off8   = 8-bit signed jump offset
 *   off16  = 16-bit signed jump offset
 *   off32  = 32-bit signed jump offset
 *   argc   = argument count (uint8_t)
 * ========================================================================= */

typedef enum {
    /* =====================================================================
     * Category: Load/Store (register and variable access)
     * ===================================================================== */
    R8E_OP_LOAD_LOCAL       = 0x00,  /* reg         -> push locals[reg] */
    R8E_OP_STORE_LOCAL      = 0x01,  /* reg         -> locals[reg] = pop */
    R8E_OP_LOAD_GLOBAL      = 0x02,  /* atom        -> push global[atom] */
    R8E_OP_STORE_GLOBAL     = 0x03,  /* atom        -> global[atom] = pop */
    R8E_OP_LOAD_CONST       = 0x04,  /* idx         -> push constants[idx] */
    R8E_OP_PUSH_INT8        = 0x05,  /* imm8        -> push int8 literal */
    R8E_OP_PUSH_INT16       = 0x06,  /* imm16       -> push int16 literal */
    R8E_OP_PUSH_INT32       = 0x07,  /* imm32       -> push int32 literal */
    R8E_OP_PUSH_DOUBLE      = 0x08,  /* idx         -> push double from const pool */
    R8E_OP_PUSH_UNDEFINED   = 0x09,  /*             -> push undefined */
    R8E_OP_PUSH_NULL        = 0x0A,  /*             -> push null */
    R8E_OP_PUSH_TRUE        = 0x0B,  /*             -> push true */
    R8E_OP_PUSH_FALSE       = 0x0C,  /*             -> push false */
    R8E_OP_PUSH_ZERO        = 0x0D,  /*             -> push int32(0) */
    R8E_OP_PUSH_ONE         = 0x0E,  /*             -> push int32(1) */
    R8E_OP_PUSH_STRING      = 0x0F,  /* idx         -> push string from const pool */

    /* =====================================================================
     * Category: Stack Manipulation
     * ===================================================================== */
    R8E_OP_DUP              = 0x10,  /*             -> duplicate top of stack */
    R8E_OP_DROP             = 0x11,  /*             -> pop and discard top */
    R8E_OP_SWAP             = 0x12,  /*             -> swap top two values */
    R8E_OP_ROT3             = 0x13,  /*             -> rotate top 3 values */

    /* =====================================================================
     * Category: Property Access
     * ===================================================================== */
    R8E_OP_GET_PROP         = 0x14,  /* atom        -> pop obj, push obj[atom] */
    R8E_OP_SET_PROP         = 0x15,  /* atom        -> pop val, pop obj, obj[atom]=val */
    R8E_OP_GET_ELEM         = 0x16,  /*             -> pop key, pop obj, push obj[key] */
    R8E_OP_SET_ELEM         = 0x17,  /*             -> pop val, pop key, pop obj, obj[key]=val */
    R8E_OP_DELETE_PROP      = 0x18,  /* atom        -> pop obj, delete obj[atom] */
    R8E_OP_DELETE_ELEM      = 0x19,  /*             -> pop key, pop obj, delete obj[key] */
    R8E_OP_GET_PROP_2       = 0x1A,  /* atom        -> pop obj, push obj, push obj[atom] */
    R8E_OP_DEFINE_PROP      = 0x1B,  /* atom,flags  -> define property with descriptor */
    R8E_OP_GET_SUPER        = 0x1C,  /* atom        -> super.prop */
    R8E_OP_SET_SUPER        = 0x1D,  /* atom        -> super.prop = val */

    /* =====================================================================
     * Category: Arithmetic
     * ===================================================================== */
    R8E_OP_ADD              = 0x20,  /*             -> pop b, pop a, push a+b */
    R8E_OP_SUB              = 0x21,  /*             -> pop b, pop a, push a-b */
    R8E_OP_MUL              = 0x22,  /*             -> pop b, pop a, push a*b */
    R8E_OP_DIV              = 0x23,  /*             -> pop b, pop a, push a/b */
    R8E_OP_MOD              = 0x24,  /*             -> pop b, pop a, push a%b */
    R8E_OP_POW              = 0x25,  /*             -> pop b, pop a, push a**b */
    R8E_OP_NEG              = 0x26,  /*             -> pop a, push -a */
    R8E_OP_POS              = 0x27,  /*             -> pop a, push +a (ToNumber) */
    R8E_OP_INC              = 0x28,  /*             -> pop a, push a+1 */
    R8E_OP_DEC              = 0x29,  /*             -> pop a, push a-1 */
    R8E_OP_POST_INC         = 0x2A,  /* reg         -> push locals[reg], locals[reg]++ */
    R8E_OP_POST_DEC         = 0x2B,  /* reg         -> push locals[reg], locals[reg]-- */

    /* =====================================================================
     * Category: Bitwise Operations
     * ===================================================================== */
    R8E_OP_BITAND           = 0x2C,  /*             -> pop b, pop a, push a&b */
    R8E_OP_BITOR            = 0x2D,  /*             -> pop b, pop a, push a|b */
    R8E_OP_BITXOR           = 0x2E,  /*             -> pop b, pop a, push a^b */
    R8E_OP_BITNOT           = 0x2F,  /*             -> pop a, push ~a */
    R8E_OP_SHL              = 0x30,  /*             -> pop b, pop a, push a<<b */
    R8E_OP_SHR              = 0x31,  /*             -> pop b, pop a, push a>>b */
    R8E_OP_USHR             = 0x32,  /*             -> pop b, pop a, push a>>>b */

    /* =====================================================================
     * Category: Comparison and Logic
     * ===================================================================== */
    R8E_OP_EQ               = 0x34,  /*             -> pop b, pop a, push a==b */
    R8E_OP_SEQ              = 0x35,  /*             -> pop b, pop a, push a===b */
    R8E_OP_NE               = 0x36,  /*             -> pop b, pop a, push a!=b */
    R8E_OP_SNE              = 0x37,  /*             -> pop b, pop a, push a!==b */
    R8E_OP_LT               = 0x38,  /*             -> pop b, pop a, push a<b */
    R8E_OP_LE               = 0x39,  /*             -> pop b, pop a, push a<=b */
    R8E_OP_GT               = 0x3A,  /*             -> pop b, pop a, push a>b */
    R8E_OP_GE               = 0x3B,  /*             -> pop b, pop a, push a>=b */
    R8E_OP_NOT              = 0x3C,  /*             -> pop a, push !a */
    R8E_OP_TYPEOF           = 0x3D,  /*             -> pop a, push typeof a */
    R8E_OP_INSTANCEOF       = 0x3E,  /*             -> pop b, pop a, push a instanceof b */
    R8E_OP_IN               = 0x3F,  /*             -> pop b, pop a, push a in b */

    /* =====================================================================
     * Category: Control Flow
     * ===================================================================== */
    R8E_OP_JUMP             = 0x40,  /* off32       -> unconditional jump */
    R8E_OP_JUMP8            = 0x41,  /* off8        -> short unconditional jump */
    R8E_OP_JUMP16           = 0x42,  /* off16       -> medium unconditional jump */
    R8E_OP_JUMP_IF_FALSE    = 0x43,  /* off32       -> pop, jump if falsy */
    R8E_OP_JUMP_IF_TRUE     = 0x44,  /* off32       -> pop, jump if truthy */
    R8E_OP_JUMP_IF_FALSE8   = 0x45,  /* off8        -> pop, short jump if falsy */
    R8E_OP_JUMP_IF_TRUE8    = 0x46,  /* off8        -> pop, short jump if truthy */
    R8E_OP_JUMP_IF_FALSE16  = 0x47,  /* off16       -> pop, medium jump if falsy */
    R8E_OP_JUMP_IF_TRUE16   = 0x48,  /* off16       -> pop, medium jump if truthy */
    R8E_OP_JUMP_IF_NULLISH  = 0x49,  /* off32       -> pop, jump if null/undefined */

    /* =====================================================================
     * Category: Function Calls
     * ===================================================================== */
    R8E_OP_CALL             = 0x4C,  /* argc        -> call function(argc args) */
    R8E_OP_CALL_METHOD      = 0x4D,  /* argc        -> call method(argc args) */
    R8E_OP_NEW              = 0x4E,  /* argc        -> new Constructor(argc args) */
    R8E_OP_RETURN           = 0x4F,  /*             -> return top of stack */
    R8E_OP_RETURN_UNDEFINED = 0x50,  /*             -> return undefined */
    R8E_OP_TAIL_CALL        = 0x51,  /* argc        -> tail call optimization */

    /* =====================================================================
     * Category: Exception Handling
     * ===================================================================== */
    R8E_OP_THROW            = 0x54,  /*             -> throw top of stack */
    R8E_OP_TRY_PUSH         = 0x55,  /* off32       -> push try/catch handler at offset */
    R8E_OP_TRY_POP          = 0x56,  /*             -> pop try/catch handler */
    R8E_OP_CATCH            = 0x57,  /*             -> push caught exception value */
    R8E_OP_FINALLY          = 0x58,  /*             -> enter finally block */

    /* =====================================================================
     * Category: Object/Array Creation
     * ===================================================================== */
    R8E_OP_NEW_OBJECT       = 0x5C,  /*             -> push new {} */
    R8E_OP_NEW_ARRAY        = 0x5D,  /* count       -> push new Array with count elements */
    R8E_OP_NEW_FUNCTION     = 0x5E,  /* idx         -> push closure from function index */
    R8E_OP_NEW_REGEXP       = 0x5F,  /* idx         -> push regexp from constant pool */
    R8E_OP_INIT_PROP        = 0x60,  /* atom        -> pop val, obj[atom]=val (no proto lookup) */
    R8E_OP_INIT_ELEM        = 0x61,  /*             -> pop val, pop key, obj[key]=val */
    R8E_OP_SPREAD           = 0x62,  /*             -> spread iterable into array/call */

    /* =====================================================================
     * Category: Variable Scope Operations
     * ===================================================================== */
    R8E_OP_LOAD_CLOSURE     = 0x64,  /* idx         -> push captured variable[idx] */
    R8E_OP_STORE_CLOSURE    = 0x65,  /* idx         -> captured variable[idx] = pop */
    R8E_OP_LOAD_ARG         = 0x66,  /* idx         -> push arguments[idx] */
    R8E_OP_LOAD_REST_ARGS   = 0x67,  /* idx         -> push rest params as array */
    R8E_OP_ENTER_SCOPE      = 0x68,  /*             -> push new block scope */
    R8E_OP_EXIT_SCOPE       = 0x69,  /*             -> pop block scope */
    R8E_OP_LOAD_THIS        = 0x6A,  /*             -> push this */
    R8E_OP_LOAD_NEW_TARGET  = 0x6B,  /*             -> push new.target */

    /* =====================================================================
     * Category: Destructuring
     * ===================================================================== */
    R8E_OP_DESTRUCTURE_ARRAY  = 0x6C, /* count      -> destructure array */
    R8E_OP_DESTRUCTURE_OBJECT = 0x6D, /* count      -> destructure object */
    R8E_OP_SET_DEFAULT        = 0x6E, /* off8       -> if TOS is undefined, jump */

    /* =====================================================================
     * Category: Iterator / Generator / Async
     * ===================================================================== */
    R8E_OP_GET_ITERATOR     = 0x70,  /*             -> pop obj, push iterator */
    R8E_OP_ITERATOR_NEXT    = 0x71,  /*             -> push iterator.next() result */
    R8E_OP_ITERATOR_CLOSE   = 0x72,  /*             -> close iterator */
    R8E_OP_YIELD            = 0x73,  /*             -> yield top of stack */
    R8E_OP_YIELD_STAR       = 0x74,  /*             -> yield* iterable */
    R8E_OP_AWAIT            = 0x75,  /*             -> await promise */
    R8E_OP_ASYNC_FUNC_START = 0x76,  /*             -> init async function state */
    R8E_OP_ASYNC_FUNC_RESOLVE = 0x77, /*            -> resolve async function */
    R8E_OP_ASYNC_FUNC_REJECT  = 0x78, /*            -> reject async function */

    /* =====================================================================
     * Category: Class
     * ===================================================================== */
    R8E_OP_CLASS_CREATE     = 0x7C,  /* atom        -> create class */
    R8E_OP_CLASS_EXTENDS    = 0x7D,  /*             -> set class extends */
    R8E_OP_CLASS_METHOD     = 0x7E,  /* atom,flags  -> define class method */
    R8E_OP_CLASS_FIELD      = 0x7F,  /* atom        -> define class field */
    R8E_OP_CLASS_STATIC     = 0x80,  /* atom,flags  -> define static member */
    R8E_OP_CLASS_PRIVATE    = 0x81,  /* atom        -> define private member */

    /* =====================================================================
     * Category: Module
     * ===================================================================== */
    R8E_OP_IMPORT           = 0x84,  /* idx         -> import module */
    R8E_OP_EXPORT           = 0x85,  /* atom        -> export binding */
    R8E_OP_IMPORT_DYNAMIC   = 0x86,  /*             -> dynamic import() */
    R8E_OP_IMPORT_META      = 0x87,  /*             -> push import.meta */

    /* =====================================================================
     * Category: Optional Chaining / Nullish Coalescing
     * ===================================================================== */
    R8E_OP_OPTIONAL_CHAIN   = 0x88,  /* off32       -> if nullish, jump and push undefined */
    R8E_OP_NULLISH_COALESCE = 0x89,  /* off32       -> if not nullish, jump (keep value) */

    /* =====================================================================
     * Category: Miscellaneous
     * ===================================================================== */
    R8E_OP_VOID             = 0x8C,  /*             -> pop a, push undefined */
    R8E_OP_DEBUGGER         = 0x8D,  /*             -> debugger statement (nop in release) */
    R8E_OP_WITH_ENTER       = 0x8E,  /*             -> enter with scope */
    R8E_OP_WITH_EXIT        = 0x8F,  /*             -> exit with scope */
    R8E_OP_FOR_IN_INIT      = 0x90,  /*             -> init for-in enumeration */
    R8E_OP_FOR_IN_NEXT      = 0x91,  /* off32       -> next for-in key, jump if done */
    R8E_OP_FOR_OF_INIT      = 0x92,  /*             -> init for-of iteration */
    R8E_OP_FOR_OF_NEXT      = 0x93,  /* off32       -> next for-of value, jump if done */

    /* =====================================================================
     * Category: Template Literals
     * ===================================================================== */
    R8E_OP_TEMPLATE_HEAD    = 0x94,  /* idx         -> push template string head */
    R8E_OP_TEMPLATE_MIDDLE  = 0x95,  /* idx         -> concat template middle */
    R8E_OP_TEMPLATE_TAIL    = 0x96,  /* idx         -> concat template tail */
    R8E_OP_TAGGED_TEMPLATE  = 0x97,  /* argc        -> tagged template call */

    /* =====================================================================
     * Category: Chain Superinstructions (CLAUDE.md Section 6.2)
     *
     * EXP-02: chain superinstructions cover 50%+ of all dispatches.
     * Each chain fuses 3-5 instructions into one dispatch, eliminating
     * intermediate stack ops and RC operations.
     * ===================================================================== */

    /**
     * CALL_METHOD_FAST: fuses get_var + get_field2 + get_loc_check + call_method
     * Covers 17.8% of all dispatches. Handles obj.method(args) in one op.
     */
    R8E_OP_CALL_METHOD_FAST = 0xE0,  /* atom,argc   -> fused method call */

    /**
     * FOR_INC_LOOP: fuses get_loc + post_inc + put_loc + drop + goto8
     * Covers 18.0% of dispatches. The for-loop i++ pattern.
     * Zero RC operations on fast path (int32 that does not overflow).
     */
    R8E_OP_FOR_INC_LOOP     = 0xE1,  /* reg,off8    -> fused loop increment */

    /**
     * LOOP_CMP_BRANCH: fuses get_loc + push_i32 + lt + if_false8
     * Covers ~7% of dispatches. The for-loop condition (i < n).
     */
    R8E_OP_LOOP_CMP_BRANCH  = 0xE2,  /* reg,imm,off -> fused loop condition */

    /**
     * LOAD_FIELD_CHECK: fuses get_field2 + get_loc_check
     * Covers ~6% of dispatches. Property access pattern.
     */
    R8E_OP_LOAD_FIELD_CHECK = 0xE3,  /* atom        -> fused property load + check */

    /**
     * BRANCH_LOAD: fuses if_false8 + get_loc_check
     * Covers ~3% of dispatches. Branch then load pattern.
     */
    R8E_OP_BRANCH_LOAD      = 0xE4,  /* off8,reg    -> fused branch + load */

    /**
     * STORE_LOCAL_POP: fuses store_local + drop
     * Common in assignment statements.
     */
    R8E_OP_STORE_LOCAL_POP  = 0xE5,  /* reg         -> fused store + pop */

    /**
     * LOAD_LOCAL_LOAD_LOCAL: fuses two load_local operations.
     * Common when accessing two variables in a binary expression.
     */
    R8E_OP_LOAD_LOCAL2      = 0xE6,  /* reg1,reg2   -> push two locals */

    /* =====================================================================
     * Category: Special / Security (CLAUDE.md Section 11)
     * ===================================================================== */

    /**
     * EVAL_TRAP: deferred eval handling (Section 5.7).
     * Emitted when parser encounters eval(). At runtime, lazy scope
     * materialization only pays for what eval actually accesses.
     */
    R8E_OP_EVAL_TRAP        = 0xF0,

    /**
     * CHECK_LIMITS: resource limit check (Section 11.6).
     * Inserted at loop back-edges and function calls.
     * Checks CPU time, memory, and stack depth against realm limits.
     */
    R8E_OP_CHECK_LIMITS     = 0xF1,

    /**
     * NOP: no operation. Used for alignment and patching.
     */
    R8E_OP_NOP              = 0xFE,

    /**
     * HALT: terminate execution. Used at end of top-level scripts.
     */
    R8E_OP_HALT             = 0xFF

} R8EOpcode;


/* =========================================================================
 * Opcode Count
 * ========================================================================= */

/* Total number of defined opcodes (not including gaps) */
#define R8E_OPCODE_COUNT  256


/* =========================================================================
 * Opcode Metadata Table
 *
 * Each opcode has associated metadata used by the compiler, interpreter,
 * bytecode verifier, and disassembler:
 *   - name:       human-readable name string
 *   - operand_bytes: total bytes of operands following the opcode
 *   - stack_in:   number of values popped from operand stack (-1 = variable)
 *   - stack_out:  number of values pushed to operand stack (-1 = variable)
 *   - flags:      opcode behavioral flags
 * ========================================================================= */

/* Opcode flags */
#define R8E_OPF_NONE       0x00
#define R8E_OPF_JUMP       0x01  /* control flow: jump instruction */
#define R8E_OPF_CALL       0x02  /* function call or method call */
#define R8E_OPF_SIDE_EFFECT 0x04 /* has side effects (store, delete, etc.) */
#define R8E_OPF_SUPER      0x08  /* chain superinstruction */
#define R8E_OPF_COND       0x10  /* conditional branch */
#define R8E_OPF_TERMINAL   0x20  /* terminates function (return, throw, halt) */

typedef struct {
    const char *name;       /* human-readable opcode name */
    int8_t  operand_bytes;  /* bytes of operands after opcode (0-4) */
    int8_t  stack_in;       /* values consumed (-1 = variable/argc-dependent) */
    int8_t  stack_out;      /* values produced (-1 = variable) */
    uint8_t flags;          /* R8E_OPF_* flags */
} R8EOpcodeInfo;

/**
 * Global opcode metadata table, indexed by opcode value.
 * Defined in r8e_opcodes.c (or included inline for header-only use).
 */
#ifdef R8E_OPCODE_IMPL

/* Suppress pedantic warning for range-based designated initializers.
 * This is a GCC/Clang extension used to zero-fill opcode table gaps. */
#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
#endif

static const R8EOpcodeInfo r8e_opcode_info[R8E_OPCODE_COUNT] = {
    /* 0x00 */ { "load_local",         1,  0,  1, R8E_OPF_NONE },
    /* 0x01 */ { "store_local",        1,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x02 */ { "load_global",        4,  0,  1, R8E_OPF_NONE },
    /* 0x03 */ { "store_global",       4,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x04 */ { "load_const",         2,  0,  1, R8E_OPF_NONE },
    /* 0x05 */ { "push_int8",          1,  0,  1, R8E_OPF_NONE },
    /* 0x06 */ { "push_int16",         2,  0,  1, R8E_OPF_NONE },
    /* 0x07 */ { "push_int32",         4,  0,  1, R8E_OPF_NONE },
    /* 0x08 */ { "push_double",        2,  0,  1, R8E_OPF_NONE },
    /* 0x09 */ { "push_undefined",     0,  0,  1, R8E_OPF_NONE },
    /* 0x0A */ { "push_null",          0,  0,  1, R8E_OPF_NONE },
    /* 0x0B */ { "push_true",          0,  0,  1, R8E_OPF_NONE },
    /* 0x0C */ { "push_false",         0,  0,  1, R8E_OPF_NONE },
    /* 0x0D */ { "push_zero",          0,  0,  1, R8E_OPF_NONE },
    /* 0x0E */ { "push_one",           0,  0,  1, R8E_OPF_NONE },
    /* 0x0F */ { "push_string",        2,  0,  1, R8E_OPF_NONE },

    /* 0x10 */ { "dup",                0,  1,  2, R8E_OPF_NONE },
    /* 0x11 */ { "drop",               0,  1,  0, R8E_OPF_NONE },
    /* 0x12 */ { "swap",               0,  2,  2, R8E_OPF_NONE },
    /* 0x13 */ { "rot3",               0,  3,  3, R8E_OPF_NONE },
    /* 0x14 */ { "get_prop",           4,  1,  1, R8E_OPF_NONE },
    /* 0x15 */ { "set_prop",           4,  2,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x16 */ { "get_elem",           0,  2,  1, R8E_OPF_NONE },
    /* 0x17 */ { "set_elem",           0,  3,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x18 */ { "delete_prop",        4,  1,  1, R8E_OPF_SIDE_EFFECT },
    /* 0x19 */ { "delete_elem",        0,  2,  1, R8E_OPF_SIDE_EFFECT },
    /* 0x1A */ { "get_prop_2",         4,  1,  2, R8E_OPF_NONE },
    /* 0x1B */ { "define_prop",        5,  2,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x1C */ { "get_super",          4,  1,  1, R8E_OPF_NONE },
    /* 0x1D */ { "set_super",          4,  2,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x1E */ { NULL, 0, 0, 0, 0 },
    /* 0x1F */ { NULL, 0, 0, 0, 0 },

    /* 0x20 */ { "add",                0,  2,  1, R8E_OPF_NONE },
    /* 0x21 */ { "sub",                0,  2,  1, R8E_OPF_NONE },
    /* 0x22 */ { "mul",                0,  2,  1, R8E_OPF_NONE },
    /* 0x23 */ { "div",                0,  2,  1, R8E_OPF_NONE },
    /* 0x24 */ { "mod",                0,  2,  1, R8E_OPF_NONE },
    /* 0x25 */ { "pow",                0,  2,  1, R8E_OPF_NONE },
    /* 0x26 */ { "neg",                0,  1,  1, R8E_OPF_NONE },
    /* 0x27 */ { "pos",                0,  1,  1, R8E_OPF_NONE },
    /* 0x28 */ { "inc",                0,  1,  1, R8E_OPF_NONE },
    /* 0x29 */ { "dec",                0,  1,  1, R8E_OPF_NONE },
    /* 0x2A */ { "post_inc",           1,  0,  1, R8E_OPF_SIDE_EFFECT },
    /* 0x2B */ { "post_dec",           1,  0,  1, R8E_OPF_SIDE_EFFECT },
    /* 0x2C */ { "bitand",             0,  2,  1, R8E_OPF_NONE },
    /* 0x2D */ { "bitor",              0,  2,  1, R8E_OPF_NONE },
    /* 0x2E */ { "bitxor",             0,  2,  1, R8E_OPF_NONE },
    /* 0x2F */ { "bitnot",             0,  1,  1, R8E_OPF_NONE },

    /* 0x30 */ { "shl",                0,  2,  1, R8E_OPF_NONE },
    /* 0x31 */ { "shr",                0,  2,  1, R8E_OPF_NONE },
    /* 0x32 */ { "ushr",               0,  2,  1, R8E_OPF_NONE },
    /* 0x33 */ { NULL, 0, 0, 0, 0 },
    /* 0x34 */ { "eq",                 0,  2,  1, R8E_OPF_NONE },
    /* 0x35 */ { "seq",                0,  2,  1, R8E_OPF_NONE },
    /* 0x36 */ { "ne",                 0,  2,  1, R8E_OPF_NONE },
    /* 0x37 */ { "sne",                0,  2,  1, R8E_OPF_NONE },
    /* 0x38 */ { "lt",                 0,  2,  1, R8E_OPF_NONE },
    /* 0x39 */ { "le",                 0,  2,  1, R8E_OPF_NONE },
    /* 0x3A */ { "gt",                 0,  2,  1, R8E_OPF_NONE },
    /* 0x3B */ { "ge",                 0,  2,  1, R8E_OPF_NONE },
    /* 0x3C */ { "not",                0,  1,  1, R8E_OPF_NONE },
    /* 0x3D */ { "typeof",             0,  1,  1, R8E_OPF_NONE },
    /* 0x3E */ { "instanceof",         0,  2,  1, R8E_OPF_NONE },
    /* 0x3F */ { "in",                 0,  2,  1, R8E_OPF_NONE },

    /* 0x40 */ { "jump",               4,  0,  0, R8E_OPF_JUMP },
    /* 0x41 */ { "jump8",              1,  0,  0, R8E_OPF_JUMP },
    /* 0x42 */ { "jump16",             2,  0,  0, R8E_OPF_JUMP },
    /* 0x43 */ { "jump_if_false",      4,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x44 */ { "jump_if_true",       4,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x45 */ { "jump_if_false8",     1,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x46 */ { "jump_if_true8",      1,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x47 */ { "jump_if_false16",    2,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x48 */ { "jump_if_true16",     2,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x49 */ { "jump_if_nullish",    4,  1,  0, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x4A */ { NULL, 0, 0, 0, 0 },
    /* 0x4B */ { NULL, 0, 0, 0, 0 },
    /* 0x4C */ { "call",               1, -1,  1, R8E_OPF_CALL },
    /* 0x4D */ { "call_method",        1, -1,  1, R8E_OPF_CALL },
    /* 0x4E */ { "new",                1, -1,  1, R8E_OPF_CALL },
    /* 0x4F */ { "return",             0,  1,  0, R8E_OPF_TERMINAL },

    /* 0x50 */ { "return_undefined",   0,  0,  0, R8E_OPF_TERMINAL },
    /* 0x51 */ { "tail_call",          1, -1,  1, R8E_OPF_CALL | R8E_OPF_TERMINAL },
    /* 0x52 */ { NULL, 0, 0, 0, 0 },
    /* 0x53 */ { NULL, 0, 0, 0, 0 },
    /* 0x54 */ { "throw",              0,  1,  0, R8E_OPF_TERMINAL },
    /* 0x55 */ { "try_push",           4,  0,  0, R8E_OPF_NONE },
    /* 0x56 */ { "try_pop",            0,  0,  0, R8E_OPF_NONE },
    /* 0x57 */ { "catch",              0,  0,  1, R8E_OPF_NONE },
    /* 0x58 */ { "finally",            0,  0,  0, R8E_OPF_NONE },
    /* 0x59 */ { NULL, 0, 0, 0, 0 },
    /* 0x5A */ { NULL, 0, 0, 0, 0 },
    /* 0x5B */ { NULL, 0, 0, 0, 0 },
    /* 0x5C */ { "new_object",         0,  0,  1, R8E_OPF_NONE },
    /* 0x5D */ { "new_array",          2,  0,  1, R8E_OPF_NONE },
    /* 0x5E */ { "new_function",       2,  0,  1, R8E_OPF_NONE },
    /* 0x5F */ { "new_regexp",         2,  0,  1, R8E_OPF_NONE },

    /* 0x60 */ { "init_prop",          4,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x61 */ { "init_elem",          0,  2,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x62 */ { "spread",             0,  1,  0, R8E_OPF_NONE },
    /* 0x63 */ { NULL, 0, 0, 0, 0 },
    /* 0x64 */ { "load_closure",       1,  0,  1, R8E_OPF_NONE },
    /* 0x65 */ { "store_closure",      1,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x66 */ { "load_arg",           1,  0,  1, R8E_OPF_NONE },
    /* 0x67 */ { "load_rest_args",     1,  0,  1, R8E_OPF_NONE },
    /* 0x68 */ { "enter_scope",        0,  0,  0, R8E_OPF_NONE },
    /* 0x69 */ { "exit_scope",         0,  0,  0, R8E_OPF_NONE },
    /* 0x6A */ { "load_this",          0,  0,  1, R8E_OPF_NONE },
    /* 0x6B */ { "load_new_target",    0,  0,  1, R8E_OPF_NONE },
    /* 0x6C */ { "destructure_array",  1, -1, -1, R8E_OPF_NONE },
    /* 0x6D */ { "destructure_object", 1, -1, -1, R8E_OPF_NONE },
    /* 0x6E */ { "set_default",        1,  1,  1, R8E_OPF_COND },
    /* 0x6F */ { NULL, 0, 0, 0, 0 },

    /* 0x70 */ { "get_iterator",       0,  1,  1, R8E_OPF_NONE },
    /* 0x71 */ { "iterator_next",      0,  1,  2, R8E_OPF_NONE },
    /* 0x72 */ { "iterator_close",     0,  1,  0, R8E_OPF_NONE },
    /* 0x73 */ { "yield",              0,  1,  1, R8E_OPF_NONE },
    /* 0x74 */ { "yield_star",         0,  1,  1, R8E_OPF_NONE },
    /* 0x75 */ { "await",              0,  1,  1, R8E_OPF_NONE },
    /* 0x76 */ { "async_func_start",   0,  0,  0, R8E_OPF_NONE },
    /* 0x77 */ { "async_func_resolve", 0,  1,  0, R8E_OPF_NONE },
    /* 0x78 */ { "async_func_reject",  0,  1,  0, R8E_OPF_NONE },
    /* 0x79 */ { NULL, 0, 0, 0, 0 },
    /* 0x7A */ { NULL, 0, 0, 0, 0 },
    /* 0x7B */ { NULL, 0, 0, 0, 0 },
    /* 0x7C */ { "class_create",       4,  0,  1, R8E_OPF_NONE },
    /* 0x7D */ { "class_extends",      0,  2,  1, R8E_OPF_NONE },
    /* 0x7E */ { "class_method",       5,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x7F */ { "class_field",        4,  1,  0, R8E_OPF_SIDE_EFFECT },

    /* 0x80 */ { "class_static",       5,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x81 */ { "class_private",      4,  1,  0, R8E_OPF_SIDE_EFFECT },
    /* 0x82 */ { NULL, 0, 0, 0, 0 },
    /* 0x83 */ { NULL, 0, 0, 0, 0 },
    /* 0x84 */ { "import",             2,  0,  1, R8E_OPF_NONE },
    /* 0x85 */ { "export",             4,  1,  0, R8E_OPF_NONE },
    /* 0x86 */ { "import_dynamic",     0,  1,  1, R8E_OPF_CALL },
    /* 0x87 */ { "import_meta",        0,  0,  1, R8E_OPF_NONE },
    /* 0x88 */ { "optional_chain",     4,  1,  1, R8E_OPF_COND },
    /* 0x89 */ { "nullish_coalesce",   4,  1,  1, R8E_OPF_COND },
    /* 0x8A */ { NULL, 0, 0, 0, 0 },
    /* 0x8B */ { NULL, 0, 0, 0, 0 },
    /* 0x8C */ { "void",               0,  1,  1, R8E_OPF_NONE },
    /* 0x8D */ { "debugger",           0,  0,  0, R8E_OPF_NONE },
    /* 0x8E */ { "with_enter",         0,  1,  0, R8E_OPF_NONE },
    /* 0x8F */ { "with_exit",          0,  0,  0, R8E_OPF_NONE },

    /* 0x90 */ { "for_in_init",        0,  1,  1, R8E_OPF_NONE },
    /* 0x91 */ { "for_in_next",        4,  1,  2, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x92 */ { "for_of_init",        0,  1,  1, R8E_OPF_NONE },
    /* 0x93 */ { "for_of_next",        4,  1,  2, R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0x94 */ { "template_head",      2,  0,  1, R8E_OPF_NONE },
    /* 0x95 */ { "template_middle",    2,  1,  1, R8E_OPF_NONE },
    /* 0x96 */ { "template_tail",      2,  1,  1, R8E_OPF_NONE },
    /* 0x97 */ { "tagged_template",    1, -1,  1, R8E_OPF_CALL },
    /* 0x98-0xDF: reserved (NULL entries) */
    [0x98 ... 0xDF] = { NULL, 0, 0, 0, 0 },

    /* 0xE0 */ { "call_method_fast",   5, -1,  1, R8E_OPF_SUPER | R8E_OPF_CALL },
    /* 0xE1 */ { "for_inc_loop",       2,  0,  0, R8E_OPF_SUPER | R8E_OPF_JUMP },
    /* 0xE2 */ { "loop_cmp_branch",    6,  0,  0, R8E_OPF_SUPER | R8E_OPF_JUMP | R8E_OPF_COND },
    /* 0xE3 */ { "load_field_check",   4,  1,  1, R8E_OPF_SUPER },
    /* 0xE4 */ { "branch_load",        2,  0,  1, R8E_OPF_SUPER | R8E_OPF_COND },
    /* 0xE5 */ { "store_local_pop",    1,  1,  0, R8E_OPF_SUPER | R8E_OPF_SIDE_EFFECT },
    /* 0xE6 */ { "load_local2",        2,  0,  2, R8E_OPF_SUPER },
    /* 0xE7-0xEF: reserved superinstructions */
    [0xE7 ... 0xEF] = { NULL, 0, 0, 0, 0 },

    /* 0xF0 */ { "eval_trap",          0,  0,  0, R8E_OPF_NONE },
    /* 0xF1 */ { "check_limits",       0,  0,  0, R8E_OPF_NONE },
    /* 0xF2-0xFD: reserved */
    [0xF2 ... 0xFD] = { NULL, 0, 0, 0, 0 },
    /* 0xFE */ { "nop",                0,  0,  0, R8E_OPF_NONE },
    /* 0xFF */ { "halt",               0,  0,  0, R8E_OPF_TERMINAL },
};

#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
#endif

#else
/* Declaration only: defined in the compilation unit that defines R8E_OPCODE_IMPL */
extern const R8EOpcodeInfo r8e_opcode_info[R8E_OPCODE_COUNT];
#endif /* R8E_OPCODE_IMPL */


/* =========================================================================
 * Convenience Accessors
 * ========================================================================= */

static inline const char *r8e_opcode_name(uint8_t op) {
    const char *name = r8e_opcode_info[op].name;
    return name ? name : "???";
}

static inline int r8e_opcode_operand_bytes(uint8_t op) {
    return r8e_opcode_info[op].operand_bytes;
}

static inline bool r8e_opcode_is_jump(uint8_t op) {
    return (r8e_opcode_info[op].flags & R8E_OPF_JUMP) != 0;
}

static inline bool r8e_opcode_is_super(uint8_t op) {
    return (r8e_opcode_info[op].flags & R8E_OPF_SUPER) != 0;
}

static inline bool r8e_opcode_is_terminal(uint8_t op) {
    return (r8e_opcode_info[op].flags & R8E_OPF_TERMINAL) != 0;
}


/* =========================================================================
 * Variable-Width Encoding Helpers (CLAUDE.md Section 6.1)
 *
 * Top 16 opcodes use a 4-bit prefix (half byte), covering 90% of dispatches.
 * Next 48 opcodes use 8-bit code, covering 9.8%.
 * Remaining use 16-bit code, covering 0.2%.
 *
 * The top-16 list is determined by profiling and stored here for the encoder.
 * ========================================================================= */

/* Top-16 opcodes (4-bit prefix, indices 0-15): most frequent by EXP-06 */
#define R8E_TOP16_COUNT  16

static const uint8_t r8e_top16_opcodes[R8E_TOP16_COUNT] = {
    R8E_OP_LOAD_LOCAL,        /*  0: most frequent */
    R8E_OP_STORE_LOCAL,       /*  1 */
    R8E_OP_LOAD_CONST,        /*  2 */
    R8E_OP_PUSH_INT8,         /*  3 */
    R8E_OP_GET_PROP,          /*  4 */
    R8E_OP_CALL,              /*  5 */
    R8E_OP_RETURN,            /*  6 */
    R8E_OP_JUMP_IF_FALSE8,    /*  7 */
    R8E_OP_ADD,               /*  8 */
    R8E_OP_LT,               /*  9 */
    R8E_OP_FOR_INC_LOOP,     /* 10: superinstruction */
    R8E_OP_CALL_METHOD_FAST, /* 11: superinstruction */
    R8E_OP_LOAD_LOCAL2,      /* 12: superinstruction */
    R8E_OP_JUMP8,            /* 13 */
    R8E_OP_SEQ,              /* 14 */
    R8E_OP_DROP,             /* 15 */
};

#ifdef __cplusplus
}
#endif

#endif /* R8E_OPCODES_H */
