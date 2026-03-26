/*
 * test_display_list.c - Unit tests for GPU display list and arena allocator
 *
 * Tests cover:
 *   - Arena init, alloc, alignment, grow, reset, destroy
 *   - Display list create, destroy, clear
 *   - All display command types: fill_rect, stroke_rect, fill_text,
 *     draw_image, clip push/pop, opacity push/pop, fill_path
 *   - Mixed command ordering
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* =========================================================================
 * Test infrastructure (shared globals from test_runner.c)
 * ========================================================================= */

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;

#define ASSERT_TRUE(e) do { \
    if (!(e)) { \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n" \
                "      at %s:%d\n", #e, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FALSE(e) ASSERT_TRUE(!(e))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n" \
                "      got %lld vs %lld\n" \
                "      at %s:%d\n", #a, #b, _a, _b, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define ASSERT_FLOAT_EQ(a, b) do { \
    float _a = (float)(a), _b = (float)(b); \
    if (fabsf(_a - _b) > 0.001f) { \
        fprintf(stderr, "    ASSERT_FLOAT_EQ failed: %s == %s\n" \
                "      got %f vs %f\n" \
                "      at %s:%d\n", #a, #b, _a, _b, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    g_tests_run++; \
    printf("  %-60s ", #name); \
    fflush(stdout); \
    g_assert_fail = 0; \
    test_##name(); \
    if (g_assert_fail) { \
        g_tests_failed++; \
        printf("FAIL\n"); \
    } else { \
        g_tests_passed++; \
        printf("ok\n"); \
    } \
} while (0)

/* =========================================================================
 * Include display list header (linked via library, not included as .c)
 * ========================================================================= */

#include "../../src/gpu/r8e_display_list.h"

/* =========================================================================
 * Arena Tests
 * ========================================================================= */

TEST(dl_arena_init_reset) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    ASSERT_TRUE(arena.head != NULL);
    ASSERT_TRUE(arena.current != NULL);
    ASSERT_EQ_INT(arena.default_block_size, 65536);

    /* Allocate something */
    void *p1 = r8e_dl_arena_alloc(&arena, 128);
    ASSERT_TRUE(p1 != NULL);

    /* Reset and verify reusable */
    r8e_dl_arena_reset(&arena);
    ASSERT_EQ_INT(arena.head->used, 0);

    void *p2 = r8e_dl_arena_alloc(&arena, 128);
    ASSERT_TRUE(p2 != NULL);
    /* After reset, the same block is reused so p2 should equal p1 */
    ASSERT_TRUE(p2 == p1);

    r8e_dl_arena_destroy(&arena);
}

TEST(dl_arena_alloc_alignment) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);

    /* Allocate various sizes and verify 8-byte alignment */
    void *p1 = r8e_dl_arena_alloc(&arena, 1);
    void *p2 = r8e_dl_arena_alloc(&arena, 3);
    void *p3 = r8e_dl_arena_alloc(&arena, 7);
    void *p4 = r8e_dl_arena_alloc(&arena, 16);
    void *p5 = r8e_dl_arena_alloc(&arena, 33);

    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p3 != NULL);
    ASSERT_TRUE(p4 != NULL);
    ASSERT_TRUE(p5 != NULL);

    ASSERT_EQ_INT((uintptr_t)p1 % 8, 0);
    ASSERT_EQ_INT((uintptr_t)p2 % 8, 0);
    ASSERT_EQ_INT((uintptr_t)p3 % 8, 0);
    ASSERT_EQ_INT((uintptr_t)p4 % 8, 0);
    ASSERT_EQ_INT((uintptr_t)p5 % 8, 0);

    r8e_dl_arena_destroy(&arena);
}

TEST(dl_arena_grow) {
    /* Use a tiny block size to force growth */
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 64);

    /* Exhaust the initial block */
    void *p1 = r8e_dl_arena_alloc(&arena, 64);
    ASSERT_TRUE(p1 != NULL);

    /* This should trigger a new block */
    void *p2 = r8e_dl_arena_alloc(&arena, 32);
    ASSERT_TRUE(p2 != NULL);

    /* Verify the arena grew (current block is different from head) */
    ASSERT_TRUE(arena.head->next != NULL);

    /* Allocate more to ensure continued operation */
    void *p3 = r8e_dl_arena_alloc(&arena, 16);
    ASSERT_TRUE(p3 != NULL);

    r8e_dl_arena_destroy(&arena);
}

