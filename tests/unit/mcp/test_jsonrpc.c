/*
 * test_jsonrpc.c - Unit tests for MCP JSON-RPC, stdio transport, and session
 *
 * Tests cover:
 *   - Parse valid request, response, notification, error
 *   - Handle missing fields gracefully
 *   - Serialize request and verify valid JSON
 *   - Serialize response and verify structure
 *   - Round-trip: serialize -> parse -> verify fields match
 *   - Parse batch (detect and reject)
 *   - Edge cases: empty params, null id, string id
 *   - stdio: write then read back from pipe pair
 *   - Session: initialize handshake
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "r8e_jsonrpc.h"
#include "r8e_mcp_stdio.h"
#include "r8e_mcp_session.h"

/* =========================================================================
 * Test infrastructure (mirrors test_runner.c macros)
 * ========================================================================= */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;
extern int g_assert_fail;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    g_assert_fail = 0;                                              \
    g_tests_run++;                                                  \
    printf("  %-60s ", #name);                                      \
    test_##name();                                                  \
    if (g_assert_fail) {                                            \
        g_tests_failed++;                                           \
        printf("FAIL\n");                                           \
    } else {                                                        \
        g_tests_passed++;                                           \
        printf("ok\n");                                             \
    }                                                               \
} while (0)

#define ASSERT_TRUE(expr) do {                                      \
    if (!(expr)) {                                                  \
        fprintf(stderr, "    ASSERT_TRUE failed: %s\n"              \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_FALSE(expr) do {                                     \
    if (expr) {                                                     \
        fprintf(stderr, "    ASSERT_FALSE failed: %s\n"             \
                "      at %s:%d\n", #expr, __FILE__, __LINE__);     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_EQ_INT(a, b) do {                                    \
    long long _a = (long long)(a), _b = (long long)(b);             \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    ASSERT_EQ_INT failed: %s == %s\n"      \
                "      got %lld vs %lld\n"                           \
                "      at %s:%d\n",                                 \
                #a, #b, _a, _b, __FILE__, __LINE__);                \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

#define ASSERT_STRN_EQ(s, slen, expected) do {                      \
    const char *_e = (expected);                                    \
    int _elen = (int)strlen(_e);                                    \
    if ((slen) != _elen || memcmp((s), _e, _elen) != 0) {          \
        fprintf(stderr, "    ASSERT_STRN_EQ failed: %s\n"           \
                "      got \"%.*s\" (len=%d) vs \"%s\" (len=%d)\n"  \
                "      at %s:%d\n",                                 \
                #s, (slen), (s) ? (s) : "(null)", (slen),           \
                _e, _elen, __FILE__, __LINE__);                     \
        g_assert_fail = 1;                                          \
        return;                                                     \
    }                                                               \
} while (0)

/* =========================================================================
 * JSON-RPC Parse Tests
 * ========================================================================= */

TEST(parse_valid_request)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/list\","
        "\"params\":{\"cursor\":\"abc\"}}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 42);
    ASSERT_FALSE(msg.is_response);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "tools/list");
    ASSERT_TRUE(msg.params != NULL);
    ASSERT_TRUE(msg.params_len > 0);
    /* params should contain the cursor object */
    ASSERT_TRUE(memcmp(msg.params, "{\"cursor\":\"abc\"}", msg.params_len) == 0);
}

TEST(parse_valid_response)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"tools\":[]}}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 7);
    ASSERT_TRUE(msg.is_response);
    ASSERT_TRUE(msg.result != NULL);
    ASSERT_TRUE(msg.result_len > 0);
}

TEST(parse_valid_notification)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, -1);  /* notification: no id */
    ASSERT_FALSE(msg.is_response);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "notifications/initialized");
    ASSERT_TRUE(msg.params == NULL);
}

