/*
 * r8e_mcp_session.h - MCP Session and Capability Management
 *
 * Part of the r8e JavaScript engine MCP (Model Context Protocol) layer.
 * Manages the initialize/initialized handshake and tracks negotiated
 * capabilities for both server and client sides.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_MCP_SESSION_H
#define R8E_MCP_SESSION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * MCP Protocol Version
 * ========================================================================= */

#define R8E_MCP_PROTOCOL_VERSION  1

/* =========================================================================
 * Capability Structures
 * ========================================================================= */

typedef struct {
    bool has_tools;
    bool has_resources;
    bool has_prompts;
    bool tools_list_changed;
} R8EMCPServerCaps;

typedef struct {
    bool has_sampling;
    bool has_roots;
} R8EMCPClientCaps;

/* =========================================================================
 * Session State
 * ========================================================================= */

typedef struct {
    char              session_id[64];
    R8EMCPServerCaps  server_caps;
    R8EMCPClientCaps  client_caps;
    int               protocol_version;
    bool              initialized;
} R8EMCPSession;

/**
 * Initialize session state to defaults.
 *
 * @param session  Session to initialize.
 */
void r8e_mcp_session_init(R8EMCPSession *session);

/**
 * Handle an MCP "initialize" request.
 *
 * Parses the client's capabilities from the params JSON, stores them
 * in the session, generates a session ID, and writes the initialize
 * response JSON into resp_buf.
 *
 * @param session     Session state (updated in place).
 * @param params_json Raw JSON params from the initialize request.
 * @param resp_buf    Output buffer for the response result JSON.
 * @param resp_cap    Capacity of resp_buf in bytes.
 * @return            Bytes written to resp_buf, or -1 on error.
 */
int r8e_mcp_handle_initialize(R8EMCPSession *session,
                               const char *params_json,
                               char *resp_buf, int resp_cap);

/**
 * Handle an MCP "initialized" notification.
 *
 * Marks the session as fully initialized. After this, normal
 * request/response processing can begin.
 *
 * @param session  Session state (updated in place).
 * @return         0 on success, -1 if initialize was not called first.
 */
int r8e_mcp_handle_initialized(R8EMCPSession *session);

#ifdef __cplusplus
}
#endif

#endif /* R8E_MCP_SESSION_H */
