/*
 * r8e_jsonrpc.c - JSON-RPC 2.0 Parser and Serializer
 *
 * Part of the r8e JavaScript engine MCP layer.
 *
 * Parsing uses targeted field scanning: we look for known top-level keys
 * ("jsonrpc", "id", "method", "params", "result", "error") without building
 * a full parse tree. This keeps the code small and avoids allocations.
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_jsonrpc.h"
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Internal: JSON scanning helpers
 *
 * These operate on a bounded buffer [buf, buf+len). No null-terminator
 * is required. They skip whitespace, match literals, and balance nested
 * structures to extract field boundaries.
 * ========================================================================= */

/* Skip whitespace. Returns new position or len if exhausted. */
static int skip_ws(const char *buf, int len, int pos)
{
    while (pos < len) {
        char c = buf[pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        pos++;
    }
    return pos;
}

/*
 * Skip a complete JSON value starting at buf[pos].
 * Handles: strings, numbers, true/false/null, objects, arrays.
 * Returns position after the value, or -1 on malformed input.
 */
static int skip_value(const char *buf, int len, int pos)
{
    pos = skip_ws(buf, len, pos);
    if (pos >= len)
        return -1;

    char c = buf[pos];

    /* String */
    if (c == '"') {
        pos++;
        while (pos < len) {
            if (buf[pos] == '\\') {
                pos += 2;  /* skip escaped char */
                continue;
            }
            if (buf[pos] == '"')
                return pos + 1;
            pos++;
        }
        return -1;  /* unterminated string */
    }

    /* Object or array */
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 1;
        pos++;
        while (pos < len && depth > 0) {
            char ch = buf[pos];
            if (ch == '"') {
                /* skip string contents (may contain braces) */
                pos++;
                while (pos < len) {
                    if (buf[pos] == '\\') { pos += 2; continue; }
                    if (buf[pos] == '"') { pos++; break; }
                    pos++;
                }
                continue;
            }
            if (ch == open) depth++;
            else if (ch == close) depth--;
            pos++;
        }
        return (depth == 0) ? pos : -1;
    }

    /* true, false, null */
    if (c == 't' && pos + 4 <= len && memcmp(buf + pos, "true", 4) == 0)
        return pos + 4;
    if (c == 'f' && pos + 5 <= len && memcmp(buf + pos, "false", 5) == 0)
        return pos + 5;
    if (c == 'n' && pos + 4 <= len && memcmp(buf + pos, "null", 4) == 0)
        return pos + 4;

    /* Number: optional minus, digits, optional fraction, optional exponent */
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (c == '-') pos++;
        while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        if (pos < len && buf[pos] == '.') {
            pos++;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        }
        if (pos < len && (buf[pos] == 'e' || buf[pos] == 'E')) {
            pos++;
            if (pos < len && (buf[pos] == '+' || buf[pos] == '-')) pos++;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        }
        return pos;
    }

    return -1;  /* unknown value */
}

/*
 * Match a quoted key at buf[pos]. Returns true if the key matches and
 * advances *pos past the key and the following colon.
 */
static bool match_key(const char *buf, int len, int *pos, const char *key)
{
    int p = skip_ws(buf, len, *pos);
    if (p >= len || buf[p] != '"')
        return false;

    int klen = (int)strlen(key);
    if (p + 1 + klen + 1 > len)
        return false;
    if (memcmp(buf + p + 1, key, klen) != 0)
        return false;
    if (buf[p + 1 + klen] != '"')
        return false;

    p = skip_ws(buf, len, p + 1 + klen + 1);
    if (p >= len || buf[p] != ':')
        return false;
    *pos = p + 1;
    return true;
}

/*
 * Parse a JSON integer at buf[pos]. Sets *out and returns new position.
 * Handles optional minus sign. Returns -1 on failure.
 */
static int parse_int(const char *buf, int len, int pos, int *out)
{
    pos = skip_ws(buf, len, pos);
    if (pos >= len)
        return -1;

    int sign = 1;
    if (buf[pos] == '-') {
        sign = -1;
        pos++;
    }
    if (pos >= len || buf[pos] < '0' || buf[pos] > '9')
        return -1;

    int val = 0;
    while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
        val = val * 10 + (buf[pos] - '0');
        pos++;
    }
    *out = val * sign;
    return pos;
}

/*
 * Extract a JSON string value at buf[pos].
 * Sets *str to point inside buf (past the opening quote) and *slen to
 * the string length (not including quotes). Returns position after
 * the closing quote, or -1 on failure.
 */
static int extract_string(const char *buf, int len, int pos,
                           const char **str, int *slen)
{
    pos = skip_ws(buf, len, pos);
    if (pos >= len || buf[pos] != '"')
        return -1;

    int start = pos + 1;
    pos = start;
    while (pos < len) {
        if (buf[pos] == '\\') { pos += 2; continue; }
        if (buf[pos] == '"') {
            *str = buf + start;
            *slen = pos - start;
            return pos + 1;
        }
        pos++;
    }
    return -1;
}