TEST(parse_valid_error)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":3,"
        "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 3);
    ASSERT_TRUE(msg.is_response);
    ASSERT_EQ_INT(msg.error_code, -32601);
    ASSERT_STRN_EQ(msg.error_msg, msg.error_msg_len, "Method not found");
}

TEST(parse_missing_jsonrpc_field)
{
    const char *json = "{\"id\":1,\"method\":\"foo\"}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, R8E_JSONRPC_INVALID_REQUEST);
}

TEST(parse_empty_input)
{
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse("", 0, &msg);
    ASSERT_TRUE(rc != 0);
}

TEST(parse_null_input)
{
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(NULL, 10, &msg);
    ASSERT_TRUE(rc != 0);
}

TEST(parse_batch_rejected)
{
    const char *json = "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"foo\"}]";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, R8E_JSONRPC_INVALID_REQUEST);
}

TEST(parse_null_id)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"test\"}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, -1);  /* null id treated as notification */
}

TEST(parse_string_id)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":\"99\",\"method\":\"test\"}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 99);  /* numeric string id parsed */
}

TEST(parse_empty_params)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test\",\"params\":{}}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(msg.params != NULL);
    ASSERT_EQ_INT(msg.params_len, 2);  /* "{}" */
}

TEST(parse_notification_with_params)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"method\":\"progress\","
        "\"params\":{\"token\":1,\"value\":50}}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, -1);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "progress");
    ASSERT_TRUE(msg.params != NULL);
}

TEST(parse_response_null_result)
{
    const char *json =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":null}";
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(json, (int)strlen(json), &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(msg.is_response);
    ASSERT_STRN_EQ(msg.result, msg.result_len, "null");
}

/* =========================================================================
 * JSON-RPC Serialize Tests
 * ========================================================================= */

TEST(serialize_request)
{
    char buf[512];
    int n = r8e_jsonrpc_write_request(buf, sizeof(buf), 1, "initialize",
                                       "{\"protocolVersion\":\"2024-11-05\"}");
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(n < (int)sizeof(buf));

    /* Verify it parses back */
    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 1);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "initialize");
}

TEST(serialize_response)
{
    char buf[512];
    int n = r8e_jsonrpc_write_response(buf, sizeof(buf), 1,
                                        "{\"tools\":[]}");
    ASSERT_TRUE(n > 0);

    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(msg.is_response);
    ASSERT_EQ_INT(msg.id, 1);
}

TEST(serialize_error)
{
    char buf[512];
    int n = r8e_jsonrpc_write_error(buf, sizeof(buf), 5,
                                     R8E_JSONRPC_METHOD_NOT_FOUND,
                                     "Method not found");
    ASSERT_TRUE(n > 0);

    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(msg.is_response);
    ASSERT_EQ_INT(msg.id, 5);
    ASSERT_EQ_INT(msg.error_code, R8E_JSONRPC_METHOD_NOT_FOUND);
    ASSERT_STRN_EQ(msg.error_msg, msg.error_msg_len, "Method not found");
}

TEST(serialize_notification)
{
    char buf[512];
    int n = r8e_jsonrpc_write_notification(buf, sizeof(buf),
                                            "notifications/initialized",
                                            NULL);
    ASSERT_TRUE(n > 0);

    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, -1);
    ASSERT_FALSE(msg.is_response);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "notifications/initialized");
}

TEST(serialize_buffer_too_small)
{
    char buf[10];  /* way too small */
    int n = r8e_jsonrpc_write_request(buf, sizeof(buf), 1, "test", NULL);
    ASSERT_EQ_INT(n, -1);
}

TEST(serialize_null_params)
{
    char buf[256];
    int n = r8e_jsonrpc_write_request(buf, sizeof(buf), 1, "test", NULL);
    ASSERT_TRUE(n > 0);
    /* Should not contain "params" */
    ASSERT_TRUE(strstr(buf, "params") == NULL);
}

/* =========================================================================
 * Round-trip Tests
 * ========================================================================= */

