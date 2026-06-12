/*
 * HTTPS page fetcher into PSRAM buffer
 * Written by Alun Morris and Claude Code
 *
 * 060326 HTTPS page fetcher into PSRAM buffer
 * 120326 Added fetch_disconnect() for persistent connection cleanup
 */
#pragma once
#include <stddef.h>

#define FETCH_BUF_SIZE (2 * 1024 * 1024)  // 2MB

// HTTP status codes
#define HTTP_OK              200
#define HTTP_3XX_START       300   // exclusive upper bound for 2xx checks
#define HTTP_REDIRECT_MIN    301
#define HTTP_REDIRECT_MAX    308
#define HTTP_4XX_START       400   // exclusive upper bound for 2xx+3xx checks
#define HTTP_FORBIDDEN       403
#define HTTP_TOO_MANY_REQS   429
#define HTTP_UNAVAILABLE     503

// Network I/O timeouts (ms)
#define NET_IO_TIMEOUT_MS    10000
#define NET_DRAIN_TIMEOUT_MS  5000

// Header parsing limits
#define NET_HEADER_MAX_BYTES  8192
#define HTTP_STATUS_LINE_MIN    12  // min header bytes needed to parse status

// Standard HTTPS port
#define HTTPS_PORT             443

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
