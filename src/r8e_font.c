/* r8e_font.c — Hardened TrueType font parser and rasterizer
 * SPDX-License-Identifier: MIT
 */
#include "r8e_font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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

/* =========================================================================
 * Glyph outline parsing
 * ========================================================================= */

/* Get glyph offset within glyf table via loca table */
static bool get_glyph_offset(R8EFont *f, uint32_t glyph_id,
                              uint32_t *glyph_off, uint32_t *glyph_len) {
    if (glyph_id >= f->num_glyphs) return false;
    const R8EFontReader *r = &f->reader;
    uint32_t off0, off1;

    if (f->loca_format == 0) {
        /* Short format: uint16 entries, multiply by 2 */
        uint16_t v0, v1;
        if (!safe_read_u16(r, f->loca_off + glyph_id * 2, &v0)) return false;
        if (!safe_read_u16(r, f->loca_off + (glyph_id + 1) * 2, &v1)) return false;
        off0 = (uint32_t)v0 * 2;
        off1 = (uint32_t)v1 * 2;
    } else {
        /* Long format: uint32 entries */
        if (!safe_read_u32(r, f->loca_off + glyph_id * 4, &off0)) return false;
        if (!safe_read_u32(r, f->loca_off + (glyph_id + 1) * 4, &off1)) return false;
    }

    if (off0 == off1) {
        /* Empty glyph (e.g., space) */
        *glyph_off = 0;
        *glyph_len = 0;
        return true;
    }
    if (off0 > off1) return false;
    if (off0 >= f->glyf_len) return false;

    *glyph_off = f->glyf_off + off0;
    *glyph_len = off1 - off0;
    return true;
}

/* Internal outline representation */
typedef struct {
    float x, y;
    bool on_curve;
} FontPoint;

typedef struct {
    FontPoint *points;
    uint32_t count;
    uint16_t *contour_ends;
    uint16_t num_contours;
} FontOutline;

static void outline_free(FontOutline *o) {
    free(o->points);
    free(o->contour_ends);
    o->points = NULL;
    o->contour_ends = NULL;
}

/* Parse glyph outline, applying scale and Y-flip.
 * ascent is in font units; used to flip Y so baseline is at top. */
static bool parse_glyph_outline(R8EFont *font, uint32_t glyph_id,
                                  float scale, FontOutline *out) {
    memset(out, 0, sizeof(*out));
    const R8EFontReader *r = &font->reader;

    uint32_t goff, glen;
    if (!get_glyph_offset(font, glyph_id, &goff, &glen)) return false;
    if (glen == 0) return false; /* empty glyph */

    /* Read number of contours */
    int16_t num_contours;
    if (!safe_read_i16(r, goff, &num_contours)) return false;
    if (num_contours < 0) return false; /* composite glyph — skip for v1 */
    if (num_contours == 0) return false;

    /* Read contour end points */
    uint16_t nc = (uint16_t)num_contours;
    out->num_contours = nc;
    out->contour_ends = (uint16_t *)calloc(nc, sizeof(uint16_t));
    if (!out->contour_ends) return false;

    uint32_t cursor = goff + 10; /* skip header: 2(nContours) + 8(bbox) */
    for (uint16_t i = 0; i < nc; i++) {
        if (!safe_read_u16(r, cursor, &out->contour_ends[i])) goto fail;
        cursor += 2;
    }

    uint32_t total_points = (uint32_t)out->contour_ends[nc - 1] + 1;
    if (total_points > 65535) goto fail;

    /* Skip instructions */
    uint16_t inst_len;
    if (!safe_read_u16(r, cursor, &inst_len)) goto fail;
    cursor += 2 + inst_len;

    /* Parse flags (with repeat compression) */
    uint8_t *flags = (uint8_t *)calloc(total_points, 1);
    if (!flags) goto fail;

    {
        uint32_t fi = 0;
        while (fi < total_points) {
            uint8_t flag;
            if (!safe_read_u8(r, cursor, &flag)) { free(flags); goto fail; }
            cursor++;
            flags[fi++] = flag;
            if (flag & 0x08) { /* REPEAT bit */
                uint8_t repeat;
                if (!safe_read_u8(r, cursor, &repeat)) { free(flags); goto fail; }
                cursor++;
                for (uint8_t ri = 0; ri < repeat && fi < total_points; ri++) {
                    flags[fi++] = flag;
                }
            }
        }
    }

    /* Parse X coordinates (delta-encoded) */
    int16_t *xs = (int16_t *)calloc(total_points, sizeof(int16_t));
    int16_t *ys = (int16_t *)calloc(total_points, sizeof(int16_t));
    if (!xs || !ys) { free(flags); free(xs); free(ys); goto fail; }

    {
        int32_t x = 0;
        for (uint32_t i = 0; i < total_points; i++) {
            uint8_t fl = flags[i];
            if (fl & 0x02) { /* X_SHORT */
                uint8_t dx;
                if (!safe_read_u8(r, cursor, &dx)) { free(flags); free(xs); free(ys); goto fail; }
                cursor++;
                x += (fl & 0x10) ? (int32_t)dx : -(int32_t)dx;
            } else {
                if (!(fl & 0x10)) { /* not same, read int16 */
                    int16_t dx;
                    if (!safe_read_i16(r, cursor, &dx)) { free(flags); free(xs); free(ys); goto fail; }
                    cursor += 2;
                    x += dx;
                }
                /* else: same as previous (x unchanged) */
            }
            xs[i] = (int16_t)x;
        }
    }

    /* Parse Y coordinates */
    {
        int32_t y = 0;
        for (uint32_t i = 0; i < total_points; i++) {
            uint8_t fl = flags[i];
            if (fl & 0x04) { /* Y_SHORT */
                uint8_t dy;
                if (!safe_read_u8(r, cursor, &dy)) { free(flags); free(xs); free(ys); goto fail; }
                cursor++;
                y += (fl & 0x20) ? (int32_t)dy : -(int32_t)dy;
            } else {
                if (!(fl & 0x20)) {
                    int16_t dy;
                    if (!safe_read_i16(r, cursor, &dy)) { free(flags); free(xs); free(ys); goto fail; }
                    cursor += 2;
                    y += dy;
                }
            }
            ys[i] = (int16_t)y;
        }
    }

    /* Build points array with scale and Y-flip */
    out->points = (FontPoint *)calloc(total_points, sizeof(FontPoint));
    if (!out->points) { free(flags); free(xs); free(ys); goto fail; }
    out->count = total_points;

    for (uint32_t i = 0; i < total_points; i++) {
        out->points[i].x = (float)xs[i] * scale;
        out->points[i].y = -(float)ys[i] * scale; /* flip Y */
        out->points[i].on_curve = (flags[i] & 0x01) != 0;
    }

    free(flags);
    free(xs);
    free(ys);
    return true;

fail:
    outline_free(out);
    return false;
}

