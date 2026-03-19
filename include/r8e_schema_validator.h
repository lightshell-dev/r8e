/*
 * r8e_schema_validator.h - Compiled JSON Schema Validator for AI Tool Calls
 *
 * Part of the r8e JavaScript engine.
 *
 * Two-phase design:
 *   1. Compile: Parse JSON Schema string, emit validation bytecode (once per tool)
 *   2. Validate: Run bytecode against JSON string (per invocation, zero heap alloc)
 *
 * Supported JSON Schema subset (covers AI tool call argument validation):
 *   type, properties, required, items, enum, minLength, maxLength,
 *   minimum, maximum, minItems, maxItems, additionalProperties,
 *   oneOf, anyOf, description (ignored at validation time)
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_SCHEMA_VALIDATOR_H
#define R8E_SCHEMA_VALIDATOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Error Codes
 * ========================================================================= */

#define R8E_SCHEMA_OK               0
#define R8E_SCHEMA_ERR_TYPE         1   /* wrong JSON type */
#define R8E_SCHEMA_ERR_REQUIRED     2   /* missing required property */
#define R8E_SCHEMA_ERR_STRING_LEN   3   /* string length out of bounds */
#define R8E_SCHEMA_ERR_NUM_RANGE    4   /* number out of range */
#define R8E_SCHEMA_ERR_ENUM         5   /* value not in enum set */
#define R8E_SCHEMA_ERR_ARRAY_LEN    6   /* array length out of bounds */
#define R8E_SCHEMA_ERR_ADDITIONAL   7   /* additional properties not allowed */
#define R8E_SCHEMA_ERR_ONEOF        8   /* no branch matched (oneOf/anyOf) */
#define R8E_SCHEMA_ERR_COMPILE      9   /* schema compilation failed */
#define R8E_SCHEMA_ERR_JSON        10   /* malformed JSON input */

/* =========================================================================
 * Opaque Validator Handle
 * ========================================================================= */

typedef struct R8ESchemaValidator R8ESchemaValidator;

/* =========================================================================
 * API
 * ========================================================================= */

/**
 * Compile a JSON Schema string into a validator.
 *
 * The schema is parsed and compiled into bytecode for fast repeated
 * validation. Call once per tool registration.
 *
 * @param schema_json  JSON Schema string (UTF-8).
 * @param len          Length in bytes (0 = use strlen).
 * @return             Compiled validator, or NULL on compilation error.
 *                     Must be freed with r8e_schema_free().
 */
R8ESchemaValidator *r8e_schema_compile(const char *schema_json, int len);

/**
 * Validate a JSON string against a compiled schema.
 *
 * This function performs zero heap allocations. It walks the JSON input
 * using the compiled bytecode to verify structural and value constraints.
 *
 * @param v          Compiled schema validator.
 * @param json       JSON string to validate (UTF-8).
 * @param len        Length in bytes (0 = use strlen).
 * @param error_msg  If non-NULL and validation fails, set to a static
 *                   error description string.
 * @return           R8E_SCHEMA_OK (0) on success, >0 = error code.
 */
int r8e_schema_validate(const R8ESchemaValidator *v, const char *json,
                        int len, const char **error_msg);

/**
 * Free a compiled schema validator.
 *
 * @param v  Validator to free (NULL is safe, does nothing).
 */
void r8e_schema_free(R8ESchemaValidator *v);

#ifdef __cplusplus
}
#endif

#endif /* R8E_SCHEMA_VALIDATOR_H */