/* =========================================================================
 * Display List Tests
 * ========================================================================= */

TEST(dl_create_destroy) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);

    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    ASSERT_EQ_INT(dl.count, 0);
    ASSERT_TRUE(dl.arena == &arena);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_fill_rect) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    r8e_dl_push_fill_rect(&dl, 10.0f, 20.0f, 100.0f, 50.0f, 0xFF0000FFu, 5.0f);

    ASSERT_EQ_INT(dl.count, 1);
    ASSERT_EQ_INT(dl.commands[0].type, DL_FILL_RECT);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_rect.x, 10.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_rect.y, 20.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_rect.w, 100.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_rect.h, 50.0f);
    ASSERT_EQ(dl.commands[0].fill_rect.color, 0xFF0000FFu);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_rect.border_radius, 5.0f);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_stroke_rect) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    r8e_dl_push_stroke_rect(&dl, 5.0f, 10.0f, 200.0f, 100.0f, 0x00FF00FFu, 2.0f, 3.0f);

    ASSERT_EQ_INT(dl.count, 1);
    ASSERT_EQ_INT(dl.commands[0].type, DL_STROKE_RECT);
    ASSERT_FLOAT_EQ(dl.commands[0].stroke_rect.x, 5.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].stroke_rect.y, 10.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].stroke_rect.w, 200.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].stroke_rect.h, 100.0f);
    ASSERT_EQ(dl.commands[0].stroke_rect.color, 0x00FF00FFu);
    ASSERT_FLOAT_EQ(dl.commands[0].stroke_rect.width, 2.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].stroke_rect.border_radius, 3.0f);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_fill_text) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    /* Allocate a glyph run via the arena helper */
    R8EGlyphRun *run = r8e_dl_arena_alloc_glyph_run(&dl, 3, 42);
    ASSERT_TRUE(run != NULL);
    ASSERT_EQ_INT(run->count, 3);
    ASSERT_EQ_INT(run->font_id, 42);

    /* Fill in glyph data */
    run->glyphs[0] = (R8EGlyphInfo){ .glyph_id = 65, .x_offset = 0, .y_offset = 0, .x_advance = 10.0f };
    run->glyphs[1] = (R8EGlyphInfo){ .glyph_id = 66, .x_offset = 0, .y_offset = 0, .x_advance = 11.0f };
    run->glyphs[2] = (R8EGlyphInfo){ .glyph_id = 67, .x_offset = 0, .y_offset = 0, .x_advance = 12.0f };

    r8e_dl_push_fill_text(&dl, 50.0f, 100.0f, run, 1, 16.0f, 0x000000FFu);

    ASSERT_EQ_INT(dl.count, 1);
    ASSERT_EQ_INT(dl.commands[0].type, DL_FILL_TEXT);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_text.x, 50.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_text.y, 100.0f);
    ASSERT_TRUE(dl.commands[0].fill_text.glyphs == run);
    ASSERT_EQ_INT(dl.commands[0].fill_text.glyph_count, 1);
    ASSERT_FLOAT_EQ(dl.commands[0].fill_text.font_size, 16.0f);
    ASSERT_EQ(dl.commands[0].fill_text.color, 0x000000FFu);

    /* Verify glyph data is accessible */
    ASSERT_EQ_INT(run->glyphs[0].glyph_id, 65);
    ASSERT_FLOAT_EQ(run->glyphs[2].x_advance, 12.0f);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_draw_image) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    r8e_dl_push_draw_image(&dl, 0.0f, 0.0f, 320.0f, 240.0f, 7);

    ASSERT_EQ_INT(dl.count, 1);
    ASSERT_EQ_INT(dl.commands[0].type, DL_DRAW_IMAGE);
    ASSERT_FLOAT_EQ(dl.commands[0].draw_image.x, 0.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].draw_image.y, 0.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].draw_image.w, 320.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].draw_image.h, 240.0f);
    ASSERT_EQ(dl.commands[0].draw_image.texture_id, 7u);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_clip_ops) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    r8e_dl_push_clip(&dl, 10.0f, 10.0f, 400.0f, 300.0f);
    r8e_dl_pop_clip(&dl);

    ASSERT_EQ_INT(dl.count, 2);
    ASSERT_EQ_INT(dl.commands[0].type, DL_PUSH_CLIP);
    ASSERT_FLOAT_EQ(dl.commands[0].clip.x, 10.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].clip.y, 10.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].clip.w, 400.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].clip.h, 300.0f);

    ASSERT_EQ_INT(dl.commands[1].type, DL_POP_CLIP);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_opacity) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    r8e_dl_push_opacity(&dl, 0.5f);
    r8e_dl_pop_opacity(&dl);

    ASSERT_EQ_INT(dl.count, 2);
    ASSERT_EQ_INT(dl.commands[0].type, DL_PUSH_OPACITY);
    ASSERT_FLOAT_EQ(dl.commands[0].opacity.alpha, 0.5f);

    ASSERT_EQ_INT(dl.commands[1].type, DL_POP_OPACITY);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_fill_path) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    /* Allocate path commands via arena */
    R8EPathCommand *path = r8e_dl_arena_alloc_path(&dl, 4);
    ASSERT_TRUE(path != NULL);
    path[0] = (R8EPathCommand){ .type = R8E_PATH_MOVE_TO, .x = 0, .y = 0 };
    path[1] = (R8EPathCommand){ .type = R8E_PATH_LINE_TO, .x = 100, .y = 0 };
    path[2] = (R8EPathCommand){ .type = R8E_PATH_LINE_TO, .x = 50, .y = 80 };
    path[3] = (R8EPathCommand){ .type = R8E_PATH_CLOSE };

    r8e_dl_push_fill_path(&dl, path, 4, 0x0000FFFFu);

    ASSERT_EQ_INT(dl.count, 1);
    ASSERT_EQ_INT(dl.commands[0].type, DL_FILL_PATH);
    ASSERT_TRUE(dl.commands[0].fill_path.commands == path);
    ASSERT_EQ_INT(dl.commands[0].fill_path.count, 4);
    ASSERT_EQ(dl.commands[0].fill_path.color, 0x0000FFFFu);

    /* Verify path data */
    ASSERT_EQ_INT(path[0].type, R8E_PATH_MOVE_TO);
    ASSERT_EQ_INT(path[3].type, R8E_PATH_CLOSE);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_push_box_shadow) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    r8e_dl_push_box_shadow(&dl, 100.0f, 200.0f, 300.0f, 150.0f,
                            0.0f, 4.0f, 24.0f, 0x0A000000u, 8.0f);

    ASSERT_EQ_INT(dl.count, 1);
    ASSERT_EQ_INT(dl.commands[0].type, DL_BOX_SHADOW);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.x, 100.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.y, 200.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.w, 300.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.h, 150.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.offset_x, 0.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.offset_y, 4.0f);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.blur, 24.0f);
    ASSERT_EQ(dl.commands[0].box_shadow.color, 0x0A000000u);
    ASSERT_FLOAT_EQ(dl.commands[0].box_shadow.border_radius, 8.0f);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_clear) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    /* Push several commands */
    r8e_dl_push_fill_rect(&dl, 0, 0, 10, 10, 0xFFu, 0);
    r8e_dl_push_fill_rect(&dl, 10, 10, 20, 20, 0xFFu, 0);
    r8e_dl_push_clip(&dl, 0, 0, 100, 100);
    ASSERT_EQ_INT(dl.count, 3);

    /* Clear and verify */
    r8e_dl_clear(&dl);
    ASSERT_EQ_INT(dl.count, 0);

    /* Verify we can still push after clear */
    r8e_dl_push_fill_rect(&dl, 0, 0, 5, 5, 0xFFu, 0);
    ASSERT_EQ_INT(dl.count, 1);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

