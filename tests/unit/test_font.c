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

/* === Bundled Font Tests (added in later tasks) === */

void run_font_tests(void) {
    RUN_TEST(font_load_null);
    RUN_TEST(font_load_zero_length);
    RUN_TEST(font_load_truncated_1byte);
    RUN_TEST(font_load_truncated_header);
    RUN_TEST(font_load_bad_magic);
    RUN_TEST(font_load_table_offset_past_eof);
}