/* =========================================================================
 * Scanline Bezier rasterizer with 4x vertical supersampling
 * ========================================================================= */

#define SUPERSAMPLE 4
#define MAX_CROSSINGS 4096

typedef struct {
    float x;
    int winding;
} Crossing;

static int crossing_cmp(const void *a, const void *b) {
    float xa = ((const Crossing *)a)->x;
    float xb = ((const Crossing *)b)->x;
    if (xa < xb) return -1;
    if (xa > xb) return 1;
    return 0;
}

/* Add a line crossing at scanline y */
static int add_line_crossing(float x0, float y0, float x1, float y1,
                              float scan_y, Crossing *crossings, int count) {
    if (count >= MAX_CROSSINGS) return count;
    if ((y0 <= scan_y && y1 > scan_y) || (y1 <= scan_y && y0 > scan_y)) {
        float t = (scan_y - y0) / (y1 - y0);
        crossings[count].x = x0 + t * (x1 - x0);
        crossings[count].winding = (y1 > y0) ? 1 : -1;
        count++;
    }
    return count;
}

/* Add quadratic Bezier crossings at scanline y.
 * P0 -> P1 (control) -> P2 */
static int add_bezier_crossing(float p0x, float p0y, float p1x, float p1y,
                                float p2x, float p2y, float scan_y,
                                Crossing *crossings, int count) {
    float a = p0y - 2.0f * p1y + p2y;
    float b = 2.0f * (p1y - p0y);
    float c = p0y - scan_y;

    if (fabsf(a) < 1e-6f) {
        /* Degenerate to linear */
        if (fabsf(b) < 1e-6f) return count;
        float t = -c / b;
        if (t >= 0.0f && t < 1.0f && count < MAX_CROSSINGS) {
            float omt = 1.0f - t;
            crossings[count].x = omt * omt * p0x + 2.0f * omt * t * p1x + t * t * p2x;
            /* Winding: direction of curve at this t */
            float dy_dt = 2.0f * (1.0f - t) * (p1y - p0y) + 2.0f * t * (p2y - p1y);
            crossings[count].winding = (dy_dt > 0) ? 1 : -1;
            count++;
        }
        return count;
    }

    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return count;
    float sq = sqrtf(disc);
    float inv2a = 1.0f / (2.0f * a);

    for (int sign = -1; sign <= 1; sign += 2) {
        float t = (-b + (float)sign * sq) * inv2a;
        if (t >= 0.0f && t < 1.0f && count < MAX_CROSSINGS) {
            float omt = 1.0f - t;
            crossings[count].x = omt * omt * p0x + 2.0f * omt * t * p1x + t * t * p2x;
            float dy_dt = 2.0f * (1.0f - t) * (p1y - p0y) + 2.0f * t * (p2y - p1y);
            crossings[count].winding = (dy_dt > 0) ? 1 : -1;
            count++;
        }
    }
    return count;
}

