/* r8e_font.h — Hardened TrueType font parser and rasterizer
 *
 * Security: every byte read from font data is bounds-checked.
 * No raw pointer arithmetic on font data. Malformed fonts
 * return errors, never crash.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef R8E_FONT_H
#define R8E_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct R8EFont R8EFont;

/* --- Loading --- */
R8EFont *r8e_font_load(const uint8_t *data, uint32_t length);
R8EFont *r8e_font_load_default(void);   /* bundled Inter */
R8EFont *r8e_font_load_default2(void);  /* bundled Open Sans */
void     r8e_font_free(R8EFont *font);

/* --- Metrics --- */
float r8e_font_scale(R8EFont *font, float pixel_height);
void  r8e_font_vmetrics(R8EFont *font, int *ascent, int *descent, int *line_gap);
void  r8e_font_hmetrics(R8EFont *font, uint32_t glyph_id, int *advance, int *lsb);
int   r8e_font_kern(R8EFont *font, uint32_t g1, uint32_t g2);

/* --- Glyph lookup --- */
uint32_t r8e_font_glyph_id(R8EFont *font, uint32_t codepoint);

/* --- Rasterization --- */
typedef struct {
    uint8_t *pixels;          /* caller-provided buffer, or NULL to query size */
    int      width, height;   /* bitmap dimensions (output) */
    int      x_bearing;       /* left side bearing in pixels */
    int      y_bearing;       /* top bearing in pixels (from baseline) */
} R8EGlyphBitmap;

bool r8e_font_glyph_box(R8EFont *font, uint32_t glyph_id, float scale,
                          int *x0, int *y0, int *x1, int *y1);
bool r8e_font_rasterize(R8EFont *font, uint32_t glyph_id, float scale,
                          R8EGlyphBitmap *out);

#endif /* R8E_FONT_H */
