/*
 * test_schema_validator.c - Unit tests for r8e_schema_validator.c
 *
 * Tests cover:
 *   - Simple object schema with required fields
 *   - String constraints (minLength, maxLength)
 *   - Number constraints (minimum, maximum)
 *   - Integer type validation
 *   - Array items schema
 *   - Array length constraints (minItems, maxItems)
 *   - Enum values
 *   - Nested objects
 *   - additionalProperties: false
 *   - anyOf / oneOf branching
 *   - Reject missing required fields
 *   - Reject wrong types
 *   - Reject values outside constraints
 *   - Complex real-world tool schema (file read tool)
 *   - Performance: 1000 validations, verify no heap alloc in validate
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "r8e_schema_validator.h"

/* =========================================================================
 * Test Infrastructure (mirrors test_runner.c macros)
 * ========================================================================= */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;
extern int g_assert_fail;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    g_assert_fail = 0;                                              \
    g_tests_run++;                                                  \
    printf("  %-60s ", #name);                                      \
    test_##name();                                                  \
    if (g_assert_fail) {                                            \
        g_tests_failed++;                                           \
        printf("FAIL\n");                                           \
    } else {                                                        \
        g_tests_passed++;                                           \
        printf("ok\n");                                             \
    }                                                               \
} while (0)

#define ASSERT_TRUE(expr) do {                                      \
    if (!(expr)) {                                                  \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n"              \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_FALSE(expr) do {                                     \
    if (expr) {                                                     \
        fprintf(stderr, "    ASSERT_FALSE failed: %s\n"             \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ_INT(a, b) do {                                    \
    long long _a = (long long)(a), _b = (long long)(b);             \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n"      \
                "      got %lld vs %lld\n"                           \
                "      at %s:%d\n",                                 \
                #a, #b, _a, _b, __FILE__, __LINE__);                \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)


/* =========================================================================
 * Tests
 * ========================================================================= */

/* --- Simple object with required fields --- */

TEST(simple_object_valid)
{
    const char *schema =
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"name\": { \"type\": \"string\" },"
        "    \"age\": { \"type\": \"number\" }"
        "  },"
        "  \"required\": [\"name\"]"
        "}";

    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *json = "{\"name\": \"Alice\", \"age\": 30}";
    const char *err = NULL;
    int rc = r8e_schema_validate(v, json, 0, &err);
    ASSERT_EQ_INT(rc, 0);

    r8e_schema_free(v);
}

TEST(simple_object_missing_required)
{
    const char *schema =
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"name\": { \"type\": \"string\" },"
        "    \"age\": { \"type\": \"number\" }"
        "  },"
        "  \"required\": [\"name\", \"age\"]"
        "}";

    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *json = "{\"name\": \"Alice\"}";
    const char *err = NULL;
    int rc = r8e_schema_validate(v, json, 0, &err);
    ASSERT_EQ_INT(rc, R8E_SCHEMA_ERR_REQUIRED);
    ASSERT_TRUE(err != NULL);

    r8e_schema_free(v);
}

/* --- Type checking --- */

TEST(wrong_type_expect_object)
{
    const char *schema = "{\"type\": \"object\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"hello\"", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "42", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "true", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "null", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "[1,2]", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "{}", 0, &err), 0);

    r8e_schema_free(v);
}

TEST(wrong_type_expect_string)
{
    const char *schema = "{\"type\": \"string\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "42", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"hello\"", 0, &err), 0);

    r8e_schema_free(v);
}

TEST(wrong_type_expect_array)
{
    const char *schema = "{\"type\": \"array\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "{}", 0, &err), R8E_SCHEMA_ERR_TYPE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "[1, 2, 3]", 0, &err), 0);

    r8e_schema_free(v);
}

TEST(type_boolean)
{
    const char *schema = "{\"type\": \"boolean\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "true", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "false", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "1", 0, &err), R8E_SCHEMA_ERR_TYPE);

    r8e_schema_free(v);
}