/* =========================================================================
 * Public: r8e_jsonrpc_parse
 * ========================================================================= */

int r8e_jsonrpc_parse(const char *buf, int len, R8EJsonRpcMsg *out)
{
    if (!buf || len <= 0 || !out)
        return R8E_JSONRPC_INVALID_REQUEST;

    /* Zero output */
    memset(out, 0, sizeof(*out));
    out->id = -1;  /* default: notification */

    /* Detect batch (array at top level) */
    int p = skip_ws(buf, len, 0);
    if (p >= len)
        return R8E_JSONRPC_PARSE_ERROR;
    if (buf[p] == '[')
        return R8E_JSONRPC_INVALID_REQUEST;  /* batch not supported */
    if (buf[p] != '{')
        return R8E_JSONRPC_PARSE_ERROR;

    /* Scan top-level object fields */
    p++;  /* skip '{' */

    bool has_jsonrpc = false;
    bool has_result = false;
    bool has_error = false;

    while (p < len) {
        p = skip_ws(buf, len, p);
        if (p >= len) break;
        if (buf[p] == '}') break;
        if (buf[p] == ',') { p++; continue; }

        /* Try each known key */
        int saved = p;

        if (match_key(buf, len, &p, "jsonrpc")) {
            const char *s;
            int slen;
            int np = extract_string(buf, len, p, &s, &slen);
            if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
            if (slen == 3 && memcmp(s, "2.0", 3) == 0)
                has_jsonrpc = true;
            p = np;
            continue;
        }
        p = saved;

        if (match_key(buf, len, &p, "id")) {
            int vp = skip_ws(buf, len, p);
            if (vp < len && buf[vp] == 'n') {
                /* null id */
                out->id = -1;
                p = skip_value(buf, len, vp);
                if (p < 0) return R8E_JSONRPC_PARSE_ERROR;
            } else if (vp < len && buf[vp] == '"') {
                /* string id: extract but store as -1 (unsupported, but parse) */
                const char *s;
                int slen;
                int np = extract_string(buf, len, vp, &s, &slen);
                if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
                /* Try to parse string as integer */
                if (slen > 0 && slen < 10) {
                    int val = 0;
                    bool ok = true;
                    for (int i = 0; i < slen; i++) {
                        if (s[i] < '0' || s[i] > '9') { ok = false; break; }
                        val = val * 10 + (s[i] - '0');
                    }
                    if (ok) out->id = val;
                }
                p = np;
            } else {
                int np = parse_int(buf, len, vp, &out->id);
                if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
                p = np;
            }
            continue;
        }
        p = saved;

        if (match_key(buf, len, &p, "method")) {
            int np = extract_string(buf, len, p, &out->method, &out->method_len);
            if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
            p = np;
            continue;
        }
        p = saved;

        if (match_key(buf, len, &p, "params")) {
            int vp = skip_ws(buf, len, p);
            out->params = buf + vp;
            int np = skip_value(buf, len, vp);
            if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
            out->params_len = np - vp;
            p = np;
            continue;
        }
        p = saved;

        if (match_key(buf, len, &p, "result")) {
            int vp = skip_ws(buf, len, p);
            out->result = buf + vp;
            int np = skip_value(buf, len, vp);
            if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
            out->result_len = np - vp;
            has_result = true;
            p = np;
            continue;
        }
        p = saved;

        if (match_key(buf, len, &p, "error")) {
            has_error = true;
            /* Parse the error object for code and message */
            int vp = skip_ws(buf, len, p);
            if (vp >= len || buf[vp] != '{') {
                p = skip_value(buf, len, vp);
                if (p < 0) return R8E_JSONRPC_PARSE_ERROR;
                continue;
            }
            int ep = vp + 1;
            while (ep < len) {
                ep = skip_ws(buf, len, ep);
                if (ep >= len) break;
                if (buf[ep] == '}') { ep++; break; }
                if (buf[ep] == ',') { ep++; continue; }

                int esaved = ep;

                if (match_key(buf, len, &ep, "code")) {
                    int np = parse_int(buf, len, ep, &out->error_code);
                    if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
                    ep = np;
                    continue;
                }
                ep = esaved;

                if (match_key(buf, len, &ep, "message")) {
                    int np = extract_string(buf, len, ep,
                                             &out->error_msg, &out->error_msg_len);
                    if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
                    ep = np;
                    continue;
                }
                ep = esaved;

                /* Skip unknown error field */
                int np = extract_string(buf, len, ep, &(const char *){0},
                                         &(int){0});
                if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
                np = skip_ws(buf, len, np);
                if (np < len && buf[np] == ':') {
                    np = skip_value(buf, len, np + 1);
                    if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
                }
                ep = np;
            }
            p = ep;
            continue;
        }
        p = saved;

        /* Unknown top-level key: skip key + value */
        {
            const char *dummy_s;
            int dummy_l;
            int np = extract_string(buf, len, p, &dummy_s, &dummy_l);
            if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
            np = skip_ws(buf, len, np);
            if (np < len && buf[np] == ':') {
                np = skip_value(buf, len, np + 1);
                if (np < 0) return R8E_JSONRPC_PARSE_ERROR;
            }
            p = np;
        }
    }

    /* Validate: must have jsonrpc:"2.0" */
    if (!has_jsonrpc)
        return R8E_JSONRPC_INVALID_REQUEST;

    /* Determine message type */
    if (has_result || has_error) {
        out->is_response = true;
    }

    return 0;
}

