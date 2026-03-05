/*
 * r8e_bc.c - Bytecode Buffer Management
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 6 (Bytecode Format and Superinstructions).
 *
 * This module provides the bytecode emission buffer used by the single-pass
 * parser/compiler. It manages:
 *   - Dynamic byte buffer for opcode/operand emission
 *   - Constant pool for literals (numbers, strings, functions)
 *   - Jump patching for forward references
 *   - Debug disassembly output
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "../include/r8e_types.h"
#define R8E_OPCODE_IMPL
#include "../include/r8e_opcodes.h"

/* =========================================================================
 * Bytecode Buffer Structure
 * ========================================================================= */

/* Initial and growth parameters */
#define R8E_BC_INIT_CAP       256
#define R8E_BC_CONST_INIT_CAP 32
#define R8E_BC_FUNC_INIT_CAP  8

typedef struct R8EBytecodeBuffer {
    uint8_t  *code;          /* bytecode bytes */
    uint32_t  length;        /* current byte count */
    uint32_t  capacity;      /* allocated byte capacity */

    R8EValue *constants;     /* constant pool (NaN-boxed values) */
    uint16_t  const_count;   /* number of constants */
    uint16_t  const_capacity;/* allocated constant slots */

    /* Line number table: maps bytecode offset to source line.
     * Stored as pairs of (offset, line). Sorted by offset. */
    uint32_t *line_table;    /* [offset0, line0, offset1, line1, ...] */
    uint16_t  line_count;    /* number of entries */
    uint16_t  line_capacity; /* allocated entry pairs */

    /* Nested function table: compiled inner functions */
    struct R8EFuncDesc {
        struct R8EBytecodeBuffer *bc;  /* inner function bytecode */
        uint32_t  name_atom;           /* function name atom (0 = anon) */
        uint16_t  param_count;
        uint16_t  local_count;
        uint16_t  capture_count;
        uint16_t  stack_size;
        uint8_t   is_strict;
        uint8_t   is_arrow;
        uint8_t   is_generator;
        uint8_t   is_async;
        uint16_t  source_line;
        /* Capture descriptors: which variables are captured from outer scope */
        struct R8ECaptureInfo {
            uint32_t atom;
            uint8_t  src_reg;     /* register in parent scope */
            uint8_t  src_depth;   /* scope depth relative to parent function */
            uint8_t  is_mutable;
        } *captures;
    } *functions;
    uint16_t  func_count;
    uint16_t  func_capacity;

    /* Maximum stack depth tracking for the verifier */
    uint16_t  max_stack;
    uint16_t  cur_stack;

} R8EBytecodeBuffer;

/* =========================================================================
 * Initialization / Destruction
 * ========================================================================= */

static int r8e_bc_init(R8EBytecodeBuffer *bc)
{
    memset(bc, 0, sizeof(*bc));

    bc->code = (uint8_t *)malloc(R8E_BC_INIT_CAP);
    if (!bc->code) return -1;
    bc->capacity = R8E_BC_INIT_CAP;

    bc->constants = (R8EValue *)malloc(R8E_BC_CONST_INIT_CAP * sizeof(R8EValue));
    if (!bc->constants) {
        free(bc->code);
        bc->code = NULL;
        return -1;
    }
    bc->const_capacity = R8E_BC_CONST_INIT_CAP;

    bc->functions = NULL;
    bc->func_count = 0;
    bc->func_capacity = 0;

    bc->line_table = NULL;
    bc->line_count = 0;
    bc->line_capacity = 0;

    return 0;
}

static void r8e_bc_destroy(R8EBytecodeBuffer *bc)
{
    if (!bc) return;

    free(bc->code);
    bc->code = NULL;

    free(bc->constants);
    bc->constants = NULL;

    /* Free nested function descriptors */
    for (uint16_t i = 0; i < bc->func_count; i++) {
        if (bc->functions[i].bc) {
            r8e_bc_destroy(bc->functions[i].bc);
            free(bc->functions[i].bc);
        }
        free(bc->functions[i].captures);
    }
    free(bc->functions);
    bc->functions = NULL;

    free(bc->line_table);
    bc->line_table = NULL;

    memset(bc, 0, sizeof(*bc));
}