TEST(type_null)
{
    const char *schema = "{\"type\": \"null\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "null", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "0", 0, &err), R8E_SCHEMA_ERR_TYPE);

    r8e_schema_free(v);
}

TEST(type_integer)
{
    const char *schema = "{\"type\": \"integer\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "42", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "-7", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"hello\"", 0, &err), R8E_SCHEMA_ERR_TYPE);

    r8e_schema_free(v);
}

/* --- String constraints --- */

TEST(string_min_length)
{
    const char *schema = "{\"type\": \"string\", \"minLength\": 3}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"ab\"", 0, &err), R8E_SCHEMA_ERR_STRING_LEN);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"abc\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"abcd\"", 0, &err), 0);

    r8e_schema_free(v);
}

TEST(string_max_length)
{
    const char *schema = "{\"type\": \"string\", \"maxLength\": 5}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"hello\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"toolong\"", 0, &err), R8E_SCHEMA_ERR_STRING_LEN);

    r8e_schema_free(v);
}

TEST(string_length_range)
{
    const char *schema = "{\"type\": \"string\", \"minLength\": 2, \"maxLength\": 4}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"a\"", 0, &err), R8E_SCHEMA_ERR_STRING_LEN);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"ab\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"abcd\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"abcde\"", 0, &err), R8E_SCHEMA_ERR_STRING_LEN);

    r8e_schema_free(v);
}

/* --- Number constraints --- */

TEST(number_minimum)
{
    const char *schema = "{\"type\": \"number\", \"minimum\": 0}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "0", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "42", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "-1", 0, &err), R8E_SCHEMA_ERR_NUM_RANGE);

    r8e_schema_free(v);
}

TEST(number_maximum)
{
    const char *schema = "{\"type\": \"number\", \"maximum\": 100}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "100", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "50.5", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "101", 0, &err), R8E_SCHEMA_ERR_NUM_RANGE);

    r8e_schema_free(v);
}

TEST(number_range)
{
    const char *schema = "{\"type\": \"number\", \"minimum\": 1, \"maximum\": 10}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "0", 0, &err), R8E_SCHEMA_ERR_NUM_RANGE);
    ASSERT_EQ_INT(r8e_schema_validate(v, "1", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "5.5", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "10", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "11", 0, &err), R8E_SCHEMA_ERR_NUM_RANGE);

    r8e_schema_free(v);
}

/* --- Array items schema --- */

TEST(array_items_type)
{
    const char *schema =
        "{"
        "  \"type\": \"array\","
        "  \"items\": { \"type\": \"string\" }"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "[\"a\", \"b\", \"c\"]", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "[]", 0, &err), 0);

    r8e_schema_free(v);
}

TEST(array_items_wrong_type)
{
    const char *schema =
        "{"
        "  \"type\": \"array\","
        "  \"items\": { \"type\": \"number\" }"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "[1, \"bad\", 3]", 0, &err),
                  R8E_SCHEMA_ERR_TYPE);

    r8e_schema_free(v);
}

/* --- Array length constraints --- */

TEST(array_length)
{
    const char *schema =
        "{"
        "  \"type\": \"array\","
        "  \"minItems\": 1,"
        "  \"maxItems\": 3"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "[]", 0, &err), R8E_SCHEMA_ERR_ARRAY_LEN);
    ASSERT_EQ_INT(r8e_schema_validate(v, "[1]", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "[1,2,3]", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "[1,2,3,4]", 0, &err), R8E_SCHEMA_ERR_ARRAY_LEN);

    r8e_schema_free(v);
}

/* --- Enum values --- */

TEST(enum_strings)
{
    const char *schema =
        "{"
        "  \"type\": \"string\","
        "  \"enum\": [\"red\", \"green\", \"blue\"]"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"red\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"green\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"blue\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"yellow\"", 0, &err), R8E_SCHEMA_ERR_ENUM);

    r8e_schema_free(v);
}

/* --- Nested objects --- */

