/*
 * r8e_html.c - HTML parser for r8e
 *
 * Streaming tokenizer and tree builder. Converts well-formed HTML
 * into a DOM tree, extracting <style> and <script> blocks.
 *
 * Not a full HTML5 parser — no error recovery, no adoption agency
 * algorithm. Assumes well-formed input.
 *
 * SPDX-License-Identifier: MIT
 */

#include "r8e_html.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* =========================================================================
 * External DOM API (from src/ui/r8e_dom.c)
 * ========================================================================= */

typedef struct R8EUIDOMNode R8EUIDOMNode;

R8EUIDOMNode *r8e_ui_dom_create_element(const char *tag);
R8EUIDOMNode *r8e_ui_dom_create_text_node(const char *text, uint32_t len);
R8EUIDOMNode *r8e_ui_dom_append_child(R8EUIDOMNode *parent, R8EUIDOMNode *child);
void r8e_ui_dom_set_attribute(R8EUIDOMNode *node, const char *name,
                              const char *value);

/* =========================================================================
 * Tokenizer State Machine
 * ========================================================================= */

typedef enum {
    HTML_TEXT,
    HTML_TAG_OPEN,
    HTML_TAG_NAME,
    HTML_ATTR_SPACE,
    HTML_ATTR_NAME,
    HTML_ATTR_EQ,
    HTML_ATTR_VALUE,
    HTML_TAG_CLOSE,
    HTML_COMMENT,
    HTML_DOCTYPE,
    HTML_RAW_TEXT       /* inside <style> or <script> */
} HTMLState;

/* =========================================================================
 * Void / Self-Closing Tags
 * ========================================================================= */

static bool html_is_void_tag(const char *tag) {
    static const char *void_tags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; void_tags[i]; i++) {
        if (strcmp(tag, void_tags[i]) == 0) return true;
    }
    return false;
}

/* =========================================================================
 * Entity Decoding (minimal)
 * ========================================================================= */

/*
 * Decode HTML entities in-place. Returns new length.
 * Supports: &amp; &lt; &gt; &quot; &apos; &#NNN; &#xHH;
 */
static uint32_t html_decode_entities(char *buf, uint32_t len) {
    uint32_t r = 0, w = 0;
    while (r < len) {
        if (buf[r] == '&' && r + 1 < len) {
            /* Try to decode entity */
            const char *start = buf + r;
            const char *semi = memchr(start, ';', len - r);
            if (semi && (semi - start) < 12) {
                uint32_t elen = (uint32_t)(semi - start + 1);
                char ch = 0;
                bool decoded = false;

                if (elen == 4 && memcmp(start, "&lt;", 4) == 0) {
                    ch = '<'; decoded = true;
                } else if (elen == 4 && memcmp(start, "&gt;", 4) == 0) {
                    ch = '>'; decoded = true;
                } else if (elen == 5 && memcmp(start, "&amp;", 5) == 0) {
                    ch = '&'; decoded = true;
                } else if (elen == 6 && memcmp(start, "&quot;", 6) == 0) {
                    ch = '"'; decoded = true;
                } else if (elen == 6 && memcmp(start, "&apos;", 6) == 0) {
                    ch = '\''; decoded = true;
                } else if (start[1] == '#') {
                    /* Numeric entity */
                    unsigned long codepoint = 0;
                    if (start[2] == 'x' || start[2] == 'X') {
                        /* Hex: &#xHH; */
                        for (const char *p = start + 3; p < semi; p++) {
                            codepoint = codepoint * 16;
                            if (*p >= '0' && *p <= '9') codepoint += *p - '0';
                            else if (*p >= 'a' && *p <= 'f') codepoint += *p - 'a' + 10;
                            else if (*p >= 'A' && *p <= 'F') codepoint += *p - 'A' + 10;
                        }
                    } else {
                        /* Decimal: &#NNN; */
                        for (const char *p = start + 2; p < semi; p++) {
                            if (*p >= '0' && *p <= '9')
                                codepoint = codepoint * 10 + (*p - '0');
                        }
                    }
                    if (codepoint > 0 && codepoint < 128) {
                        ch = (char)codepoint;
                        decoded = true;
                    }
                }

                if (decoded) {
                    buf[w++] = ch;
                    r += elen;
                    continue;
                }
            }
        }
        buf[w++] = buf[r++];
    }
    buf[w] = '\0';
    return w;
}

/* =========================================================================
 * Attribute Storage
 * ========================================================================= */

#define HTML_MAX_ATTRS 32
#define HTML_MAX_NAME  64
#define HTML_MAX_VALUE 1024

typedef struct {
    char name[HTML_MAX_NAME];
    char value[HTML_MAX_VALUE];
    bool has_value;
} HTMLAttr;

/* =========================================================================
 * Element Stack
 * ========================================================================= */