/* =========================================================================
 * Internal: safe buffer append helpers
 * ========================================================================= */

typedef struct {
    char *buf;
    int   cap;
    int   pos;
    bool  overflow;
} WriteBuf;

static void wb_init(WriteBuf *wb, char *buf, int cap)
{
    wb->buf = buf;
    wb->cap = cap;
    wb->pos = 0;
    wb->overflow = false;
}

static void wb_putc(WriteBuf *wb, char c)
{
    if (wb->pos < wb->cap - 1)
        wb->buf[wb->pos] = c;
    else
        wb->overflow = true;
    wb->pos++;
}

static void wb_puts(WriteBuf *wb, const char *s)
{
    while (*s)
        wb_putc(wb, *s++);
}

static void wb_putint(WriteBuf *wb, int val)
{
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%d", val);
    for (int i = 0; i < n; i++)
        wb_putc(wb, tmp[i]);
}

/* Write a JSON-escaped string (with surrounding quotes) */
static void wb_putstr(WriteBuf *wb, const char *s)
{
    wb_putc(wb, '"');
    while (*s) {
        char c = *s++;
        switch (c) {
        case '"':  wb_putc(wb, '\\'); wb_putc(wb, '"');  break;
        case '\\': wb_putc(wb, '\\'); wb_putc(wb, '\\'); break;
        case '\n': wb_putc(wb, '\\'); wb_putc(wb, 'n');  break;
        case '\r': wb_putc(wb, '\\'); wb_putc(wb, 'r');  break;
        case '\t': wb_putc(wb, '\\'); wb_putc(wb, 't');  break;
        default:   wb_putc(wb, c); break;
        }
    }
    wb_putc(wb, '"');
}

static int wb_finish(WriteBuf *wb)
{
    if (wb->overflow)
        return -1;
    if (wb->pos < wb->cap)
        wb->buf[wb->pos] = '\0';
    return wb->pos;
}

/* =========================================================================
 * Public: Serialization
 * ========================================================================= */

int r8e_jsonrpc_write_request(char *buf, int cap, int id,
                               const char *method, const char *params_json)
{
    WriteBuf wb;
    wb_init(&wb, buf, cap);

    wb_puts(&wb, "{\"jsonrpc\":\"2.0\",\"id\":");
    wb_putint(&wb, id);
    wb_puts(&wb, ",\"method\":");
    wb_putstr(&wb, method);
    if (params_json) {
        wb_puts(&wb, ",\"params\":");
        wb_puts(&wb, params_json);
    }
    wb_putc(&wb, '}');

    return wb_finish(&wb);
}

int r8e_jsonrpc_write_response(char *buf, int cap, int id,
                                const char *result_json)
{
    WriteBuf wb;
    wb_init(&wb, buf, cap);

    wb_puts(&wb, "{\"jsonrpc\":\"2.0\",\"id\":");
    wb_putint(&wb, id);
    wb_puts(&wb, ",\"result\":");
    wb_puts(&wb, result_json ? result_json : "null");
    wb_putc(&wb, '}');

    return wb_finish(&wb);
}

int r8e_jsonrpc_write_error(char *buf, int cap, int id, int code,
                             const char *message)
{
    WriteBuf wb;
    wb_init(&wb, buf, cap);

    wb_puts(&wb, "{\"jsonrpc\":\"2.0\",\"id\":");
    wb_putint(&wb, id);
    wb_puts(&wb, ",\"error\":{\"code\":");
    wb_putint(&wb, code);
    wb_puts(&wb, ",\"message\":");
    wb_putstr(&wb, message);
    wb_puts(&wb, "}}");

    return wb_finish(&wb);
}

int r8e_jsonrpc_write_notification(char *buf, int cap, const char *method,
                                    const char *params_json)
{
    WriteBuf wb;
    wb_init(&wb, buf, cap);

    wb_puts(&wb, "{\"jsonrpc\":\"2.0\",\"method\":");
    wb_putstr(&wb, method);
    if (params_json) {
        wb_puts(&wb, ",\"params\":");
        wb_puts(&wb, params_json);
    }
    wb_putc(&wb, '}');

    return wb_finish(&wb);
}
