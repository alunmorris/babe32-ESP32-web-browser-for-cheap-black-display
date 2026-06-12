/*
 * Image fetcher via resize proxy
 * Written by Alun Morris and Claude Code
 *
 * 160326 Image fetcher via resize proxy
 * 100626 Stop closing page TLS connection on image fetch — both stay persistent
 * 100626 If resized image < screen, transform Wikimedia URL and re-fetch via proxy
 */
#include "image_fetch.h"
#include "url_utils.h"
#include "dbglog.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>

static WiFiClientSecure *s_img_client = nullptr;
static SemaphoreHandle_t s_img_mutex = nullptr;

// Parse image pixel dimensions from a raw JPEG or PNG header.
static bool get_image_dims(const uint8_t *data, size_t len, int *w, int *h) {
    if (!data || len < 24) return false;
    // PNG: 8-byte magic, then IHDR chunk: 4-byte len, "IHDR", 4-byte w, 4-byte h
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        *w = (data[16]<<24)|(data[17]<<16)|(data[18]<<8)|data[19];
        *h = (data[20]<<24)|(data[21]<<16)|(data[22]<<8)|data[23];
        return *w > 0 && *h > 0;
    }
    // JPEG: scan for SOF marker (FF C0-C3, C5-C7, C9-CB)
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 8 < len) {
        while (i < len && data[i] != 0xFF) i++;
        if (i + 1 >= len) break;
        uint8_t m = data[i+1];
        if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB)) {
            *h = (data[i+5]<<8)|data[i+6];
            *w = (data[i+7]<<8)|data[i+8];
            return *w > 0 && *h > 0;
        }
        if (i + 3 >= len) break;
        uint16_t seg = (data[i+2]<<8)|data[i+3];
        if (seg < 2) break;
        i += 2 + seg;
    }
    return false;
}

// Transform a Wikimedia thumbnail URL to the full-resolution URL.
// e.g. .../thumb/a/ab/File.jpg/250px-File.jpg  ->  .../a/ab/File.jpg
// Returns true if the URL matched the pattern.
static bool wikimedia_thumb_to_full(const char *thumb_url, char *out, size_t out_cap) {
    if (!strstr(thumb_url, "upload.wikimedia.org")) return false;
    const char *thumb = strstr(thumb_url, "/thumb/");
    if (!thumb) return false;
    size_t pre = (size_t)(thumb - thumb_url);
    const char *after = thumb + 6;  // skip "/thumb"
    const char *last_slash = strrchr(after, '/');
    if (!last_slash) return false;
    size_t rest = (size_t)(last_slash - after);
    if (pre + rest + 1 > out_cap) return false;
    memcpy(out, thumb_url, pre);
    memcpy(out + pre, after, rest);
    out[pre + rest] = '\0';
    return true;
}

static void img_mutex_init() {
    if (!s_img_mutex) s_img_mutex = xSemaphoreCreateMutex();
}

static bool ensure_connected() {
    // SSL buffers live in PSRAM, so this connection can coexist with the
    // page fetcher's — reuse it across images to avoid TLS handshakes
    if (s_img_client && s_img_client->connected()) return true;

    // Destroy old client to fully release SSL buffers
    if (s_img_client) {
        s_img_client->stop();
        delete s_img_client;
        s_img_client = nullptr;
    }

    s_img_client = new WiFiClientSecure();
    s_img_client->setInsecure();
    s_img_client->setTimeout(10);

    dbg("img_fetch: connecting to webmashing.com...");
    if (!s_img_client->connect("webmashing.com", 443)) {
        dbg("img_fetch: connect failed");
        delete s_img_client;
        s_img_client = nullptr;
        return false;
    }
    return true;
}

