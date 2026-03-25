/*
 * r8e_agent.c - AI Agent Loop (generate → tool call → feed result → generate)
 *
 * Part of the r8e JavaScript engine.
 *
 * Architecture:
 *   - Parses Anthropic Messages API response format for tool calls
 *   - Builds well-formed request JSON with system prompt, messages, and tools
 *   - Lightweight JSON scanning without full parser dependency
 *   - All string operations bounded to prevent buffer overflows
 *   - No heap allocations in hot paths (caller provides buffers)
 *
 * Anthropic Messages API response shape:
 *   {
 *     "content": [
 *       {"type": "text", "text": "..."},
 *       {"type": "tool_use", "id": "...", "name": "...", "input": {...}}
 *     ],
 *     "stop_reason": "end_turn" | "tool_use",
 *     "usage": {"input_tokens": N, "output_tokens": N}
 *   }
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "r8e_agent.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

/* =========================================================================
 * Internal Helpers - Lightweight JSON Scanning
 *
 * These functions scan JSON by looking for known field patterns without
 * building a full parse tree. This is safe because we control the expected
 * JSON shapes (Anthropic Messages API).
 * ========================================================================= */

/**
 * Find a JSON string value for a given key.
 * Scans for "key":"value" and returns pointer to the opening quote of value.
 * Returns NULL if not found.
 */
static const char *find_json_string_value(const char *json, int len,
                                           const char *key)
{
    int klen = (int)strlen(key);
    const char *end = json + len;
    const char *p = json;

    while (p < end - klen - 4) {  /* need room for "key":"x" minimum */
        /* Look for the key */
        if (*p == '"' && (p + klen + 1) < end &&
            memcmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            /* Skip past "key" */
            const char *after_key = p + klen + 2;
            /* Skip whitespace and colon */
            while (after_key < end && (*after_key == ' ' || *after_key == ':' ||
                   *after_key == '\t' || *after_key == '\n' || *after_key == '\r')) {
                after_key++;
            }
            if (after_key < end && *after_key == '"') {
                return after_key;
            }
        }
        p++;
    }
    return NULL;
}

/**
 * Extract a JSON string value into a buffer.
 * src should point to the opening '"'. Handles basic escape sequences.
 * Returns length written, or -1 on error.
 */
static int extract_json_string(const char *src, const char *end,
                                char *out, int cap)
{
    if (!src || src >= end || *src != '"') return -1;
    src++;  /* skip opening quote */

    int written = 0;
    while (src < end && *src != '"') {
        if (*src == '\\' && src + 1 < end) {
            src++;
            char esc = 0;
            switch (*src) {
                case '"':  esc = '"';  break;
                case '\\': esc = '\\'; break;
                case '/':  esc = '/';  break;
                case 'b':  esc = '\b'; break;
                case 'f':  esc = '\f'; break;
                case 'n':  esc = '\n'; break;
                case 'r':  esc = '\r'; break;
                case 't':  esc = '\t'; break;
                case 'u':
                    /* Skip \uXXXX - write as '?' for now */
                    if (src + 4 < end) src += 4;
                    esc = '?';
                    break;
                default:
                    esc = *src;
                    break;
            }
            if (written < cap) out[written] = esc;
            written++;
        } else {
            if (written < cap) out[written] = *src;
            written++;
        }
        src++;
    }

    if (written < cap) out[written] = '\0';
    return written;
}

/**
 * Get the raw extent of a JSON string (including quotes).
 * src points to opening '"'. Returns pointer past closing '"'.
 */
static const char *skip_json_string(const char *src, const char *end)
{
    if (!src || src >= end || *src != '"') return NULL;
    src++;  /* skip opening quote */

    while (src < end) {
        if (*src == '\\' && src + 1 < end) {
            src += 2;  /* skip escape sequence */
        } else if (*src == '"') {
            return src + 1;  /* past closing quote */
        } else {
            src++;
        }
    }
    return NULL;
}

/**
 * Skip whitespace in JSON.
 */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/**
 * Find the matching closing bracket/brace, respecting nesting and strings.
 * p points to the opening '{' or '['.
 * Returns pointer past the closing '}' or ']'.
 */