static void rasterize_outline(const FontOutline *outline,
                                uint8_t *pixels, int width, int height,
                                float x_offset, float y_offset) {
    if (!outline || !pixels || width <= 0 || height <= 0) return;

    /* Accumulation buffer for supersampled rows (one row at a time) */
    float *accum = (float *)calloc((size_t)width, sizeof(float));
    if (!accum) return;

    Crossing *crossings = (Crossing *)malloc(sizeof(Crossing) * MAX_CROSSINGS);
    if (!crossings) { free(accum); return; }

    for (int py = 0; py < height; py++) {
        memset(accum, 0, (size_t)width * sizeof(float));

        for (int sub = 0; sub < SUPERSAMPLE; sub++) {
            float scan_y = y_offset + (float)py + ((float)sub + 0.5f) / (float)SUPERSAMPLE;
            int num_crossings = 0;

            /* Walk all contour edges */
            uint32_t contour_start = 0;
            for (uint16_t ci = 0; ci < outline->num_contours; ci++) {
                uint32_t contour_end = (uint32_t)outline->contour_ends[ci];
                uint32_t npts = contour_end - contour_start + 1;
                if (npts < 2) { contour_start = contour_end + 1; continue; }

                /* Walk points in this contour */
                for (uint32_t i = 0; i < npts; i++) {
                    uint32_t idx0 = contour_start + i;
                    uint32_t idx1 = contour_start + ((i + 1) % npts);

                    FontPoint p0 = outline->points[idx0];
                    FontPoint p1 = outline->points[idx1];

                    if (p0.on_curve && p1.on_curve) {
                        /* Line segment */
                        num_crossings = add_line_crossing(
                            p0.x, p0.y, p1.x, p1.y,
                            scan_y, crossings, num_crossings);
                    } else if (p0.on_curve && !p1.on_curve) {
                        /* Start of quadratic: on -> off -> on/off */
                        uint32_t idx2 = contour_start + ((i + 2) % npts);
                        FontPoint p2 = outline->points[idx2];
                        float end_x, end_y;
                        if (p2.on_curve) {
                            end_x = p2.x; end_y = p2.y;
                        } else {
                            /* Implied on-curve midpoint */
                            end_x = (p1.x + p2.x) * 0.5f;
                            end_y = (p1.y + p2.y) * 0.5f;
                        }
                        num_crossings = add_bezier_crossing(
                            p0.x, p0.y, p1.x, p1.y, end_x, end_y,
                            scan_y, crossings, num_crossings);
                    } else if (!p0.on_curve && p1.on_curve) {
                        /* off -> on: this is the end of a curve, handled by previous segment */
                        /* Skip — the previous on->off handled this */
                    } else {
                        /* off -> off: implied on-curve midpoint as start */
                        float mid_x = (p0.x + p1.x) * 0.5f;
                        float mid_y = (p0.y + p1.y) * 0.5f;
                        uint32_t idx2 = contour_start + ((i + 2) % npts);
                        FontPoint p2 = outline->points[idx2];
                        float end_x, end_y;
                        if (p2.on_curve) {
                            end_x = p2.x; end_y = p2.y;
                        } else {
                            end_x = (p1.x + p2.x) * 0.5f;
                            end_y = (p1.y + p2.y) * 0.5f;
                        }
                        num_crossings = add_bezier_crossing(
                            mid_x, mid_y, p1.x, p1.y, end_x, end_y,
                            scan_y, crossings, num_crossings);
                    }
                }

                contour_start = contour_end + 1;
            }

            /* Sort crossings by X */
            if (num_crossings > 1) {
                qsort(crossings, (size_t)num_crossings, sizeof(Crossing), crossing_cmp);
            }

            /* Fill using non-zero winding rule */
            int winding = 0;
            for (int ci = 0; ci < num_crossings; ci++) {
                int old_winding = winding;
                winding += crossings[ci].winding;
                if (old_winding == 0 && winding != 0) {
                    /* Start of filled region */
                } else if (old_winding != 0 && winding == 0) {
                    /* End of filled region — fill from previous crossing to this */
                    float x_start = crossings[ci - 1].x - x_offset;
                    float x_end = crossings[ci].x - x_offset;
                    /* Clamp to bitmap */
                    if (x_start < 0) x_start = 0;
                    if (x_end > (float)width) x_end = (float)width;
                    if (x_start >= x_end) continue;

                    int ix_start = (int)x_start;
                    int ix_end = (int)x_end;
                    if (ix_end > width) ix_end = width;

                    /* Partial first pixel */
                    if (ix_start < width) {
                        float frac = (float)(ix_start + 1) - x_start;
                        if (frac > x_end - x_start) frac = x_end - x_start;
                        accum[ix_start] += frac;
                    }
                    /* Full pixels */
                    for (int px = ix_start + 1; px < ix_end; px++) {
                        accum[px] += 1.0f;
                    }
                    /* Partial last pixel */
                    if (ix_end < width && ix_end > ix_start) {
                        float frac = x_end - (float)ix_end;
                        if (frac > 0) accum[ix_end] += frac;
                    }
                } else if (winding != 0) {
                    /* Still inside filled region — fill from previous crossing */
                    float x_start = crossings[ci - 1].x - x_offset;
                    float x_end = crossings[ci].x - x_offset;
                    if (x_start < 0) x_start = 0;
                    if (x_end > (float)width) x_end = (float)width;
                    if (x_start >= x_end) continue;

                    int ix_start = (int)x_start;
                    int ix_end = (int)x_end;
                    if (ix_end > width) ix_end = width;

                    if (ix_start < width) {
                        float frac = (float)(ix_start + 1) - x_start;
                        if (frac > x_end - x_start) frac = x_end - x_start;
                        accum[ix_start] += frac;
                    }
                    for (int px = ix_start + 1; px < ix_end; px++) {
                        accum[px] += 1.0f;
                    }
                    if (ix_end < width && ix_end > ix_start) {
                        float frac = x_end - (float)ix_end;
                        if (frac > 0) accum[ix_end] += frac;
                    }
                }
            }
        }

        /* Convert accumulated coverage to 8-bit grayscale */
        for (int px = 0; px < width; px++) {
            float coverage = accum[px] / (float)SUPERSAMPLE;
            if (coverage < 0) coverage = 0;
            if (coverage > 1.0f) coverage = 1.0f;
            int val = (int)(coverage * 255.0f + 0.5f);
            if (val > 255) val = 255;
            pixels[py * width + px] = (uint8_t)val;
        }
    }

    free(crossings);
    free(accum);
}