/* =========================================================================
 * Buffer Growth
 * ========================================================================= */

static int r8e_bc_grow(R8EBytecodeBuffer *bc, uint32_t needed)
{
    if (bc->length + needed <= bc->capacity) return 0;

    uint32_t new_cap = bc->capacity;
    while (new_cap < bc->length + needed) {
        new_cap = new_cap < 4096 ? new_cap * 2 : new_cap + (new_cap / 2);
    }

    uint8_t *new_code = (uint8_t *)realloc(bc->code, new_cap);
    if (!new_code) return -1;

    bc->code = new_code;
    bc->capacity = new_cap;
    return 0;
}

/* =========================================================================
 * Emission Primitives
 * ========================================================================= */

static int r8e_bc_emit(R8EBytecodeBuffer *bc, uint8_t byte)
{
    if (r8e_bc_grow(bc, 1) != 0) return -1;
    bc->code[bc->length++] = byte;
    return 0;
}

static int r8e_bc_emit_u16(R8EBytecodeBuffer *bc, uint16_t val)
{
    if (r8e_bc_grow(bc, 2) != 0) return -1;
    bc->code[bc->length++] = (uint8_t)(val & 0xFF);
    bc->code[bc->length++] = (uint8_t)((val >> 8) & 0xFF);
    return 0;
}

static int r8e_bc_emit_u32(R8EBytecodeBuffer *bc, uint32_t val)
{
    if (r8e_bc_grow(bc, 4) != 0) return -1;
    bc->code[bc->length++] = (uint8_t)(val & 0xFF);
    bc->code[bc->length++] = (uint8_t)((val >> 8) & 0xFF);
    bc->code[bc->length++] = (uint8_t)((val >> 16) & 0xFF);
    bc->code[bc->length++] = (uint8_t)((val >> 24) & 0xFF);
    return 0;
}

static int r8e_bc_emit_i32(R8EBytecodeBuffer *bc, int32_t val)
{
    return r8e_bc_emit_u32(bc, (uint32_t)val);
}

static int r8e_bc_emit_i8(R8EBytecodeBuffer *bc, int8_t val)
{
    return r8e_bc_emit(bc, (uint8_t)val);
}

/* Emit an opcode byte */
static int r8e_bc_emit_op(R8EBytecodeBuffer *bc, uint8_t op)
{
    return r8e_bc_emit(bc, op);
}

/* Emit opcode + uint8 operand */
static int r8e_bc_emit_op_u8(R8EBytecodeBuffer *bc, uint8_t op, uint8_t val)
{
    if (r8e_bc_emit(bc, op) != 0) return -1;
    return r8e_bc_emit(bc, val);
}

/* Emit opcode + uint16 operand */
static int r8e_bc_emit_op_u16(R8EBytecodeBuffer *bc, uint8_t op, uint16_t val)
{
    if (r8e_bc_emit(bc, op) != 0) return -1;
    return r8e_bc_emit_u16(bc, val);
}

/* Emit opcode + uint32 operand */
static int r8e_bc_emit_op_u32(R8EBytecodeBuffer *bc, uint8_t op, uint32_t val)
{
    if (r8e_bc_emit(bc, op) != 0) return -1;
    return r8e_bc_emit_u32(bc, val);
}

/* =========================================================================
 * Stack Depth Tracking
 * ========================================================================= */

static void r8e_bc_adjust_stack(R8EBytecodeBuffer *bc, int delta)
{
    int new_depth = (int)bc->cur_stack + delta;
    if (new_depth < 0) new_depth = 0;  /* should not happen in correct code */
    bc->cur_stack = (uint16_t)new_depth;
    if (bc->cur_stack > bc->max_stack) {
        bc->max_stack = bc->cur_stack;
    }
}

/* =========================================================================
 * Current Offset
 * ========================================================================= */

static uint32_t r8e_bc_current_offset(const R8EBytecodeBuffer *bc)
{
    return bc->length;
}