TEST(roundtrip_request)
{
    char buf[512];
    int n = r8e_jsonrpc_write_request(buf, sizeof(buf), 42,
                                       "tools/call",
                                       "{\"name\":\"echo\",\"args\":{}}");
    ASSERT_TRUE(n > 0);

    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 42);
    ASSERT_FALSE(msg.is_response);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "tools/call");
    ASSERT_TRUE(msg.params != NULL);
    ASSERT_TRUE(msg.params_len > 0);
}

TEST(roundtrip_response)
{
    char buf[512];
    const char *result = "{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}";
    int n = r8e_jsonrpc_write_response(buf, sizeof(buf), 42, result);
    ASSERT_TRUE(n > 0);

    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 42);
    ASSERT_TRUE(msg.is_response);
    ASSERT_TRUE(msg.result != NULL);
    /* Verify the result JSON is preserved */
    ASSERT_TRUE(msg.result_len == (int)strlen(result));
    ASSERT_TRUE(memcmp(msg.result, result, msg.result_len) == 0);
}

TEST(roundtrip_error)
{
    char buf[512];
    int n = r8e_jsonrpc_write_error(buf, sizeof(buf), 10,
                                     -32602, "Invalid params");
    ASSERT_TRUE(n > 0);

    R8EJsonRpcMsg msg;
    int rc = r8e_jsonrpc_parse(buf, n, &msg);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(msg.id, 10);
    ASSERT_EQ_INT(msg.error_code, -32602);
    ASSERT_STRN_EQ(msg.error_msg, msg.error_msg_len, "Invalid params");
}

/* =========================================================================
 * stdio Transport Tests
 * ========================================================================= */

TEST(stdio_write_read_pipe)
{
    int pipefd[2];
    ASSERT_EQ_INT(pipe(pipefd), 0);

    R8EMCPStdio writer;
    r8e_mcp_stdio_init(&writer, -1, pipefd[1]);

    R8EMCPStdio reader;
    r8e_mcp_stdio_init(&reader, pipefd[0], -1);

    /* Write a JSON-RPC request via the writer transport */
    char buf[512];
    int n = r8e_jsonrpc_write_request(buf, sizeof(buf), 1, "ping", NULL);
    ASSERT_TRUE(n > 0);

    int wrc = r8e_mcp_stdio_write(&writer, buf, n);
    ASSERT_EQ_INT(wrc, 0);

    /* Read it back via the reader transport */
    R8EJsonRpcMsg msg;
    int rrc = r8e_mcp_stdio_read(&reader, &msg);
    ASSERT_EQ_INT(rrc, 0);
    ASSERT_EQ_INT(msg.id, 1);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "ping");

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(stdio_multiple_messages)
{
    int pipefd[2];
    ASSERT_EQ_INT(pipe(pipefd), 0);

    R8EMCPStdio writer;
    r8e_mcp_stdio_init(&writer, -1, pipefd[1]);

    R8EMCPStdio reader;
    r8e_mcp_stdio_init(&reader, pipefd[0], -1);

    /* Write two messages */
    char buf[512];
    int n;

    n = r8e_jsonrpc_write_request(buf, sizeof(buf), 1, "first", NULL);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_INT(r8e_mcp_stdio_write(&writer, buf, n), 0);

    n = r8e_jsonrpc_write_request(buf, sizeof(buf), 2, "second", NULL);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_INT(r8e_mcp_stdio_write(&writer, buf, n), 0);

    /* Read them back in order */
    R8EJsonRpcMsg msg;

    ASSERT_EQ_INT(r8e_mcp_stdio_read(&reader, &msg), 0);
    ASSERT_EQ_INT(msg.id, 1);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "first");

    ASSERT_EQ_INT(r8e_mcp_stdio_read(&reader, &msg), 0);
    ASSERT_EQ_INT(msg.id, 2);
    ASSERT_STRN_EQ(msg.method, msg.method_len, "second");

    close(pipefd[0]);
    close(pipefd[1]);
}

