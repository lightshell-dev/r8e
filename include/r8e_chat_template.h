/*
 * r8e_chat_template.h - Chat Template Engine for LLM Prompt Formatting
 *
 * Part of the r8e JavaScript engine.
 * Minimal Jinja-subset template engine for formatting chat messages into
 * prompts suitable for different LLM architectures (ChatML, LLaMA 3, Phi-3).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_CHAT_TEMPLATE_H
#define R8E_CHAT_TEMPLATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Chat Message
 * ========================================================================= */

typedef struct {
    const char *role;    /* "system", "user", "assistant" */
    const char *content;
} R8EChatMessage;

/* =========================================================================
 * Template Formats
 * ========================================================================= */

typedef enum {
    R8E_CHAT_FMT_UNKNOWN = 0,
    R8E_CHAT_FMT_CHATML  = 1,  /* <|im_start|>role\ncontent<|im_end|> */
    R8E_CHAT_FMT_LLAMA3  = 2,  /* <|start_header_id|>role<|end_header_id|>\n\ncontent<|eot_id|> */
    R8E_CHAT_FMT_PHI3    = 3,  /* <|user|>\ncontent<|end|>\n<|assistant|>\n */
} R8EChatFormat;

/* =========================================================================
 * Chat Template
 * ========================================================================= */

typedef struct {
    const char *bos_token;       /* beginning-of-sequence token string (owned copy) */
    const char *eos_token;       /* end-of-sequence token string (owned copy) */
    const char *template_str;    /* Jinja-like template from GGUF (owned copy) */
    uint8_t     format;          /* R8EChatFormat: auto-detected or from template */
} R8EChatTemplate;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * Create a chat template with auto-detection of format.
 *
 * The format is detected by examining the template string for known markers:
 *   - "im_start" -> ChatML
 *   - "start_header_id" -> LLaMA 3
 *   - "<|user|>" -> Phi-3
 *
 * If the template string is NULL or unrecognized, falls back to ChatML.
 * If model_name contains "llama" or "phi", that helps disambiguation.
 *
 * @param template_str  Jinja-like template string from GGUF metadata (or NULL).
 * @param model_name    Model name for format hints (or NULL).
 * @param bos           BOS token string (or NULL for default).
 * @param eos           EOS token string (or NULL for default).
 * @return              Newly allocated template, or NULL on OOM.
 *                      Must be freed with r8e_chat_template_free().
 */
R8EChatTemplate *r8e_chat_template_new(const char *template_str,
                                        const char *model_name,
                                        const char *bos, const char *eos);

/**
 * Free a chat template and all associated memory.
 *
 * @param t  Template to free (NULL is safe, does nothing).
 */
void r8e_chat_template_free(R8EChatTemplate *t);

/* =========================================================================
 * Template Application
 * ========================================================================= */

/**
 * Format chat messages into a prompt string.
 *
 * Applies the detected template format to the array of messages, writing
 * the formatted prompt into out_buf. If add_generation_prompt is implied
 * (the last message is not from "assistant"), the appropriate generation
 * prompt prefix is appended.
 *
 * @param t           Chat template instance.
 * @param messages    Array of chat messages.
 * @param n_messages  Number of messages.
 * @param out_buf     Output buffer for the formatted prompt.
 * @param buf_cap     Capacity of the output buffer in bytes.
 * @return            Number of bytes written (excluding null terminator),
 *                    or -1 on error. If the buffer is too small, returns
 *                    the required size (excluding null terminator) and
 *                    out_buf is not modified.
 */
int r8e_chat_template_apply(const R8EChatTemplate *t,
                             const R8EChatMessage *messages, int n_messages,
                             char *out_buf, int buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* R8E_CHAT_TEMPLATE_H */
