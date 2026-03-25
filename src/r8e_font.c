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

    /* Find cmap subtable (prefer format 4 for BMP) */
    {
        uint16_t cmap_num_sub;
        if (!safe_read_u16(&r, font->cmap_off + 2, &cmap_num_sub)) goto fail;
        if (cmap_num_sub > 100) goto fail;

        uint32_t best_off = 0;
        int best_score = -1;
        for (uint16_t ci = 0; ci < cmap_num_sub; ci++) {
            uint32_t rec_off = font->cmap_off + 4 + (uint32_t)ci * 8;
            uint16_t platID, encID;
            uint32_t sub_offset;
            if (!safe_read_u16(&r, rec_off, &platID)) goto fail;
            if (!safe_read_u16(&r, rec_off + 2, &encID)) goto fail;
            if (!safe_read_u32(&r, rec_off + 4, &sub_offset)) goto fail;

            /* Score: prefer platformID=3,encodingID=1 (Windows Unicode BMP) */
            int score = -1;
            if (platID == 3 && encID == 1) score = 10;
            else if (platID == 0) score = 5;

            if (score > best_score) {
                uint32_t abs_off = font->cmap_off + sub_offset;
                uint16_t fmt;
                if (abs_off + 2 > length) continue;
                if (!safe_read_u16(&r, abs_off, &fmt)) continue;
                if (fmt == 4) {
                    best_off = abs_off;
                    best_score = score;
                }
            }
        }

        if (best_off != 0) {
            font->cmap_sub_off = best_off;
            font->cmap_format = 4;
        } else {
            font->cmap_format = 0;
        }
    }

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
    if (!f) { *a = 0; *d = 0; *lg = 0; return; }
    const R8EFontReader *r = &f->reader;
    int16_t ascent, descent, line_gap;
    if (!safe_read_i16(r, f->hhea_off + 4, &ascent)) { *a = 0; *d = 0; *lg = 0; return; }
    if (!safe_read_i16(r, f->hhea_off + 6, &descent)) { *a = 0; *d = 0; *lg = 0; return; }
    if (!safe_read_i16(r, f->hhea_off + 8, &line_gap)) { *a = 0; *d = 0; *lg = 0; return; }
    *a = ascent;
    *d = descent;
    *lg = line_gap;
}

void r8e_font_hmetrics(R8EFont *f, uint32_t g, int *adv, int *lsb) {
    if (!f) { *adv = 0; *lsb = 0; return; }
    const R8EFontReader *r = &f->reader;
    uint16_t advance;
    int16_t lsb_val;

    if (g < f->num_hmetrics) {
        uint32_t off = f->hmtx_off + g * 4;
        if (!safe_read_u16(r, off, &advance)) { *adv = 0; *lsb = 0; return; }
        if (!safe_read_i16(r, off + 2, &lsb_val)) { *adv = 0; *lsb = 0; return; }
    } else {
        /* Use last advance width, read lsb from extended array */
        uint32_t last_off = f->hmtx_off + (uint32_t)(f->num_hmetrics - 1) * 4;
        if (!safe_read_u16(r, last_off, &advance)) { *adv = 0; *lsb = 0; return; }
        uint32_t ext_off = f->hmtx_off + (uint32_t)f->num_hmetrics * 4
                         + (g - f->num_hmetrics) * 2;
        if (!safe_read_i16(r, ext_off, &lsb_val)) { *adv = 0; *lsb = 0; return; }
    }
    *adv = advance;
    *lsb = lsb_val;
}