/* =========================================================================
 * Jump Emission and Patching
 *
 * Jumps are emitted as opcode + 32-bit signed offset (relative to the
 * instruction AFTER the jump operand). The emit_jump function returns the
 * offset within the bytecode buffer where the 32-bit offset is stored,
 * so that patch_jump can fill it in later.
 * ========================================================================= */

/* Emit a jump opcode with a placeholder offset. Returns the patch address. */
static uint32_t r8e_bc_emit_jump(R8EBytecodeBuffer *bc, uint8_t op)
{
    r8e_bc_emit(bc, op);
    uint32_t patch_addr = bc->length;
    r8e_bc_emit_u32(bc, 0); /* placeholder */
    return patch_addr;
}

/* Emit a short jump (8-bit offset). Returns patch address. */
static uint32_t r8e_bc_emit_jump8(R8EBytecodeBuffer *bc, uint8_t op)
{
    r8e_bc_emit(bc, op);
    uint32_t patch_addr = bc->length;
    r8e_bc_emit(bc, 0); /* placeholder */
    return patch_addr;
}

/* Emit a medium jump (16-bit offset). Returns patch address. */
static uint32_t r8e_bc_emit_jump16(R8EBytecodeBuffer *bc, uint8_t op)
{
    r8e_bc_emit(bc, op);
    uint32_t patch_addr = bc->length;
    r8e_bc_emit_u16(bc, 0); /* placeholder */
    return patch_addr;
}

/* Patch a 32-bit jump target. offset is relative to the instruction after
 * the jump operand. */
static void r8e_bc_patch_jump(R8EBytecodeBuffer *bc, uint32_t patch_addr)
{
    int32_t offset = (int32_t)(bc->length - (patch_addr + 4));
    bc->code[patch_addr + 0] = (uint8_t)(((uint32_t)offset) & 0xFF);
    bc->code[patch_addr + 1] = (uint8_t)(((uint32_t)offset >> 8) & 0xFF);
    bc->code[patch_addr + 2] = (uint8_t)(((uint32_t)offset >> 16) & 0xFF);
    bc->code[patch_addr + 3] = (uint8_t)(((uint32_t)offset >> 24) & 0xFF);
}

/* Patch a 16-bit jump target */
static void r8e_bc_patch_jump16(R8EBytecodeBuffer *bc, uint32_t patch_addr)
{
    int16_t offset = (int16_t)(bc->length - (patch_addr + 2));
    bc->code[patch_addr + 0] = (uint8_t)(((uint16_t)offset) & 0xFF);
    bc->code[patch_addr + 1] = (uint8_t)(((uint16_t)offset >> 8) & 0xFF);
}

/* Patch a raw 16-bit value at a given address */
static void r8e_bc_patch_u16(R8EBytecodeBuffer *bc, uint32_t addr, uint16_t val)
{
    if (addr + 1 < bc->length) {
        bc->code[addr + 0] = (uint8_t)(val & 0xFF);
        bc->code[addr + 1] = (uint8_t)((val >> 8) & 0xFF);
    }
}

/* Patch a raw 32-bit value at a given address */
static void r8e_bc_patch_u32(R8EBytecodeBuffer *bc, uint32_t addr, uint32_t val)
{
    if (addr + 3 < bc->length) {
        bc->code[addr + 0] = (uint8_t)(val & 0xFF);
        bc->code[addr + 1] = (uint8_t)((val >> 8) & 0xFF);
        bc->code[addr + 2] = (uint8_t)((val >> 16) & 0xFF);
        bc->code[addr + 3] = (uint8_t)((val >> 24) & 0xFF);
    }
}

/* Read a 32-bit signed value at an offset */
static int32_t r8e_bc_read_i32(const R8EBytecodeBuffer *bc, uint32_t addr)
{
    uint32_t v = (uint32_t)bc->code[addr]
               | ((uint32_t)bc->code[addr + 1] << 8)
               | ((uint32_t)bc->code[addr + 2] << 16)
               | ((uint32_t)bc->code[addr + 3] << 24);
    return (int32_t)v;
}

