#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/r8e_font.h"

extern int g_tests_run, g_tests_passed, g_tests_failed, g_assert_fail;
#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n" \
                "      at %s:%d\n", #expr, __FILE__, __LINE__); \
        g_assert_fail = 1; return; \
    } \
} while (0)
#define TEST(name) static void test_##name(void)

#define RUN_TEST(fn) do { \
    g_assert_fail = 0; \
    g_tests_run++; \
    printf("  %-60s ", #fn); \
    fflush(stdout); \
    test_##fn(); \
    if (g_assert_fail) { g_tests_failed++; printf("FAIL\n"); } \
    else { g_tests_passed++; printf("ok\n"); } \
    fflush(stdout); \
} while (0)

/* === Security Tests === */

TEST(font_load_null) {
    R8EFont *f = r8e_font_load(NULL, 0);
    ASSERT_TRUE(f == NULL);
}

TEST(font_load_zero_length) {
    uint8_t data[1] = {0};
    R8EFont *f = r8e_font_load(data, 0);
    ASSERT_TRUE(f == NULL);
}

TEST(font_load_truncated_1byte) {
    uint8_t data[1] = {0x00};
    R8EFont *f = r8e_font_load(data, 1);
    ASSERT_TRUE(f == NULL);
}

TEST(font_load_truncated_header) {
    /* A valid TrueType starts with 00 01 00 00 (sfVersion 1.0)
     * followed by numTables(2), searchRange(2), entrySelector(2), rangeShift(2) = 12 bytes
     * Truncate at 8 bytes */
    uint8_t data[8] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10};
    R8EFont *f = r8e_font_load(data, 8);
    ASSERT_TRUE(f == NULL);
}

TEST(font_load_bad_magic) {
    uint8_t data[12] = {0xDE, 0xAD, 0xBE, 0xEF, 0,0,0,0, 0,0,0,0};
    R8EFont *f = r8e_font_load(data, 12);
    ASSERT_TRUE(f == NULL);
}

TEST(font_load_table_offset_past_eof) {
    /* Valid header but table record points past end of data */
    uint8_t data[32];
    memset(data, 0, 32);
    data[0] = 0x00; data[1] = 0x01; /* sfVersion 1.0 */
    data[4] = 0x00; data[5] = 0x01; /* numTables = 1 */
    /* Table record at offset 12: tag(4) + checksum(4) + offset(4) + length(4) */
    data[12] = 'h'; data[13] = 'e'; data[14] = 'a'; data[15] = 'd';
    /* offset = 0xFFFF (past EOF) */
    data[20] = 0x00; data[21] = 0x00; data[22] = 0xFF; data[23] = 0xFF;
    /* length = 10 */
    data[24] = 0x00; data[25] = 0x00; data[26] = 0x00; data[27] = 0x0A;

    R8EFont *f = r8e_font_load(data, 32);
    ASSERT_TRUE(f == NULL);
}

/* === Helper to load a system font for testing === */
static uint8_t *load_test_font(uint32_t *out_len) {
    const char *paths[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = malloc((size_t)sz);
        if (!data) { fclose(f); continue; }
        fread(data, 1, (size_t)sz, f);
        fclose(f);
        *out_len = (uint32_t)sz;
        return data;
    }
    return NULL;
}

/* === cmap tests === */

TEST(font_load_system_font) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return; /* skip gracefully */
    R8EFont *f = r8e_font_load(data, len);
    ASSERT_TRUE(f != NULL);
    r8e_font_free(f);
    free(data);
}

TEST(font_glyph_id_A) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    uint32_t gid = r8e_font_glyph_id(f, 'A');
    ASSERT_TRUE(gid != 0);
    r8e_font_free(f);
    free(data);
}

TEST(font_glyph_id_space) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    uint32_t gid = r8e_font_glyph_id(f, ' ');
    ASSERT_TRUE(gid != 0);
    r8e_font_free(f);
    free(data);
}

TEST(font_glyph_id_a_differs_A) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    uint32_t gid_a = r8e_font_glyph_id(f, 'a');
    uint32_t gid_A = r8e_font_glyph_id(f, 'A');
    ASSERT_TRUE(gid_a != gid_A);
    r8e_font_free(f);
    free(data);
}

TEST(font_glyph_id_missing) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    uint32_t gid = r8e_font_glyph_id(f, 0xFFFF);
    ASSERT_TRUE(gid == 0);
    r8e_font_free(f);
    free(data);
}

/* === metrics tests === */

TEST(font_vmetrics) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    int ascent, descent, line_gap;
    r8e_font_vmetrics(f, &ascent, &descent, &line_gap);
    ASSERT_TRUE(ascent > 0);
    ASSERT_TRUE(descent <= 0);
    r8e_font_free(f);
    free(data);
}

TEST(font_hmetrics_A) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    uint32_t gid = r8e_font_glyph_id(f, 'A');
    if (gid == 0) { r8e_font_free(f); free(data); return; }
    int advance, lsb;
    r8e_font_hmetrics(f, gid, &advance, &lsb);
    ASSERT_TRUE(advance > 0);
    r8e_font_free(f);
    free(data);
}

