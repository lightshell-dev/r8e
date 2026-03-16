/*
 * test_bc.c - Unit tests for r8e_bc.c (Bytecode Buffer Management)
 *
 * Tests cover:
 *   - Buffer initialization and destruction
 *   - Byte/u16/u32/i32/i8 emission primitives
 *   - Buffer growth and capacity management
 *   - Opcode emission (op, op_u8, op_u16, op_u32)
 *   - Stack depth tracking (adjust_stack, max_stack)
 *   - Jump emission and patching (8-bit, 16-bit, 32-bit)
 *   - Constant pool (add, deduplication, growth)
 *   - Nested function registration
 *   - Line number table (add, dedup same line)
 *   - Raw value patching (patch_u16, patch_u32)
 *   - Value reading (read_i32, read_u16)
 *   - Current offset tracking
 *   - Bytecode dump (smoke test)
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

/* Include the implementation directly to test static functions */
#include "../../src/r8e_bc.c"

/* =========================================================================
 * Test Harness
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { if (!(e)) { printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #e); g_assert_fail = 1; return; } } while(0)
#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a,b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a,b) ASSERT_TRUE((a) != (b))
#define ASSERT_EQ_INT(a,b) do { int _a=(int)(a), _b=(int)(b); if (_a != _b) { printf("  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); g_assert_fail = 1; return; } } while(0)
#define RUN_TEST(fn) do { g_assert_fail = 0; g_tests_run++; printf("  %s... ", #fn); fn(); if (g_assert_fail) { g_tests_failed++; } else { g_tests_passed++; printf("OK\n"); } } while(0)

/* =========================================================================
 * Tests: Buffer Initialization and Destruction
 * ========================================================================= */

static void test_bc_init_success(void) {
    R8EBytecodeBuffer bc;
    int rc = r8e_bc_init(&bc);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(bc.code != NULL);
    ASSERT_TRUE(bc.capacity > 0);
    ASSERT_EQ_INT(bc.length, 0);
    ASSERT_TRUE(bc.constants != NULL);
    ASSERT_EQ_INT(bc.const_count, 0);
    ASSERT_TRUE(bc.const_capacity > 0);
    ASSERT_EQ_INT(bc.func_count, 0);
    ASSERT_EQ_INT(bc.line_count, 0);
    ASSERT_EQ_INT(bc.max_stack, 0);
    ASSERT_EQ_INT(bc.cur_stack, 0);
    r8e_bc_destroy(&bc);
}

static void test_bc_destroy_null_safe(void) {
    /* Passing NULL should not crash */
    r8e_bc_destroy(NULL);
    /* If we reach here, no crash occurred */
    ASSERT_TRUE(1);
}

static void test_bc_destroy_clears_fields(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Emit some data so fields are non-zero */
    r8e_bc_emit(&bc, 0x42);
    r8e_bc_add_constant(&bc, r8e_from_int32(99));
    r8e_bc_destroy(&bc);
    ASSERT_TRUE(bc.code == NULL);
    ASSERT_TRUE(bc.constants == NULL);
    ASSERT_EQ_INT(bc.length, 0);
    ASSERT_EQ_INT(bc.const_count, 0);
}

/* =========================================================================
 * Tests: Emission Primitives
 * ========================================================================= */

