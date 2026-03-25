/* r8e_font.c — Hardened TrueType font parser and rasterizer
 * SPDX-License-Identifier: MIT
 */
#include "r8e_font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Mark functions as potentially unused (used in later tasks) */
#ifdef __GNUC__
#define R8E_MAYBE_UNUSED __attribute__((unused))
#else
#define R8E_MAYBE_UNUSED
#endif

/* =========================================================================
 * SafeReader — bounds-checked access to font data
 * ========================================================================= */

typedef struct {
    const uint8_t *data;
    uint32_t length;
} R8EFontReader;

R8E_MAYBE_UNUSED
static bool safe_read_u8(const R8EFontReader *r, uint32_t off, uint8_t *out) {
    if (off >= r->length) return false;
    *out = r->data[off];
    return true;
}

static bool safe_read_u16(const R8EFontReader *r, uint32_t off, uint16_t *out) {
    if (off + 2 > r->length) return false;
    *out = (uint16_t)((r->data[off] << 8) | r->data[off + 1]);  /* big-endian */
    return true;
}

static bool safe_read_i16(const R8EFontReader *r, uint32_t off, int16_t *out) {
    uint16_t u;
    if (!safe_read_u16(r, off, &u)) return false;
    *out = (int16_t)u;
    return true;
}

static bool safe_read_u32(const R8EFontReader *r, uint32_t off, uint32_t *out) {
    if (off + 4 > r->length) return false;
    *out = ((uint32_t)r->data[off] << 24) | ((uint32_t)r->data[off+1] << 16)
         | ((uint32_t)r->data[off+2] << 8) | r->data[off+3];
    return true;
}

static bool safe_read_tag(const R8EFontReader *r, uint32_t off, char tag[4]) {
    if (off + 4 > r->length) return false;
    tag[0] = (char)r->data[off]; tag[1] = (char)r->data[off+1];
    tag[2] = (char)r->data[off+2]; tag[3] = (char)r->data[off+3];
    return true;
}

/* =========================================================================
 * Font structure
 * ========================================================================= */

struct R8EFont {
    R8EFontReader reader;
    /* Table locations (validated at load time) */
    uint32_t head_off, head_len;
    uint32_t cmap_off, cmap_len;
    uint32_t glyf_off, glyf_len;
    uint32_t loca_off, loca_len;
    uint32_t hhea_off, hhea_len;
    uint32_t hmtx_off, hmtx_len;
    uint32_t maxp_off, maxp_len;
    uint32_t kern_off, kern_len;
    /* Parsed values */
    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_hmetrics;
    int16_t  loca_format;   /* 0=short, 1=long */
    /* cmap */
    uint32_t cmap_sub_off;  /* offset of selected cmap subtable */
    uint16_t cmap_format;
};

