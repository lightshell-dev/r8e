/*
 * r8e_sse_parser.c - SSE (Server-Sent Events) Stream Parser for AI Providers
 *
 * Part of the r8e JavaScript engine.
 *
 * Architecture:
 *   - Incremental SSE framing: buffers partial lines across TCP chunks
 *   - Provider-specific JSON extraction via targeted string scanning
 *     (no full JSON parser needed for known response shapes)
 *   - Normalizes Anthropic and OpenAI SSE formats into R8EAIEvent
 *   - Zero external dependencies
 *
 * SSE Protocol (https://html.spec.whatwg.org/multipage/server-sent-events.html):
 *   - Lines starting with "data:" contain event payload
 *   - Lines starting with "event:" set event type
 *   - Lines starting with "id:" set event ID
 *   - Lines starting with ":" are comments (ignored)
 *   - Empty lines ("\n\n") delimit events
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "r8e_sse_parser.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal Constants
 * ========================================================================= */

#define SSE_LINE_BUF_INIT   512
#define SSE_LINE_BUF_MAX    65536
#define SSE_EVENT_TYPE_MAX  64
#define SSE_DATA_BUF_INIT   1024

/* =========================================================================
 * Parser State
 * ========================================================================= */

struct R8ESSEParser {
    uint8_t  provider;

    /* Line accumulation buffer (handles partial lines across chunks) */
    char    *line_buf;
    int      line_len;
    int      line_cap;

    /* Current SSE event fields (accumulated across lines) */
    char     event_type[SSE_EVENT_TYPE_MAX];
    int      event_type_len;
    char    *data_buf;
    int      data_len;
    int      data_cap;
    char     event_id[64];
    int      event_id_len;

    /* Track whether we have any fields for the current event */
    int      has_fields;
};

/* =========================================================================
 * Internal: Memory Helpers
 * ========================================================================= */

static int sse_grow_buf(char **buf, int *cap, int needed)
{
    if (needed <= *cap) return 1;
    int new_cap = *cap * 2;
    if (new_cap < needed) new_cap = needed;
    if (new_cap > SSE_LINE_BUF_MAX) {
        if (needed > SSE_LINE_BUF_MAX) return 0;
        new_cap = SSE_LINE_BUF_MAX;
    }
    char *tmp = (char *)realloc(*buf, (size_t)new_cap);
    if (!tmp) return 0;
    *buf = tmp;
    *cap = new_cap;
    return 1;
}

/* =========================================================================
 * Internal: Minimal JSON Field Extraction
 *
 * These functions scan known JSON structures to extract specific fields
 * without a full JSON parser. They handle escaped quotes within strings.
 * ========================================================================= */

/*
 * Find the value of a string field: "key":"value"
 * Returns pointer to first char of value, sets *out_len.
 * Returns NULL if not found.
 */
static const char *json_find_str(const char *json, int json_len,
                                 const char *key, int key_len,
                                 int *out_len)
{
    /* Search for "key":" pattern */
    const char *end = json + json_len;
    const char *p = json;

    while (p < end) {
        /* Find next quote */
        const char *q = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!q) return NULL;

        /* Check if this matches our key */
        if (q + 1 + key_len + 1 < end &&
            memcmp(q + 1, key, (size_t)key_len) == 0 &&
            q[1 + key_len] == '"')
        {
            /* Found the key - skip to value */
            const char *after_key = q + 1 + key_len + 1;

            /* Skip optional whitespace and colon */
            while (after_key < end && (*after_key == ' ' || *after_key == ':'))
                after_key++;

            if (after_key >= end || *after_key != '"') {
                p = after_key;
                continue;
            }

            /* Extract string value (handle escaped quotes) */
            const char *val_start = after_key + 1;
            const char *v = val_start;
            while (v < end) {
                if (*v == '\\') {
                    v += 2; /* skip escaped char */
                    continue;
                }
                if (*v == '"') break;
                v++;
            }
            *out_len = (int)(v - val_start);
            return val_start;
        }

        p = q + 1;
    }
    return NULL;
}

