/*
 * Image fetcher via resize proxy
 * Written by Alun Morris and Claude Code
 *
 * 160326 Image fetcher via resize proxy
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define IMAGE_THUMB_W 120
#define IMAGE_THUMB_H 80
#define IMAGE_FULL_W  480
#define IMAGE_FULL_H  310
#define IMAGE_PROXY "https://webmashing.com/image-resize.php"

// JPEG quality (0-100) for each fetch mode
#define IMAGE_THUMB_QUALITY   50
#define IMAGE_FULL_QUALITY    80

// Maximum response bytes per fetch
#define IMAGE_THUMB_MAX_BYTES (128 * 1024)
#define IMAGE_FULL_MAX_BYTES  (512 * 1024)

// Body read timeout — longer than header timeout to allow large image transfers
#define IMG_BODY_TIMEOUT_MS   15000

// Fetch a resized image via proxy. Returns PSRAM-allocated buffer with
// raw image file data (PNG/JPEG), or nullptr on failure.
// *out_len receives the data length. Caller must free with heap_caps_free().
uint8_t *image_fetch(const char *img_url, size_t *out_len);

// Fetch full-size version (480x310, quality 80)
uint8_t *image_fetch_full(const char *img_url, size_t *out_len);

// Close image fetcher's TLS connection to free internal RAM
void image_fetch_disconnect();