TEST(dl_mixed_commands) {
    R8EDLArena arena;
    r8e_dl_arena_init(&arena, 65536);
    DisplayList dl;
    r8e_dl_init(&dl, &arena);

    /* Push a mix of all command types and verify ordering */
    r8e_dl_push_clip(&dl, 0, 0, 800, 600);            /* 0: PUSH_CLIP */
    r8e_dl_push_opacity(&dl, 0.8f);                    /* 1: PUSH_OPACITY */
    r8e_dl_push_fill_rect(&dl, 10, 10, 100, 50,       /* 2: FILL_RECT */
                          0xFF0000FFu, 4.0f);
    r8e_dl_push_stroke_rect(&dl, 10, 10, 100, 50,     /* 3: STROKE_RECT */
                            0x000000FFu, 1.0f, 4.0f);
    r8e_dl_push_draw_image(&dl, 200, 50, 64, 64, 1);  /* 4: DRAW_IMAGE */

    /* Fill text with arena-allocated glyph run */
    R8EGlyphRun *run = r8e_dl_arena_alloc_glyph_run(&dl, 2, 1);
    ASSERT_TRUE(run != NULL);
    run->glyphs[0] = (R8EGlyphInfo){ .glyph_id = 72, .x_advance = 8.0f };
    run->glyphs[1] = (R8EGlyphInfo){ .glyph_id = 73, .x_advance = 4.0f };
    r8e_dl_push_fill_text(&dl, 10, 80, run, 1,        /* 5: FILL_TEXT */
                          14.0f, 0x333333FFu);

    /* Fill path with arena-allocated path commands */
    R8EPathCommand *path = r8e_dl_arena_alloc_path(&dl, 3);
    ASSERT_TRUE(path != NULL);
    path[0] = (R8EPathCommand){ .type = R8E_PATH_MOVE_TO, .x = 0, .y = 0 };
    path[1] = (R8EPathCommand){ .type = R8E_PATH_LINE_TO, .x = 50, .y = 50 };
    path[2] = (R8EPathCommand){ .type = R8E_PATH_CLOSE };
    r8e_dl_push_fill_path(&dl, path, 3, 0x00FF00FFu); /* 6: FILL_PATH */

    r8e_dl_pop_opacity(&dl);                           /* 7: POP_OPACITY */
    r8e_dl_pop_clip(&dl);                              /* 8: POP_CLIP */

    /* Verify count and ordering */
    ASSERT_EQ_INT(dl.count, 9);
    ASSERT_EQ_INT(dl.commands[0].type, DL_PUSH_CLIP);
    ASSERT_EQ_INT(dl.commands[1].type, DL_PUSH_OPACITY);
    ASSERT_EQ_INT(dl.commands[2].type, DL_FILL_RECT);
    ASSERT_EQ_INT(dl.commands[3].type, DL_STROKE_RECT);
    ASSERT_EQ_INT(dl.commands[4].type, DL_DRAW_IMAGE);
    ASSERT_EQ_INT(dl.commands[5].type, DL_FILL_TEXT);
    ASSERT_EQ_INT(dl.commands[6].type, DL_FILL_PATH);
    ASSERT_EQ_INT(dl.commands[7].type, DL_POP_OPACITY);
    ASSERT_EQ_INT(dl.commands[8].type, DL_POP_CLIP);

    r8e_dl_destroy(&dl);
    r8e_dl_arena_destroy(&arena);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_display_list_tests(void) {
    /* Arena tests */
    RUN_TEST(dl_arena_init_reset);
    RUN_TEST(dl_arena_alloc_alignment);
    RUN_TEST(dl_arena_grow);

    /* Display list tests */
    RUN_TEST(dl_create_destroy);
    RUN_TEST(dl_push_fill_rect);
    RUN_TEST(dl_push_stroke_rect);
    RUN_TEST(dl_push_fill_text);
    RUN_TEST(dl_push_draw_image);
    RUN_TEST(dl_push_clip_ops);
    RUN_TEST(dl_push_opacity);
    RUN_TEST(dl_push_fill_path);
    RUN_TEST(dl_push_box_shadow);
    RUN_TEST(dl_clear);
    RUN_TEST(dl_mixed_commands);
}