TEST(font_hmetrics_space) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    uint32_t gid = r8e_font_glyph_id(f, ' ');
    if (gid == 0) { r8e_font_free(f); free(data); return; }
    int advance, lsb;
    r8e_font_hmetrics(f, gid, &advance, &lsb);
    ASSERT_TRUE(advance > 0);
    r8e_font_free(f);
    free(data);
}

TEST(font_scale) {
    uint32_t len = 0;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    if (!f) { free(data); return; }
    float s = r8e_font_scale(f, 32.0f);
    ASSERT_TRUE(s > 0.0f);
    ASSERT_TRUE(s < 1.0f);
    r8e_font_free(f);
    free(data);
}

/* === Glyph outline + rasterizer tests === */

TEST(font_glyph_box_A) {
    uint32_t len;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    ASSERT_TRUE(f != NULL);
    uint32_t gid = r8e_font_glyph_id(f, 'A');
    float scale = r8e_font_scale(f, 32.0f);
    int x0, y0, x1, y1;
    bool ok = r8e_font_glyph_box(f, gid, scale, &x0, &y0, &x1, &y1);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(x1 > x0);
    ASSERT_TRUE(y1 > y0);
    ASSERT_TRUE(x1 - x0 < 100);
    ASSERT_TRUE(y1 - y0 < 100);
    r8e_font_free(f);
    free(data);
}

TEST(font_rasterize_A) {
    uint32_t len;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    ASSERT_TRUE(f != NULL);
    uint32_t gid = r8e_font_glyph_id(f, 'A');
    float scale = r8e_font_scale(f, 32.0f);
    int x0, y0, x1, y1;
    r8e_font_glyph_box(f, gid, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    ASSERT_TRUE(w > 0 && h > 0);
    uint8_t *pixels = calloc((size_t)(w * h), 1);
    R8EGlyphBitmap bmp = { pixels, w, h, x0, y0 };
    bool ok = r8e_font_rasterize(f, gid, scale, &bmp);
    ASSERT_TRUE(ok);
    int nonzero = 0;
    for (int i = 0; i < w * h; i++) if (pixels[i] > 0) nonzero++;
    ASSERT_TRUE(nonzero > 0);
    free(pixels);
    r8e_font_free(f);
    free(data);
}

TEST(font_rasterize_all_ascii) {
    uint32_t len;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    ASSERT_TRUE(f != NULL);
    float scale = r8e_font_scale(f, 24.0f);
    int crash_count = 0;
    for (int c = 0x21; c <= 0x7E; c++) {
        uint32_t gid = r8e_font_glyph_id(f, (uint32_t)c);
        if (gid == 0) continue;
        int x0, y0, x1, y1;
        if (!r8e_font_glyph_box(f, gid, scale, &x0, &y0, &x1, &y1)) continue;
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) continue;
        uint8_t *px = calloc((size_t)(w * h), 1);
        R8EGlyphBitmap bmp = { px, w, h, x0, y0 };
        r8e_font_rasterize(f, gid, scale, &bmp);
        free(px);
    }
    (void)crash_count;
    r8e_font_free(f);
    free(data);
}

TEST(font_rasterize_size_scaling) {
    uint32_t len;
    uint8_t *data = load_test_font(&len);
    if (!data) return;
    R8EFont *f = r8e_font_load(data, len);
    ASSERT_TRUE(f != NULL);
    uint32_t gid = r8e_font_glyph_id(f, 'H');
    int x0_16, y0_16, x1_16, y1_16;
    int x0_32, y0_32, x1_32, y1_32;
    r8e_font_glyph_box(f, gid, r8e_font_scale(f, 16.0f), &x0_16, &y0_16, &x1_16, &y1_16);
    r8e_font_glyph_box(f, gid, r8e_font_scale(f, 32.0f), &x0_32, &y0_32, &x1_32, &y1_32);
    int h16 = y1_16 - y0_16;
    int h32 = y1_32 - y0_32;
    ASSERT_TRUE(h32 > h16);
    ASSERT_TRUE(h32 >= h16 * 3 / 2);
    r8e_font_free(f);
    free(data);
}

/* === Bundled Font Tests (added in later tasks) === */

void run_font_tests(void) {
    RUN_TEST(font_load_null);
    RUN_TEST(font_load_zero_length);
    RUN_TEST(font_load_truncated_1byte);
    RUN_TEST(font_load_truncated_header);
    RUN_TEST(font_load_bad_magic);
    RUN_TEST(font_load_table_offset_past_eof);

    /* cmap tests */
    RUN_TEST(font_load_system_font);
    RUN_TEST(font_glyph_id_A);
    RUN_TEST(font_glyph_id_space);
    RUN_TEST(font_glyph_id_a_differs_A);
    RUN_TEST(font_glyph_id_missing);

    /* metrics tests */
    RUN_TEST(font_vmetrics);
    RUN_TEST(font_hmetrics_A);
    RUN_TEST(font_hmetrics_space);
    RUN_TEST(font_scale);

    /* glyph outline + rasterizer tests */
    RUN_TEST(font_glyph_box_A);
    RUN_TEST(font_rasterize_A);
    RUN_TEST(font_rasterize_all_ascii);
    RUN_TEST(font_rasterize_size_scaling);
}
