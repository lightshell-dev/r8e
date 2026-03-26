/*
 * r8e_display_list.c - GPU display list and per-frame arena allocator
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "r8e_display_list.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Arena Allocator
 * ========================================================================= */

/* Alignment for all arena allocations */
#define DL_ARENA_ALIGN 8

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static R8EDLArenaBlock *arena_block_new(size_t size) {
    R8EDLArenaBlock *block = malloc(sizeof(R8EDLArenaBlock));
    if (!block) return NULL;
    block->data = malloc(size);
    if (!block->data) {
        free(block);
        return NULL;
    }
    block->size = size;
    block->used = 0;
    block->next = NULL;
    return block;
}

void r8e_dl_arena_init(R8EDLArena *arena, size_t block_size) {
    if (block_size == 0) block_size = 65536; /* 64KB default */
    arena->default_block_size = block_size;
    arena->head = arena_block_new(block_size);
    arena->current = arena->head;
}

void *r8e_dl_arena_alloc(R8EDLArena *arena, size_t size) {
    if (size == 0) return NULL;
    size = align_up(size, DL_ARENA_ALIGN);

    /* Try current block */
    R8EDLArenaBlock *block = arena->current;
    if (block && (block->used + size <= block->size)) {
        void *ptr = block->data + block->used;
        block->used += size;
        return ptr;
    }

    /* Try next existing block (after a reset, blocks are reused) */
    if (block && block->next) {
        block = block->next;
        if (size <= block->size) {
            block->used = size;
            arena->current = block;
            return block->data;
        }
    }

    /* Allocate a new block (at least big enough for this request) */
    size_t new_size = arena->default_block_size;
    if (size > new_size) new_size = size;
    R8EDLArenaBlock *new_block = arena_block_new(new_size);
    if (!new_block) return NULL;

    new_block->used = size;
    /* Append after current block */
    if (block) {
        new_block->next = block->next;
        block->next = new_block;
    } else if (arena->current) {
        arena->current->next = new_block;
    } else {
        arena->head = new_block;
    }
    arena->current = new_block;
    return new_block->data;
}

void r8e_dl_arena_reset(R8EDLArena *arena) {
    /* Rewind all blocks to used=0, start from head */
    R8EDLArenaBlock *block = arena->head;
    while (block) {
        block->used = 0;
        block = block->next;
    }
    arena->current = arena->head;
}

void r8e_dl_arena_destroy(R8EDLArena *arena) {
    R8EDLArenaBlock *block = arena->head;
    while (block) {
        R8EDLArenaBlock *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    arena->head = NULL;
    arena->current = NULL;
}

/* =========================================================================
 * Display List
 * ========================================================================= */

#define DL_INITIAL_CAPACITY 256

void r8e_dl_init(DisplayList *dl, R8EDLArena *arena) {
    dl->commands = NULL;
    dl->count = 0;
    dl->capacity = 0;
    dl->arena = arena;
}

void r8e_dl_clear(DisplayList *dl) {
    dl->count = 0;
}

void r8e_dl_destroy(DisplayList *dl) {
    free(dl->commands);
    dl->commands = NULL;
    dl->count = 0;
    dl->capacity = 0;
}

DisplayCommand *r8e_dl_push(DisplayList *dl) {
    if (dl->count >= dl->capacity) {
        uint32_t new_cap = dl->capacity == 0 ? DL_INITIAL_CAPACITY : dl->capacity * 2;
        DisplayCommand *new_cmds = realloc(dl->commands, new_cap * sizeof(DisplayCommand));
        if (!new_cmds) return NULL;
        dl->commands = new_cmds;
        dl->capacity = new_cap;
    }
    DisplayCommand *cmd = &dl->commands[dl->count++];
    memset(cmd, 0, sizeof(DisplayCommand));
    return cmd;
}

/* =========================================================================
 * Convenience Builders
 * ========================================================================= */

void r8e_dl_push_fill_rect(DisplayList *dl, float x, float y, float w, float h,
                           uint32_t color, float border_radius) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_FILL_RECT;
    cmd->fill_rect.x = x;
    cmd->fill_rect.y = y;
    cmd->fill_rect.w = w;
    cmd->fill_rect.h = h;
    cmd->fill_rect.color = color;
    cmd->fill_rect.border_radius = border_radius;
}