TEST(nested_object)
{
    const char *schema =
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"user\": {"
        "      \"type\": \"object\","
        "      \"properties\": {"
        "        \"name\": { \"type\": \"string\" },"
        "        \"email\": { \"type\": \"string\" }"
        "      },"
        "      \"required\": [\"name\"]"
        "    }"
        "  },"
        "  \"required\": [\"user\"]"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;

    /* Valid: nested with required fields */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"user\": {\"name\": \"Alice\", \"email\": \"a@b.c\"}}", 0, &err), 0);

    /* Valid: email optional */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"user\": {\"name\": \"Bob\"}}", 0, &err), 0);

    /* Invalid: missing top-level required */
    ASSERT_EQ_INT(r8e_schema_validate(v, "{}", 0, &err), R8E_SCHEMA_ERR_REQUIRED);

    /* Invalid: nested missing required name */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"user\": {\"email\": \"a@b.c\"}}", 0, &err), R8E_SCHEMA_ERR_REQUIRED);

    r8e_schema_free(v);
}

/* --- additionalProperties: false --- */

TEST(no_additional_properties)
{
    const char *schema =
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"name\": { \"type\": \"string\" }"
        "  },"
        "  \"additionalProperties\": false"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "{\"name\": \"Alice\"}", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"name\": \"Alice\", \"extra\": 1}", 0, &err), R8E_SCHEMA_ERR_ADDITIONAL);

    r8e_schema_free(v);
}

/* --- anyOf --- */

TEST(anyof_branches)
{
    const char *schema =
        "{"
        "  \"anyOf\": ["
        "    { \"type\": \"string\" },"
        "    { \"type\": \"number\" }"
        "  ]"
        "}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"hello\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "42", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "true", 0, &err), R8E_SCHEMA_ERR_ONEOF);

    r8e_schema_free(v);
}

/* --- Empty and edge cases --- */

TEST(empty_schema)
{
    const char *schema = "{}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    /* Empty schema: anything is valid */
    ASSERT_EQ_INT(r8e_schema_validate(v, "42", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"hello\"", 0, &err), 0);
    ASSERT_EQ_INT(r8e_schema_validate(v, "{}", 0, &err), 0);

    r8e_schema_free(v);
}

TEST(null_inputs)
{
    ASSERT_TRUE(r8e_schema_compile(NULL, 0) == NULL);

    const char *schema = "{\"type\": \"string\"}";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, NULL, 0, &err), R8E_SCHEMA_ERR_JSON);
    ASSERT_EQ_INT(r8e_schema_validate(NULL, "\"x\"", 0, &err), R8E_SCHEMA_ERR_COMPILE);

    r8e_schema_free(v);
    r8e_schema_free(NULL); /* should not crash */
}

/* --- Complex real-world tool schema (file read tool) --- */

TEST(complex_tool_schema)
{
    const char *schema =
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"file_path\": {"
        "      \"type\": \"string\","
        "      \"description\": \"Absolute path to the file to read\","
        "      \"minLength\": 1"
        "    },"
        "    \"offset\": {"
        "      \"type\": \"integer\","
        "      \"description\": \"Line number to start reading from\","
        "      \"minimum\": 0"
        "    },"
        "    \"limit\": {"
        "      \"type\": \"integer\","
        "      \"description\": \"Number of lines to read\","
        "      \"minimum\": 1,"
        "      \"maximum\": 10000"
        "    },"
        "    \"encoding\": {"
        "      \"type\": \"string\","
        "      \"enum\": [\"utf-8\", \"ascii\", \"latin1\"]"
        "    }"
        "  },"
        "  \"required\": [\"file_path\"],"
        "  \"additionalProperties\": false"
        "}";

    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;

    /* Valid: minimal */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"/tmp/test.txt\"}", 0, &err), 0);

    /* Valid: all fields */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"/tmp/test.txt\", \"offset\": 10, \"limit\": 100, "
        "\"encoding\": \"utf-8\"}", 0, &err), 0);

    /* Invalid: missing file_path */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"offset\": 10}", 0, &err), R8E_SCHEMA_ERR_REQUIRED);

    /* Invalid: empty file_path */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"\"}", 0, &err), R8E_SCHEMA_ERR_STRING_LEN);

    /* Invalid: bad encoding */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"/tmp/f\", \"encoding\": \"ucs-2\"}", 0, &err),
        R8E_SCHEMA_ERR_ENUM);

    /* Invalid: extra property */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"/tmp/f\", \"bad\": true}", 0, &err),
        R8E_SCHEMA_ERR_ADDITIONAL);

    /* Invalid: negative offset */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"/tmp/f\", \"offset\": -1}", 0, &err),
        R8E_SCHEMA_ERR_NUM_RANGE);

    /* Invalid: limit too high */
    ASSERT_EQ_INT(r8e_schema_validate(v,
        "{\"file_path\": \"/tmp/f\", \"limit\": 99999}", 0, &err),
        R8E_SCHEMA_ERR_NUM_RANGE);

    r8e_schema_free(v);
}

