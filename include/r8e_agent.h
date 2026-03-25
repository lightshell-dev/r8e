/*
 * r8e_agent.h - AI Agent Loop (generate → tool call → feed result → generate)
 *
 * Part of the r8e JavaScript engine.
 * Provides message formatting, tool call parsing, and conversation building
 * for LLM agent loops. The actual LLM call is provided by the caller.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_AGENT_H
#define R8E_AGENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Agent Status Codes
 * ========================================================================= */

#define R8E_AGENT_OK          0
#define R8E_AGENT_MAX_STEPS   1
#define R8E_AGENT_MAX_TOKENS  2
#define R8E_AGENT_TIMEOUT     3
#define R8E_AGENT_ERROR       4

/* =========================================================================
 * Tool Definition
 * ========================================================================= */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema for tool arguments */
} R8EAgentTool;

/* =========================================================================
 * Agent Configuration
 * ========================================================================= */

typedef struct {
    int          max_steps;      /* max LLM round-trips */
    int          max_tokens;     /* total token budget */
    uint64_t     timeout_us;     /* total timeout in microseconds */
    R8EAgentTool *tools;
    int           n_tools;
} R8EAgentConfig;

/* =========================================================================
 * Agent Result
 * ========================================================================= */

typedef struct {
    const char *text;           /* final response text */
    int         text_len;
    int         steps;          /* number of LLM round-trips */
    int         input_tokens;
    int         output_tokens;
    uint64_t    duration_us;
    int         status;         /* R8E_AGENT_OK .. R8E_AGENT_ERROR */
} R8EAgentResult;

/* =========================================================================
 * Tool Call (parsed from model output)
 * ========================================================================= */

typedef struct {
    const char *name;
    int         name_len;
    const char *args_json;
    int         args_len;
    const char *id;
    int         id_len;
} R8EToolCall;

/* =========================================================================
 * Agent API
 * ========================================================================= */

/**
 * Parse tool calls from a model response JSON (Anthropic Messages API format).
 *
 * Scans the response for content blocks of type "tool_use" and extracts
 * tool name, arguments, and ID.
 *
 * @param response_json  Raw JSON response body
 * @param len            Length of response_json
 * @param out_calls      Output array for parsed tool calls
 * @param max_calls      Capacity of out_calls array
 * @return               Number of tool calls parsed, or -1 on error
 */
int r8e_agent_parse_tool_calls(const char *response_json, int len,
                                R8EToolCall *out_calls, int max_calls);

/**
 * Format a tool result into a JSON message object.
 *
 * Produces: {"type":"tool_result","tool_use_id":"<id>","content":"<result>"}
 *
 * @param tool_call_id   The tool_use_id from the original tool call
 * @param result         The tool execution result string
 * @param out_buf        Output buffer for formatted JSON
 * @param buf_cap        Capacity of out_buf
 * @return               Bytes written, or -1 if buffer too small
 */
int r8e_agent_format_tool_result(const char *tool_call_id, const char *result,
                                  char *out_buf, int buf_cap);

/**
 * Check if a model response JSON contains tool calls.
 *
 * A lightweight check that avoids full parsing when you only need
 * to know whether to enter the tool-call handling path.
 *
 * @param response_json  Raw JSON response body
 * @param len            Length of response_json
 * @return               true if tool calls are present
 */
bool r8e_agent_has_tool_calls(const char *response_json, int len);

/**
 * Build the complete messages array JSON for an LLM API call.
 *
 * Constructs a JSON object with "system", "messages", and "tools" fields
 * suitable for the Anthropic Messages API.
 *
 * @param out_buf        Output buffer
 * @param buf_cap        Capacity of out_buf
 * @param system_prompt  System prompt text (may be NULL)
 * @param messages       Array of JSON message strings
 * @param n_messages     Number of messages
 * @param tools          Tool definitions (may be NULL if n_tools == 0)
 * @param n_tools        Number of tools
 * @return               Bytes written, or -1 if buffer too small
 */
int r8e_agent_build_messages(char *out_buf, int buf_cap,
                              const char *system_prompt,
                              const char **messages, int n_messages,
                              const R8EAgentTool *tools, int n_tools);

/**
 * Extract the text content from a model response JSON.
 *
 * Scans for content blocks of type "text" and returns the combined text.
 *
 * @param response_json  Raw JSON response body
 * @param len            Length of response_json
 * @param out_buf        Output buffer for text
 * @param buf_cap        Capacity of out_buf
 * @return               Bytes written, or -1 on error
 */
int r8e_agent_extract_text(const char *response_json, int len,
                            char *out_buf, int buf_cap);

/**
 * Extract the stop reason from a model response JSON.
 *
 * @param response_json  Raw JSON response body
 * @param len            Length of response_json
 * @param out_buf        Output buffer for stop reason string
 * @param buf_cap        Capacity of out_buf
 * @return               Bytes written, or -1 on error
 */
int r8e_agent_extract_stop_reason(const char *response_json, int len,
                                   char *out_buf, int buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* R8E_AGENT_H */
