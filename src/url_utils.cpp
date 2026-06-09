/*
 * URL resolution logic
 * Written by Alun Morris and Claude Code
 *
 * 060326 URL resolution logic
 */
#include "url_utils.h"
#include <string.h>
#include <stdio.h>

static char base_url[512] = "";

void url_set_base(const char *navigated_url) {
    strncpy(base_url, navigated_url, sizeof(base_url) - 1);
    base_url[sizeof(base_url) - 1] = '\0';
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) { base_url[0] = '\0'; return; }
    char *last_slash = strrchr(scheme_end + 3, '/');
    if (last_slash) *(last_slash + 1) = '\0';
}

const char *url_get_base() { return base_url; }

char *url_resolve(const char *base, const char *href, char *out, size_t out_len) {
    if (!href || !*href) return nullptr;

    // Already absolute
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        strncpy(out, href, out_len - 1);
        out[out_len - 1] = '\0';
        return out;
    }

    // Protocol-relative: //host/path
    if (strncmp(href, "//", 2) == 0) {
        const char *colon = strchr(base, ':');
        if (!colon) return nullptr;
        size_t scheme_len = (size_t)(colon - base + 1);
        strncpy(out, base, scheme_len);
        out[scheme_len] = '\0';
        strncat(out, href, out_len - scheme_len - 1);
        return out;
    }

    // Root-relative: /path
    if (href[0] == '/') {
        const char *scheme_end = strstr(base, "://");
        if (!scheme_end) return nullptr;
        const char *host_end = strchr(scheme_end + 3, '/');
        size_t prefix_len = host_end ? (size_t)(host_end - base) : strlen(base);
        strncpy(out, base, prefix_len);
        out[prefix_len] = '\0';
        strncat(out, href, out_len - prefix_len - 1);
        return out;
    }

    // Fragment or query only — not useful for navigation
    if (href[0] == '#' || href[0] == '?') return nullptr;

    // Relative: append to base dir
    snprintf(out, out_len, "%s%s", base, href);
    return out;
}

size_t url_encode(const char *src, char *out, size_t out_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (; *src && w < out_len - 3; src++) {
        char c = *src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[w++] = c;
        } else if (c == ' ') {
            out[w++] = '+';
        } else {
            out[w++] = '%';
            out[w++] = hex[(uint8_t)c >> 4];
            out[w++] = hex[(uint8_t)c & 0x0F];
        }
    }
    out[w] = '\0';
    return w;
}
