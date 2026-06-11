/*
 * Image fetcher via resize proxy
 * Written by Alun Morris and Claude Code
 *
 * 160326 Image fetcher via resize proxy
 * 100626 Stop closing page TLS connection on image fetch — both stay persistent
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
    uint8_t *r = fetch_resized(img_url, out_len,
                         IMAGE_FULL_W, IMAGE_FULL_H, 80, 512 * 1024);
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