static void test_bc_emit_single_byte(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit(&bc, 0xAB);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 1);
    ASSERT_EQ_INT(bc.code[0], 0xAB);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_multiple_bytes(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit(&bc, 0x01);
    r8e_bc_emit(&bc, 0x02);
    r8e_bc_emit(&bc, 0x03);
    ASSERT_EQ_INT(bc.length, 3);
    ASSERT_EQ_INT(bc.code[0], 0x01);
    ASSERT_EQ_INT(bc.code[1], 0x02);
    ASSERT_EQ_INT(bc.code[2], 0x03);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_u16_little_endian(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_u16(&bc, 0xBEEF);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 2);
    /* Little-endian: low byte first */
    ASSERT_EQ_INT(bc.code[0], 0xEF);
    ASSERT_EQ_INT(bc.code[1], 0xBE);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_u32_little_endian(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_u32(&bc, 0xDEADBEEF);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 4);
    ASSERT_EQ_INT(bc.code[0], 0xEF);
    ASSERT_EQ_INT(bc.code[1], 0xBE);
    ASSERT_EQ_INT(bc.code[2], 0xAD);
    ASSERT_EQ_INT(bc.code[3], 0xDE);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_i32_negative(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_i32(&bc, -1);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 4);
    /* -1 as uint32 = 0xFFFFFFFF, little endian */
    ASSERT_EQ_INT(bc.code[0], 0xFF);
    ASSERT_EQ_INT(bc.code[1], 0xFF);
    ASSERT_EQ_INT(bc.code[2], 0xFF);
    ASSERT_EQ_INT(bc.code[3], 0xFF);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_i8(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_i8(&bc, -42);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 1);
    /* -42 as uint8 = 0xD6 */
    ASSERT_EQ_INT(bc.code[0], (uint8_t)(int8_t)-42);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_u16_zero(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_u16(&bc, 0);
    ASSERT_EQ_INT(bc.length, 2);
    ASSERT_EQ_INT(bc.code[0], 0);
    ASSERT_EQ_INT(bc.code[1], 0);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_u16_max(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_u16(&bc, 0xFFFF);
    ASSERT_EQ_INT(bc.length, 2);
    ASSERT_EQ_INT(bc.code[0], 0xFF);
    ASSERT_EQ_INT(bc.code[1], 0xFF);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Opcode Emission Helpers
 * ========================================================================= */

static void test_bc_emit_op(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_op(&bc, R8E_OP_ADD);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 1);
    ASSERT_EQ_INT(bc.code[0], R8E_OP_ADD);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_op_u8(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_op_u8(&bc, R8E_OP_LOAD_LOCAL, 5);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 2);
    ASSERT_EQ_INT(bc.code[0], R8E_OP_LOAD_LOCAL);
    ASSERT_EQ_INT(bc.code[1], 5);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_op_u16(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_op_u16(&bc, R8E_OP_LOAD_CONST, 300);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 3);
    ASSERT_EQ_INT(bc.code[0], R8E_OP_LOAD_CONST);
    /* 300 = 0x012C, little endian */
    ASSERT_EQ_INT(bc.code[1], 0x2C);
    ASSERT_EQ_INT(bc.code[2], 0x01);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_op_u32(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int rc = r8e_bc_emit_op_u32(&bc, R8E_OP_LOAD_GLOBAL, 0x00010203);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.length, 5);
    ASSERT_EQ_INT(bc.code[0], R8E_OP_LOAD_GLOBAL);
    ASSERT_EQ_INT(bc.code[1], 0x03);
    ASSERT_EQ_INT(bc.code[2], 0x02);
    ASSERT_EQ_INT(bc.code[3], 0x01);
    ASSERT_EQ_INT(bc.code[4], 0x00);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Buffer Growth
 * ========================================================================= */

static void test_bc_grow_no_growth_needed(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t initial_cap = bc.capacity;
    /* Growth not needed for small emit */
    int rc = r8e_bc_grow(&bc, 1);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(bc.capacity, (int)initial_cap);
    r8e_bc_destroy(&bc);
}

static void test_bc_grow_doubles_capacity(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t initial_cap = bc.capacity;
    /* Force growth beyond initial capacity */
    int rc = r8e_bc_grow(&bc, initial_cap + 1);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(bc.capacity >= initial_cap + 1);
    /* Capacity should have grown */
    ASSERT_TRUE(bc.capacity > initial_cap);
    r8e_bc_destroy(&bc);
}

static void test_bc_emit_triggers_growth(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t initial_cap = bc.capacity;
    /* Emit enough bytes to exceed initial capacity */
    for (uint32_t i = 0; i < initial_cap + 10; i++) {
        int rc = r8e_bc_emit(&bc, (uint8_t)(i & 0xFF));
        ASSERT_EQ_INT(rc, 0);
    }
    ASSERT_TRUE(bc.capacity > initial_cap);
    ASSERT_EQ_INT(bc.length, (int)(initial_cap + 10));
    /* Verify data integrity after growth */
    ASSERT_EQ_INT(bc.code[0], 0);
    ASSERT_EQ_INT(bc.code[1], 1);
    ASSERT_EQ_INT(bc.code[initial_cap], (int)(initial_cap & 0xFF));
    r8e_bc_destroy(&bc);
}

static void test_bc_grow_large_request(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Request a large growth - should use the incremental strategy (cap + cap/2) */
    int rc = r8e_bc_grow(&bc, 10000);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(bc.capacity >= 10000);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Current Offset
 * ========================================================================= */

static void test_bc_current_offset_initial(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    ASSERT_EQ_INT(r8e_bc_current_offset(&bc), 0);
    r8e_bc_destroy(&bc);
}

static void test_bc_current_offset_after_emit(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit(&bc, 0x01);
    ASSERT_EQ_INT(r8e_bc_current_offset(&bc), 1);
    r8e_bc_emit_u16(&bc, 0x1234);
    ASSERT_EQ_INT(r8e_bc_current_offset(&bc), 3);
    r8e_bc_emit_u32(&bc, 0x12345678);
    ASSERT_EQ_INT(r8e_bc_current_offset(&bc), 7);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Stack Depth Tracking
 * ========================================================================= */

static void test_bc_adjust_stack_positive(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_adjust_stack(&bc, 3);
    ASSERT_EQ_INT(bc.cur_stack, 3);
    ASSERT_EQ_INT(bc.max_stack, 3);
    r8e_bc_destroy(&bc);
}

static void test_bc_adjust_stack_negative(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_adjust_stack(&bc, 5);
    r8e_bc_adjust_stack(&bc, -2);
    ASSERT_EQ_INT(bc.cur_stack, 3);
    ASSERT_EQ_INT(bc.max_stack, 5);
    r8e_bc_destroy(&bc);
}

static void test_bc_adjust_stack_tracks_max(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_adjust_stack(&bc, 10);
    r8e_bc_adjust_stack(&bc, -5);
    r8e_bc_adjust_stack(&bc, 3);
    /* max should be 10, cur should be 8 */
    ASSERT_EQ_INT(bc.max_stack, 10);
    ASSERT_EQ_INT(bc.cur_stack, 8);
    r8e_bc_destroy(&bc);
}

static void test_bc_adjust_stack_clamps_at_zero(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_adjust_stack(&bc, 2);
    r8e_bc_adjust_stack(&bc, -10); /* underflow should clamp to 0 */
    ASSERT_EQ_INT(bc.cur_stack, 0);
    ASSERT_EQ_INT(bc.max_stack, 2);
    r8e_bc_destroy(&bc);
}

static void test_bc_adjust_stack_max_monotonic(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_adjust_stack(&bc, 20);
    r8e_bc_adjust_stack(&bc, -20);
    r8e_bc_adjust_stack(&bc, 5);
    /* max should still be 20, not 5 */
    ASSERT_EQ_INT(bc.max_stack, 20);
    ASSERT_EQ_INT(bc.cur_stack, 5);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Jump Emission and Patching (32-bit)
 * ========================================================================= */

static void test_bc_emit_jump_returns_patch_addr(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP);
    /* Opcode at offset 0, patch address at offset 1 (4 bytes of placeholder) */
    ASSERT_EQ_INT(patch, 1);
    ASSERT_EQ_INT(bc.length, 5);
    ASSERT_EQ_INT(bc.code[0], R8E_OP_JUMP);
    /* Placeholder should be zeros */
    ASSERT_EQ_INT(bc.code[1], 0);
    ASSERT_EQ_INT(bc.code[2], 0);
    ASSERT_EQ_INT(bc.code[3], 0);
    ASSERT_EQ_INT(bc.code[4], 0);
    r8e_bc_destroy(&bc);
}

static void test_bc_patch_jump_forward(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Emit a jump instruction */
    uint32_t patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP_IF_FALSE);
    /* Emit some filler bytes to create a forward offset */
    r8e_bc_emit(&bc, R8E_OP_PUSH_TRUE);
    r8e_bc_emit(&bc, R8E_OP_DROP);
    r8e_bc_emit(&bc, R8E_OP_PUSH_FALSE);
    /* Now patch the jump to land here */
    r8e_bc_patch_jump(&bc, patch);
    /* The offset should be: bc.length - (patch + 4)
     * bc.length = 8, patch = 1, so offset = 8 - 5 = 3 */
    int32_t offset = r8e_bc_read_i32(&bc, patch);
    ASSERT_EQ_INT(offset, 3);
    r8e_bc_destroy(&bc);
}

static void test_bc_patch_jump_backward(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Emit some code first */
    r8e_bc_emit(&bc, R8E_OP_PUSH_ZERO);  /* offset 0 */
    r8e_bc_emit(&bc, R8E_OP_PUSH_ONE);   /* offset 1 */
    /* Emit the jump at current offset */
    uint32_t patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP);
    /* patch is at offset 3 */
    /* Now "patch" it so it points back to some earlier position
     * by manipulating bc.length to simulate where we want to jump.
     * Actually, patch_jump uses bc.length as destination, which is
     * the END of the buffer. We need a different approach for backward
     * jumps: manually write the offset.
     * Let's just verify the read/write roundtrip. */

    /* Write a known negative offset: jump back 10 bytes */
    r8e_bc_patch_u32(&bc, patch, (uint32_t)(int32_t)(-10));
    int32_t offset = r8e_bc_read_i32(&bc, patch);
    ASSERT_EQ_INT(offset, -10);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Jump Emission and Patching (8-bit)
 * ========================================================================= */

static void test_bc_emit_jump8(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t patch = r8e_bc_emit_jump8(&bc, R8E_OP_JUMP);
    /* Opcode at offset 0, 1-byte placeholder at offset 1 */
    ASSERT_EQ_INT(patch, 1);
    ASSERT_EQ_INT(bc.length, 2);
    ASSERT_EQ_INT(bc.code[1], 0); /* placeholder */
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Jump Emission and Patching (16-bit)
 * ========================================================================= */

static void test_bc_emit_jump16(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t patch = r8e_bc_emit_jump16(&bc, R8E_OP_JUMP_IF_FALSE);
    ASSERT_EQ_INT(patch, 1);
    ASSERT_EQ_INT(bc.length, 3);
    /* 2-byte placeholder should be zeros */
    ASSERT_EQ_INT(bc.code[1], 0);
    ASSERT_EQ_INT(bc.code[2], 0);
    r8e_bc_destroy(&bc);
}

static void test_bc_patch_jump16(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint32_t patch = r8e_bc_emit_jump16(&bc, R8E_OP_JUMP_IF_TRUE);
    /* Emit 5 filler bytes */
    for (int i = 0; i < 5; i++) r8e_bc_emit(&bc, R8E_OP_DROP);
    r8e_bc_patch_jump16(&bc, patch);
    /* offset = bc.length - (patch + 2) = 8 - 3 = 5 */
    uint16_t val = r8e_bc_read_u16(&bc, patch);
    ASSERT_EQ_INT((int16_t)val, 5);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Raw Value Patching
 * ========================================================================= */

static void test_bc_patch_u16(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Emit 4 bytes so we have room */
    r8e_bc_emit_u32(&bc, 0);
    r8e_bc_patch_u16(&bc, 0, 0xCAFE);
    ASSERT_EQ_INT(bc.code[0], 0xFE);
    ASSERT_EQ_INT(bc.code[1], 0xCA);
    r8e_bc_destroy(&bc);
}

static void test_bc_patch_u32(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_u32(&bc, 0);
    r8e_bc_patch_u32(&bc, 0, 0x12345678);
    ASSERT_EQ_INT(bc.code[0], 0x78);
    ASSERT_EQ_INT(bc.code[1], 0x56);
    ASSERT_EQ_INT(bc.code[2], 0x34);
    ASSERT_EQ_INT(bc.code[3], 0x12);
    r8e_bc_destroy(&bc);
}

static void test_bc_patch_u16_out_of_bounds(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit(&bc, 0x42);
    /* addr + 1 >= length, so the patch should be silently ignored */
    r8e_bc_patch_u16(&bc, 0, 0xBEEF);
    /* Original byte should be unchanged because addr+1 is out of bounds */
    ASSERT_EQ_INT(bc.code[0], 0x42);
    r8e_bc_destroy(&bc);
}

static void test_bc_patch_u32_out_of_bounds(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_u16(&bc, 0x1234);
    /* Only 2 bytes in buffer, patching u32 at addr 0 needs 4 bytes => out of bounds */
    r8e_bc_patch_u32(&bc, 0, 0xDEADBEEF);
    /* Original bytes should be unchanged */
    ASSERT_EQ_INT(bc.code[0], 0x34);
    ASSERT_EQ_INT(bc.code[1], 0x12);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Read Helpers
 * ========================================================================= */

static void test_bc_read_i32(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_i32(&bc, -12345);
    int32_t val = r8e_bc_read_i32(&bc, 0);
    ASSERT_EQ_INT(val, -12345);
    r8e_bc_destroy(&bc);
}

static void test_bc_read_u16(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_u16(&bc, 54321);
    uint16_t val = r8e_bc_read_u16(&bc, 0);
    ASSERT_EQ_INT(val, 54321);
    r8e_bc_destroy(&bc);
}

static void test_bc_read_i32_positive(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_i32(&bc, 2147483647); /* INT32_MAX */
    int32_t val = r8e_bc_read_i32(&bc, 0);
    ASSERT_EQ_INT(val, 2147483647);
    r8e_bc_destroy(&bc);
}

static void test_bc_read_i32_at_offset(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_u32(&bc, 0x11111111);
    r8e_bc_emit_i32(&bc, 42);
    int32_t val = r8e_bc_read_i32(&bc, 4);
    ASSERT_EQ_INT(val, 42);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Constant Pool
 * ========================================================================= */

static void test_bc_add_constant_first(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    R8EValue v = r8e_from_int32(42);
    int idx = r8e_bc_add_constant(&bc, v);
    ASSERT_EQ_INT(idx, 0);
    ASSERT_EQ_INT(bc.const_count, 1);
    ASSERT_TRUE(bc.constants[0] == v);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_constant_multiple(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int idx0 = r8e_bc_add_constant(&bc, r8e_from_int32(10));
    int idx1 = r8e_bc_add_constant(&bc, r8e_from_int32(20));
    int idx2 = r8e_bc_add_constant(&bc, r8e_from_int32(30));
    ASSERT_EQ_INT(idx0, 0);
    ASSERT_EQ_INT(idx1, 1);
    ASSERT_EQ_INT(idx2, 2);
    ASSERT_EQ_INT(bc.const_count, 3);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_constant_dedup(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    R8EValue v = r8e_from_int32(99);
    int idx0 = r8e_bc_add_constant(&bc, v);
    int idx1 = r8e_bc_add_constant(&bc, v);
    /* Same value should return the same index */
    ASSERT_EQ_INT(idx0, idx1);
    ASSERT_EQ_INT(bc.const_count, 1);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_constant_dedup_different_types(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* int32 42 and double 42.0 have different NaN-box encodings */
    int idx_int = r8e_bc_add_constant(&bc, r8e_from_int32(42));
    int idx_dbl = r8e_bc_add_constant(&bc, r8e_from_double(42.0));
    /* They should be different constants (different bit patterns) */
    ASSERT_NE(idx_int, idx_dbl);
    ASSERT_EQ_INT(bc.const_count, 2);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_constant_pool_growth(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    uint16_t initial_cap = bc.const_capacity;
    /* Add more constants than initial capacity to force growth */
    for (int i = 0; i < (int)initial_cap + 5; i++) {
        int idx = r8e_bc_add_constant(&bc, r8e_from_int32(i));
        ASSERT_EQ_INT(idx, i);
    }
    ASSERT_TRUE(bc.const_capacity > initial_cap);
    ASSERT_EQ_INT(bc.const_count, (int)(initial_cap + 5));
    r8e_bc_destroy(&bc);
}

static void test_bc_add_constant_special_values(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int idx_undef = r8e_bc_add_constant(&bc, R8E_UNDEFINED);
    int idx_null  = r8e_bc_add_constant(&bc, R8E_NULL);
    int idx_true  = r8e_bc_add_constant(&bc, R8E_TRUE);
    int idx_false = r8e_bc_add_constant(&bc, R8E_FALSE);
    ASSERT_EQ_INT(idx_undef, 0);
    ASSERT_EQ_INT(idx_null, 1);
    ASSERT_EQ_INT(idx_true, 2);
    ASSERT_EQ_INT(idx_false, 3);
    ASSERT_EQ_INT(bc.const_count, 4);
    /* Dedup: adding same value again */
    ASSERT_EQ_INT(r8e_bc_add_constant(&bc, R8E_UNDEFINED), 0);
    ASSERT_EQ_INT(bc.const_count, 4);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_constant_inline_string(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    R8EValue s = r8e_from_inline_str("hello", 5);
    int idx = r8e_bc_add_constant(&bc, s);
    ASSERT_EQ_INT(idx, 0);
    /* Adding the same string again should dedup */
    int idx2 = r8e_bc_add_constant(&bc, s);
    ASSERT_EQ_INT(idx2, 0);
    ASSERT_EQ_INT(bc.const_count, 1);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Nested Function Registration
 * ========================================================================= */

static void test_bc_add_function_first(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int idx = r8e_bc_add_function(&bc);
    ASSERT_EQ_INT(idx, 0);
    ASSERT_EQ_INT(bc.func_count, 1);
    /* The function descriptor should be zero-initialized */
    ASSERT_TRUE(bc.functions[0].bc == NULL);
    ASSERT_EQ_INT(bc.functions[0].name_atom, 0);
    ASSERT_EQ_INT(bc.functions[0].param_count, 0);
    ASSERT_EQ_INT(bc.functions[0].is_arrow, 0);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_function_multiple(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int idx0 = r8e_bc_add_function(&bc);
    int idx1 = r8e_bc_add_function(&bc);
    int idx2 = r8e_bc_add_function(&bc);
    ASSERT_EQ_INT(idx0, 0);
    ASSERT_EQ_INT(idx1, 1);
    ASSERT_EQ_INT(idx2, 2);
    ASSERT_EQ_INT(bc.func_count, 3);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_function_growth(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Add more functions than initial capacity (8) */
    for (int i = 0; i < 20; i++) {
        int idx = r8e_bc_add_function(&bc);
        ASSERT_EQ_INT(idx, i);
    }
    ASSERT_EQ_INT(bc.func_count, 20);
    ASSERT_TRUE(bc.func_capacity >= 20);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_function_set_metadata(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    int idx = r8e_bc_add_function(&bc);
    ASSERT_EQ_INT(idx, 0);
    /* Set some metadata */
    bc.functions[0].name_atom = 42;
    bc.functions[0].param_count = 3;
    bc.functions[0].is_arrow = 1;
    bc.functions[0].is_async = 1;
    bc.functions[0].source_line = 10;
    ASSERT_EQ_INT(bc.functions[0].name_atom, 42);
    ASSERT_EQ_INT(bc.functions[0].param_count, 3);
    ASSERT_EQ_INT(bc.functions[0].is_arrow, 1);
    ASSERT_EQ_INT(bc.functions[0].is_async, 1);
    ASSERT_EQ_INT(bc.functions[0].source_line, 10);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Line Number Table
 * ========================================================================= */

static void test_bc_add_line_first(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_add_line(&bc, 1);
    ASSERT_EQ_INT(bc.line_count, 1);
    /* line_table[0] = bytecode offset (0), line_table[1] = line number (1) */
    ASSERT_EQ_INT(bc.line_table[0], 0);
    ASSERT_EQ_INT(bc.line_table[1], 1);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_line_dedup_same_line(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_add_line(&bc, 5);
    r8e_bc_emit(&bc, R8E_OP_ADD);
    r8e_bc_add_line(&bc, 5); /* same line, should be deduplicated */
    ASSERT_EQ_INT(bc.line_count, 1);
    r8e_bc_destroy(&bc);
}

static void test_bc_add_line_different_lines(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_add_line(&bc, 1);
    r8e_bc_emit(&bc, R8E_OP_ADD);
    r8e_bc_add_line(&bc, 2);
    r8e_bc_emit(&bc, R8E_OP_SUB);
    r8e_bc_add_line(&bc, 3);
    ASSERT_EQ_INT(bc.line_count, 3);
    /* Check offsets and lines */
    ASSERT_EQ_INT(bc.line_table[0], 0); /* offset for line 1 */
    ASSERT_EQ_INT(bc.line_table[1], 1); /* line 1 */
    ASSERT_EQ_INT(bc.line_table[2], 1); /* offset for line 2 (after 1 byte) */
    ASSERT_EQ_INT(bc.line_table[3], 2); /* line 2 */
    ASSERT_EQ_INT(bc.line_table[4], 2); /* offset for line 3 (after 2 bytes) */
    ASSERT_EQ_INT(bc.line_table[5], 3); /* line 3 */
    r8e_bc_destroy(&bc);
}

static void test_bc_add_line_growth(void) {
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    /* Add many line entries to force line_table growth */
    for (uint32_t i = 1; i <= 100; i++) {
        r8e_bc_emit(&bc, R8E_OP_DROP);
        r8e_bc_add_line(&bc, i);
    }
    ASSERT_EQ_INT(bc.line_count, 100);
    ASSERT_TRUE(bc.line_capacity >= 100);
    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Tests: Combined Scenarios (Integration-style)
 * ========================================================================= */

static void test_bc_opcode_sequence(void) {
    /* Emit a realistic opcode sequence: PUSH_INT8 42, PUSH_INT8 3, ADD, HALT */
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_op_u8(&bc, R8E_OP_PUSH_INT8, 42);
    r8e_bc_emit_op_u8(&bc, R8E_OP_PUSH_INT8, 3);
    r8e_bc_emit_op(&bc, R8E_OP_ADD);
    r8e_bc_emit_op(&bc, R8E_OP_HALT);
    ASSERT_EQ_INT(bc.length, 6);
    ASSERT_EQ_INT(bc.code[0], R8E_OP_PUSH_INT8);
    ASSERT_EQ_INT(bc.code[1], 42);
    ASSERT_EQ_INT(bc.code[2], R8E_OP_PUSH_INT8);
    ASSERT_EQ_INT(bc.code[3], 3);
    ASSERT_EQ_INT(bc.code[4], R8E_OP_ADD);
    ASSERT_EQ_INT(bc.code[5], R8E_OP_HALT);
    r8e_bc_destroy(&bc);
}

static void test_bc_if_else_jump_pattern(void) {
    /* Simulate: if (cond) { A } else { B }
     * JUMP_IF_FALSE -> else_label
     * [A code]
     * JUMP -> end_label
     * else_label:
     * [B code]
     * end_label:
     */
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);

    /* Condition already on stack */
    uint32_t else_patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP_IF_FALSE);

    /* A code: push 1 */
    r8e_bc_emit_op_u8(&bc, R8E_OP_PUSH_INT8, 1);

    uint32_t end_patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP);

    /* else_label: patch JUMP_IF_FALSE to land here */
    r8e_bc_patch_jump(&bc, else_patch);

    /* B code: push 2 */
    r8e_bc_emit_op_u8(&bc, R8E_OP_PUSH_INT8, 2);

    /* end_label: patch JUMP to land here */
    r8e_bc_patch_jump(&bc, end_patch);

    r8e_bc_emit_op(&bc, R8E_OP_HALT);

    /* Verify structure */
    ASSERT_EQ_INT(bc.code[0], R8E_OP_JUMP_IF_FALSE);
    /* The else jump offset should skip over A code + JUMP instruction */
    int32_t else_off = r8e_bc_read_i32(&bc, else_patch);
    ASSERT_TRUE(else_off > 0);
    /* The end jump offset should skip over B code */
    int32_t end_off = r8e_bc_read_i32(&bc, end_patch);
    ASSERT_TRUE(end_off > 0);

    r8e_bc_destroy(&bc);
}

static void test_bc_nested_jumps(void) {
    /* Simulate nested if: if (a) { if (b) { ... } } */
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);

    uint32_t outer_patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP_IF_FALSE);
    uint32_t inner_patch = r8e_bc_emit_jump(&bc, R8E_OP_JUMP_IF_FALSE);

    /* Inner body */
    r8e_bc_emit_op_u8(&bc, R8E_OP_PUSH_INT8, 99);

    /* Patch inner jump */
    r8e_bc_patch_jump(&bc, inner_patch);

    /* Patch outer jump */
    r8e_bc_patch_jump(&bc, outer_patch);

    r8e_bc_emit_op(&bc, R8E_OP_HALT);

    /* Both jumps should have valid forward offsets */
    int32_t inner_off = r8e_bc_read_i32(&bc, inner_patch);
    int32_t outer_off = r8e_bc_read_i32(&bc, outer_patch);
    ASSERT_TRUE(inner_off >= 0);
    ASSERT_TRUE(outer_off >= 0);
    /* Outer jump should cover more bytes than inner */
    ASSERT_TRUE(outer_off > inner_off);

    r8e_bc_destroy(&bc);
}

static void test_bc_dump_smoke(void) {
    /* Just ensure r8e_bc_dump does not crash */
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);
    r8e_bc_emit_op_u8(&bc, R8E_OP_PUSH_INT8, 42);
    r8e_bc_emit_op(&bc, R8E_OP_ADD);
    r8e_bc_emit_op(&bc, R8E_OP_HALT);
    r8e_bc_add_constant(&bc, r8e_from_int32(100));
    r8e_bc_add_line(&bc, 1);
    /* Redirect stderr to suppress output during test */
    /* Just call it and verify no crash */
    r8e_bc_dump(&bc);
    ASSERT_TRUE(1);
    r8e_bc_destroy(&bc);
}

static void test_bc_roundtrip_emit_read(void) {
    /* Emit various values and read them back to verify roundtrip */
    R8EBytecodeBuffer bc;
    r8e_bc_init(&bc);

    /* offset 0: u32 = 0x01020304 */
    r8e_bc_emit_u32(&bc, 0x01020304);
    /* offset 4: u16 = 0x0506 */
    r8e_bc_emit_u16(&bc, 0x0506);
    /* offset 6: i32 = -999 */
    r8e_bc_emit_i32(&bc, -999);

    ASSERT_EQ_INT(bc.length, 10);

    /* Read back */
    int32_t v32 = r8e_bc_read_i32(&bc, 0);
    ASSERT_EQ_INT(v32, 0x01020304);
    uint16_t v16 = r8e_bc_read_u16(&bc, 4);
    ASSERT_EQ_INT(v16, 0x0506);
    int32_t neg = r8e_bc_read_i32(&bc, 6);
    ASSERT_EQ_INT(neg, -999);

    r8e_bc_destroy(&bc);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_bc_tests(void) {
    /* Initialization / Destruction */
    RUN_TEST(test_bc_init_success);
    RUN_TEST(test_bc_destroy_null_safe);
    RUN_TEST(test_bc_destroy_clears_fields);

    /* Emission Primitives */
    RUN_TEST(test_bc_emit_single_byte);
    RUN_TEST(test_bc_emit_multiple_bytes);
    RUN_TEST(test_bc_emit_u16_little_endian);
    RUN_TEST(test_bc_emit_u32_little_endian);
    RUN_TEST(test_bc_emit_i32_negative);
    RUN_TEST(test_bc_emit_i8);
    RUN_TEST(test_bc_emit_u16_zero);
    RUN_TEST(test_bc_emit_u16_max);

    /* Opcode Emission */
    RUN_TEST(test_bc_emit_op);
    RUN_TEST(test_bc_emit_op_u8);
    RUN_TEST(test_bc_emit_op_u16);
    RUN_TEST(test_bc_emit_op_u32);

    /* Buffer Growth */
    RUN_TEST(test_bc_grow_no_growth_needed);
    RUN_TEST(test_bc_grow_doubles_capacity);
    RUN_TEST(test_bc_emit_triggers_growth);
    RUN_TEST(test_bc_grow_large_request);

    /* Current Offset */
    RUN_TEST(test_bc_current_offset_initial);
    RUN_TEST(test_bc_current_offset_after_emit);

    /* Stack Depth Tracking */
    RUN_TEST(test_bc_adjust_stack_positive);
    RUN_TEST(test_bc_adjust_stack_negative);
    RUN_TEST(test_bc_adjust_stack_tracks_max);
    RUN_TEST(test_bc_adjust_stack_clamps_at_zero);
    RUN_TEST(test_bc_adjust_stack_max_monotonic);

    /* Jump Emission and Patching */
    RUN_TEST(test_bc_emit_jump_returns_patch_addr);
    RUN_TEST(test_bc_patch_jump_forward);
    RUN_TEST(test_bc_patch_jump_backward);
    RUN_TEST(test_bc_emit_jump8);
    RUN_TEST(test_bc_emit_jump16);
    RUN_TEST(test_bc_patch_jump16);

    /* Raw Value Patching */
    RUN_TEST(test_bc_patch_u16);
    RUN_TEST(test_bc_patch_u32);
    RUN_TEST(test_bc_patch_u16_out_of_bounds);
    RUN_TEST(test_bc_patch_u32_out_of_bounds);

    /* Read Helpers */
    RUN_TEST(test_bc_read_i32);
    RUN_TEST(test_bc_read_u16);
    RUN_TEST(test_bc_read_i32_positive);
    RUN_TEST(test_bc_read_i32_at_offset);

    /* Constant Pool */
    RUN_TEST(test_bc_add_constant_first);
    RUN_TEST(test_bc_add_constant_multiple);
    RUN_TEST(test_bc_add_constant_dedup);
    RUN_TEST(test_bc_add_constant_dedup_different_types);
    RUN_TEST(test_bc_add_constant_pool_growth);
    RUN_TEST(test_bc_add_constant_special_values);
    RUN_TEST(test_bc_add_constant_inline_string);

    /* Nested Functions */
    RUN_TEST(test_bc_add_function_first);
    RUN_TEST(test_bc_add_function_multiple);
    RUN_TEST(test_bc_add_function_growth);
    RUN_TEST(test_bc_add_function_set_metadata);

    /* Line Number Table */
    RUN_TEST(test_bc_add_line_first);
    RUN_TEST(test_bc_add_line_dedup_same_line);
    RUN_TEST(test_bc_add_line_different_lines);
    RUN_TEST(test_bc_add_line_growth);

    /* Combined / Integration */
    RUN_TEST(test_bc_opcode_sequence);
    RUN_TEST(test_bc_if_else_jump_pattern);
    RUN_TEST(test_bc_nested_jumps);
    RUN_TEST(test_bc_dump_smoke);
    RUN_TEST(test_bc_roundtrip_emit_read);
}
