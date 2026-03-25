/*
 * r8e_chat_template.c - Chat Template Engine for LLM Prompt Formatting
 *
 * Part of the r8e JavaScript engine.
 * Implements a minimal Jinja-subset template engine for formatting chat
 * conversations into prompts for different LLM architectures.
 *
 * Supported formats:
 *   - ChatML:  <|im_start|>role\ncontent<|im_end|>\n
 *   - LLaMA 3: <|begin_of_text|><|start_header_id|>role<|end_header_id|>\n\ncontent<|eot_id|>
 *   - Phi-3:   <|user|>\ncontent<|end|>\n<|assistant|>\n
 *
 * Format auto-detection examines the template string for known markers.
 * Falls back to ChatML if the format cannot be determined.
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_chat_template.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Internal Constants
 * ========================================================================= */

/* Default special tokens */
#define DEFAULT_BOS  "<s>"
#define DEFAULT_EOS  "</s>"

/* ChatML tokens */
#define CHATML_IM_START  "<|im_start|>"
#define CHATML_IM_END    "<|im_end|>"

/* LLaMA 3 tokens */
#define LLAMA3_BEGIN_OF_TEXT   "<|begin_of_text|>"
#define LLAMA3_START_HEADER   "<|start_header_id|>"
#define LLAMA3_END_HEADER     "<|end_header_id|>"
#define LLAMA3_EOT            "<|eot_id|>"

/* Phi-3 tokens */
#define PHI3_USER       "<|user|>"
#define PHI3_ASSISTANT  "<|assistant|>"
#define PHI3_SYSTEM     "<|system|>"
#define PHI3_END        "<|end|>"

/* Maximum output buffer for a single format call (safety limit) */
#define MAX_OUTPUT_SIZE  (4 * 1024 * 1024)

/* =========================================================================
 * String Helpers
 * ========================================================================= */

/**
 * Duplicate a string. Returns NULL if input is NULL.
 */
static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

/**
 * Case-insensitive substring search.
 */
static const char *str_istr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;

    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;

    size_t hlen = strlen(haystack);
    if (nlen > hlen) return NULL;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            /* Lowercase ASCII */
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return haystack + i;
    }
    return NULL;
}

/* =========================================================================
 * Safe Buffer Appender
 * ========================================================================= */

typedef struct {
    char  *buf;
    int    cap;
    int    len;
    int    overflow; /* set to 1 if we ran out of space */
} BufWriter;

static void buf_init(BufWriter *w, char *buf, int cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
}

static void buf_append(BufWriter *w, const char *str, int slen) {
    if (w->overflow) return;
    if (slen < 0) slen = (int)strlen(str);

    if (w->len + slen >= w->cap) {
        w->overflow = 1;
        return;
    }

    memcpy(w->buf + w->len, str, (size_t)slen);
    w->len += slen;
    w->buf[w->len] = '\0';
}

static void buf_append_cstr(BufWriter *w, const char *str) {
    if (str) buf_append(w, str, (int)strlen(str));
}

/* =========================================================================
 * Dry-Run Buffer (for computing required size)
 * ========================================================================= */

typedef struct {
    int len;
} DryWriter;

static void dry_init(DryWriter *w) {
    w->len = 0;
}

static void dry_append(DryWriter *w, const char *str, int slen) {
    if (slen < 0) slen = (int)strlen(str);
    w->len += slen;
}

static void dry_append_cstr(DryWriter *w, const char *str) {
    if (str) dry_append(w, str, (int)strlen(str));
}

/* =========================================================================
 * Format Auto-Detection
 * ========================================================================= */

