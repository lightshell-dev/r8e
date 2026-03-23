/*
 * r8e_display_list.h - GPU display list format and per-frame arena allocator
 *
 * The display list is the contract between the paint layer and GPU backends.
 * Commands are appended each frame, then consumed by the active GPU backend.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_DISPLAY_LIST_H
#define R8E_DISPLAY_LIST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- Per-frame arena allocator --- */
typedef struct R8EDLArenaBlock {
    uint8_t *data;
    size_t size;
    size_t used;
    struct R8EDLArenaBlock *next;
} R8EDLArenaBlock;

typedef struct {
    R8EDLArenaBlock *head;
    R8EDLArenaBlock *current;
    size_t default_block_size;  /* 64KB default */
} R8EDLArena;

void   r8e_dl_arena_init(R8EDLArena *arena, size_t block_size);
void  *r8e_dl_arena_alloc(R8EDLArena *arena, size_t size);
void   r8e_dl_arena_reset(R8EDLArena *arena);
void   r8e_dl_arena_destroy(R8EDLArena *arena);

/* --- Glyph run (shaped text output) --- */
typedef struct {
    uint32_t glyph_id;
    float    x_offset;
    float    y_offset;
    float    x_advance;
} R8EGlyphInfo;

typedef struct {
    R8EGlyphInfo *glyphs;
    uint32_t      count;
    uint32_t      font_id;
} R8EGlyphRun;

/* --- Path commands --- */
typedef enum {
    R8E_PATH_MOVE_TO,
    R8E_PATH_LINE_TO,
    R8E_PATH_QUAD_TO,
    R8E_PATH_CLOSE,
} R8EPathCmdType;

typedef struct {
    R8EPathCmdType type;
    float x, y;
    float cx, cy;  /* control point for quad_to */
} R8EPathCommand;

/* --- Display commands --- */
typedef enum {
    DL_FILL_RECT,
    DL_STROKE_RECT,
    DL_FILL_TEXT,
    DL_DRAW_IMAGE,
    DL_PUSH_CLIP,
    DL_POP_CLIP,
    DL_PUSH_OPACITY,
    DL_POP_OPACITY,
    DL_FILL_PATH,
} DisplayCommandType;

typedef struct {
    DisplayCommandType type;
    union {
        struct { float x, y, w, h; uint32_t color; float border_radius; } fill_rect;
        struct { float x, y, w, h; uint32_t color; float width; float border_radius; } stroke_rect;
        struct { float x, y; R8EGlyphRun *glyphs; uint32_t glyph_count;
                 float font_size; uint32_t color; } fill_text;
        struct { float x, y, w, h; uint32_t texture_id; } draw_image;
        struct { float x, y, w, h; } clip;
        struct { float alpha; } opacity;
        struct { R8EPathCommand *commands; uint32_t count; uint32_t color; } fill_path;
    };
} DisplayCommand;

typedef struct {
    DisplayCommand *commands;
    uint32_t count;
    uint32_t capacity;
    R8EDLArena *arena;  /* arena for per-frame allocations (GlyphRun*, PathCommand*) */
} DisplayList;

/* --- Display list API --- */
void r8e_dl_init(DisplayList *dl, R8EDLArena *arena);
void r8e_dl_clear(DisplayList *dl);
void r8e_dl_destroy(DisplayList *dl);

DisplayCommand *r8e_dl_push(DisplayList *dl);

/* Convenience builders */
void r8e_dl_push_fill_rect(DisplayList *dl, float x, float y, float w, float h,
                           uint32_t color, float border_radius);
void r8e_dl_push_stroke_rect(DisplayList *dl, float x, float y, float w, float h,
                             uint32_t color, float width, float border_radius);
void r8e_dl_push_fill_text(DisplayList *dl, float x, float y,
                           R8EGlyphRun *glyphs, uint32_t glyph_count,
                           float font_size, uint32_t color);
void r8e_dl_push_draw_image(DisplayList *dl, float x, float y, float w, float h,
                            uint32_t texture_id);
void r8e_dl_push_clip(DisplayList *dl, float x, float y, float w, float h);
void r8e_dl_pop_clip(DisplayList *dl);
void r8e_dl_push_opacity(DisplayList *dl, float alpha);
void r8e_dl_pop_opacity(DisplayList *dl);
void r8e_dl_push_fill_path(DisplayList *dl, R8EPathCommand *commands,
                           uint32_t count, uint32_t color);

/* Arena-allocated helpers */
R8EGlyphRun *r8e_dl_arena_alloc_glyph_run(DisplayList *dl, uint32_t glyph_count,
                                           uint32_t font_id);
R8EPathCommand *r8e_dl_arena_alloc_path(DisplayList *dl, uint32_t count);

#endif /* R8E_DISPLAY_LIST_H */