void r8e_dl_push_stroke_rect(DisplayList *dl, float x, float y, float w, float h,
                             uint32_t color, float width, float border_radius) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_STROKE_RECT;
    cmd->stroke_rect.x = x;
    cmd->stroke_rect.y = y;
    cmd->stroke_rect.w = w;
    cmd->stroke_rect.h = h;
    cmd->stroke_rect.color = color;
    cmd->stroke_rect.width = width;
    cmd->stroke_rect.border_radius = border_radius;
}

void r8e_dl_push_fill_text(DisplayList *dl, float x, float y,
                           R8EGlyphRun *glyphs, uint32_t glyph_count,
                           float font_size, uint32_t color) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_FILL_TEXT;
    cmd->fill_text.x = x;
    cmd->fill_text.y = y;
    cmd->fill_text.glyphs = glyphs;
    cmd->fill_text.glyph_count = glyph_count;
    cmd->fill_text.font_size = font_size;
    cmd->fill_text.color = color;
}

void r8e_dl_push_draw_image(DisplayList *dl, float x, float y, float w, float h,
                            uint32_t texture_id) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_DRAW_IMAGE;
    cmd->draw_image.x = x;
    cmd->draw_image.y = y;
    cmd->draw_image.w = w;
    cmd->draw_image.h = h;
    cmd->draw_image.texture_id = texture_id;
}

void r8e_dl_push_clip(DisplayList *dl, float x, float y, float w, float h) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_PUSH_CLIP;
    cmd->clip.x = x;
    cmd->clip.y = y;
    cmd->clip.w = w;
    cmd->clip.h = h;
}

void r8e_dl_pop_clip(DisplayList *dl) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_POP_CLIP;
}

void r8e_dl_push_opacity(DisplayList *dl, float alpha) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_PUSH_OPACITY;
    cmd->opacity.alpha = alpha;
}

void r8e_dl_pop_opacity(DisplayList *dl) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_POP_OPACITY;
}

void r8e_dl_push_fill_path(DisplayList *dl, R8EPathCommand *commands,
                           uint32_t count, uint32_t color) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_FILL_PATH;
    cmd->fill_path.commands = commands;
    cmd->fill_path.count = count;
    cmd->fill_path.color = color;
}

void r8e_dl_push_box_shadow(DisplayList *dl, float x, float y, float w, float h,
                             float offset_x, float offset_y, float blur,
                             uint32_t color, float border_radius) {
    DisplayCommand *cmd = r8e_dl_push(dl);
    if (!cmd) return;
    cmd->type = DL_BOX_SHADOW;
    cmd->box_shadow.x = x;
    cmd->box_shadow.y = y;
    cmd->box_shadow.w = w;
    cmd->box_shadow.h = h;
    cmd->box_shadow.offset_x = offset_x;
    cmd->box_shadow.offset_y = offset_y;
    cmd->box_shadow.blur = blur;
    cmd->box_shadow.color = color;
    cmd->box_shadow.border_radius = border_radius;
}

/* =========================================================================
 * Arena-Allocated Helpers
 * ========================================================================= */

R8EGlyphRun *r8e_dl_arena_alloc_glyph_run(DisplayList *dl, uint32_t glyph_count,
                                           uint32_t font_id) {
    R8EGlyphRun *run = r8e_dl_arena_alloc(dl->arena, sizeof(R8EGlyphRun));
    if (!run) return NULL;
    run->glyphs = r8e_dl_arena_alloc(dl->arena, glyph_count * sizeof(R8EGlyphInfo));
    if (!run->glyphs && glyph_count > 0) return NULL;
    run->count = glyph_count;
    run->font_id = font_id;
    return run;
}

R8EPathCommand *r8e_dl_arena_alloc_path(DisplayList *dl, uint32_t count) {
    return r8e_dl_arena_alloc(dl->arena, count * sizeof(R8EPathCommand));
}