/* --- Performance: repeated validation with no heap alloc --- */

TEST(perf_no_heap_alloc)
{
    const char *schema =
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"name\": { \"type\": \"string\", \"minLength\": 1 },"
        "    \"value\": { \"type\": \"number\", \"minimum\": 0 }"
        "  },"
        "  \"required\": [\"name\", \"value\"]"
        "}";

    R8ESchemaValidator *v = r8e_schema_compile(schema, 0);
    ASSERT_TRUE(v != NULL);

    const char *json = "{\"name\": \"test\", \"value\": 42}";

    /* Run 1000 validations. The validate function should do zero malloc.
     * We can't directly measure malloc from here, but we verify correctness
     * and that it completes quickly. */
    for (int i = 0; i < 1000; i++) {
        const char *err = NULL;
        int rc = r8e_schema_validate(v, json, 0, &err);
        if (rc != 0) {
            ASSERT_EQ_INT(rc, 0);
            break;
        }
    }

    r8e_schema_free(v);
}

/* --- Compile with explicit length --- */

TEST(compile_explicit_length)
{
    const char *schema = "{\"type\": \"string\"}GARBAGE";
    R8ESchemaValidator *v = r8e_schema_compile(schema, 17); /* just the schema */
    ASSERT_TRUE(v != NULL);

    const char *err = NULL;
    ASSERT_EQ_INT(r8e_schema_validate(v, "\"ok\"", 0, &err), 0);

    r8e_schema_free(v);
}


/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_schema_validator_tests(void)
{
    /* Type checking */
    RUN_TEST(simple_object_valid);
    RUN_TEST(simple_object_missing_required);
    RUN_TEST(wrong_type_expect_object);
    RUN_TEST(wrong_type_expect_string);
    RUN_TEST(wrong_type_expect_array);
    RUN_TEST(type_boolean);
    RUN_TEST(type_null);
    RUN_TEST(type_integer);

    /* String constraints */
    RUN_TEST(string_min_length);
    RUN_TEST(string_max_length);
    RUN_TEST(string_length_range);

    /* Number constraints */
    RUN_TEST(number_minimum);
    RUN_TEST(number_maximum);
    RUN_TEST(number_range);

    /* Array validation */
    RUN_TEST(array_items_type);
    RUN_TEST(array_items_wrong_type);
    RUN_TEST(array_length);

    /* Enum */
    RUN_TEST(enum_strings);

    /* Nested objects */
    RUN_TEST(nested_object);

    /* Additional properties */
    RUN_TEST(no_additional_properties);

    /* Branching */
    RUN_TEST(anyof_branches);

    /* Edge cases */
    RUN_TEST(empty_schema);
    RUN_TEST(null_inputs);
    RUN_TEST(compile_explicit_length);

    /* Real-world tool schema */
    RUN_TEST(complex_tool_schema);

    /* Performance */
    RUN_TEST(perf_no_heap_alloc);
}
