/*
 * r8e_sse_parser.h - SSE (Server-Sent Events) Stream Parser for AI Providers
 *
 * Part of the r8e JavaScript engine.
 * Parses SSE streams from Anthropic and OpenAI APIs and normalizes them
 * into a unified event format.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_SSE_PARSER_H
#define R8E_SSE_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Provider Constants
 * ========================================================================= */

#define R8E_PROVIDER_ANTHROPIC  0
#define R8E_PROVIDER_OPENAI     1

/* =========================================================================
 * Unified AI Event Types
 *
 * Normalized from provider-specific SSE formats into a common enum.
 * ========================================================================= */

typedef enum {
    R8E_AI_EVENT_TEXT_DELTA,       /* text content chunk */
    R8E_AI_EVENT_TOOL_CALL_START, /* tool call begins (name + id) */
    R8E_AI_EVENT_TOOL_CALL_DELTA, /* tool call argument chunk */
    R8E_AI_EVENT_TOOL_CALL_END,   /* tool call complete */
    R8E_AI_EVENT_USAGE,           /* token usage info */
    R8E_AI_EVENT_DONE,            /* stream finished */
    R8E_AI_EVENT_ERROR            /* parse or provider error */
} R8EAIEventType;

/* =========================================================================
 * AI Event Structure
 *
 * Carries a single normalized event. String fields point into the parser's
 * internal buffer and are only valid until the next feed() call.
 * ========================================================================= */

typedef struct {
    R8EAIEventType type;

    /* R8E_AI_EVENT_TEXT_DELTA / R8E_AI_EVENT_TOOL_CALL_DELTA */
    const char    *text;
    int            text_len;

    /* R8E_AI_EVENT_TOOL_CALL_START */
    const char    *tool_name;
    int            tool_name_len;
    const char    *tool_id;
    int            tool_id_len;

    /* R8E_AI_EVENT_USAGE */
    int            input_tokens;
    int            output_tokens;

    /* R8E_AI_EVENT_ERROR */
    const char    *error_msg;
    int            error_msg_len;
} R8EAIEvent;

/* =========================================================================
 * Parser API
 * ========================================================================= */

typedef struct R8ESSEParser R8ESSEParser;

/**
 * Create a new SSE parser for the given provider.
 *
 * @param provider  R8E_PROVIDER_ANTHROPIC or R8E_PROVIDER_OPENAI
 * @return          Newly allocated parser, or NULL on OOM.
 *                  Must be freed with r8e_sse_parser_free().
 */
R8ESSEParser *r8e_sse_parser_new(uint8_t provider);

/**
 * Free a parser and its internal buffers.
 */
void r8e_sse_parser_free(R8ESSEParser *p);

/**
 * Feed a chunk of raw SSE data into the parser.
 *
 * As complete events are parsed, the callback is invoked for each one.
 * Event data pointers are valid only within the callback invocation.
 *
 * @param p         Parser instance
 * @param chunk     Raw bytes from the HTTP response body
 * @param len       Length of chunk in bytes
 * @param callback  Called for each complete event
 * @param ud        User data passed through to callback
 */
void r8e_sse_parser_feed(R8ESSEParser *p, const char *chunk, int len,
                         void (*callback)(R8EAIEvent *event, void *ud),
                         void *ud);

/**
 * Reset parser state for reuse with a new stream.
 * Provider setting is preserved.
 */
void r8e_sse_parser_reset(R8ESSEParser *p);

#ifdef __cplusplus
}
#endif

#endif /* R8E_SSE_PARSER_H */
