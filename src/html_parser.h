/*
 * Single-pass HTML tokenizer -> PageElement array in PSRAM
 * Written by Alun Morris and Claude Code
 *
 * 060326 Single-pass HTML tokenizer -> PageElement array in PSRAM
 * 160326 Add assume_body param to html_parse for partial HTML
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ELEM_HEADING,
    ELEM_PARAGRAPH,
    ELEM_LINK,
    ELEM_LINEBREAK,
    ELEM_INPUT,      // text input or textarea
    ELEM_HIDDEN,     // hidden input (not rendered)
    ELEM_SUBMIT,     // submit button
    ELEM_SELECT,     // dropdown select
    ELEM_IMAGE,      // inline image (src in href, alt in text)
    ELEM_HR          // horizontal rule
} ElemType;

typedef struct {
    ElemType type;
    char    *text;    // label/placeholder for inputs, button text for submit
    char    *href;    // resolved absolute URL (ELEM_LINK only), or NULL
    char    *name;    // form field name (ELEM_INPUT/HIDDEN/SELECT)
    char    *value;   // default value (ELEM_INPUT/HIDDEN) or options (ELEM_SELECT, \n-separated)
    uint8_t  level;   // 1-6 for ELEM_HEADING, 0 otherwise
    uint8_t  form_id;  // form scope index (for HIDDEN/SUBMIT/INPUT/SELECT)
    bool     multiline; // true for <textarea>
    bool     bold;      // true if inside <b>/<strong>
    bool     italic;    // true if inside <em>/<i>/<dfn>
    bool     monospace; // true if inside <code>/<kbd>/<samp>
    uint32_t color;     // 0 = default, else 0xRRGGBB | 0x01000000 flag
    uint8_t  font_size; // 0 = default, else snapped to 14/16/18/20/24
} PageElement;

#define MAX_ELEMENTS 1024
#define MAX_FORMS    8

typedef struct {
    char action[512];
    bool is_post;
} FormInfo;

typedef struct {
    PageElement elems[MAX_ELEMENTS];
    int         count;
    bool        error;              // true if fetch/parse failed
    int         http_status;        // HTTP status code (e.g. 404), 0 if unknown
    char        form_action[512];   // first form action (legacy, kept for compat)
    bool        form_is_post;       // first form method
    FormInfo    forms[MAX_FORMS];
    int         form_count;
    char        text_pool[131072];  // 128KB inline text pool
    size_t      pool_used;
} ParseResult;

// Allocate ParseResult in PSRAM
ParseResult *parse_result_alloc();
void         parse_result_free(ParseResult *r);

// Parse html_buf (null-terminated) into r. base_url used for link resolution.
// If assume_body is true, skip waiting for <body> tag (for partial/truncated HTML).
void html_parse(const char *html_buf, const char *base_url, ParseResult *r,
                bool assume_body = false);
