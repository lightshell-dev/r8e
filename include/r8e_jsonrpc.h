/*
 * r8e_jsonrpc.h - JSON-RPC 2.0 Parser and Serializer
 *
 * Part of the r8e JavaScript engine MCP (Model Context Protocol) layer.
 * Provides zero-allocation parsing (pointers into source buffer) and
 * fixed-buffer serialization for JSON-RPC 2.0 messages.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_JSONRPC_H
#define R8E_JSONRPC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * JSON-RPC 2.0 Error Codes (spec-defined)
 * ========================================================================= */

#define R8E_JSONRPC_PARSE_ERROR      (-32700)
#define R8E_JSONRPC_INVALID_REQUEST  (-32600)
#define R8E_JSONRPC_METHOD_NOT_FOUND (-32601)
#define R8E_JSONRPC_INVALID_PARAMS   (-32602)
#define R8E_JSONRPC_INTERNAL_ERROR   (-32603)

/* =========================================================================
 * Parsed Message
 *
 * All string pointers reference positions within the original input buffer.
 * They are NOT null-terminated; use the corresponding _len field.
 * ========================================================================= */

typedef struct {
    int         id;             /* request ID (-1 for notifications) */
    const char *method;         /* method name pointer into source buffer */
    int         method_len;
    const char *params;         /* raw JSON params pointer into source buffer */
    int         params_len;
    bool        is_response;    /* true if this is a response, not a request */
    const char *result;         /* for responses: result JSON */
    int         result_len;
    int         error_code;     /* for error responses (0 = no error) */
    const char *error_msg;
    int         error_msg_len;
} R8EJsonRpcMsg;

/* =========================================================================
 * Parsing
 * ========================================================================= */

/**
 * Parse a JSON-RPC 2.0 message from raw bytes.
 *
 * Uses targeted field scanning (not a full JSON parser). Extracts the
 * "jsonrpc", "id", "method", "params", "result", and "error" fields
 * from well-formed JSON-RPC messages.
 *
 * @param buf   Input buffer (UTF-8 JSON text).
 * @param len   Length of input in bytes.
 * @param out   Output: parsed message fields.
 * @return      0 on success, negative error code on failure.
 */
int r8e_jsonrpc_parse(const char *buf, int len, R8EJsonRpcMsg *out);

/* =========================================================================
 * Serialization
 *
 * All write functions return the number of bytes written (excluding the
 * null terminator), or -1 if the buffer is too small.
 * ========================================================================= */

/**
 * Write a JSON-RPC request.
 * Output: {"jsonrpc":"2.0","id":N,"method":"...","params":...}
 *
 * @param buf         Output buffer.
 * @param cap         Buffer capacity in bytes.
 * @param id          Request ID.
 * @param method      Method name (null-terminated).
 * @param params_json Raw JSON for params (null-terminated, or NULL for no params).
 * @return            Bytes written, or -1 on overflow.
 */
int r8e_jsonrpc_write_request(char *buf, int cap, int id,
                               const char *method, const char *params_json);

/**
 * Write a JSON-RPC success response.
 * Output: {"jsonrpc":"2.0","id":N,"result":...}
 *
 * @param buf          Output buffer.
 * @param cap          Buffer capacity in bytes.
 * @param id           Request ID being responded to.
 * @param result_json  Raw JSON for result (null-terminated, or NULL for null).
 * @return             Bytes written, or -1 on overflow.
 */
int r8e_jsonrpc_write_response(char *buf, int cap, int id,
                                const char *result_json);

/**
 * Write a JSON-RPC error response.
 * Output: {"jsonrpc":"2.0","id":N,"error":{"code":N,"message":"..."}}
 *
 * @param buf      Output buffer.
 * @param cap      Buffer capacity in bytes.
 * @param id       Request ID being responded to.
 * @param code     Error code.
 * @param message  Error message (null-terminated).
 * @return         Bytes written, or -1 on overflow.
 */
int r8e_jsonrpc_write_error(char *buf, int cap, int id, int code,
                             const char *message);

/**
 * Write a JSON-RPC notification (no id field).
 * Output: {"jsonrpc":"2.0","method":"...","params":...}
 *
 * @param buf         Output buffer.
 * @param cap         Buffer capacity in bytes.
 * @param method      Method name (null-terminated).
 * @param params_json Raw JSON for params (null-terminated, or NULL for no params).
 * @return            Bytes written, or -1 on overflow.
 */
int r8e_jsonrpc_write_notification(char *buf, int cap, const char *method,
                                    const char *params_json);

#ifdef __cplusplus
}
#endif

#endif /* R8E_JSONRPC_H */
