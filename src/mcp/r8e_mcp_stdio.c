/*
 * r8e_mcp_stdio.c - MCP stdio Transport Layer
 *
 * Part of the r8e JavaScript engine MCP layer.
 * Line-delimited JSON-RPC framing over file descriptors.
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_mcp_stdio.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* =========================================================================
 * Public API
 * ========================================================================= */

void r8e_mcp_stdio_init(R8EMCPStdio *t, int fd_in, int fd_out)
{
    memset(t, 0, sizeof(*t));
    t->fd_in = fd_in;
    t->fd_out = fd_out;
}

int r8e_mcp_stdio_read(R8EMCPStdio *t, R8EJsonRpcMsg *out)
{
    /*
     * Compact the buffer: move unprocessed data to the front.
     * This invalidates any pointers from previous parse results,
     * which is fine because callers should have consumed them.
     */
    if (t->scan_pos > 0) {
        int remaining = t->line_len - t->scan_pos;
        if (remaining > 0)
            memmove(t->line_buf, t->line_buf + t->scan_pos, remaining);
        t->line_len = remaining;
        t->scan_pos = 0;
    }

    for (;;) {
        /* Scan for newline in buffered data */
        for (int i = t->scan_pos; i < t->line_len; i++) {
            if (t->line_buf[i] == '\n') {
                int line_start = t->scan_pos;
                int line_end = i;

                /* Advance scan_pos past this line (including newline) */
                t->scan_pos = i + 1;

                /* Skip empty lines */
                if (line_end == line_start)
                    continue;

                /* Strip trailing \r if present */
                int parse_end = line_end;
                if (parse_end > line_start &&
                    t->line_buf[parse_end - 1] == '\r')
                    parse_end--;

                int parse_len = parse_end - line_start;
                if (parse_len <= 0)
                    continue;

                /*
                 * Parse the line as JSON-RPC. The parsed message will
                 * contain pointers into line_buf[line_start..parse_end-1].
                 * These remain valid until the next call to
                 * r8e_mcp_stdio_read(), which compacts the buffer.
                 */
                int rc = r8e_jsonrpc_parse(t->line_buf + line_start,
                                            parse_len, out);
                return (rc == 0) ? 0 : -2;
            }
        }

        /* No complete line yet — compact if needed to make room */
        if (t->scan_pos > 0) {
            int remaining = t->line_len - t->scan_pos;
            if (remaining > 0)
                memmove(t->line_buf, t->line_buf + t->scan_pos, remaining);
            t->line_len = remaining;
            t->scan_pos = 0;
        }

        if (t->line_len >= R8E_MCP_STDIO_BUFSIZE - 1)
            return -1;  /* line too long */

        ssize_t n = read(t->fd_in, t->line_buf + t->line_len,
                         R8E_MCP_STDIO_BUFSIZE - 1 - t->line_len);
        if (n <= 0)
            return -1;  /* EOF or error */

        t->line_len += (int)n;
    }
}

int r8e_mcp_stdio_write(R8EMCPStdio *t, const char *json, int len)
{
    /* Write the JSON payload */
    const char *p = json;
    int remaining = len;
    while (remaining > 0) {
        ssize_t n = write(t->fd_out, p, remaining);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        remaining -= (int)n;
    }

    /* Write the trailing newline */
    char nl = '\n';
    while (write(t->fd_out, &nl, 1) < 0) {
        if (errno != EINTR)
            return -1;
    }

    return 0;
}