/*
 * Find integer value for a field: "key":1234
 * Returns the integer, or -1 if not found.
 */
static int json_find_int(const char *json, int json_len,
                         const char *key, int key_len)
{
    const char *end = json + json_len;
    const char *p = json;

    while (p < end) {
        const char *q = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!q) return -1;

        if (q + 1 + key_len + 1 < end &&
            memcmp(q + 1, key, (size_t)key_len) == 0 &&
            q[1 + key_len] == '"')
        {
            const char *after_key = q + 1 + key_len + 1;
            while (after_key < end && (*after_key == ' ' || *after_key == ':'))
                after_key++;

            /* Parse integer */
            int val = 0;
            int neg = 0;
            if (after_key < end && *after_key == '-') {
                neg = 1;
                after_key++;
            }
            while (after_key < end && *after_key >= '0' && *after_key <= '9') {
                val = val * 10 + (*after_key - '0');
                after_key++;
            }
            return neg ? -val : val;
        }

        p = q + 1;
    }
    return -1;
}

/*
 * Find the Nth occurrence of a string field value.
 * start_after: pointer into json to start searching after (NULL = beginning).
 */
static const char *json_find_str_after(const char *json, int json_len,
                                       const char *key, int key_len,
                                       int *out_len,
                                       const char *start_after)
{
    const char *end = json + json_len;
    const char *p = start_after ? start_after : json;

    while (p < end) {
        const char *q = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!q) return NULL;

        if (q + 1 + key_len + 1 < end &&
            memcmp(q + 1, key, (size_t)key_len) == 0 &&
            q[1 + key_len] == '"')
        {
            const char *after_key = q + 1 + key_len + 1;
            while (after_key < end && (*after_key == ' ' || *after_key == ':'))
                after_key++;

            if (after_key >= end || *after_key != '"') {
                p = after_key;
                continue;
            }

            const char *val_start = after_key + 1;
            const char *v = val_start;
            while (v < end) {
                if (*v == '\\') { v += 2; continue; }
                if (*v == '"') break;
                v++;
            }
            *out_len = (int)(v - val_start);
            return val_start;
        }

        p = q + 1;
    }
    return NULL;
}

/*
 * Check if a JSON string contains a field with a specific string value.
 * Checks ALL occurrences of the key, not just the first.
 * Returns 1 if "key":"value" is found.
 */
static int json_has_str_value(const char *json, int json_len,
                              const char *key, int key_len,
                              const char *value, int value_len)
{
    const char *start = NULL;
    for (;;) {
        int found_len = 0;
        const char *found = json_find_str_after(json, json_len, key, key_len,
                                                &found_len, start);
        if (!found) return 0;
        if (found_len == value_len &&
            memcmp(found, value, (size_t)value_len) == 0)
            return 1;
        /* Move past this match */
        start = found + found_len + 1;
    }
}

/*
 * Unescape a JSON string in-place. Handles \n, \t, \\, \", \/, \uXXXX (BMP only).
 * Returns new length.
 */