static uint8_t *fetch_resized(const char *img_url, size_t *out_len,
                              int w, int h, int quality, size_t max_cap) {
    *out_len = 0;
    if (!img_url || !img_url[0]) return nullptr;

    // Build proxy URL path
    char encoded_url[1024];
    url_encode(img_url, encoded_url, sizeof(encoded_url));

    char path[1280];
    snprintf(path, sizeof(path),
             "/image-resize.php?url=%s&ip_width=%d&ip_height=%d&ip_quality=%d",
             encoded_url, w, h, quality);

    if (!ensure_connected()) return nullptr;

    // Send GET request with keep-alive
    char req[1536];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: webmashing.com\r\n"
        "Connection: keep-alive\r\n"
        "\r\n", path);
    s_img_client->write((const uint8_t *)req, req_len);

    // Read response headers
    char hdr_buf[1024];
    size_t hdr_len = 0;
    uint32_t idle = millis();
    bool found_end = false;
    while (hdr_len < sizeof(hdr_buf) - 1 && millis() - idle < 10000) {
        if (s_img_client->available()) {
            hdr_buf[hdr_len++] = s_img_client->read();
            idle = millis();
            if (hdr_len >= 4 &&
                hdr_buf[hdr_len-4] == '\r' && hdr_buf[hdr_len-3] == '\n' &&
                hdr_buf[hdr_len-2] == '\r' && hdr_buf[hdr_len-1] == '\n') {
                found_end = true;
                break;
            }
        } else if (!s_img_client->connected()) {
            break;
        } else {
            delay(1);
        }
    }
    hdr_buf[hdr_len] = '\0';

    if (!found_end) {
        dbg("img_fetch: no header end");
        s_img_client->stop();
        return nullptr;
    }

    // Parse status
    int status = 0;
    if (hdr_len > 12) sscanf(hdr_buf, "HTTP/%*d.%*d %d", &status);
    if (status != 200) {
        dbg("img_fetch: HTTP %d", status);
        s_img_client->stop();
        return nullptr;
    }

    // Parse Content-Length
    long content_length = -1;
    char *cl = strcasestr(hdr_buf, "\r\nContent-Length:");
    if (cl) {
        cl += 17;
        while (*cl == ' ') cl++;
        content_length = strtol(cl, nullptr, 10);
    }

    // Check for chunked transfer encoding
    bool chunked = (strcasestr(hdr_buf, "\r\nTransfer-Encoding: chunked") != nullptr);

    // Determine read strategy
    size_t max_size;
    bool read_until_close = false;
    if (content_length > 0) {
        max_size = (size_t)content_length;
        if (max_size > max_cap) max_size = max_cap;
    } else {
        // No Content-Length — read until connection closes
        max_size = max_cap;
        read_until_close = true;
        dbg("img_fetch: no Content-Length, reading until close (chunked=%d)", chunked);
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        dbg("img_fetch: malloc %zu failed", max_size);
        s_img_client->stop();
        return nullptr;
    }

    // Read body
    size_t total = 0;
    idle = millis();
    if (chunked) {
        // Chunked: read chunk-size\r\n...data...\r\n until 0\r\n
        while (total < max_size && millis() - idle < 15000) {
            // Read chunk size line
            char line[16];
            size_t li = 0;
            while (li < sizeof(line) - 1 && millis() - idle < 15000) {
                if (s_img_client->available()) {
                    char c = s_img_client->read();
                    idle = millis();
                    if (c == '\n') break;
                    if (c != '\r') line[li++] = c;
                } else if (!s_img_client->connected()) {
                    goto done;
                } else {
                    delay(1);
                }
            }
            line[li] = '\0';
            size_t chunk_sz = strtoul(line, nullptr, 16);
            if (chunk_sz == 0) break;  // final chunk

            // Read chunk data
            size_t chunk_read = 0;
            while (chunk_read < chunk_sz && total < max_size && millis() - idle < 15000) {
                int av = s_img_client->available();
                if (av > 0) {
                    size_t want = chunk_sz - chunk_read;
                    if (total + want > max_size) want = max_size - total;
                    if ((size_t)av < want) want = (size_t)av;
                    size_t got = s_img_client->readBytes(buf + total, want);
                    total += got;
                    chunk_read += got;
                    idle = millis();
                } else if (!s_img_client->connected()) {
                    goto done;
                } else {
                    delay(1);
                }
            }
            // Skip trailing \r\n after chunk data
            int skip = 0;
            while (skip < 2 && millis() - idle < 15000) {
                if (s_img_client->available()) {
                    s_img_client->read();
                    skip++;
                    idle = millis();
                } else if (!s_img_client->connected()) {
                    break;
                } else {
                    delay(1);
                }
            }
        }
    } else {
        while (total < max_size && millis() - idle < 15000) {
            int av = s_img_client->available();
            if (av > 0) {
                size_t want = (size_t)av;
                if (total + want > max_size) want = max_size - total;
                total += s_img_client->readBytes(buf + total, want);
                idle = millis();
            } else if (!s_img_client->connected()) {
                break;
            } else {
                delay(1);
            }
        }
    }
    done:

    // If we read without Content-Length, connection is unreliable — close it
    if (read_until_close) {
        s_img_client->stop();
        delete s_img_client;
        s_img_client = nullptr;
    }

    if (total == 0) {
        dbg("img_fetch: 0 bytes");
        heap_caps_free(buf);
        return nullptr;
    }

    dbg("img_fetch: %zu bytes", total);
    *out_len = total;
    return buf;
}

