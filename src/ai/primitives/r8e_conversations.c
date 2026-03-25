/*
 * r8e_conversations.c - In-Memory Conversation State Management
 *
 * Part of the r8e JavaScript engine.
 *
 * Architecture:
 *   - Dynamic array of messages with geometric growth (2x)
 *   - Token counting uses a simple heuristic (chars / 4) until a
 *     proper tokenizer integration is added
 *   - Truncation preserves system message + most recent messages
 *   - All strings are heap-allocated copies (caller retains ownership)
 *   - Conversation IDs are generated from a simple counter + timestamp
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "r8e_conversations.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* =========================================================================
 * Internal Constants
 * ========================================================================= */

#define CONV_INITIAL_CAP   16
#define CONV_ID_PREFIX     "conv_"

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/**
 * Get current time in microseconds (portable approximation).
 * Uses a monotonic-ish source; not meant for wall-clock accuracy.
 */
static uint64_t now_us(void)
{
#if defined(__APPLE__)
    #include <mach/mach_time.h>
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return (t * tb.numer / tb.denom) / 1000;
#elif defined(__linux__)
    #include <time.h>
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
#else
    #include <time.h>
    return (uint64_t)time(NULL) * 1000000;
#endif
}

/**
 * Simple ID generation. Produces "conv_XXXXXXXXXXXX" from counter + timestamp.
 */
static uint64_t s_conv_counter = 0;

static void generate_id(char *out, int cap)
{
    uint64_t ts = now_us();
    uint64_t id = (ts << 16) | (s_conv_counter++ & 0xFFFF);
    snprintf(out, cap, "%s%016llx", CONV_ID_PREFIX, (unsigned long long)id);
}

/**
 * Duplicate a string. Returns NULL if src is NULL.
 */
static char *str_dup(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, src, len + 1);
    return copy;
}

/**
 * Estimate token count for a string.
 * Uses the common heuristic of ~4 characters per token (English text).
 * Returns at least 1 for non-empty strings.
 */
static int estimate_tokens(const char *text)
{
    if (!text || !text[0]) return 0;
    int len = (int)strlen(text);
    int tokens = (len + 3) / 4;  /* ceil(len / 4) */
    return tokens > 0 ? tokens : 1;
}

/**
 * Free a single message's heap-allocated strings.
 */
static void message_free_strings(R8EMessage *msg)
{
    free(msg->role);
    free(msg->content);
    free(msg->tool_call_id);
    free(msg->tool_calls_json);
    msg->role = NULL;
    msg->content = NULL;
    msg->tool_call_id = NULL;
    msg->tool_calls_json = NULL;
}

/* =========================================================================
 * Conversation Lifecycle
 * ========================================================================= */

R8EConversation *r8e_conversation_new(void)
{
    R8EConversation *conv = (R8EConversation *)calloc(1, sizeof(R8EConversation));
    if (!conv) return NULL;

    conv->messages = (R8EMessage *)calloc(CONV_INITIAL_CAP, sizeof(R8EMessage));
    if (!conv->messages) {
        free(conv);
        return NULL;
    }

    conv->cap_messages = CONV_INITIAL_CAP;
    conv->n_messages = 0;

    generate_id(conv->id, sizeof(conv->id));

    uint64_t ts = now_us();
    conv->created_at = ts;
    conv->updated_at = ts;

    return conv;
}

void r8e_conversation_free(R8EConversation *conv)
{
    if (!conv) return;

    for (int i = 0; i < conv->n_messages; i++) {
        message_free_strings(&conv->messages[i]);
    }
    free(conv->messages);
    free(conv);
}

/* =========================================================================
 * Message Management
 * ========================================================================= */

/**
 * Grow the messages array to accommodate at least one more message.
 * Returns false on OOM.
 */
