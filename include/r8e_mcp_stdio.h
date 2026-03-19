/*
 * r8e_mcp_stdio.h - MCP stdio Transport Layer
 *
 * Part of the r8e JavaScript engine MCP (Model Context Protocol) layer.
 * Implements line-delimited JSON-RPC message framing over file descriptors,
 * per the MCP specification.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_MCP_STDIO_H
#define R8E_MCP_STDIO_H

#include "r8e_jsonrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * stdio Transport
 *
 * Messages are newline-delimited JSON. Each message is a single line
 * terminated by '\n'. Maximum message size is 64KB.
 * ========================================================================= */

#define R8E_MCP_STDIO_BUFSIZE  65536  /* 64KB max message */

typedef struct {
    int   fd_in;                          /* read file descriptor */
    int   fd_out;                         /* write file descriptor */
    char  line_buf[R8E_MCP_STDIO_BUFSIZE]; /* line accumulation buffer */
    int   line_len;                       /* total bytes in line_buf */
    int   scan_pos;                       /* start of unprocessed data */
} R8EMCPStdio;

/**
 * Initialize a stdio transport.
 *
 * @param t       Transport state to initialize.
 * @param fd_in   File descriptor to read from (e.g., STDIN_FILENO).
 * @param fd_out  File descriptor to write to (e.g., STDOUT_FILENO).
 */
void r8e_mcp_stdio_init(R8EMCPStdio *t, int fd_in, int fd_out);

/**
 * Read one newline-delimited JSON-RPC message (blocking).
 *
 * Reads bytes from fd_in until a complete newline-delimited line is
 * received, then parses it as a JSON-RPC message.
 *
 * @param t    Transport state.
 * @param out  Output: parsed JSON-RPC message.
 * @return     0 on success, -1 on EOF/error, -2 on parse error.
 */
int r8e_mcp_stdio_read(R8EMCPStdio *t, R8EJsonRpcMsg *out);

/**
 * Write one JSON-RPC message followed by a newline.
 *
 * @param t     Transport state.
 * @param json  JSON message text (not null-terminated is fine).
 * @param len   Length of json in bytes.
 * @return      0 on success, -1 on write error.
 */
int r8e_mcp_stdio_write(R8EMCPStdio *t, const char *json, int len);

#ifdef __cplusplus
}
#endif

#endif /* R8E_MCP_STDIO_H */