uint8_t *image_fetch(const char *img_url, size_t *out_len) {
    img_mutex_init();
    xSemaphoreTake(s_img_mutex, portMAX_DELAY);
    uint8_t *r = fetch_resized(img_url, out_len,
                         IMAGE_THUMB_W, IMAGE_THUMB_H, 50, 128 * 1024);
    xSemaphoreGive(s_img_mutex);
    return r;
}

uint8_t *image_fetch_full(const char *img_url, size_t *out_len) {
    img_mutex_init();
    xSemaphoreTake(s_img_mutex, portMAX_DELAY);
    uint8_t *r = fetch_resized(img_url, out_len, IMAGE_FULL_W, IMAGE_FULL_H, 80, 512 * 1024);

    // If the proxy returned something smaller than the screen in both dimensions,
    // the img_url is itself a thumbnail. For Wikimedia, derive the full-resolution
    // URL and re-fetch through the proxy at screen size.
    if (r && *out_len > 0) {
        int img_w = 0, img_h = 0;
        if (get_image_dims(r, *out_len, &img_w, &img_h) &&
            img_w > 0 && img_h > 0 &&
            img_w < IMAGE_FULL_W && img_h < IMAGE_FULL_H) {
            char full_url[768];
            if (wikimedia_thumb_to_full(img_url, full_url, sizeof(full_url))) {
                dbg("img_full: %dx%d < screen, fetching full: %.50s", img_w, img_h, full_url);
                size_t full_len = 0;
                uint8_t *full_r = fetch_resized(full_url, &full_len, IMAGE_FULL_W, IMAGE_FULL_H, 80, 512 * 1024);
                if (full_r && full_len > 0) {
                    int full_w = 0, full_h = 0;
                    bool bigger = get_image_dims(full_r, full_len, &full_w, &full_h) &&
                                  (full_w > img_w || full_h > img_h);
                    if (bigger) {
                        heap_caps_free(r);
                        r = full_r;
                        *out_len = full_len;
                    } else {
                        dbg("img_full: full %dx%d not bigger, keeping thumbnail", full_w, full_h);
                        heap_caps_free(full_r);
                        // *out_len already holds thumbnail size from first fetch
                    }
                } else {
                    if (full_r) heap_caps_free(full_r);
                    dbg("img_full: full fetch failed, keeping thumbnail");
                    // *out_len already holds thumbnail size from first fetch
                }
            } else {
                dbg("img_full: source %dx%d is genuinely small", img_w, img_h);
            }
        }
    }

    xSemaphoreGive(s_img_mutex);
    return r;
}

void image_fetch_disconnect() {
    img_mutex_init();
    xSemaphoreTake(s_img_mutex, portMAX_DELAY);
    if (s_img_client) {
        s_img_client->stop();
        delete s_img_client;
        s_img_client = nullptr;
    }
    xSemaphoreGive(s_img_mutex);
}
