/*
 * r8e_html.h - HTML parser for r8e
 *
 * Streaming tokenizer and tree builder that converts HTML source
 * into a DOM tree (R8EUIDOMNode), extracting <style> and <script>
 * blocks for CSS and JS processing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_HTML_H
#define R8E_HTML_H

#include <stdint.h>
#include <stdbool.h>

/* Parsed result from HTML */
typedef struct {
    /* Extracted style blocks (concatenated) */
    char *css;
    uint32_t css_len;

    /* Extracted script sources (array of script contents/src paths) */
    struct {
        char *content;      /* inline script content, or NULL if src */
        char *src;          /* src attribute value, or NULL if inline */
        uint32_t content_len;
    } *scripts;
    uint32_t script_count;

    /* Root DOM node (the <body> or document root) */
    void *root_node;        /* R8EUIDOMNode* */
} R8EHTMLResult;

/* Parse HTML string into DOM tree + extracted styles and scripts */
R8EHTMLResult *r8e_html_parse(const char *html, uint32_t len);

/* Free parsed result (does NOT free DOM nodes — they're managed by the DOM) */
void r8e_html_result_free(R8EHTMLResult *result);

#endif /* R8E_HTML_H */
