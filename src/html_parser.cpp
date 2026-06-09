/*
 * Single-pass HTML tokenizer
 * Written by Alun Morris and Claude Code
 *
 * 060326 Single-pass HTML tokenizer
 * 160326 Add assume_body param for parsing partial/truncated HTML
 * 130526 Increase tag buffer 1024→2048 to handle long WordPress img tags
 */
#include "html_parser.h"
#include "url_utils.h"
#include <Arduino.h>
#include <string.h>
#include <ctype.h>

ParseResult *parse_result_alloc() {
    ParseResult *r = (ParseResult *)heap_caps_malloc(sizeof(ParseResult),
                                                      MALLOC_CAP_SPIRAM);
    if (r) memset(r, 0, sizeof(ParseResult));
    return r;
}

void parse_result_free(ParseResult *r) {
    if (r) heap_caps_free(r);
}

static char *pool_add(ParseResult *r, const char *text, size_t len) {
    if (r->pool_used + len + 1 > sizeof(r->text_pool)) return NULL;
    char *p = r->text_pool + r->pool_used;
    memcpy(p, text, len);
    p[len] = '\0';
    r->pool_used += len + 1;
    return p;
}

// Parse a CSS color name or #hex value. Returns 0xRRGGBB | 0x01000000, or 0 on failure.
static uint32_t parse_css_color(const char *s) {
    while (*s == ' ') s++;
    if (*s == '#') {
        s++;
        unsigned long v = strtoul(s, nullptr, 16);
        size_t len = 0;
        while (isxdigit((uint8_t)s[len])) len++;
        if (len == 3)
            v = ((v & 0xF00) << 8 | (v & 0x0F0) << 4 | (v & 0x00F)) * 0x11;
        else if (len != 6)
            return 0;
        return (uint32_t)v | 0x01000000;
    }
    // Named colors (common subset)
    struct { const char *name; uint32_t val; } names[] = {
        {"black",   0x000000}, {"white",   0xFFFFFF}, {"red",     0xFF0000},
        {"green",   0x008000}, {"blue",    0x0000FF}, {"yellow",  0xFFFF00},
        {"cyan",    0x00FFFF}, {"magenta", 0xFF00FF}, {"orange",  0xFFA500},
        {"purple",  0x800080}, {"gray",    0x808080}, {"grey",    0x808080},
        {"lime",    0x00FF00}, {"navy",    0x000080}, {"teal",    0x008080},
        {"brown",   0xA52A2A}, {"silver",  0xC0C0C0}, {"maroon", 0x800000},
        {"pink",    0xFFC0CB}, {"coral",   0xFF7F50},
    };
    for (auto &n : names) {
        if (strcasecmp(s, n.name) == 0) return n.val | 0x01000000;
    }
    return 0;
}

// Snap a pixel value to nearest available Montserrat size
static uint8_t snap_font_size(int px) {
    if (px < 15) return 14;
    if (px < 17) return 16;
    if (px < 19) return 18;
    if (px < 22) return 20;
    return 24;
}

// Extract font-size from style attribute. Returns snapped size or 0 if not found.
static uint8_t parse_style_font_size(const char *tag_str) {
    if (!tag_str) return 0;
    const char *p = strcasestr(tag_str, "font-size:");
    if (!p) return 0;
    p += 10;
    while (*p == ' ') p++;
    // Keywords (longest first to avoid prefix matches)
    if (strncasecmp(p, "xx-large", 8) == 0) return 24;
    if (strncasecmp(p, "x-large",  7) == 0) return 20;
    if (strncasecmp(p, "xx-small", 8) == 0) return 14;
    if (strncasecmp(p, "x-small",  7) == 0) return 14;
    if (strncasecmp(p, "larger",   6) == 0) return 18;
    if (strncasecmp(p, "smaller",  7) == 0) return 14;
    if (strncasecmp(p, "large",    5) == 0) return 18;
    if (strncasecmp(p, "medium",   6) == 0) return 16;
    if (strncasecmp(p, "small",    5) == 0) return 14;
    // Numeric
    char *end;
    float val = strtof(p, &end);
    if (end == p || val <= 0) return 0;
    while (*end == ' ') end++;
    if (strncasecmp(end, "em", 2) == 0 || strncasecmp(end, "rem", 3) == 0 || *end == '%')
        return 0;  // relative units — skip
    return snap_font_size((int)(val + 0.5f));
}

