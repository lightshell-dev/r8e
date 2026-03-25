/*
 * r8e_conversations.h - In-Memory Conversation State Management
 *
 * Part of the r8e JavaScript engine.
 * Manages conversation history for AI agent loops, with support for
 * token-budget-aware truncation.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_CONVERSATIONS_H
#define R8E_CONVERSATIONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Message Structure
 * ========================================================================= */

typedef struct {
    char     *role;            /* "system", "user", "assistant", "tool" */
    char     *content;
    char     *tool_call_id;    /* for tool result messages */
    char     *tool_calls_json; /* for assistant messages with tool calls */
    int       tokens;          /* cached token count (estimated) */
    uint64_t  created_at;      /* microsecond timestamp */
} R8EMessage;

/* =========================================================================
 * Conversation Structure
 * ========================================================================= */

typedef struct {
    char        id[64];
    R8EMessage *messages;
    int         n_messages;
    int         cap_messages;
    uint64_t    created_at;
    uint64_t    updated_at;
} R8EConversation;

/* =========================================================================
 * Conversation API
 * ========================================================================= */

/**
 * Create a new empty conversation with a generated ID.
 *
 * @return  Newly allocated conversation, or NULL on OOM.
 *          Must be freed with r8e_conversation_free().
 */
R8EConversation *r8e_conversation_new(void);

/**
 * Free a conversation and all its messages.
 */
void r8e_conversation_free(R8EConversation *conv);

/**
 * Add a message to the conversation.
 *
 * All string arguments are copied. Pass NULL for optional fields.
 *
 * @param conv            Conversation instance
 * @param role            Message role ("system", "user", "assistant", "tool")
 * @param content         Message content text
 * @param tool_call_id    Tool use ID (for "tool" role messages, else NULL)
 * @param tool_calls_json Tool calls JSON (for "assistant" messages, else NULL)
 */
void r8e_conversation_add_message(R8EConversation *conv,
                                   const char *role, const char *content,
                                   const char *tool_call_id,
                                   const char *tool_calls_json);

/**
 * Get all messages in the conversation.
 *
 * @param conv          Conversation instance
 * @param out_messages  Set to point at the internal messages array
 * @return              Number of messages
 */
int r8e_conversation_get_messages(const R8EConversation *conv,
                                   R8EMessage **out_messages);

/**
 * Get messages truncated to fit within a token budget.
 *
 * Returns the most recent messages that fit within max_tokens.
 * The system message (if first) is always preserved.
 * The returned array is heap-allocated and must be freed by the caller
 * (but the strings within are borrowed from the conversation and must
 * not be freed).
 *
 * @param conv          Conversation instance
 * @param max_tokens    Maximum total token count
 * @param out_messages  Set to a newly allocated array of message pointers
 * @return              Number of messages in the truncated window
 */
int r8e_conversation_get_messages_truncated(const R8EConversation *conv,
                                             int max_tokens,
                                             R8EMessage **out_messages);

/**
 * Remove all messages from the conversation.
 */
void r8e_conversation_clear(R8EConversation *conv);

/**
 * Get the total estimated token count across all messages.
 *
 * @param conv  Conversation instance
 * @return      Sum of all message token counts
 */
int r8e_conversation_total_tokens(const R8EConversation *conv);

/**
 * Serialize the conversation to JSON.
 *
 * @param conv     Conversation instance
 * @param out_buf  Output buffer
 * @param buf_cap  Capacity of out_buf
 * @return         Bytes written, or -1 if buffer too small
 */
int r8e_conversation_to_json(const R8EConversation *conv,
                              char *out_buf, int buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* R8E_CONVERSATIONS_H */