int r8e_font_kern(R8EFont *f, uint32_t g1, uint32_t g2) {
    if (!f || f->kern_len == 0) return 0;
    const R8EFontReader *r = &f->reader;
    uint32_t base = f->kern_off;

    uint16_t version, n_tables;
    if (!safe_read_u16(r, base, &version)) return 0;
    if (!safe_read_u16(r, base + 2, &n_tables)) return 0;

    uint32_t sub_off = base + 4;
    for (uint16_t t = 0; t < n_tables; t++) {
        uint16_t sub_version, sub_length, coverage;
        if (!safe_read_u16(r, sub_off, &sub_version)) return 0;
        if (!safe_read_u16(r, sub_off + 2, &sub_length)) return 0;
        if (!safe_read_u16(r, sub_off + 4, &coverage)) return 0;

        /* format 0, horizontal kerning */
        uint16_t format = (uint16_t)(coverage >> 8);
        bool horizontal = (coverage & 1) != 0;
        if (format == 0 && horizontal) {
            uint16_t n_pairs;
            if (!safe_read_u16(r, sub_off + 6, &n_pairs)) return 0;

            /* Binary search the pairs */
            uint32_t pair_base = sub_off + 14; /* 6 header + 8 search params */
            uint32_t target = (g1 << 16) | g2;
            uint16_t lo = 0, hi = n_pairs;
            while (lo < hi) {
                uint16_t mid = (uint16_t)((lo + hi) / 2);
                uint32_t pair_off = pair_base + (uint32_t)mid * 6;
                uint16_t left, right;
                if (!safe_read_u16(r, pair_off, &left)) return 0;
                if (!safe_read_u16(r, pair_off + 2, &right)) return 0;
                uint32_t key = ((uint32_t)left << 16) | right;
                if (key < target) lo = (uint16_t)(mid + 1);
                else if (key > target) hi = mid;
                else {
                    int16_t value;
                    if (!safe_read_i16(r, pair_off + 4, &value)) return 0;
                    return value;
                }
            }
        }
        sub_off += sub_length;
    }
    return 0;
}

uint32_t r8e_font_glyph_id(R8EFont *f, uint32_t cp) {
    if (!f || f->cmap_format != 4 || cp > 0xFFFF) return 0;

    const R8EFontReader *r = &f->reader;
    uint32_t base = f->cmap_sub_off;

    uint16_t length16;
    if (!safe_read_u16(r, base + 2, &length16)) return 0;
    /* Bounds-check the whole subtable */
    if (base + length16 > r->length) return 0;

    uint16_t seg_count_x2;
    if (!safe_read_u16(r, base + 6, &seg_count_x2)) return 0;
    uint16_t seg_count = seg_count_x2 / 2;
    if (seg_count == 0 || seg_count > 10000) return 0;

    /* Array offsets within the subtable */
    uint32_t end_codes   = base + 14;
    uint32_t start_codes = end_codes + seg_count_x2 + 2; /* +2 for reservedPad */
    uint32_t id_deltas   = start_codes + seg_count_x2;
    uint32_t id_range_off= id_deltas + seg_count_x2;

    /* Binary search for the segment */
    uint16_t lo = 0, hi = seg_count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        uint16_t end_code;
        if (!safe_read_u16(r, end_codes + (uint32_t)mid * 2, &end_code)) return 0;
        if (end_code < cp)
            lo = (uint16_t)(mid + 1);
        else
            hi = mid;
    }
    if (lo >= seg_count) return 0;

    uint16_t end_code, start_code;
    int16_t id_delta;
    uint16_t range_off;
    if (!safe_read_u16(r, end_codes + (uint32_t)lo * 2, &end_code)) return 0;
    if (!safe_read_u16(r, start_codes + (uint32_t)lo * 2, &start_code)) return 0;
    if (!safe_read_i16(r, id_deltas + (uint32_t)lo * 2, &id_delta)) return 0;
    if (!safe_read_u16(r, id_range_off + (uint32_t)lo * 2, &range_off)) return 0;

    if (cp < start_code) return 0;

    if (range_off == 0) {
        return (uint32_t)((cp + (uint16_t)id_delta) & 0xFFFF);
    } else {
        /* idRangeOffset is relative to its own position in the array */
        uint32_t glyph_addr = id_range_off + (uint32_t)lo * 2
                            + range_off
                            + (cp - start_code) * 2;
        uint16_t glyph_id;
        if (!safe_read_u16(r, glyph_addr, &glyph_id)) return 0;
        if (glyph_id == 0) return 0;
        return (uint32_t)((glyph_id + (uint16_t)id_delta) & 0xFFFF);
    }
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