static uint8_t detect_format(const char *template_str, const char *model_name) {
    /* Check template string for known markers */
    if (template_str) {
        if (strstr(template_str, "im_start") || strstr(template_str, "im_end")) {
            return R8E_CHAT_FMT_CHATML;
        }
        if (strstr(template_str, "start_header_id") ||
            strstr(template_str, "end_header_id")) {
            return R8E_CHAT_FMT_LLAMA3;
        }
        if (strstr(template_str, "<|user|>") ||
            strstr(template_str, "<|assistant|>")) {
            return R8E_CHAT_FMT_PHI3;
        }
    }

    /* Fall back to model name hints */
    if (model_name) {
        if (str_istr(model_name, "llama-3") || str_istr(model_name, "llama3")) {
            return R8E_CHAT_FMT_LLAMA3;
        }
        if (str_istr(model_name, "phi-3") || str_istr(model_name, "phi3")) {
            return R8E_CHAT_FMT_PHI3;
        }
    }

    /* Default to ChatML */
    return R8E_CHAT_FMT_CHATML;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

R8EChatTemplate *r8e_chat_template_new(const char *template_str,
                                        const char *model_name,
                                        const char *bos, const char *eos) {
    R8EChatTemplate *t = (R8EChatTemplate *)calloc(1, sizeof(R8EChatTemplate));
    if (!t) return NULL;

    t->template_str = str_dup(template_str);
    t->bos_token    = str_dup(bos ? bos : DEFAULT_BOS);
    t->eos_token    = str_dup(eos ? eos : DEFAULT_EOS);

    if (!t->bos_token || !t->eos_token) {
        r8e_chat_template_free(t);
        return NULL;
    }

    t->format = detect_format(template_str, model_name);

    return t;
}

void r8e_chat_template_free(R8EChatTemplate *t) {
    if (!t) return;
    free((void *)t->bos_token);
    free((void *)t->eos_token);
    free((void *)t->template_str);
    free(t);
}

/* =========================================================================
 * ChatML Format
 *
 * Format:
 *   <|im_start|>system\nYou are a helpful assistant.<|im_end|>\n
 *   <|im_start|>user\nHello!<|im_end|>\n
 *   <|im_start|>assistant\n
 * ========================================================================= */

static int apply_chatml_dry(const R8EChatMessage *messages, int n,
                             int add_gen_prompt) {
    DryWriter w;
    dry_init(&w);

    for (int i = 0; i < n; i++) {
        dry_append_cstr(&w, CHATML_IM_START);
        dry_append_cstr(&w, messages[i].role);
        dry_append(&w, "\n", 1);
        dry_append_cstr(&w, messages[i].content);
        dry_append_cstr(&w, CHATML_IM_END);
        dry_append(&w, "\n", 1);
    }

    if (add_gen_prompt) {
        dry_append_cstr(&w, CHATML_IM_START);
        dry_append(&w, "assistant\n", 10);
    }

    return w.len;
}

static int apply_chatml(const R8EChatMessage *messages, int n,
                         int add_gen_prompt, char *buf, int cap) {
    BufWriter w;
    buf_init(&w, buf, cap);

    for (int i = 0; i < n; i++) {
        buf_append_cstr(&w, CHATML_IM_START);
        buf_append_cstr(&w, messages[i].role);
        buf_append(&w, "\n", 1);
        buf_append_cstr(&w, messages[i].content);
        buf_append_cstr(&w, CHATML_IM_END);
        buf_append(&w, "\n", 1);
    }

    if (add_gen_prompt) {
        buf_append_cstr(&w, CHATML_IM_START);
        buf_append(&w, "assistant\n", 10);
    }

    if (w.overflow) return -1;
    return w.len;
}

/* =========================================================================
 * LLaMA 3 Format
 *
 * Format:
 *   <|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n
 *   You are a helpful assistant.<|eot_id|>
 *   <|start_header_id|>user<|end_header_id|>\n\nHello!<|eot_id|>
 *   <|start_header_id|>assistant<|end_header_id|>\n\n
 * ========================================================================= */

static int apply_llama3_dry(const R8EChatMessage *messages, int n,
                             int add_gen_prompt) {
    DryWriter w;
    dry_init(&w);

    dry_append_cstr(&w, LLAMA3_BEGIN_OF_TEXT);

    for (int i = 0; i < n; i++) {
        dry_append_cstr(&w, LLAMA3_START_HEADER);
        dry_append_cstr(&w, messages[i].role);
        dry_append_cstr(&w, LLAMA3_END_HEADER);
        dry_append(&w, "\n\n", 2);
        dry_append_cstr(&w, messages[i].content);
        dry_append_cstr(&w, LLAMA3_EOT);
    }

    if (add_gen_prompt) {
        dry_append_cstr(&w, LLAMA3_START_HEADER);
        dry_append(&w, "assistant", 9);
        dry_append_cstr(&w, LLAMA3_END_HEADER);
        dry_append(&w, "\n\n", 2);
    }

    return w.len;
}

static int apply_llama3(const R8EChatMessage *messages, int n,
                         int add_gen_prompt, char *buf, int cap) {
    BufWriter w;
    buf_init(&w, buf, cap);

    buf_append_cstr(&w, LLAMA3_BEGIN_OF_TEXT);

    for (int i = 0; i < n; i++) {
        buf_append_cstr(&w, LLAMA3_START_HEADER);
        buf_append_cstr(&w, messages[i].role);
        buf_append_cstr(&w, LLAMA3_END_HEADER);
        buf_append(&w, "\n\n", 2);
        buf_append_cstr(&w, messages[i].content);
        buf_append_cstr(&w, LLAMA3_EOT);
    }

    if (add_gen_prompt) {
        buf_append_cstr(&w, LLAMA3_START_HEADER);
        buf_append(&w, "assistant", 9);
        buf_append_cstr(&w, LLAMA3_END_HEADER);
        buf_append(&w, "\n\n", 2);
    }

    if (w.overflow) return -1;
    return w.len;
}

/* =========================================================================
 * Phi-3 Format
 *
 * Format:
 *   <|system|>\nYou are a helpful assistant.<|end|>\n
 *   <|user|>\nHello!<|end|>\n
 *   <|assistant|>\n
 * ========================================================================= */

static const char *phi3_role_token(const char *role) {
    if (!role) return PHI3_USER;
    if (strcmp(role, "system") == 0)    return PHI3_SYSTEM;
    if (strcmp(role, "user") == 0)      return PHI3_USER;
    if (strcmp(role, "assistant") == 0) return PHI3_ASSISTANT;
    return PHI3_USER; /* fallback */
}

static int apply_phi3_dry(const R8EChatMessage *messages, int n,
                           int add_gen_prompt) {
    DryWriter w;
    dry_init(&w);

    for (int i = 0; i < n; i++) {
        dry_append_cstr(&w, phi3_role_token(messages[i].role));
        dry_append(&w, "\n", 1);
        dry_append_cstr(&w, messages[i].content);
        dry_append_cstr(&w, PHI3_END);
        dry_append(&w, "\n", 1);
    }

    if (add_gen_prompt) {
        dry_append_cstr(&w, PHI3_ASSISTANT);
        dry_append(&w, "\n", 1);
    }

    return w.len;
}

static int apply_phi3(const R8EChatMessage *messages, int n,
                       int add_gen_prompt, char *buf, int cap) {
    BufWriter w;
    buf_init(&w, buf, cap);

    for (int i = 0; i < n; i++) {
        buf_append_cstr(&w, phi3_role_token(messages[i].role));
        buf_append(&w, "\n", 1);
        buf_append_cstr(&w, messages[i].content);
        buf_append_cstr(&w, PHI3_END);
        buf_append(&w, "\n", 1);
    }

    if (add_gen_prompt) {
        buf_append_cstr(&w, PHI3_ASSISTANT);
        buf_append(&w, "\n", 1);
    }

    if (w.overflow) return -1;
    return w.len;
}

/* =========================================================================
 * Template Application
 * ========================================================================= */

int r8e_chat_template_apply(const R8EChatTemplate *t,
                             const R8EChatMessage *messages, int n_messages,
                             char *out_buf, int buf_cap) {
    if (!t || !messages || n_messages <= 0) return -1;

    /* Determine if we should add a generation prompt */
    int add_gen_prompt = 1;
    if (n_messages > 0 && messages[n_messages - 1].role &&
        strcmp(messages[n_messages - 1].role, "assistant") == 0) {
        add_gen_prompt = 0;
    }

    /* First, compute required size */
    int required;
    switch (t->format) {
        case R8E_CHAT_FMT_LLAMA3:
            required = apply_llama3_dry(messages, n_messages, add_gen_prompt);
            break;
        case R8E_CHAT_FMT_PHI3:
            required = apply_phi3_dry(messages, n_messages, add_gen_prompt);
            break;
        case R8E_CHAT_FMT_CHATML:
        default:
            required = apply_chatml_dry(messages, n_messages, add_gen_prompt);
            break;
    }

    if (required < 0 || required > MAX_OUTPUT_SIZE) return -1;

    /* If buffer is too small, return required size */
    if (!out_buf || buf_cap <= required) {
        return required;
    }

    /* Apply the template */
    int written;
    switch (t->format) {
        case R8E_CHAT_FMT_LLAMA3:
            written = apply_llama3(messages, n_messages, add_gen_prompt,
                                    out_buf, buf_cap);
            break;
        case R8E_CHAT_FMT_PHI3:
            written = apply_phi3(messages, n_messages, add_gen_prompt,
                                  out_buf, buf_cap);
            break;
        case R8E_CHAT_FMT_CHATML:
        default:
            written = apply_chatml(messages, n_messages, add_gen_prompt,
                                    out_buf, buf_cap);
            break;
    }

    return written;
}