/* Read a 16-bit unsigned value at an offset */
static uint16_t r8e_bc_read_u16(const R8EBytecodeBuffer *bc, uint32_t addr)
{
    return (uint16_t)bc->code[addr]
         | ((uint16_t)bc->code[addr + 1] << 8);
}

/* =========================================================================
 * Constant Pool
 * ========================================================================= */

/* Add a constant to the pool. Returns the constant index, or -1 on error. */
static int r8e_bc_add_constant(R8EBytecodeBuffer *bc, R8EValue val)
{
    /* Check for duplicate constants (small pool optimization) */
    for (uint16_t i = 0; i < bc->const_count; i++) {
        if (bc->constants[i] == val) return (int)i;
    }

    if (bc->const_count >= bc->const_capacity) {
        uint16_t new_cap = bc->const_capacity < 256
                         ? bc->const_capacity * 2
                         : bc->const_capacity + 128;
        R8EValue *new_consts = (R8EValue *)realloc(
            bc->constants, (size_t)new_cap * sizeof(R8EValue));
        if (!new_consts) return -1;
        bc->constants = new_consts;
        bc->const_capacity = new_cap;
    }

    int idx = (int)bc->const_count;
    bc->constants[bc->const_count++] = val;
    return idx;
}

/* =========================================================================
 * Nested Function Registration
 * ========================================================================= */

static int r8e_bc_add_function(R8EBytecodeBuffer *bc)
{
    if (bc->func_count >= bc->func_capacity) {
        uint16_t new_cap = bc->func_capacity == 0
                         ? R8E_BC_FUNC_INIT_CAP
                         : bc->func_capacity * 2;
        struct R8EFuncDesc *new_funcs = (struct R8EFuncDesc *)realloc(
            bc->functions, (size_t)new_cap * sizeof(struct R8EFuncDesc));
        if (!new_funcs) return -1;
        bc->functions = new_funcs;
        bc->func_capacity = new_cap;
    }

    int idx = (int)bc->func_count;
    memset(&bc->functions[idx], 0, sizeof(struct R8EFuncDesc));
    bc->func_count++;
    return idx;
}

/* =========================================================================
 * Line Number Table
 * ========================================================================= */

static void r8e_bc_add_line(R8EBytecodeBuffer *bc, uint32_t line)
{
    /* Only add if line changed from last entry */
    if (bc->line_count > 0) {
        uint32_t last_line = bc->line_table[(bc->line_count - 1) * 2 + 1];
        if (last_line == line) return;
    }

    if (bc->line_count >= bc->line_capacity) {
        uint16_t new_cap = bc->line_capacity == 0 ? 32 : bc->line_capacity * 2;
        uint32_t *new_tbl = (uint32_t *)realloc(
            bc->line_table, (size_t)new_cap * 2 * sizeof(uint32_t));
        if (!new_tbl) return;
        bc->line_table = new_tbl;
        bc->line_capacity = new_cap;
    }

    uint32_t idx = bc->line_count * 2;
    bc->line_table[idx]     = bc->length;   /* bytecode offset */
    bc->line_table[idx + 1] = line;         /* source line */
    bc->line_count++;
}

/* =========================================================================
 * Debug Disassembly
 * ========================================================================= */