#define HTML_STACK_MAX 256

/* =========================================================================
 * CSS/Script Accumulator
 * ========================================================================= */

typedef struct {
    char *data;
    uint32_t len;
    uint32_t cap;
} HTMLBuf;

static void html_buf_init(HTMLBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void html_buf_append(HTMLBuf *b, const char *str, uint32_t len) {
    if (len == 0) return;
    if (b->len + len + 1 > b->cap) {
        uint32_t new_cap = b->cap == 0 ? 256 : b->cap * 2;
        if (new_cap < b->len + len + 1) new_cap = b->len + len + 1;
        char *new_data = (char *)realloc(b->data, new_cap);
        if (!new_data) return;
        b->data = new_data;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, str, len);
    b->len += len;
    b->data[b->len] = '\0';
}

/* =========================================================================
 * Script Entry Accumulator
 * ========================================================================= */

typedef struct {
    char *content;
    char *src;
    uint32_t content_len;
} HTMLScript;

typedef struct {
    HTMLScript *entries;
    uint32_t count;
    uint32_t cap;
} HTMLScriptList;

static void html_script_list_init(HTMLScriptList *sl) {
    sl->entries = NULL;
    sl->count = 0;
    sl->cap = 0;
}

static void html_script_list_add(HTMLScriptList *sl, const char *content,
                                  uint32_t content_len, const char *src) {
    if (sl->count >= sl->cap) {
        uint32_t new_cap = sl->cap == 0 ? 4 : sl->cap * 2;
        HTMLScript *new_entries = (HTMLScript *)realloc(sl->entries,
            new_cap * sizeof(HTMLScript));
        if (!new_entries) return;
        sl->entries = new_entries;
        sl->cap = new_cap;
    }
    HTMLScript *s = &sl->entries[sl->count++];
    s->content = NULL;
    s->src = NULL;
    s->content_len = 0;

    if (content && content_len > 0) {
        s->content = (char *)malloc(content_len + 1);
        if (s->content) {
            memcpy(s->content, content, content_len);
            s->content[content_len] = '\0';
            s->content_len = content_len;
        }
    }
    if (src) {
        s->src = strdup(src);
    }
}

/* =========================================================================
 * Parser Core
 * ========================================================================= */

/*
 * Lower-case a tag name in place (bounded).
 */
static void html_lowercase(char *s, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = s[i] + ('a' - 'A');
    }
}

/*
 * Find the closing tag for a raw text element (style, script).
 * Returns pointer to the '<' of </tag>, or NULL if not found.
 */
static const char *html_find_raw_close(const char *p, const char *end,
                                        const char *tag) {
    uint32_t tag_len = (uint32_t)strlen(tag);
    while (p < end) {
        if (*p == '<' && p + 1 < end && p[1] == '/') {
            const char *t = p + 2;
            /* Compare tag name case-insensitively */
            bool match = true;
            for (uint32_t i = 0; i < tag_len && t + i < end; i++) {
                char c = t[i];
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != tag[i]) { match = false; break; }
            }
            if (match && t + tag_len < end) {
                const char *after = t + tag_len;
                /* Skip whitespace */
                while (after < end && (*after == ' ' || *after == '\t'))
                    after++;
                if (after < end && *after == '>') return p;
            }
        }
        p++;
    }
    return NULL;
}

/*
 * Parse attributes from the current position.
 * p points right after the tag name. Returns pointer after '>'.
 * Sets *self_close if '/>' is found.
 */
static const char *html_parse_attrs(const char *p, const char *end,
                                     HTMLAttr *attrs, uint32_t *attr_count,
                                     bool *self_close) {
    *attr_count = 0;
    *self_close = false;

    while (p < end) {
        /* Skip whitespace */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;

        if (p >= end) break;

        /* Check for end of tag */
        if (*p == '>') { p++; return p; }
        if (*p == '/' && p + 1 < end && p[1] == '>') {
            *self_close = true;
            p += 2;
            return p;
        }

        /* Parse attribute name */
        if (*attr_count >= HTML_MAX_ATTRS) {
            /* Skip to end of tag */
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            return p;
        }

        HTMLAttr *a = &attrs[*attr_count];
        uint32_t ni = 0;
        while (p < end && *p != '=' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r' && *p != '>' && *p != '/' &&
               ni < HTML_MAX_NAME - 1) {
            a->name[ni++] = *p++;
        }
        a->name[ni] = '\0';
        if (ni == 0) {
            /* Skip unexpected character */
            if (p < end && *p != '>' && *p != '/') p++;
            continue;
        }

        /* Lowercase attribute name */
        html_lowercase(a->name, ni);

        /* Skip whitespace */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;

        /* Check for = */
        if (p < end && *p == '=') {
            p++;
            a->has_value = true;

            /* Skip whitespace */
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;

            /* Parse value */
            uint32_t vi = 0;
            if (p < end && (*p == '"' || *p == '\'')) {
                char quote = *p++;
                while (p < end && *p != quote && vi < HTML_MAX_VALUE - 1) {
                    a->value[vi++] = *p++;
                }
                if (p < end && *p == quote) p++;
            } else {
                /* Unquoted value */
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                       *p != '\r' && *p != '>' && *p != '/' &&
                       vi < HTML_MAX_VALUE - 1) {
                    a->value[vi++] = *p++;
                }
            }
            a->value[vi] = '\0';
        } else {
            /* Boolean attribute */
            a->has_value = false;
            a->value[0] = '\0';
        }

        (*attr_count)++;
    }

    return p;
}