static bool ensure_capacity(R8EConversation *conv)
{
    if (conv->n_messages < conv->cap_messages)
        return true;

    int new_cap = conv->cap_messages * 2;
    R8EMessage *new_msgs = (R8EMessage *)realloc(conv->messages,
                                                  new_cap * sizeof(R8EMessage));
    if (!new_msgs) return false;

    /* Zero-init new slots */
    memset(new_msgs + conv->cap_messages, 0,
           (new_cap - conv->cap_messages) * sizeof(R8EMessage));

    conv->messages = new_msgs;
    conv->cap_messages = new_cap;
    return true;
}

void r8e_conversation_add_message(R8EConversation *conv,
                                   const char *role, const char *content,
                                   const char *tool_call_id,
                                   const char *tool_calls_json)
{
    if (!conv || !role) return;

    if (!ensure_capacity(conv)) return;

    R8EMessage *msg = &conv->messages[conv->n_messages];
    memset(msg, 0, sizeof(*msg));

    msg->role = str_dup(role);
    msg->content = str_dup(content);
    msg->tool_call_id = str_dup(tool_call_id);
    msg->tool_calls_json = str_dup(tool_calls_json);

    /* Estimate tokens from all text content */
    msg->tokens = estimate_tokens(content);
    if (tool_calls_json)
        msg->tokens += estimate_tokens(tool_calls_json);

    msg->created_at = now_us();

    conv->n_messages++;
    conv->updated_at = msg->created_at;
}

int r8e_conversation_get_messages(const R8EConversation *conv,
                                   R8EMessage **out_messages)
{
    if (!conv || !out_messages) return 0;
    *out_messages = conv->messages;
    return conv->n_messages;
}

/* =========================================================================
 * Token-Budget Truncation
 *
 * Strategy:
 *   1. Always include the system message (if first message is role "system")
 *   2. Walk backwards from the most recent message, accumulating token counts
 *   3. Stop when adding the next message would exceed max_tokens
 *   4. Return the selected messages in chronological order
 *
 * The returned array is a newly allocated array of R8EMessage structs
 * with borrowed string pointers (do NOT free the strings, only free the array).
 * ========================================================================= */

int r8e_conversation_get_messages_truncated(const R8EConversation *conv,
                                             int max_tokens,
                                             R8EMessage **out_messages)
{
    if (!conv || !out_messages || max_tokens <= 0) return 0;
    if (conv->n_messages == 0) {
        *out_messages = NULL;
        return 0;
    }

    int n = conv->n_messages;

    /* Check if system message is present at index 0 */
    bool has_system = (n > 0 && conv->messages[0].role &&
                       strcmp(conv->messages[0].role, "system") == 0);

    int system_tokens = has_system ? conv->messages[0].tokens : 0;
    int budget = max_tokens - system_tokens;
    if (budget < 0) budget = 0;

    /* Walk backward to find the first message to include */
    int first_idx = has_system ? 1 : 0;  /* skip system in backward scan */
    int selected_start = n;  /* exclusive start (will decrement) */
    int token_sum = 0;

    for (int i = n - 1; i >= first_idx; i--) {
        int msg_tokens = conv->messages[i].tokens;
        if (token_sum + msg_tokens > budget)
            break;
        token_sum += msg_tokens;
        selected_start = i;
    }

    /* Count total messages to return */
    int result_count = (has_system ? 1 : 0) + (n - selected_start);

    R8EMessage *result = (R8EMessage *)malloc(result_count * sizeof(R8EMessage));
    if (!result) {
        *out_messages = NULL;
        return 0;
    }

    int out_idx = 0;

    /* Copy system message first (borrowed pointers) */
    if (has_system) {
        result[out_idx] = conv->messages[0];
        out_idx++;
    }

    /* Copy selected recent messages (borrowed pointers) */
    for (int i = selected_start; i < n; i++) {
        result[out_idx] = conv->messages[i];
        out_idx++;
    }

    *out_messages = result;
    return result_count;
}

/* =========================================================================
 * Conversation Operations
 * ========================================================================= */

void r8e_conversation_clear(R8EConversation *conv)
{
    if (!conv) return;

    for (int i = 0; i < conv->n_messages; i++) {
        message_free_strings(&conv->messages[i]);
    }
    conv->n_messages = 0;
    conv->updated_at = now_us();
}