static void r8e_bc_dump(const R8EBytecodeBuffer *bc)
{
    fprintf(stderr, "=== Bytecode Dump (%u bytes, %u constants, %u functions) ===\n",
            bc->length, bc->const_count, bc->func_count);

    uint32_t pc = 0;
    while (pc < bc->length) {
        uint8_t op = bc->code[pc];
        const R8EOpcodeInfo *info = &r8e_opcode_info[op];
        const char *name = info->name ? info->name : "???";

        fprintf(stderr, "  %04u: %-20s", pc, name);

        pc++;

        /* Print operands based on operand_bytes */
        int op_bytes = info->operand_bytes;
        if (op_bytes == 1 && pc < bc->length) {
            fprintf(stderr, " %u", bc->code[pc]);
            pc++;
        } else if (op_bytes == 2 && pc + 1 < bc->length) {
            uint16_t v = (uint16_t)bc->code[pc] | ((uint16_t)bc->code[pc + 1] << 8);
            fprintf(stderr, " %u", v);
            pc += 2;
        } else if (op_bytes == 4 && pc + 3 < bc->length) {
            uint32_t v = (uint32_t)bc->code[pc]
                       | ((uint32_t)bc->code[pc + 1] << 8)
                       | ((uint32_t)bc->code[pc + 2] << 16)
                       | ((uint32_t)bc->code[pc + 3] << 24);
            /* If it is a jump, show as signed offset */
            if (info->flags & R8E_OPF_JUMP) {
                int32_t offset = (int32_t)v;
                fprintf(stderr, " %+d -> %u", offset, (uint32_t)((int32_t)pc + 4 + offset));
            } else {
                fprintf(stderr, " %u", v);
            }
            pc += 4;
        } else if (op_bytes == 5 && pc + 4 < bc->length) {
            /* e.g. define_prop: atom(4) + flags(1) */
            uint32_t atom = (uint32_t)bc->code[pc]
                          | ((uint32_t)bc->code[pc + 1] << 8)
                          | ((uint32_t)bc->code[pc + 2] << 16)
                          | ((uint32_t)bc->code[pc + 3] << 24);
            uint8_t flags = bc->code[pc + 4];
            fprintf(stderr, " atom=%u flags=0x%02x", atom, flags);
            pc += 5;
        } else if (op_bytes == 6 && pc + 5 < bc->length) {
            /* e.g. loop_cmp_branch: reg(1) + imm(4) + off(1) */
            fprintf(stderr, " reg=%u imm=%d off=%d",
                    bc->code[pc],
                    (int32_t)((uint32_t)bc->code[pc + 1]
                            | ((uint32_t)bc->code[pc + 2] << 8)
                            | ((uint32_t)bc->code[pc + 3] << 16)
                            | ((uint32_t)bc->code[pc + 4] << 24)),
                    (int8_t)bc->code[pc + 5]);
            pc += 6;
        } else {
            pc += op_bytes;
        }

        fprintf(stderr, "\n");
    }

    /* Dump constants */
    if (bc->const_count > 0) {
        fprintf(stderr, "--- Constants (%u) ---\n", bc->const_count);
        for (uint16_t i = 0; i < bc->const_count; i++) {
            R8EValue v = bc->constants[i];
            fprintf(stderr, "  [%u]: ", i);
            if (R8E_IS_INT32(v)) {
                fprintf(stderr, "int32 %d\n", r8e_get_int32(v));
            } else if (R8E_IS_DOUBLE(v)) {
                fprintf(stderr, "double %g\n", r8e_get_double(v));
            } else if (R8E_IS_INLINE_STR(v)) {
                char buf[8];
                int len = r8e_inline_str_decode(v, buf);
                fprintf(stderr, "inline_str \"%.*s\"\n", len, buf);
            } else if (R8E_IS_UNDEFINED(v)) {
                fprintf(stderr, "undefined\n");
            } else if (R8E_IS_NULL(v)) {
                fprintf(stderr, "null\n");
            } else if (R8E_IS_ATOM(v)) {
                fprintf(stderr, "atom #%u\n", r8e_get_atom(v));
            } else {
                fprintf(stderr, "0x%016llx\n", (unsigned long long)v);
            }
        }
    }

    /* Dump nested functions */
    if (bc->func_count > 0) {
        fprintf(stderr, "--- Nested Functions (%u) ---\n", bc->func_count);
        for (uint16_t i = 0; i < bc->func_count; i++) {
            const struct R8EFuncDesc *fd = &bc->functions[i];
            fprintf(stderr, "  [%u]: name_atom=%u params=%u locals=%u captures=%u\n",
                    i, fd->name_atom, fd->param_count,
                    fd->local_count, fd->capture_count);
            if (fd->bc) {
                r8e_bc_dump(fd->bc);
            }
        }
    }

    fprintf(stderr, "=== End Bytecode (max_stack=%u) ===\n", bc->max_stack);
}
