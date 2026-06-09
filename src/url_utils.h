/*
 * URL resolution: relative -> absolute
 * Written by Alun Morris and Claude Code
 *
 * 060326 URL resolution: relative -> absolute
 */
#pragma once
#include <stddef.h>

// Resolve href relative to base_url. Result written into out (max out_len).
// Returns out on success, nullptr on failure.
char *url_resolve(const char *base_url, const char *href,
                  char *out, size_t out_len);

// Update the base URL from a newly navigated URL (extracts scheme+host+dir)
void url_set_base(const char *navigated_url);
const char *url_get_base();

// URL-encode src into out buffer. Returns bytes written (excluding null).
size_t url_encode(const char *src, char *out, size_t out_len);