/* Match a 4-byte tag */
static bool tag_eq(const char a[4], const char *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

/* =========================================================================
 * Font loading with full validation
 * ========================================================================= */

R8EFont *r8e_font_load(const uint8_t *data, uint32_t length) {
    if (!data || length < 12) return NULL;

    R8EFontReader r = { data, length };

    /* Check sfVersion: 0x00010000 (TrueType) or 'true' */
    uint32_t version;
    if (!safe_read_u32(&r, 0, &version)) return NULL;
    if (version != 0x00010000 && version != 0x74727565) return NULL;

    uint16_t num_tables;
    if (!safe_read_u16(&r, 4, &num_tables)) return NULL;
    if (num_tables == 0 || num_tables > 100) return NULL;

    /* Validate we have space for all table records */
    uint32_t records_end = 12 + (uint32_t)num_tables * 16;
    if (records_end > length) return NULL;

    R8EFont *font = calloc(1, sizeof(R8EFont));
    if (!font) return NULL;
    font->reader = r;

    /* Parse table directory */
    for (uint16_t i = 0; i < num_tables; i++) {
        uint32_t rec = 12 + (uint32_t)i * 16;
        char tag[4];
        uint32_t toff, tlen;
        if (!safe_read_tag(&r, rec, tag)) goto fail;
        if (!safe_read_u32(&r, rec + 8, &toff)) goto fail;
        if (!safe_read_u32(&r, rec + 12, &tlen)) goto fail;

        /* Validate table fits in file */
        if (toff > length || tlen > length || toff + tlen > length) goto fail;

        if (tag_eq(tag, "head")) { font->head_off = toff; font->head_len = tlen; }
        else if (tag_eq(tag, "cmap")) { font->cmap_off = toff; font->cmap_len = tlen; }
        else if (tag_eq(tag, "glyf")) { font->glyf_off = toff; font->glyf_len = tlen; }
        else if (tag_eq(tag, "loca")) { font->loca_off = toff; font->loca_len = tlen; }
        else if (tag_eq(tag, "hhea")) { font->hhea_off = toff; font->hhea_len = tlen; }
        else if (tag_eq(tag, "hmtx")) { font->hmtx_off = toff; font->hmtx_len = tlen; }
        else if (tag_eq(tag, "maxp")) { font->maxp_off = toff; font->maxp_len = tlen; }
        else if (tag_eq(tag, "kern")) { font->kern_off = toff; font->kern_len = tlen; }
    }

    /* Required tables */
    if (!font->head_len || !font->cmap_len || !font->glyf_len ||
        !font->loca_len || !font->hhea_len || !font->hmtx_len ||
        !font->maxp_len) goto fail;

    /* Parse head table */
    if (font->head_len < 54) goto fail;
    uint32_t head_magic;
    if (!safe_read_u32(&r, font->head_off + 12, &head_magic)) goto fail;
    if (head_magic != 0x5F0F3CF5) goto fail;  /* magic number */
    if (!safe_read_u16(&r, font->head_off + 18, &font->units_per_em)) goto fail;
    if (font->units_per_em == 0) goto fail;
    if (!safe_read_i16(&r, font->head_off + 50, &font->loca_format)) goto fail;
    if (font->loca_format != 0 && font->loca_format != 1) goto fail;

    /* Parse maxp */
    if (!safe_read_u16(&r, font->maxp_off + 4, &font->num_glyphs)) goto fail;
    if (font->num_glyphs == 0) goto fail;

    /* Parse hhea */
    if (!safe_read_u16(&r, font->hhea_off + 34, &font->num_hmetrics)) goto fail;
    if (font->num_hmetrics == 0 || font->num_hmetrics > font->num_glyphs) goto fail;

    /* Validate loca table size */
    uint32_t expected_loca;
    if (font->loca_format == 0)
        expected_loca = ((uint32_t)font->num_glyphs + 1) * 2;
    else
        expected_loca = ((uint32_t)font->num_glyphs + 1) * 4;
    if (font->loca_len < expected_loca) goto fail;

    /* Find cmap subtable (prefer format 4 for BMP, or format 12 for full Unicode) */
    /* ... (implemented in Task 2) ... */
    font->cmap_format = 0;  /* placeholder */

    return font;

fail:
    free(font);
    return NULL;
}

void r8e_font_free(R8EFont *font) {
    free(font);
}

/* Stub implementations for later tasks */
R8EFont *r8e_font_load_default(void) { return NULL; }
R8EFont *r8e_font_load_default2(void) { return NULL; }

float r8e_font_scale(R8EFont *font, float pixel_height) {
    if (!font || font->units_per_em == 0) return 0;
    return pixel_height / (float)font->units_per_em;
}

void r8e_font_vmetrics(R8EFont *f, int *a, int *d, int *lg) {
    (void)f; *a = 0; *d = 0; *lg = 0;
}

void r8e_font_hmetrics(R8EFont *f, uint32_t g, int *adv, int *lsb) {
    (void)f; (void)g; *adv = 0; *lsb = 0;
}

int r8e_font_kern(R8EFont *f, uint32_t g1, uint32_t g2) {
    (void)f; (void)g1; (void)g2; return 0;
}

uint32_t r8e_font_glyph_id(R8EFont *f, uint32_t cp) {
    (void)f; (void)cp; return 0;
}

bool r8e_font_glyph_box(R8EFont *f, uint32_t g, float s,
                          int *x0, int *y0, int *x1, int *y1) {
    (void)f; (void)g; (void)s;
    *x0 = *y0 = *x1 = *y1 = 0;
    return false;
}

bool r8e_font_rasterize(R8EFont *f, uint32_t g, float s, R8EGlyphBitmap *out) {
    (void)f; (void)g; (void)s; (void)out;
    return false;
}