// Extract color from style attribute, e.g. style="color:blue"
static uint32_t parse_style_color(const char *tag_str) {
    char style[256] = "";
    // Manual extraction - get_attr would work but style values can contain semicolons
    const char *sp = strcasestr(tag_str, "style");
    if (!sp) return 0;
    sp += 5;
    while (*sp == ' ' || *sp == '=') sp++;
    char quote = *sp;
    if (quote != '"' && quote != '\'') return 0;
    sp++;
    const char *end = strchr(sp, quote);
    if (!end) return 0;
    size_t len = (size_t)(end - sp);
    if (len >= sizeof(style)) len = sizeof(style) - 1;
    memcpy(style, sp, len);
    style[len] = '\0';

    // Find "color:" (but not "background-color:")
    char *cp = style;
    while ((cp = strcasestr(cp, "color")) != nullptr) {
        // Make sure it's not "background-color"
        if (cp > style && cp[-1] != ' ' && cp[-1] != ';' && cp[-1] != '"') {
            cp += 5;
            continue;
        }
        cp += 5;
        while (*cp == ' ' || *cp == ':') cp++;
        // Extract value until ; or end
        char val[64];
        size_t vi = 0;
        while (*cp && *cp != ';' && *cp != '"' && *cp != '\'' && vi < sizeof(val) - 1) {
            val[vi++] = *cp++;
        }
        val[vi] = '\0';
        // Trim trailing spaces
        while (vi > 0 && val[vi-1] == ' ') val[--vi] = '\0';
        uint32_t c = parse_css_color(val);
        if (c) return c;
        break;
    }
    return 0;
}

static bool add_elem(ParseResult *r, ElemType type, const char *text,
                     size_t text_len, const char *href, uint8_t level,
                     bool bold = false, uint32_t color = 0,
                     bool italic = false, bool monospace = false,
                     uint8_t font_size = 0) {
    if (r->count >= MAX_ELEMENTS) return false;
    if (type != ELEM_LINEBREAK && text_len == 0) return true;
    PageElement *e = &r->elems[r->count];
    memset(e, 0, sizeof(*e));
    e->type      = type;
    e->level     = level;
    e->bold      = bold;
    e->italic    = italic;
    e->monospace = monospace;
    e->color     = color;
    e->font_size = font_size;
    e->text  = pool_add(r, text, text_len);
    if (type == ELEM_LINK && href)
        e->href = pool_add(r, href, strlen(href));
    if (e->text || type == ELEM_LINEBREAK) r->count++;
    return true;
}

static bool tag_is(const char *name, size_t name_len, const char *match) {
    size_t ml = strlen(match);
    if (name_len < ml) return false;
    return strncasecmp(name, match, ml) == 0 &&
           (name_len == ml || !isalnum((uint8_t)name[ml]));
}

static bool get_attr(const char *tag, const char *attr, char *out, size_t out_len) {
    size_t alen = strlen(attr);
    const char *p = tag;
    while ((p = strcasestr(p, attr)) != NULL) {
        // Must be preceded by whitespace (word boundary)
        if (p > tag && !isspace((uint8_t)p[-1])) { p += alen; continue; }
        const char *q = p + alen;
        while (*q == ' ') q++;
        if (*q != '=') { p += alen; continue; }
        q++;
        while (*q == ' ') q++;
        char quote = 0;
        if (*q == '"' || *q == '\'') quote = *q++;
        size_t i = 0;
        while (*q && i < out_len - 1) {
            if (quote && *q == quote) break;
            if (!quote && (*q == ' ' || *q == '>')) break;
            out[i++] = *q++;
        }
        out[i] = '\0';
        return i > 0;
    }
    return false;
}