/* =========================================================================
 * Session Tests
 * ========================================================================= */

TEST(session_init_defaults)
{
    R8EMCPSession session;
    r8e_mcp_session_init(&session);

    ASSERT_FALSE(session.initialized);
    ASSERT_TRUE(session.server_caps.has_tools);
    ASSERT_FALSE(session.server_caps.has_resources);
    ASSERT_EQ_INT(session.protocol_version, R8E_MCP_PROTOCOL_VERSION);
}

TEST(session_initialize_handshake)
{
    R8EMCPSession session;
    r8e_mcp_session_init(&session);

    const char *params =
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"sampling\":{},\"roots\":{\"listChanged\":true}},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}";

    char resp[1024];
    int n = r8e_mcp_handle_initialize(&session, params, resp, sizeof(resp));
    ASSERT_TRUE(n > 0);

    /* Verify client caps were parsed */
    ASSERT_TRUE(session.client_caps.has_sampling);
    ASSERT_TRUE(session.client_caps.has_roots);

    /* Session should have an ID now */
    ASSERT_TRUE(session.session_id[0] != '\0');

    /* Verify response is valid JSON containing expected fields */
    ASSERT_TRUE(strstr(resp, "\"protocolVersion\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"capabilities\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"serverInfo\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"r8e\"") != NULL);

    /* Not yet fully initialized until "initialized" notification */
    ASSERT_FALSE(session.initialized);

    /* Send initialized notification */
    int rc = r8e_mcp_handle_initialized(&session);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(session.initialized);
}

TEST(session_initialized_before_init)
{
    R8EMCPSession session;
    r8e_mcp_session_init(&session);

    /* Calling initialized before initialize should fail */
    int rc = r8e_mcp_handle_initialized(&session);
    ASSERT_EQ_INT(rc, -1);
}

TEST(session_no_client_caps)
{
    R8EMCPSession session;
    r8e_mcp_session_init(&session);

    const char *params =
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"minimal\"}}";

    char resp[1024];
    int n = r8e_mcp_handle_initialize(&session, params, resp, sizeof(resp));
    ASSERT_TRUE(n > 0);
    ASSERT_FALSE(session.client_caps.has_sampling);
    ASSERT_FALSE(session.client_caps.has_roots);
}

/* =========================================================================
 * Test Suite Entry Point
 * ========================================================================= */

void run_mcp_jsonrpc_tests(void)
{
    /* Parse tests */
    RUN_TEST(parse_valid_request);
    RUN_TEST(parse_valid_response);
    RUN_TEST(parse_valid_notification);
    RUN_TEST(parse_valid_error);
    RUN_TEST(parse_missing_jsonrpc_field);
    RUN_TEST(parse_empty_input);
    RUN_TEST(parse_null_input);
    RUN_TEST(parse_batch_rejected);
    RUN_TEST(parse_null_id);
    RUN_TEST(parse_string_id);
    RUN_TEST(parse_empty_params);
    RUN_TEST(parse_notification_with_params);
    RUN_TEST(parse_response_null_result);

    /* Serialize tests */
    RUN_TEST(serialize_request);
    RUN_TEST(serialize_response);
    RUN_TEST(serialize_error);
    RUN_TEST(serialize_notification);
    RUN_TEST(serialize_buffer_too_small);
    RUN_TEST(serialize_null_params);

    /* Round-trip tests */
    RUN_TEST(roundtrip_request);
    RUN_TEST(roundtrip_response);
    RUN_TEST(roundtrip_error);

    /* stdio transport tests */
    RUN_TEST(stdio_write_read_pipe);
    RUN_TEST(stdio_multiple_messages);

    /* Session tests */
    RUN_TEST(session_init_defaults);
    RUN_TEST(session_initialize_handshake);
    RUN_TEST(session_initialized_before_init);
    RUN_TEST(session_no_client_caps);
}
