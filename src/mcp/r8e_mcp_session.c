/*
 * r8e_mcp_session.c - MCP Session and Capability Management
 *
 * Part of the r8e JavaScript engine MCP layer.
 * Handles the initialize/initialized handshake and capability negotiation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_mcp_session.h"
#include "r8e_jsonrpc.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* =========================================================================
 * Internal: Simple session ID generation
 *
 * Produces a deterministic-looking hex string from a counter and some
 * bits of the session pointer. Not cryptographic — just unique enough
 * for session tracking.
 * ========================================================================= */

static uint32_t s_session_counter = 0;

static void generate_session_id(char *out, int cap, const void *ptr)
{
    uint32_t seq = ++s_session_counter;
    uintptr_t addr = (uintptr_t)ptr;
    snprintf(out, cap, "r8e-%08x-%08x",
             (unsigned)seq, (unsigned)(addr & 0xFFFFFFFFU));
}

/* =========================================================================
 * Internal: Scan params JSON for client capabilities
 *
 * Looks for patterns like "sampling" and "roots" inside the
 * capabilities object. Uses simple substring scanning since the
 * params structure is well-defined by the MCP spec.
 * ========================================================================= */

static void parse_client_caps(const char *params, R8EMCPClientCaps *caps)
{
    caps->has_sampling = false;
    caps->has_roots = false;

    if (!params)
        return;

    /*
     * Look for "capabilities" containing "sampling" and/or "roots".
     * This is a simplified scan appropriate for well-formed MCP params.
     */
    const char *p = params;
    int plen = (int)strlen(p);

    for (int i = 0; i < plen - 8; i++) {
        if (memcmp(p + i, "\"sampling\"", 10) == 0)
            caps->has_sampling = true;
        if (i < plen - 6 && memcmp(p + i, "\"roots\"", 7) == 0)
            caps->has_roots = true;
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void r8e_mcp_session_init(R8EMCPSession *session)
{
    memset(session, 0, sizeof(*session));
    session->protocol_version = R8E_MCP_PROTOCOL_VERSION;

    /* Default server capabilities */
    session->server_caps.has_tools = true;
    session->server_caps.has_resources = false;
    session->server_caps.has_prompts = false;
    session->server_caps.tools_list_changed = false;
}

int r8e_mcp_handle_initialize(R8EMCPSession *session,
                               const char *params_json,
                               char *resp_buf, int resp_cap)
{
    if (!session || !resp_buf || resp_cap <= 0)
        return -1;

    /* Generate session ID */
    generate_session_id(session->session_id, sizeof(session->session_id),
                        session);

    /* Parse client capabilities from params */
    parse_client_caps(params_json, &session->client_caps);

    /*
     * Build the initialize response result JSON.
     *
     * Format:
     * {
     *   "protocolVersion": "2024-11-05",
     *   "capabilities": {
     *     "tools": { "listChanged": true/false }
     *   },
     *   "serverInfo": {
     *     "name": "r8e",
     *     "version": "0.1.0"
     *   }
     * }
     */
    int n = 0;
    n = snprintf(resp_buf, resp_cap,
        "{"
          "\"protocolVersion\":\"2024-11-05\","
          "\"capabilities\":{"
            "\"tools\":{\"listChanged\":%s}"
            "%s%s"
          "},"
          "\"serverInfo\":{"
            "\"name\":\"r8e\","
            "\"version\":\"0.1.0\""
          "}"
        "}",
        session->server_caps.tools_list_changed ? "true" : "false",
        session->server_caps.has_resources
            ? ",\"resources\":{}" : "",
        session->server_caps.has_prompts
            ? ",\"prompts\":{\"listChanged\":false}" : "");

    if (n < 0 || n >= resp_cap)
        return -1;

    return n;
}

int r8e_mcp_handle_initialized(R8EMCPSession *session)
{
    if (!session)
        return -1;

    /* Must have received initialize first (session_id is set) */
    if (session->session_id[0] == '\0')
        return -1;

    session->initialized = true;
    return 0;
}