bool r8e_font_glyph_box(R8EFont *f, uint32_t g, float s,
                          int *x0, int *y0, int *x1, int *y1) {
    if (!f || !x0 || !y0 || !x1 || !y1) {
        if (x0) *x0 = 0; if (y0) *y0 = 0;
        if (x1) *x1 = 0; if (y1) *y1 = 0;
        return false;
    }

    const R8EFontReader *r = &f->reader;
    uint32_t goff, glen;
    if (!get_glyph_offset(f, g, &goff, &glen)) {
        *x0 = *y0 = *x1 = *y1 = 0;
        return false;
    }
    if (glen == 0) {
        *x0 = *y0 = *x1 = *y1 = 0;
        return false;
    }

    /* Read bounding box from glyph header (font units) */
    int16_t xMin, yMin, xMax, yMax;
    if (!safe_read_i16(r, goff + 2, &xMin)) { *x0 = *y0 = *x1 = *y1 = 0; return false; }
    if (!safe_read_i16(r, goff + 4, &yMin)) { *x0 = *y0 = *x1 = *y1 = 0; return false; }
    if (!safe_read_i16(r, goff + 6, &xMax)) { *x0 = *y0 = *x1 = *y1 = 0; return false; }
    if (!safe_read_i16(r, goff + 8, &yMax)) { *x0 = *y0 = *x1 = *y1 = 0; return false; }

    /* Scale and flip Y (TrueType Y up -> bitmap Y down) */
    *x0 = (int)floorf((float)xMin * s);
    *y0 = (int)floorf(-(float)yMax * s);
    *x1 = (int)ceilf((float)xMax * s);
    *y1 = (int)ceilf(-(float)yMin * s);

    return true;
}

bool r8e_font_rasterize(R8EFont *f, uint32_t g, float s, R8EGlyphBitmap *out) {
    if (!f || !out || !out->pixels) return false;
    if (out->width <= 0 || out->height <= 0) return false;

    FontOutline outline;
    if (!parse_glyph_outline(f, g, s, &outline)) return false;

    rasterize_outline(&outline, out->pixels, out->width, out->height,
                      (float)out->x_bearing, (float)out->y_bearing);

    outline_free(&outline);
    return true;
}