static int json_unescape(char *buf, int len)
{
    int r = 0, w = 0;
    while (r < len) {
        if (buf[r] == '\\' && r + 1 < len) {
            r++;
            switch (buf[r]) {
            case 'n':  buf[w++] = '\n'; r++; break;
            case 't':  buf[w++] = '\t'; r++; break;
            case 'r':  buf[w++] = '\r'; r++; break;
            case '\\': buf[w++] = '\\'; r++; break;
            case '"':  buf[w++] = '"';  r++; break;
            case '/':  buf[w++] = '/';  r++; break;
            case 'u':
                /* Basic \uXXXX - output as UTF-8 */
                if (r + 4 < len) {
                    unsigned cp = 0;
                    int i;
                    for (i = 1; i <= 4; i++) {
                        char c = buf[r + i];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                    }
                    if (cp < 0x80) {
                        buf[w++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[w++] = (char)(0xC0 | (cp >> 6));
                        buf[w++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[w++] = (char)(0xE0 | (cp >> 12));
                        buf[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[w++] = (char)(0x80 | (cp & 0x3F));
                    }
                    r += 5;
                } else {
                    buf[w++] = buf[r++]; /* malformed, copy as-is */
                }
                break;
            default:
                buf[w++] = buf[r++]; /* unknown escape, copy as-is */
                break;
            }
        } else {
            buf[w++] = buf[r++];
        }
    }
    return w;
}

/* =========================================================================
 * Internal: Provider-Specific Event Dispatch
 * ========================================================================= */

static void dispatch_anthropic(R8ESSEParser *p,
                               void (*cb)(R8EAIEvent *, void *), void *ud)
{
    const char *data = p->data_buf;
    int dlen = p->data_len;
    const char *etype = p->event_type;
    int elen = p->event_type_len;

    /* Check for [DONE] sentinel */
    if (dlen >= 6 && memcmp(data, "[DONE]", 6) == 0) {
        R8EAIEvent ev = {0};
        ev.type = R8E_AI_EVENT_DONE;
        cb(&ev, ud);
        return;
    }

    R8EAIEvent ev = {0};

    /* content_block_delta -> text delta */
    if (elen == 19 && memcmp(etype, "content_block_delta", 19) == 0) {
        if (json_has_str_value(data, dlen, "type", 4, "text_delta", 10)) {
            int text_len = 0;
            const char *text = json_find_str(data, dlen, "text", 4, &text_len);
            if (text) {
                /* Copy to data_buf tail for unescaping */
                int offset = dlen + 1;
                if (sse_grow_buf(&p->data_buf, &p->data_cap, offset + text_len + 1)) {
                    memcpy(p->data_buf + offset, text, (size_t)text_len);
                    int unesc_len = json_unescape(p->data_buf + offset, text_len);
                    p->data_buf[offset + unesc_len] = '\0';
                    ev.type = R8E_AI_EVENT_TEXT_DELTA;
                    ev.text = p->data_buf + offset;
                    ev.text_len = unesc_len;
                    cb(&ev, ud);
                }
            }
        }
        /* input_json_delta -> tool call delta */
        else if (json_has_str_value(data, dlen, "type", 4,
                                    "input_json_delta", 16)) {
            int pj_len = 0;
            const char *pj = json_find_str(data, dlen,
                                           "partial_json", 12, &pj_len);
            if (pj) {
                int offset = dlen + 1;
                if (sse_grow_buf(&p->data_buf, &p->data_cap, offset + pj_len + 1)) {
                    memcpy(p->data_buf + offset, pj, (size_t)pj_len);
                    int unesc_len = json_unescape(p->data_buf + offset, pj_len);
                    p->data_buf[offset + unesc_len] = '\0';
                    ev.type = R8E_AI_EVENT_TOOL_CALL_DELTA;
                    ev.text = p->data_buf + offset;
                    ev.text_len = unesc_len;
                    cb(&ev, ud);
                }
            }
        }
        return;
    }

    /* content_block_start -> tool call start */
    if (elen == 19 && memcmp(etype, "content_block_start", 19) == 0) {
        if (json_has_str_value(data, dlen, "type", 4, "tool_use", 8)) {
            int name_len = 0, id_len = 0;
            const char *name = json_find_str(data, dlen, "name", 4, &name_len);
            const char *id = json_find_str(data, dlen, "id", 2, &id_len);
            ev.type = R8E_AI_EVENT_TOOL_CALL_START;
            ev.tool_name = name;
            ev.tool_name_len = name_len;
            ev.tool_id = id;
            ev.tool_id_len = id_len;
            cb(&ev, ud);
        }
        return;
    }

    /* content_block_stop -> tool call end */
    if (elen == 18 && memcmp(etype, "content_block_stop", 18) == 0) {
        ev.type = R8E_AI_EVENT_TOOL_CALL_END;
        cb(&ev, ud);
        return;
    }

    /* message_delta -> usage info */
    if (elen == 13 && memcmp(etype, "message_delta", 13) == 0) {
        int out_tok = json_find_int(data, dlen, "output_tokens", 13);
        if (out_tok >= 0) {
            ev.type = R8E_AI_EVENT_USAGE;
            ev.output_tokens = out_tok;
            ev.input_tokens = 0;
            cb(&ev, ud);
        }
        return;
    }

    /* message_start -> may contain usage (input_tokens) */
    if (elen == 13 && memcmp(etype, "message_start", 13) == 0) {
        int in_tok = json_find_int(data, dlen, "input_tokens", 12);
        if (in_tok >= 0) {
            ev.type = R8E_AI_EVENT_USAGE;
            ev.input_tokens = in_tok;
            ev.output_tokens = 0;
            cb(&ev, ud);
        }
        return;
    }

    /* message_stop -> done */
    if (elen == 12 && memcmp(etype, "message_stop", 12) == 0) {
        ev.type = R8E_AI_EVENT_DONE;
        cb(&ev, ud);
        return;
    }

    /* error event */
    if (elen == 5 && memcmp(etype, "error", 5) == 0) {
        int msg_len = 0;
        const char *msg = json_find_str(data, dlen, "message", 7, &msg_len);
        ev.type = R8E_AI_EVENT_ERROR;
        ev.error_msg = msg;
        ev.error_msg_len = msg_len;
        cb(&ev, ud);
        return;
    }
}

static void dispatch_openai(R8ESSEParser *p,
                            void (*cb)(R8EAIEvent *, void *), void *ud)
{
    const char *data = p->data_buf;
    int dlen = p->data_len;

    /* Check for [DONE] sentinel */
    if (dlen >= 6 && memcmp(data, "[DONE]", 6) == 0) {
        R8EAIEvent ev = {0};
        ev.type = R8E_AI_EVENT_DONE;
        cb(&ev, ud);
        return;
    }

    R8EAIEvent ev = {0};

    /* Check for usage in response (top-level "usage" object) */
    {
        int prompt_tok = json_find_int(data, dlen, "prompt_tokens", 13);
        int comp_tok = json_find_int(data, dlen, "completion_tokens", 17);
        if (prompt_tok >= 0 || comp_tok >= 0) {
            /* Only emit usage if no "choices" in this chunk or if this is
               the final usage-only message */
            const char *choices = json_find_str(data, dlen, "choices", 7,
                                                &(int){0});
            if (!choices) {
                ev.type = R8E_AI_EVENT_USAGE;
                ev.input_tokens = prompt_tok >= 0 ? prompt_tok : 0;
                ev.output_tokens = comp_tok >= 0 ? comp_tok : 0;
                cb(&ev, ud);
                return;
            }
        }
    }

    /* Look for delta content: "content":"..." inside delta */
    {
        int content_len = 0;
        const char *content = json_find_str(data, dlen,
                                            "content", 7, &content_len);
        if (content && content_len > 0) {
            int offset = dlen + 1;
            if (sse_grow_buf(&p->data_buf, &p->data_cap,
                             offset + content_len + 1)) {
                memcpy(p->data_buf + offset, content, (size_t)content_len);
                int unesc_len = json_unescape(p->data_buf + offset,
                                              content_len);
                p->data_buf[offset + unesc_len] = '\0';
                ev.type = R8E_AI_EVENT_TEXT_DELTA;
                ev.text = p->data_buf + offset;
                ev.text_len = unesc_len;
                cb(&ev, ud);
                return;
            }
        }
    }

    /* Look for tool_calls in delta */
    {
        /* Check for "tool_calls" key presence */
        int tc_len = 0;
        const char *tc = json_find_str(data, dlen, "tool_calls", 10, &tc_len);
        (void)tc; (void)tc_len;

        /* Check for function name -> tool call start */
        int fn_name_len = 0;
        const char *fn_name = json_find_str(data, dlen, "name", 4,
                                            &fn_name_len);

        /* Check for function arguments -> tool call delta */
        int fn_args_len = 0;
        const char *fn_args = json_find_str(data, dlen, "arguments", 9,
                                            &fn_args_len);

        /* Check for tool call ID */
        int tc_id_len = 0;
        const char *tc_id = json_find_str(data, dlen, "id", 2, &tc_id_len);

        if (fn_name && fn_name_len > 0) {
            ev.type = R8E_AI_EVENT_TOOL_CALL_START;
            ev.tool_name = fn_name;
            ev.tool_name_len = fn_name_len;
            ev.tool_id = tc_id;
            ev.tool_id_len = tc_id_len;
            cb(&ev, ud);
            return;
        }

        if (fn_args && fn_args_len > 0) {
            int offset = dlen + 1;
            if (sse_grow_buf(&p->data_buf, &p->data_cap,
                             offset + fn_args_len + 1)) {
                memcpy(p->data_buf + offset, fn_args, (size_t)fn_args_len);
                int unesc_len = json_unescape(p->data_buf + offset,
                                              fn_args_len);
                p->data_buf[offset + unesc_len] = '\0';
                ev.type = R8E_AI_EVENT_TOOL_CALL_DELTA;
                ev.text = p->data_buf + offset;
                ev.text_len = unesc_len;
                cb(&ev, ud);
                return;
            }
        }

        /* finish_reason: "stop" or "tool_calls" -> done/tool_call_end */
        int fr_len = 0;
        const char *fr = json_find_str(data, dlen, "finish_reason", 13,
                                       &fr_len);
        if (fr) {
            if (fr_len == 10 && memcmp(fr, "tool_calls", 10) == 0) {
                ev.type = R8E_AI_EVENT_TOOL_CALL_END;
                cb(&ev, ud);
                return;
            }
            if (fr_len == 4 && memcmp(fr, "stop", 4) == 0) {
                /* Not DONE - [DONE] sentinel handles that */
                return;
            }
        }
    }
}

/* =========================================================================
 * Internal: SSE Line Processing
 * ========================================================================= */

static void sse_process_line(R8ESSEParser *p, const char *line, int len,
                             void (*cb)(R8EAIEvent *, void *), void *ud)
{
    /* Empty line = dispatch accumulated event */
    if (len == 0) {
        if (p->has_fields && p->data_len > 0) {
            /* Null-terminate data for safety */
            if (sse_grow_buf(&p->data_buf, &p->data_cap, p->data_len + 1))
                p->data_buf[p->data_len] = '\0';

            if (p->provider == R8E_PROVIDER_ANTHROPIC) {
                dispatch_anthropic(p, cb, ud);
            } else {
                dispatch_openai(p, cb, ud);
            }
        }
        /* Reset event state */
        p->event_type_len = 0;
        p->data_len = 0;
        p->event_id_len = 0;
        p->has_fields = 0;
        return;
    }

    /* Comment line: starts with ':' */
    if (line[0] == ':') return;

    /* Parse field name and value */
    const char *colon = (const char *)memchr(line, ':', (size_t)len);
    if (!colon) return; /* malformed, ignore */

    int field_len = (int)(colon - line);
    const char *value = colon + 1;
    int value_len = len - field_len - 1;

    /* Skip single leading space after colon (per SSE spec) */
    if (value_len > 0 && value[0] == ' ') {
        value++;
        value_len--;
    }

    p->has_fields = 1;

    if (field_len == 4 && memcmp(line, "data", 4) == 0) {
        /* Append to data buffer (multiple data: lines get joined with \n) */
        int need = p->data_len + (p->data_len > 0 ? 1 : 0) + value_len;
        if (sse_grow_buf(&p->data_buf, &p->data_cap, need + 1)) {
            if (p->data_len > 0) {
                p->data_buf[p->data_len++] = '\n';
            }
            memcpy(p->data_buf + p->data_len, value, (size_t)value_len);
            p->data_len += value_len;
        }
    } else if (field_len == 5 && memcmp(line, "event", 5) == 0) {
        int copy = value_len;
        if (copy >= SSE_EVENT_TYPE_MAX) copy = SSE_EVENT_TYPE_MAX - 1;
        memcpy(p->event_type, value, (size_t)copy);
        p->event_type[copy] = '\0';
        p->event_type_len = copy;
    } else if (field_len == 2 && memcmp(line, "id", 2) == 0) {
        int copy = value_len;
        if (copy >= 63) copy = 63;
        memcpy(p->event_id, value, (size_t)copy);
        p->event_id[copy] = '\0';
        p->event_id_len = copy;
    }
    /* retry: and unknown fields are ignored per spec */
}

/* =========================================================================
 * Public API
 * ========================================================================= */

R8ESSEParser *r8e_sse_parser_new(uint8_t provider)
{
    R8ESSEParser *p = (R8ESSEParser *)calloc(1, sizeof(R8ESSEParser));
    if (!p) return NULL;

    p->provider = provider;

    p->line_buf = (char *)malloc(SSE_LINE_BUF_INIT);
    if (!p->line_buf) { free(p); return NULL; }
    p->line_cap = SSE_LINE_BUF_INIT;

    p->data_buf = (char *)malloc(SSE_DATA_BUF_INIT);
    if (!p->data_buf) { free(p->line_buf); free(p); return NULL; }
    p->data_cap = SSE_DATA_BUF_INIT;

    return p;
}

void r8e_sse_parser_free(R8ESSEParser *p)
{
    if (!p) return;
    free(p->line_buf);
    free(p->data_buf);
    free(p);
}

void r8e_sse_parser_feed(R8ESSEParser *p, const char *chunk, int len,
                         void (*callback)(R8EAIEvent *event, void *ud),
                         void *ud)
{
    if (!p || !chunk || len <= 0 || !callback) return;

    const char *end = chunk + len;
    const char *pos = chunk;

    while (pos < end) {
        /* Find next newline */
        const char *nl = (const char *)memchr(pos, '\n', (size_t)(end - pos));

        if (!nl) {
            /* No newline found - buffer the remainder */
            int remain = (int)(end - pos);
            if (sse_grow_buf(&p->line_buf, &p->line_cap,
                             p->line_len + remain)) {
                memcpy(p->line_buf + p->line_len, pos, (size_t)remain);
                p->line_len += remain;
            }
            break;
        }

        /* Complete line found */
        int seg_len = (int)(nl - pos);

        /* Strip trailing \r (handle \r\n) */
        int line_end = seg_len;
        if (line_end > 0 && pos[line_end - 1] == '\r')
            line_end--;

        if (p->line_len > 0) {
            /* Combine buffered partial + this segment */
            int total = p->line_len + line_end;
            if (sse_grow_buf(&p->line_buf, &p->line_cap, total)) {
                memcpy(p->line_buf + p->line_len, pos, (size_t)line_end);
                sse_process_line(p, p->line_buf, total, callback, ud);
            }
            p->line_len = 0;
        } else {
            /* Process line directly from chunk */
            sse_process_line(p, pos, line_end, callback, ud);
        }

        pos = nl + 1;
    }
}

void r8e_sse_parser_reset(R8ESSEParser *p)
{
    if (!p) return;
    p->line_len = 0;
    p->event_type_len = 0;
    p->data_len = 0;
    p->event_id_len = 0;
    p->has_fields = 0;
}