/* =========================================================================
 * Main Parser
 * ========================================================================= */

R8EHTMLResult *r8e_html_parse(const char *html, uint32_t len) {
    if (!html || len == 0) return NULL;

    R8EHTMLResult *result = (R8EHTMLResult *)calloc(1, sizeof(R8EHTMLResult));
    if (!result) return NULL;

    HTMLBuf css_buf;
    html_buf_init(&css_buf);

    HTMLScriptList scripts;
    html_script_list_init(&scripts);

    /* Create a root fragment node to hold everything */
    R8EUIDOMNode *root = r8e_ui_dom_create_element("div");
    if (!root) {
        free(result);
        return NULL;
    }

    /* Element stack */
    R8EUIDOMNode *stack[HTML_STACK_MAX];
    int stack_top = 0;
    stack[0] = root;

    const char *p = html;
    const char *end = html + len;

    while (p < end) {
        if (*p == '<') {
            /* --- DOCTYPE --- */
            if (p + 1 < end && p[1] == '!') {
                /* Check for comment */
                if (p + 3 < end && p[2] == '-' && p[3] == '-') {
                    /* Comment: skip to --> */
                    const char *close = p + 4;
                    while (close + 2 < end) {
                        if (close[0] == '-' && close[1] == '-' && close[2] == '>') {
                            close += 3;
                            break;
                        }
                        close++;
                    }
                    if (close + 2 >= end) close = end;
                    p = close;
                    continue;
                }
                /* DOCTYPE: skip to > */
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            /* --- Closing tag --- */
            if (p + 1 < end && p[1] == '/') {
                p += 2;
                /* Read tag name */
                char tag[HTML_MAX_NAME];
                uint32_t ti = 0;
                while (p < end && *p != '>' && *p != ' ' && *p != '\t' &&
                       ti < HTML_MAX_NAME - 1) {
                    tag[ti++] = *p++;
                }
                tag[ti] = '\0';
                html_lowercase(tag, ti);

                /* Skip to > */
                while (p < end && *p != '>') p++;
                if (p < end) p++;

                /* Pop stack: find matching tag */
                if (stack_top > 0) {
                    /* Simple: just pop if we're above root */
                    stack_top--;
                }
                continue;
            }

            /* --- Opening tag --- */
            p++; /* skip '<' */

            /* Read tag name */
            char tag[HTML_MAX_NAME];
            uint32_t ti = 0;
            while (p < end && *p != '>' && *p != ' ' && *p != '\t' &&
                   *p != '\n' && *p != '\r' && *p != '/' &&
                   ti < HTML_MAX_NAME - 1) {
                tag[ti++] = *p++;
            }
            tag[ti] = '\0';
            html_lowercase(tag, ti);

            if (ti == 0) {
                /* Empty tag name — skip */
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            /* Parse attributes */
            HTMLAttr attrs[HTML_MAX_ATTRS];
            uint32_t attr_count = 0;
            bool self_close = false;
            p = html_parse_attrs(p, end, attrs, &attr_count, &self_close);

            /* --- Handle <style> --- */
            if (strcmp(tag, "style") == 0) {
                /* Find </style> and capture content */
                const char *close = html_find_raw_close(p, end, "style");
                if (close) {
                    uint32_t content_len = (uint32_t)(close - p);
                    html_buf_append(&css_buf, p, content_len);
                    /* Skip past </style> */
                    p = close + 2; /* skip </ */
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;
                } else {
                    /* No closing tag — take rest as style */
                    uint32_t content_len = (uint32_t)(end - p);
                    html_buf_append(&css_buf, p, content_len);
                    p = end;
                }
                continue;
            }

            /* --- Handle <script> --- */
            if (strcmp(tag, "script") == 0) {
                /* Check for src attribute */
                const char *src_val = NULL;
                for (uint32_t i = 0; i < attr_count; i++) {
                    if (strcmp(attrs[i].name, "src") == 0 && attrs[i].has_value) {
                        src_val = attrs[i].value;
                        break;
                    }
                }

                /* Find </script> and capture content */
                const char *close = html_find_raw_close(p, end, "script");
                if (close) {
                    uint32_t content_len = (uint32_t)(close - p);
                    if (src_val) {
                        html_script_list_add(&scripts, NULL, 0, src_val);
                    } else {
                        html_script_list_add(&scripts, p, content_len, NULL);
                    }
                    /* Skip past </script> */
                    p = close + 2;
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;
                } else {
                    /* No closing tag */
                    uint32_t content_len = (uint32_t)(end - p);
                    if (src_val) {
                        html_script_list_add(&scripts, NULL, 0, src_val);
                    } else {
                        html_script_list_add(&scripts, p, content_len, NULL);
                    }
                    p = end;
                }
                continue;
            }

            /* --- Skip non-rendered tags --- */
            if (strcmp(tag, "head") == 0 || strcmp(tag, "html") == 0 ||
                strcmp(tag, "body") == 0 || strcmp(tag, "title") == 0 ||
                strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0 ||
                strcmp(tag, "!doctype") == 0) {
                /* For html/head/body: don't create DOM nodes, but still
                 * parse their children. For void tags (meta, link),
                 * skip entirely. */
                if (strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0) {
                    continue; /* void elements, already consumed attrs */
                }
                /* For html/head/body/title: just continue parsing children
                 * without pushing a new element. title content is skipped. */
                if (strcmp(tag, "title") == 0) {
                    /* Skip to </title> */
                    const char *close = html_find_raw_close(p, end, "title");
                    if (close) {
                        p = close + 2;
                        while (p < end && *p != '>') p++;
                        if (p < end) p++;
                    }
                }
                continue;
            }

            /* --- Create DOM element --- */
            R8EUIDOMNode *elem = r8e_ui_dom_create_element(tag);
            if (!elem) continue;

            /* Set attributes */
            for (uint32_t i = 0; i < attr_count; i++) {
                r8e_ui_dom_set_attribute(elem, attrs[i].name,
                    attrs[i].has_value ? attrs[i].value : "");
            }

            /* Append to current parent */
            R8EUIDOMNode *parent = stack[stack_top];
            r8e_ui_dom_append_child(parent, elem);

            /* Push onto stack unless void or self-closing */
            if (!self_close && !html_is_void_tag(tag)) {
                if (stack_top + 1 < HTML_STACK_MAX) {
                    stack[++stack_top] = elem;
                }
            }
        } else {
            /* --- Text content --- */
            const char *start = p;
            while (p < end && *p != '<') p++;
            uint32_t tlen = (uint32_t)(p - start);

            if (tlen > 0) {
                /* Check if it's all whitespace */
                bool all_ws = true;
                for (uint32_t i = 0; i < tlen; i++) {
                    if (start[i] != ' ' && start[i] != '\t' &&
                        start[i] != '\n' && start[i] != '\r') {
                        all_ws = false;
                        break;
                    }
                }

                if (!all_ws) {
                    /* Create text node with entity decoding */
                    char *text = (char *)malloc(tlen + 1);
                    if (text) {
                        memcpy(text, start, tlen);
                        text[tlen] = '\0';
                        uint32_t decoded_len = html_decode_entities(text, tlen);
                        R8EUIDOMNode *tn = r8e_ui_dom_create_text_node(
                            text, decoded_len);
                        if (tn) {
                            r8e_ui_dom_append_child(stack[stack_top], tn);
                        }
                        free(text);
                    }
                }
            }
        }
    }

    /* Build result */
    result->root_node = root;

    if (css_buf.data) {
        result->css = css_buf.data;
        result->css_len = css_buf.len;
    }

    if (scripts.count > 0) {
        /* Transfer script entries to result */
        result->script_count = scripts.count;
        /* Allocate the result scripts array matching the struct layout */
        size_t scripts_size = scripts.count * sizeof(result->scripts[0]);
        result->scripts = malloc(scripts_size);
        if (result->scripts) {
            for (uint32_t i = 0; i < scripts.count; i++) {
                result->scripts[i].content = scripts.entries[i].content;
                result->scripts[i].src = scripts.entries[i].src;
                result->scripts[i].content_len = scripts.entries[i].content_len;
            }
        } else {
            /* Free script entries on allocation failure */
            for (uint32_t i = 0; i < scripts.count; i++) {
                free(scripts.entries[i].content);
                free(scripts.entries[i].src);
            }
            result->script_count = 0;
        }
        free(scripts.entries);
    }

    return result;
}

/* =========================================================================
 * Cleanup
 * ========================================================================= */

void r8e_html_result_free(R8EHTMLResult *result) {
    if (!result) return;

    free(result->css);

    for (uint32_t i = 0; i < result->script_count; i++) {
        free(result->scripts[i].content);
        free(result->scripts[i].src);
    }
    free(result->scripts);

    /* Note: root_node is NOT freed — DOM nodes are managed by the DOM */

    free(result);
}
