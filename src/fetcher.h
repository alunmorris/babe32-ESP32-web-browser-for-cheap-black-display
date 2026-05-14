// 060326 HTTPS page fetcher into PSRAM buffer
// 120326 Added fetch_disconnect() for persistent connection cleanup
#pragma once
#include <stddef.h>

#define FETCH_BUF_SIZE (2 * 1024 * 1024)  // 2MB

// fetch_page: fetches `url` via Brightdata residential proxy (GET).
// Returns raw HTML body byte count, negative HTTP status on error, or -1 on connection failure.
// Buffer is null-terminated. Caller must not free buf — static PSRAM allocation.
int fetch_page(const char *url, char **buf_out);

// fetch_page_post: same as fetch_page but sends POST with URL-encoded body.
int fetch_page_post(const char *url, const char *post_body, char **buf_out);

// Close persistent TLS connection and clear cached DNS.
// Call on WiFi disconnect or before deep sleep.
void fetch_disconnect();

// Cancel in-progress fetch. Stops reading and disconnects.
void fetch_cancel();

// Check if fetch was cancelled.
bool fetch_cancelled();

// Get pointer to the raw fetch buffer (may contain partial data during fetch).
char *fetch_buf_ptr();