static const char *skip_json_value(const char *p, const char *end)
{
    if (p >= end) return NULL;

    p = skip_ws(p, end);
    if (p >= end) return NULL;

    if (*p == '"') {
        return skip_json_string(p, end);
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                p = skip_json_string(p, end);
                if (!p) return NULL;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return (depth == 0) ? p : NULL;
    }
    /* number, true, false, null */
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

/**
 * Find the start of a JSON object field value.
 * Searches within [start, end) for "key": and returns pointer to value start.
 */
static const char *find_field_value(const char *start, const char *end,
                                     const char *key)
{
    int klen = (int)strlen(key);
    const char *p = start;

    while (p < end - klen - 3) {
        if (*p == '"' && (p + klen + 1) < end &&
            memcmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            const char *after = p + klen + 2;
            after = skip_ws(after, end);
            if (after < end && *after == ':') {
                after++;
                return skip_ws(after, end);
            }
        }
        /* Advance past strings properly */
        if (*p == '"') {
            p = skip_json_string(p, end);
            if (!p) return NULL;
        } else {
            p++;
        }
    }
    return NULL;
}

/* =========================================================================
 * Buffer Writer Helper
 * ========================================================================= */

typedef struct {
    char *buf;
    int   cap;
    int   pos;
    bool  overflow;
} BufWriter;

static void bw_init(BufWriter *w, char *buf, int cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = 0;
    w->overflow = false;
}

static void bw_write(BufWriter *w, const char *data, int len)
{
    if (w->overflow) return;
    if (w->pos + len > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}

static void bw_str(BufWriter *w, const char *s)
{
    bw_write(w, s, (int)strlen(s));
}

/**
 * Write a JSON-escaped string (including surrounding quotes).
 */
static void bw_json_str(BufWriter *w, const char *s)
{
    if (!s) {
        bw_str(w, "null");
        return;
    }

    bw_write(w, "\"", 1);
    const char *p = s;
    while (*p) {
        const char *run = p;
        while (*p && *p != '"' && *p != '\\' && *p != '\n' &&
               *p != '\r' && *p != '\t' && *p != '\b' && *p != '\f' &&
               (unsigned char)*p >= 0x20)
            p++;
        if (p > run) bw_write(w, run, (int)(p - run));
        if (!*p) break;

        char esc[2] = { '\\', 0 };
        switch (*p) {
            case '"':  esc[1] = '"';  break;
            case '\\': esc[1] = '\\'; break;
            case '\n': esc[1] = 'n';  break;
            case '\r': esc[1] = 'r';  break;
            case '\t': esc[1] = 't';  break;
            case '\b': esc[1] = 'b';  break;
            case '\f': esc[1] = 'f';  break;
            default:
                /* Control char: write as \u00XX */
                {
                    char ubuf[7];
                    int n = snprintf(ubuf, sizeof(ubuf), "\\u%04x",
                                     (unsigned char)*p);
                    bw_write(w, ubuf, n);
                    p++;
                    continue;
                }
        }
        bw_write(w, esc, 2);
        p++;
    }
    bw_write(w, "\"", 1);
}

/* =========================================================================
 * Tool Call Parsing
 *
 * Parses Anthropic Messages API content array for "tool_use" blocks.
 * Each tool_use block looks like:
 *   {"type":"tool_use","id":"toolu_xxx","name":"tool_name","input":{...}}
 * ========================================================================= */

/**
 * Find the next content block of a given type within a JSON content array.
 * start should point inside a JSON array (past '[').
 * Returns pointer to the '{' of the matching block, or NULL.
 */
static const char *find_content_block(const char *start, const char *end,
                                       const char *type_value,
                                       const char **block_end)
{
    int tvlen = (int)strlen(type_value);
    const char *p = start;

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') return NULL;
        if (*p == ',') { p++; continue; }

        if (*p == '{') {
            const char *obj_start = p;
            const char *obj_end = skip_json_value(p, end);
            if (!obj_end) return NULL;

            /* Check type field within this object */
            const char *tv = find_field_value(obj_start, obj_end, "type");
            if (tv && *tv == '"' && tv + tvlen + 2 <= obj_end &&
                memcmp(tv + 1, type_value, tvlen) == 0 && tv[tvlen + 1] == '"') {
                if (block_end) *block_end = obj_end;
                return obj_start;
            }
            p = obj_end;
        } else {
            p = skip_json_value(p, end);
            if (!p) return NULL;
        }
    }
    return NULL;
}

int r8e_agent_parse_tool_calls(const char *response_json, int len,
                                R8EToolCall *out_calls, int max_calls)
{
    if (!response_json || len <= 0 || !out_calls || max_calls <= 0)
        return -1;

    const char *end = response_json + len;

    /* Find the "content" array */
    const char *content_val = find_field_value(response_json, end, "content");
    if (!content_val || *content_val != '[') return 0;

    const char *arr_end = skip_json_value(content_val, end);
    if (!arr_end) return -1;

    /* Scan content array for tool_use blocks */
    const char *p = content_val + 1;  /* skip '[' */
    int n_calls = 0;

    while (n_calls < max_calls) {
        const char *block_end = NULL;
        const char *block = find_content_block(p, arr_end, "tool_use", &block_end);
        if (!block) break;

        R8EToolCall *tc = &out_calls[n_calls];
        memset(tc, 0, sizeof(*tc));

        /* Extract "id" */
        const char *id_val = find_field_value(block, block_end, "id");
        if (id_val && *id_val == '"') {
            tc->id = id_val + 1;  /* point past opening quote */
            const char *id_end = skip_json_string(id_val, block_end);
            tc->id_len = id_end ? (int)(id_end - id_val - 2) : 0;
        }

        /* Extract "name" */
        const char *name_val = find_field_value(block, block_end, "name");
        if (name_val && *name_val == '"') {
            tc->name = name_val + 1;
            const char *name_end = skip_json_string(name_val, block_end);
            tc->name_len = name_end ? (int)(name_end - name_val - 2) : 0;
        }

        /* Extract "input" (raw JSON object) */
        const char *input_val = find_field_value(block, block_end, "input");
        if (input_val) {
            tc->args_json = input_val;
            const char *input_end = skip_json_value(input_val, block_end);
            tc->args_len = input_end ? (int)(input_end - input_val) : 0;
        }

        n_calls++;
        p = block_end;
    }

    return n_calls;
}

/* =========================================================================
 * Tool Result Formatting
 * ========================================================================= */

int r8e_agent_format_tool_result(const char *tool_call_id, const char *result,
                                  char *out_buf, int buf_cap)
{
    if (!tool_call_id || !result || !out_buf || buf_cap <= 0)
        return -1;

    BufWriter w;
    bw_init(&w, out_buf, buf_cap);

    bw_str(&w, "{\"type\":\"tool_result\",\"tool_use_id\":");
    bw_json_str(&w, tool_call_id);
    bw_str(&w, ",\"content\":");
    bw_json_str(&w, result);
    bw_str(&w, "}");

    if (w.overflow) return -1;

    /* Null-terminate if space */
    if (w.pos < w.cap)
        w.buf[w.pos] = '\0';

    return w.pos;
}

/* =========================================================================
 * Tool Call Detection
 * ========================================================================= */

bool r8e_agent_has_tool_calls(const char *response_json, int len)
{
    if (!response_json || len <= 0) return false;

    /*
     * Quick check: look for "tool_use" in the stop_reason field,
     * or for a "type":"tool_use" content block.
     *
     * First try stop_reason (fastest).
     */
    const char *end = response_json + len;
    const char *sr = find_field_value(response_json, end, "stop_reason");
    if (sr && *sr == '"') {
        if (sr + 9 < end && memcmp(sr + 1, "tool_use", 8) == 0)
            return true;
    }

    /*
     * Fallback: scan for "type":"tool_use" pattern.
     * This handles cases where stop_reason may not be present.
     */
    const char *needle = "\"tool_use\"";
    int nlen = 10;
    for (const char *p = response_json; p <= end - nlen; p++) {
        if (memcmp(p, needle, nlen) == 0)
            return true;
    }

    return false;
}

/* =========================================================================
 * Message Array Building
 *
 * Constructs a JSON request body for the Anthropic Messages API:
 *   {
 *     "system": "...",
 *     "messages": [...],
 *     "tools": [...]
 *   }
 * ========================================================================= */

/**
 * Write a single tool definition as JSON.
 */
static void write_tool_def(BufWriter *w, const R8EAgentTool *tool)
{
    bw_str(w, "{\"name\":");
    bw_json_str(w, tool->name);
    bw_str(w, ",\"description\":");
    bw_json_str(w, tool->description);
    bw_str(w, ",\"input_schema\":");
    if (tool->input_schema_json && tool->input_schema_json[0]) {
        /* Write raw JSON schema (assumed to be valid) */
        bw_str(w, tool->input_schema_json);
    } else {
        bw_str(w, "{\"type\":\"object\",\"properties\":{}}");
    }
    bw_str(w, "}");
}

int r8e_agent_build_messages(char *out_buf, int buf_cap,
                              const char *system_prompt,
                              const char **messages, int n_messages,
                              const R8EAgentTool *tools, int n_tools)
{
    if (!out_buf || buf_cap <= 0) return -1;

    BufWriter w;
    bw_init(&w, out_buf, buf_cap);

    bw_str(&w, "{");

    /* System prompt */
    if (system_prompt && system_prompt[0]) {
        bw_str(&w, "\"system\":");
        bw_json_str(&w, system_prompt);
        bw_str(&w, ",");
    }

    /* Messages array */
    bw_str(&w, "\"messages\":[");
    for (int i = 0; i < n_messages; i++) {
        if (i > 0) bw_str(&w, ",");
        if (messages[i]) {
            /* Messages are assumed to be pre-formatted JSON objects */
            bw_str(&w, messages[i]);
        }
    }
    bw_str(&w, "]");

    /* Tools array */
    if (tools && n_tools > 0) {
        bw_str(&w, ",\"tools\":[");
        for (int i = 0; i < n_tools; i++) {
            if (i > 0) bw_str(&w, ",");
            write_tool_def(&w, &tools[i]);
        }
        bw_str(&w, "]");
    }

    bw_str(&w, "}");

    if (w.overflow) return -1;

    if (w.pos < w.cap)
        w.buf[w.pos] = '\0';

    return w.pos;
}

/* =========================================================================
 * Text Extraction
 * ========================================================================= */

int r8e_agent_extract_text(const char *response_json, int len,
                            char *out_buf, int buf_cap)
{
    if (!response_json || len <= 0 || !out_buf || buf_cap <= 0)
        return -1;

    const char *end = response_json + len;

    /* Find the "content" array */
    const char *content_val = find_field_value(response_json, end, "content");
    if (!content_val || *content_val != '[') return 0;

    const char *arr_end = skip_json_value(content_val, end);
    if (!arr_end) return -1;

    int total_written = 0;
    const char *p = content_val + 1;

    while (p < arr_end) {
        const char *block_end = NULL;
        const char *block = find_content_block(p, arr_end, "text", &block_end);
        if (!block) break;

        /* Extract "text" field from this block */
        const char *text_val = find_field_value(block, block_end, "text");
        if (text_val && *text_val == '"') {
            int n = extract_json_string(text_val, block_end,
                                         out_buf + total_written,
                                         buf_cap - total_written);
            if (n > 0) total_written += n;
        }

        p = block_end;
    }

    if (total_written < buf_cap)
        out_buf[total_written] = '\0';

    return total_written;
}

/* =========================================================================
 * Stop Reason Extraction
 * ========================================================================= */

int r8e_agent_extract_stop_reason(const char *response_json, int len,
                                   char *out_buf, int buf_cap)
{
    if (!response_json || len <= 0 || !out_buf || buf_cap <= 0)
        return -1;

    const char *end = response_json + len;
    const char *sr = find_field_value(response_json, end, "stop_reason");
    if (!sr || *sr != '"') return 0;

    int n = extract_json_string(sr, end, out_buf, buf_cap);
    return n;
}