static void decode_entities_once(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if      (strncmp(r, "&amp;",  5) == 0) { *w++ = '&';  r += 5; }
            else if (strncmp(r, "&lt;",   4) == 0) { *w++ = '<';  r += 4; }
            else if (strncmp(r, "&gt;",   4) == 0) { *w++ = '>';  r += 4; }
            else if (strncmp(r, "&nbsp;", 6) == 0) { *w++ = ' ';  r += 6; }
            else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"';  r += 6; }
            else if (strncmp(r, "&#39;",  5) == 0) { *w++ = '\''; r += 5; }
            // Typographic quotes and punctuation
            else if (strncmp(r, "&ldquo;",  7) == 0) { *w++ = '"';  r += 7; }
            else if (strncmp(r, "&rdquo;",  7) == 0) { *w++ = '"';  r += 7; }
            else if (strncmp(r, "&lsquo;",  7) == 0) { *w++ = '\''; r += 7; }
            else if (strncmp(r, "&rsquo;",  7) == 0) { *w++ = '\''; r += 7; }
            else if (strncmp(r, "&mdash;",  7) == 0) { *w++ = '-';  r += 7; }
            else if (strncmp(r, "&ndash;",  7) == 0) { *w++ = '-';  r += 7; }
            else if (strncmp(r, "&hellip;", 8) == 0) { *w++ = '.'; *w++ = '.'; *w++ = '.'; r += 8; }
            else if (strncmp(r, "&laquo;",  7) == 0) { *w++ = '"';  r += 7; }
            else if (strncmp(r, "&raquo;",  7) == 0) { *w++ = '"';  r += 7; }
            else if (strncmp(r, "&apos;",   6) == 0) { *w++ = '\''; r += 6; }
            else if (strncmp(r, "&copy;",   6) == 0) { *w++ = '('; *w++ = 'c'; *w++ = ')'; r += 6; }
            else if (strncmp(r, "&#", 2) == 0) {
                // Numeric entity &#NN; or &#xNN;
                char *semi = strchr(r, ';');
                if (semi && semi - r < 12) {
                    long cp = 0;
                    if (r[2] == 'x' || r[2] == 'X')
                        cp = strtol(r + 3, nullptr, 16);
                    else
                        cp = strtol(r + 2, nullptr, 10);
                    if (cp >= 0x20 && cp <= 0x7E) {
                        *w++ = (char)cp;
                    } else {
                        // Map common Unicode codepoints to ASCII
                        switch (cp) {
                            case 8216: case 8217: *w++ = '\''; break; // smart quotes
                            case 8218:            *w++ = ',';  break;
                            case 8220: case 8221: *w++ = '"';  break;
                            case 8211: case 8212: *w++ = '-';  break; // en/em dash
                            case 8230: *w++ = '.'; *w++ = '.'; *w++ = '.'; break;
                            case 8226: *w++ = '*';  break; // bullet
                            case 169:  *w++ = '('; *w++ = 'c'; *w++ = ')'; break;
                            case 174:  *w++ = '('; *w++ = 'R'; *w++ = ')'; break;
                            case 8482: *w++ = 'T'; *w++ = 'M'; break;
                            default: break; // drop unknown non-ASCII
                        }
                    }
                    r = semi + 1;
                } else {
                    *w++ = *r++;
                }
            }
            else                                    { *w++ = *r++; }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Run decode twice to handle double-encoded entities like &amp;lt; → &lt; → <
static void decode_entities(char *s) {
    decode_entities_once(s);
    if (strchr(s, '&')) decode_entities_once(s);
}

// IDs commonly used for "skip to main content" targets
static const char * const s_content_ids[] = {
    "main", "content", "primary", "container", "wrapper",
    "main-content", "maincontent", "mainContent",
    "main-container", "mainContainer",
    "main-wrapper", "mainWrapper",
    "content-wrapper", "contentWrapper",
    "content-area", "contentArea",
    "content-container", "contentContainer",
    "page-content", "pageContent",
    "page-body", "pageBody",
    "primary-content", "primaryContent",
    "site-content", "siteContent", "site-main", "siteMain",
    "app-content", "appContent",
    "layout-content", "layoutContent",
    "article-content", "articleContent",
    "post-content", "postContent",
    "entry-content", "entryContent",
    "body-content", "bodyContent",
    "mw-content-text",
    nullptr
};

static bool is_content_id(const char *id) {
    for (int i = 0; s_content_ids[i]; i++)
        if (strcasecmp(id, s_content_ids[i]) == 0) return true;
    return false;
}

void html_parse(const char *html, const char *base_url, ParseResult *r,
                bool assume_body) {
    url_set_base(base_url);

    static char text_acc[8192];
    size_t acc_len = 0;

    ElemType cur_type  = ELEM_PARAGRAPH;
    uint8_t  cur_level = 0;
    bool     in_body   = assume_body;
    bool     in_link   = false;
    char     link_href[512] = "";
    char     skip_to_id[128] = "";  // target id from skip-to-content link
    bool     found_content = false; // true once we've hit the target id
    // Form state
    int      cur_form_id = -1;     // current form index (-1 = outside form)
    bool     in_select   = false;
    int      select_elem = -1;       // index of current ELEM_SELECT in elems[]
    char     select_name[128] = "";
    static char select_opts[2048];   // \n-separated options for current <select>
    size_t   select_opts_len = 0;
    bool     in_textarea = false;
    int      textarea_elem = -1;     // index of current ELEM_INPUT (textarea)
    bool     in_bold = false;
    bool     acc_all_bold = true;   // true if ALL accumulated text is bold
    bool     in_italic = false;
    bool     acc_all_italic = true;
    bool     in_mono = false;
    bool     acc_all_mono = true;
    uint32_t cur_color     = 0;    // current inline color (0 = default)
    uint32_t acc_color     = 0;    // color seen during accumulation
    int      color_depth   = 0;    // nesting depth of color-setting tags
    uint8_t  cur_font_size = 0;    // current inline font size (0 = default)
    uint8_t  acc_font_size = 0;    // font size seen during accumulation
    int      font_size_depth = 0;  // nesting depth of font-size-setting tags
    // List state — stack for nested lists
    #define MAX_LIST_DEPTH 4
    struct { bool ordered; int counter; } list_stack[MAX_LIST_DEPTH];
    int      list_depth = 0;
    size_t   acc_prefix_len = 0;  // length of list bullet/number prefix in accumulator

    const char *p = html;

    auto flush_acc = [&]() {
        if (acc_len > 0) {
            text_acc[acc_len] = '\0';
            decode_entities(text_acc);
            size_t start = 0;
            while (start < acc_len && isspace((uint8_t)text_acc[start])) start++;
            size_t end = strlen(text_acc);
            while (end > start && isspace((uint8_t)text_acc[end-1])) end--;
            size_t len = end - start;
            if (len > 0) {
                add_elem(r, in_link ? ELEM_LINK : cur_type,
                         text_acc + start, len,
                         in_link ? link_href : NULL,
                         cur_level, acc_all_bold, acc_color,
                         acc_all_italic, acc_all_mono, acc_font_size);
            }
            acc_len = 0;
            acc_prefix_len = 0;
            acc_all_bold = true;
            acc_all_italic = true;
            acc_all_mono = true;
            acc_color = cur_color;
            acc_font_size = cur_font_size;
        }
    };

    // Check if accumulator has alphanumeric content beyond any list prefix
    auto acc_has_word = [&]() -> bool {
        for (size_t j = acc_prefix_len; j < acc_len; j++)
            if (isalnum((uint8_t)text_acc[j])) return true;
        return false;
    };

    while (*p) {
        if (*p != '<') {
            if (in_body && acc_len < sizeof(text_acc) - 1) {
                uint8_t c = (uint8_t)*p;
                if (c >= 0x80) {
                    // Decode UTF-8 and map common chars to ASCII
                    uint32_t cp = 0;
                    int bytes = 0;
                    if      (c >= 0xF0) { cp = c & 0x07; bytes = 4; }
                    else if (c >= 0xE0) { cp = c & 0x0F; bytes = 3; }
                    else                { cp = c & 0x1F; bytes = 2; }
                    for (int b = 1; b < bytes && ((uint8_t)p[b] & 0xC0) == 0x80; b++)
                        cp = (cp << 6) | (p[b] & 0x3F);
                    p += bytes;
                    // Map to ASCII equivalent
                    char mapped = 0;
                    switch (cp) {
                        case 0x2018: case 0x2019: case 0x201A: mapped = '\''; break;
                        case 0x201C: case 0x201D: case 0x201E: mapped = '"';  break;
                        case 0x2013: case 0x2014: mapped = '-';  break;
                        case 0x2022: mapped = '*';  break;
                        case 0x00A0: mapped = ' ';  break; // non-breaking space
                        case 0x00A9: mapped = 'c';  break; // copyright (simplified)
                        case 0x00AE: mapped = 'R';  break; // registered
                        default: break;
                    }
                    if (mapped && acc_len < sizeof(text_acc) - 1) {
                        if (mapped == ' ') {
                            if (acc_len > 0 && text_acc[acc_len-1] != ' ')
                                text_acc[acc_len++] = ' ';
                        } else {
                            text_acc[acc_len++] = mapped;
                            if (!in_bold) acc_all_bold = false;
                    if (!in_italic) acc_all_italic = false;
                    if (!in_mono) acc_all_mono = false;
                            if (cur_color) acc_color = cur_color;
                            if (cur_font_size) acc_font_size = cur_font_size;
                        }
                    }
                    continue;
                }
                if (isspace(c)) {
                    if (acc_len > 0 && text_acc[acc_len-1] != ' ')
                        text_acc[acc_len++] = ' ';
                } else {
                    text_acc[acc_len++] = *p;
                    if (!in_bold) acc_all_bold = false;
                    if (!in_italic) acc_all_italic = false;
                    if (!in_mono) acc_all_mono = false;
                    if (cur_color) acc_color = cur_color;
                    if (cur_font_size) acc_font_size = cur_font_size;
                }
            }
            p++;
            continue;
        }

        // Tag
        const char *tag_start = p + 1;
        const char *tag_end   = strchr(tag_start, '>');
        if (!tag_end) break;

        bool closing = (tag_start[0] == '/');
        const char *name = closing ? tag_start + 1 : tag_start;
        size_t name_len = 0;
        while (name[name_len] && name[name_len] != ' ' &&
               name[name_len] != '>' && name[name_len] != '/') name_len++;

        size_t tag_str_len = (size_t)(tag_end - tag_start);
        static char tag_str[2048];
        size_t copy_len = tag_str_len < sizeof(tag_str) - 1 ? tag_str_len : sizeof(tag_str) - 1;
        memcpy(tag_str, tag_start, copy_len);
        tag_str[copy_len] = '\0';

        // Check for skip-to-content target id on any opening tag
        if (!closing && skip_to_id[0] && !found_content && in_body) {
            char id_val[128] = "";
            if (get_attr(tag_str, "id", id_val, sizeof(id_val))) {
                if (strcasecmp(id_val, skip_to_id) == 0) {
                    // Discard everything accumulated so far
                    r->count = 0;
                    r->pool_used = 0;
                    acc_len = 0;
                    found_content = true;
                }
            }
        }

        // Skip script/style/title blocks entirely
        if (!closing && (tag_is(name, name_len, "script") ||
                         tag_is(name, name_len, "style") ||
                         tag_is(name, name_len, "title"))) {
            const char *close_tag = tag_is(name, name_len, "script")
                                    ? "</script>"
                                    : tag_is(name, name_len, "style")
                                    ? "</style>" : "</title>";
            const char *close = strcasestr(tag_end + 1, close_tag);
            p = close ? close + strlen(close_tag) : tag_end + 1;
            continue;
        }

        if (!in_body) {
            if (tag_is(name, name_len, "body")) in_body = true;
            p = tag_end + 1;
            continue;
        }

        // Headings h1-h6
        if (name_len == 2 && tolower((uint8_t)name[0]) == 'h' &&
            name[1] >= '1' && name[1] <= '6') {
            flush_acc();
            if (!closing) {
                cur_type  = ELEM_HEADING;
                cur_level = name[1] - '0';
            } else {
                cur_type  = ELEM_PARAGRAPH;
                cur_level = 0;
            }
        }
        // Paragraphs
        else if (tag_is(name, name_len, "p")) {
            flush_acc();
            if (!closing) {
                cur_type = ELEM_PARAGRAPH; cur_level = 0;
                uint32_t c = parse_style_color(tag_str);
                if (c) { cur_color = c; color_depth++; }
                uint8_t fs = parse_style_font_size(tag_str);
                if (fs) { cur_font_size = fs; font_size_depth++; }
            } else {
                if (color_depth > 0) {
                    color_depth--;
                    if (color_depth == 0) { cur_color = 0; acc_color = 0; }
                }
                if (font_size_depth > 0) {
                    font_size_depth--;
                    if (font_size_depth == 0) { cur_font_size = 0; acc_font_size = 0; }
                }
            }
        }
        // Line break
        else if (tag_is(name, name_len, "br")) {
            flush_acc();
            add_elem(r, ELEM_LINEBREAK, "", 0, NULL, 0);
        }
        // Blockquote — prefix lines with indent
        else if (tag_is(name, name_len, "blockquote")) {
            flush_acc();
            if (!closing) {
                // Prepend indent marker to accumulated text
                if (acc_len + 2 < sizeof(text_acc)) {
                    text_acc[acc_len++] = ' ';
                    text_acc[acc_len++] = ' ';
                }
            }
        }
        // Definition list
        else if (tag_is(name, name_len, "dl")) {
            flush_acc();
        }
        else if (tag_is(name, name_len, "dt")) {
            flush_acc();
            if (!closing) in_bold = true;
            else in_bold = false;
        }
        else if (tag_is(name, name_len, "dd")) {
            flush_acc();
            if (!closing && acc_len + 4 < sizeof(text_acc)) {
                text_acc[acc_len++] = ' ';
                text_acc[acc_len++] = ' ';
                text_acc[acc_len++] = '-';
                text_acc[acc_len++] = ' ';
                acc_prefix_len = acc_len;
            }
        }
        // Links
        else if (tag_is(name, name_len, "a")) {
            flush_acc();
            if (!closing) {
                char href_raw[512] = "";
                if (get_attr(tag_str, "href", href_raw, sizeof(href_raw))) {
                    // Check for skip-to-content anchor link
                    if (href_raw[0] == '#' && !skip_to_id[0] && !found_content) {
                        if (is_content_id(href_raw + 1)) {
                            strncpy(skip_to_id, href_raw + 1, sizeof(skip_to_id) - 1);
                        }
                    }
                    char resolved[512];
                    if (url_resolve(url_get_base(), href_raw,
                                    resolved, sizeof(resolved))) {
                        strncpy(link_href, resolved, sizeof(link_href) - 1);
                        in_link = true;
                    }
                }
            } else {
                in_link = false;
                link_href[0] = '\0';
            }
        }
        // Bold — flush to separate styled segments
        else if (tag_is(name, name_len, "b") ||
                 tag_is(name, name_len, "strong")) {
            if (closing || acc_has_word()) flush_acc();
            in_bold = !closing;
        }
        // Italic/emphasis — render in grey
        else if (tag_is(name, name_len, "em") ||
                 tag_is(name, name_len, "i") ||
                 tag_is(name, name_len, "dfn")) {
            if (closing || acc_has_word()) flush_acc();
            in_italic = !closing;
        }
        // Monospace — code/kbd/samp
        else if (tag_is(name, name_len, "code") ||
                 tag_is(name, name_len, "kbd") ||
                 tag_is(name, name_len, "samp") ||
                 tag_is(name, name_len, "var") ||
                 tag_is(name, name_len, "pre")) {
            if (closing || acc_has_word()) flush_acc();
            in_mono = !closing;
        }
        // Inline quote — insert quote marks
        else if (tag_is(name, name_len, "q")) {
            if (acc_len < sizeof(text_acc) - 1)
                text_acc[acc_len++] = '"';
        }

        // Inline color/size: <span style="...">, <font color="..." size="...">
        else if (tag_is(name, name_len, "span") ||
                 tag_is(name, name_len, "font")) {
            if (!closing) {
                uint32_t c = parse_style_color(tag_str);
                // Also try <font color="...">
                if (!c && tag_is(name, name_len, "font")) {
                    char cval[64] = "";
                    if (get_attr(tag_str, "color", cval, sizeof(cval)))
                        c = parse_css_color(cval);
                }
                if (c) { cur_color = c; color_depth++; }
                uint8_t fs = parse_style_font_size(tag_str);
                // Also try <font size="1-7">
                if (!fs && tag_is(name, name_len, "font")) {
                    char sval[8] = "";
                    if (get_attr(tag_str, "size", sval, sizeof(sval))) {
                        int n = atoi(sval);
                        if (n >= 1 && n <= 7) {
                            static const uint8_t html_sizes[7] = {14,14,16,18,20,20,24};
                            fs = html_sizes[n - 1];
                        }
                    }
                }
                if (fs) { cur_font_size = fs; font_size_depth++; }
            } else {
                if (color_depth > 0) {
                    color_depth--;
                    if (color_depth == 0) { cur_color = 0; acc_color = 0; }
                }
                if (font_size_depth > 0) {
                    font_size_depth--;
                    if (font_size_depth == 0) { cur_font_size = 0; acc_font_size = 0; }
                }
            }
        }
        // Image (self-closing)
        else if (tag_is(name, name_len, "img") && !closing) {
            flush_acc();
            char src[512] = "";
            char alt[256] = "";
            get_attr(tag_str, "src", src, sizeof(src));
            get_attr(tag_str, "alt", alt, sizeof(alt));
            if (alt[0]) decode_entities(alt);
            if (src[0] && r->count < MAX_ELEMENTS) {
                char resolved[512];
                if (url_resolve(url_get_base(), src, resolved, sizeof(resolved))) {
                    PageElement *el = &r->elems[r->count];
                    memset(el, 0, sizeof(*el));
                    el->type = ELEM_IMAGE;
                    el->href = pool_add(r, resolved, strlen(resolved));
                    el->text = alt[0] ? pool_add(r, alt, strlen(alt)) : nullptr;
                    r->count++;
                }
            }
        }

        // Check any opening tag for background-image in style attribute
        if (!closing && r->count < MAX_ELEMENTS) {
            char style[512] = "";
            if (get_attr(tag_str, "style", style, sizeof(style))) {
                char *bg = strcasestr(style, "background-image:");
                if (bg) {
                    char *url_start = strstr(bg, "url(");
                    if (url_start) {
                        url_start += 4;
                        // Skip optional quote
                        char quote = 0;
                        if (*url_start == '\'' || *url_start == '"') {
                            quote = *url_start++;
                        }
                        char *url_end = quote ? strchr(url_start, quote)
                                              : strchr(url_start, ')');
                        if (url_end) {
                            char src[512] = "";
                            size_t slen = (size_t)(url_end - url_start);
                            if (slen < sizeof(src)) {
                                memcpy(src, url_start, slen);
                                src[slen] = '\0';
                                // Decode HTML entities in URL (e.g. &#038; -> &)
                                decode_entities(src);
                                char resolved[512];
                                if (url_resolve(url_get_base(), src,
                                                resolved, sizeof(resolved))) {
                                    flush_acc();
                                    PageElement *el = &r->elems[r->count];
                                    memset(el, 0, sizeof(*el));
                                    el->type = ELEM_IMAGE;
                                    el->href = pool_add(r, resolved,
                                                        strlen(resolved));
                                    r->count++;
                                }
                            }
                        }
                    }
                }
            }
        }
        // Form
        if (tag_is(name, name_len, "form")) {
            flush_acc();
            if (!closing) {
                char action_raw[512] = "";
                get_attr(tag_str, "action", action_raw, sizeof(action_raw));
                char resolved_action[512] = "";
                if (action_raw[0]) {
                    char resolved[512];
                    if (url_resolve(url_get_base(), action_raw,
                                    resolved, sizeof(resolved)))
                        strncpy(resolved_action, resolved, sizeof(resolved_action) - 1);
                } else {
                    strncpy(resolved_action, base_url, sizeof(resolved_action) - 1);
                }
                char method[16] = "get";
                get_attr(tag_str, "method", method, sizeof(method));
                // Legacy: first form stored in form_action/form_is_post
                if (r->form_count == 0) {
                    strncpy(r->form_action, resolved_action, sizeof(r->form_action) - 1);
                    r->form_is_post = (strcasecmp(method, "post") == 0);
                }
                // Per-form storage
                if (r->form_count < MAX_FORMS) {
                    cur_form_id = r->form_count;
                    FormInfo *fi = &r->forms[r->form_count++];
                    strncpy(fi->action, resolved_action, sizeof(fi->action) - 1);
                    fi->is_post = (strcasecmp(method, "post") == 0);
                }
            } else {
                cur_form_id = -1;
            }
        }
        // Input (self-closing)
        else if (tag_is(name, name_len, "input") && !closing) {
            flush_acc();
            char itype[32] = "text";
            char fname[128] = "";
            char fvalue[256] = "";
            char placeholder[128] = "";
            get_attr(tag_str, "type", itype, sizeof(itype));
            get_attr(tag_str, "name", fname, sizeof(fname));
            get_attr(tag_str, "value", fvalue, sizeof(fvalue));
            get_attr(tag_str, "placeholder", placeholder, sizeof(placeholder));

            if (strcasecmp(itype, "hidden") == 0) {
                if (r->count < MAX_ELEMENTS) {
                    PageElement *e = &r->elems[r->count];
                    memset(e, 0, sizeof(*e));
                    e->type    = ELEM_HIDDEN;
                    e->form_id = cur_form_id >= 0 ? (uint8_t)cur_form_id : 0;
                    e->name    = fname[0] ? pool_add(r, fname, strlen(fname)) : NULL;
                    e->value   = fvalue[0] ? pool_add(r, fvalue, strlen(fvalue)) : NULL;
                    r->count++;
                }
            } else if (strcasecmp(itype, "submit") == 0) {
                // Decode for display label only; store raw value for form data
                char label[256];
                strncpy(label, fvalue[0] ? fvalue : "Submit", sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';
                decode_entities(label);
                if (r->count < MAX_ELEMENTS) {
                    PageElement *e = &r->elems[r->count];
                    memset(e, 0, sizeof(*e));
                    e->type    = ELEM_SUBMIT;
                    e->form_id = cur_form_id >= 0 ? (uint8_t)cur_form_id : 0;
                    e->text    = pool_add(r, label, strlen(label));
                    e->name    = fname[0] ? pool_add(r, fname, strlen(fname)) : NULL;
                    e->value   = fvalue[0] ? pool_add(r, fvalue, strlen(fvalue)) : NULL;
                    r->count++;
                }
            } else if (strcasecmp(itype, "text") == 0 ||
                       strcasecmp(itype, "search") == 0 ||
                       strcasecmp(itype, "email") == 0 ||
                       strcasecmp(itype, "url") == 0 ||
                       strcasecmp(itype, "password") == 0) {
                if (placeholder[0]) decode_entities(placeholder);
                if (r->count < MAX_ELEMENTS) {
                    PageElement *e = &r->elems[r->count];
                    memset(e, 0, sizeof(*e));
                    e->type      = ELEM_INPUT;
                    e->name      = fname[0] ? pool_add(r, fname, strlen(fname)) : NULL;
                    e->value     = fvalue[0] ? pool_add(r, fvalue, strlen(fvalue)) : NULL;
                    e->text      = placeholder[0] ? pool_add(r, placeholder, strlen(placeholder)) : NULL;
                    e->multiline = false;
                    r->count++;
                }
            }
        }
        // Textarea
        else if (tag_is(name, name_len, "textarea")) {
            flush_acc();
            if (!closing) {
                char fname[128] = "";
                get_attr(tag_str, "name", fname, sizeof(fname));
                if (r->count < MAX_ELEMENTS) {
                    PageElement *e = &r->elems[r->count];
                    memset(e, 0, sizeof(*e));
                    e->type      = ELEM_INPUT;
                    e->name      = fname[0] ? pool_add(r, fname, strlen(fname)) : NULL;
                    e->multiline = true;
                    textarea_elem = r->count;
                    in_textarea   = true;
                    r->count++;
                }
            } else {
                // Accumulated text becomes the textarea default value
                if (in_textarea && textarea_elem >= 0 && acc_len > 0) {
                    text_acc[acc_len] = '\0';
                    decode_entities(text_acc);
                    // Trim
                    size_t s = 0;
                    while (s < acc_len && isspace((uint8_t)text_acc[s])) s++;
                    size_t e = strlen(text_acc);
                    while (e > s && isspace((uint8_t)text_acc[e-1])) e--;
                    if (e > s)
                        r->elems[textarea_elem].value = pool_add(r, text_acc + s, e - s);
                    acc_len = 0;
                }
                in_textarea   = false;
                textarea_elem = -1;
            }
        }
        // Select
        else if (tag_is(name, name_len, "select")) {
            flush_acc();
            if (!closing) {
                get_attr(tag_str, "name", select_name, sizeof(select_name));
                select_opts_len = 0;
                select_opts[0]  = '\0';
                in_select = true;
                // Create the element now; options filled on </select>
                if (r->count < MAX_ELEMENTS) {
                    PageElement *e = &r->elems[r->count];
                    memset(e, 0, sizeof(*e));
                    e->type = ELEM_SELECT;
                    e->name = select_name[0] ? pool_add(r, select_name, strlen(select_name)) : NULL;
                    select_elem = r->count;
                    r->count++;
                }
            } else {
                // Store accumulated options
                if (in_select && select_elem >= 0 && select_opts_len > 0)
                    r->elems[select_elem].value = pool_add(r, select_opts, select_opts_len);
                in_select   = false;
                select_elem = -1;
            }
        }
        // Option (inside select) — capture text on close
        else if (tag_is(name, name_len, "option") && in_select) {
            if (closing && acc_len > 0) {
                text_acc[acc_len] = '\0';
                decode_entities(text_acc);
                size_t s = 0;
                while (s < acc_len && isspace((uint8_t)text_acc[s])) s++;
                size_t e = strlen(text_acc);
                while (e > s && isspace((uint8_t)text_acc[e-1])) e--;
                if (e > s) {
                    if (select_opts_len > 0 && select_opts_len < sizeof(select_opts) - 1)
                        select_opts[select_opts_len++] = '\n';
                    size_t len = e - s;
                    if (select_opts_len + len < sizeof(select_opts) - 1) {
                        memcpy(select_opts + select_opts_len, text_acc + s, len);
                        select_opts_len += len;
                        select_opts[select_opts_len] = '\0';
                    }
                }
                acc_len = 0;
            } else if (!closing) {
                acc_len = 0;  // reset for new option text
            }
        }
        // Button
        else if (tag_is(name, name_len, "button")) {
            if (!closing) {
                flush_acc();  // flush prior text before button
            } else {
                // Accumulated text is button label
                if (acc_len > 0) {
                    text_acc[acc_len] = '\0';
                    decode_entities(text_acc);
                    size_t s = 0;
                    while (s < acc_len && isspace((uint8_t)text_acc[s])) s++;
                    size_t e = strlen(text_acc);
                    while (e > s && isspace((uint8_t)text_acc[e-1])) e--;
                    if (e > s && r->count < MAX_ELEMENTS) {
                        PageElement *el = &r->elems[r->count];
                        memset(el, 0, sizeof(*el));
                        el->type = ELEM_SUBMIT;
                        el->text = pool_add(r, text_acc + s, e - s);
                        r->count++;
                    }
                    acc_len = 0;
                }
            }
        }
        // Lists
        else if (tag_is(name, name_len, "ul") ||
                 tag_is(name, name_len, "ol")) {
            flush_acc();
            if (!closing) {
                if (list_depth < MAX_LIST_DEPTH) {
                    list_stack[list_depth].ordered = tag_is(name, name_len, "ol");
                    list_stack[list_depth].counter = 0;
                    list_depth++;
                }
            } else {
                if (list_depth > 0) list_depth--;
            }
        }
        // List item
        else if (tag_is(name, name_len, "li")) {
            flush_acc();
            if (!closing && list_depth > 0) {
                auto &ls = list_stack[list_depth - 1];
                ls.counter++;
                // Prepend indent + bullet/number to accumulator
                for (int d = 1; d < list_depth; d++) {
                    if (acc_len + 2 < sizeof(text_acc)) {
                        text_acc[acc_len++] = ' ';
                        text_acc[acc_len++] = ' ';
                    }
                }
                if (ls.ordered) {
                    char num[12];
                    int n = snprintf(num, sizeof(num), "%d. ", ls.counter);
                    if (acc_len + n < sizeof(text_acc)) {
                        memcpy(text_acc + acc_len, num, n);
                        acc_len += n;
                    }
                } else {
                    if (acc_len + 2 < sizeof(text_acc)) {
                        text_acc[acc_len++] = '*';
                        text_acc[acc_len++] = ' ';
                    }
                }
                acc_prefix_len = acc_len;
            }
        }
        // Table — track nesting for grey text
        else if (tag_is(name, name_len, "table")) {
            flush_acc();
            in_italic = !closing;
        }
        // Table header — bold within table
        else if (tag_is(name, name_len, "th")) {
            flush_acc();
            if (!closing) in_bold = true;
            else in_bold = false;
        }
        // Horizontal rule
        else if (tag_is(name, name_len, "hr") && !closing) {
            flush_acc();
            add_elem(r, ELEM_HR, "", 0, NULL, 0);
        }
        // Block elements — paragraph break
        else if (tag_is(name, name_len, "div") ||
                 tag_is(name, name_len, "td")  ||
                 tag_is(name, name_len, "tr")) {
            flush_acc();
            if (tag_is(name, name_len, "div")) {
                if (!closing) {
                    uint32_t c = parse_style_color(tag_str);
                    if (c) { cur_color = c; color_depth++; }
                    uint8_t fs = parse_style_font_size(tag_str);
                    if (fs) { cur_font_size = fs; font_size_depth++; }
                } else {
                    if (color_depth > 0) {
                        color_depth--;
                        if (color_depth == 0) { cur_color = 0; acc_color = 0; }
                    }
                    if (font_size_depth > 0) {
                        font_size_depth--;
                        if (font_size_depth == 0) { cur_font_size = 0; acc_font_size = 0; }
                    }
                }
            }
        }

        p = tag_end + 1;
    }
    flush_acc();
    Serial.printf("HTML parse: %d elements, %zu bytes pool\n",
                  r->count, r->pool_used);
}