int r8e_conversation_total_tokens(const R8EConversation *conv)
{
    if (!conv) return 0;
    int total = 0;
    for (int i = 0; i < conv->n_messages; i++) {
        total += conv->messages[i].tokens;
    }
    return total;
}

/* =========================================================================
 * JSON Serialization
 *
 * Produces:
 *   {
 *     "id": "conv_...",
 *     "messages": [
 *       {"role": "user", "content": "..."},
 *       {"role": "assistant", "content": "...", "tool_calls": [...]},
 *       {"role": "tool", "tool_call_id": "...", "content": "..."}
 *     ]
 *   }
 * ========================================================================= */

/**
 * Write a JSON-escaped string into the buffer.
 * Returns number of bytes written, or -1 on overflow.
 */
static int json_escape_write(char *buf, int cap, int pos, const char *s)
{
    if (!s) {
        if (pos + 4 > cap) return -1;
        memcpy(buf + pos, "null", 4);
        return pos + 4;
    }

    if (pos >= cap) return -1;
    buf[pos++] = '"';

    while (*s) {
        char c = *s++;
        if (c == '"' || c == '\\') {
            if (pos + 2 > cap) return -1;
            buf[pos++] = '\\';
            buf[pos++] = c;
        } else if (c == '\n') {
            if (pos + 2 > cap) return -1;
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + 2 > cap) return -1;
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + 2 > cap) return -1;
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if ((unsigned char)c < 0x20) {
            if (pos + 6 > cap) return -1;
            pos += snprintf(buf + pos, cap - pos, "\\u%04x", (unsigned char)c);
        } else {
            if (pos >= cap) return -1;
            buf[pos++] = c;
        }
    }

    if (pos >= cap) return -1;
    buf[pos++] = '"';
    return pos;
}

#define WRITE_LITERAL(s) do {                           \
    int slen = (int)sizeof(s) - 1;                     \
    if (pos + slen > buf_cap) return -1;                \
    memcpy(out_buf + pos, s, slen);                     \
    pos += slen;                                        \
} while (0)

int r8e_conversation_to_json(const R8EConversation *conv,
                              char *out_buf, int buf_cap)
{
    if (!conv || !out_buf || buf_cap <= 0) return -1;

    int pos = 0;

    WRITE_LITERAL("{\"id\":");
    pos = json_escape_write(out_buf, buf_cap, pos, conv->id);
    if (pos < 0) return -1;

    WRITE_LITERAL(",\"messages\":[");

    for (int i = 0; i < conv->n_messages; i++) {
        if (i > 0) {
            if (pos >= buf_cap) return -1;
            out_buf[pos++] = ',';
        }

        const R8EMessage *msg = &conv->messages[i];

        WRITE_LITERAL("{\"role\":");
        pos = json_escape_write(out_buf, buf_cap, pos, msg->role);
        if (pos < 0) return -1;

        WRITE_LITERAL(",\"content\":");
        pos = json_escape_write(out_buf, buf_cap, pos, msg->content);
        if (pos < 0) return -1;

        if (msg->tool_call_id) {
            WRITE_LITERAL(",\"tool_call_id\":");
            pos = json_escape_write(out_buf, buf_cap, pos, msg->tool_call_id);
            if (pos < 0) return -1;
        }

        if (msg->tool_calls_json) {
            WRITE_LITERAL(",\"tool_calls\":");
            /* tool_calls is raw JSON, write directly */
            int tclen = (int)strlen(msg->tool_calls_json);
            if (pos + tclen > buf_cap) return -1;
            memcpy(out_buf + pos, msg->tool_calls_json, tclen);
            pos += tclen;
        }

        if (pos >= buf_cap) return -1;
        out_buf[pos++] = '}';
    }

    WRITE_LITERAL("]}");

    if (pos < buf_cap)
        out_buf[pos] = '\0';

    return pos;
}

#undef WRITE_LITERAL
