/*
 * Fetch via Brightdata proxy, return raw HTML body, follow redirects
 * Written by Alun Morris and Claude Code
 *
 * 060326 Fetch via Brightdata proxy, return raw HTML body, follow redirects
 * 120326 Persistent TLS connection, keep-alive, batched write, DNS cache
 * 190326 PHP proxy (webmashing.com) as primary, Brightdata as fallback
 * 130526 Redirect mbedTLS allocs to PSRAM — internal heap too fragmented for 40KB SSL buffers
 */
#include "fetcher.h"
#include "image_fetch.h"
#include "url_utils.h"
#include "dbglog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>

extern volatile int g_fetch_kb;

static volatile bool s_cancel = false;
static char *fetch_buf = nullptr;

// mbedTLS allocates ~40KB of SSL buffers. Internal SRAM is too fragmented to satisfy
// this as a contiguous block. Redirect to PSRAM; ESP32-S3 AES DMA handles PSRAM data.
static void *tls_psram_calloc(size_t n, size_t size) {
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    return p;
}
static void tls_psram_free(void *ptr) { heap_caps_free(ptr); }

static void ensure_buf() {
    if (!fetch_buf) {
        mbedtls_platform_set_calloc_free(tls_psram_calloc, tls_psram_free);
        fetch_buf = (char *)heap_caps_malloc(FETCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    }
}

// Own PHP proxy (primary for GET)
static const char *PHP_HOST = "webmashing.com";
static const int   PHP_PORT = 443;
static const char *PHP_PATH = "/babe32proxy.php";

// Brightdata residential proxy (fallback for GET)
static const char *PROXY_HOST = "brd.superproxy.io";
static const int   PROXY_PORT = 33335;
static const char *PROXY_AUTH =
    "Basic YnJkLWN1c3RvbWVyLWhsX2E2Zjg5MTU3LXpvbmUtcmVzaWRlbnRpYWxfcHJveHkxOmRhcWNtZnhqdTkzNA==";
static IPAddress   s_proxy_ip;
static bool        s_dns_cached = false;

// Single persistent TLS connection — shared between PHP and Brightdata to save RAM
enum ConnType { CONN_NONE, CONN_PHP, CONN_BRIGHTDATA };
static WiFiClientSecure *s_client   = nullptr;
static ConnType           s_conn_type = CONN_NONE;

// Cookie jar: store cookies for multiple hosts
#define COOKIE_MAX_HOSTS 4
struct CookieEntry {
    char host[128];
    char data[256];  // "name=value; name2=value2"
};
static CookieEntry s_cookies[COOKIE_MAX_HOSTS] = {};

static CookieEntry *cookie_find(const char *host) {
    for (int i = 0; i < COOKIE_MAX_HOSTS; i++)
        if (s_cookies[i].host[0] && strcmp(s_cookies[i].host, host) == 0)
            return &s_cookies[i];
    return nullptr;
}

static CookieEntry *cookie_alloc(const char *host) {
    // Find existing
    CookieEntry *e = cookie_find(host);
    if (e) return e;
    // Find empty slot
    for (int i = 0; i < COOKIE_MAX_HOSTS; i++)
        if (!s_cookies[i].host[0]) {
            strncpy(s_cookies[i].host, host, sizeof(s_cookies[i].host) - 1);
            s_cookies[i].data[0] = '\0';
            return &s_cookies[i];
        }
    // Evict oldest (slot 0), shift down
    memmove(&s_cookies[0], &s_cookies[1], sizeof(CookieEntry) * (COOKIE_MAX_HOSTS - 1));
    CookieEntry *last = &s_cookies[COOKIE_MAX_HOSTS - 1];
    strncpy(last->host, host, sizeof(last->host) - 1);
    last->data[0] = '\0';
    return last;
}

// Close and delete s_client unconditionally
static void close_client() {
    if (s_client) {
        s_client->stop();
        delete s_client;
        s_client = nullptr;
    }
    s_conn_type = CONN_NONE;
}

// Connect s_client to PHP proxy (closes Brightdata connection if open).
// Connects by hostname so TLS SNI is sent correctly (required by shared hosting).
static bool ensure_php_connected() {
    if (s_client && s_conn_type == CONN_PHP && s_client->connected()) {
        dbg("Reusing PHP proxy TLS");
        return true;
    }
    close_client();
    image_fetch_disconnect();

    s_client = new WiFiClientSecure();
    s_client->setInsecure();
    s_client->setTimeout(15);
    dbg("PHP TLS connect %s:%d...", PHP_HOST, PHP_PORT);
    uint32_t t0 = millis();
    if (!s_client->connect(PHP_HOST, PHP_PORT)) {
        dbg("PHP proxy FAIL after %lums", millis() - t0);
        delete s_client;
        s_client = nullptr;
        return false;
    }
    s_conn_type = CONN_PHP;
    dbg("PHP TLS connected in %lums", millis() - t0);
    return true;
}

// Connect s_client to Brightdata (closes PHP connection if open)
static bool ensure_connected() {
    if (s_client && s_conn_type == CONN_BRIGHTDATA && s_client->connected()) {
        dbg("Reusing TLS connection");
        return true;
    }
    close_client();
    image_fetch_disconnect();

    if (!s_dns_cached) {
        dbg("DNS resolve %s...", PROXY_HOST);
        uint32_t t0 = millis();
        if (!WiFiGenericClass::hostByName(PROXY_HOST, s_proxy_ip)) {
            dbg("DNS FAIL after %lums", millis() - t0);
            return false;
        }
        s_dns_cached = true;
        dbg("DNS: %s -> %s in %lums", PROXY_HOST, s_proxy_ip.toString().c_str(), millis() - t0);
    }

    s_client = new WiFiClientSecure();
    s_client->setInsecure();
    s_client->setTimeout(10);
    dbg("TLS connect %s:%d...", s_proxy_ip.toString().c_str(), PROXY_PORT);
    uint32_t t0 = millis();
    if (!s_client->connect(s_proxy_ip, PROXY_PORT)) {
        dbg("Proxy FAIL after %lums", millis() - t0);
        delete s_client;
        s_client = nullptr;
        return false;
    }
    s_conn_type = CONN_BRIGHTDATA;
    dbg("TLS connected in %lums", millis() - t0);
    return true;
}

void fetch_disconnect() {
    close_client();
    s_dns_cached = false;
    s_proxy_ip = IPAddress();
    dbg("Fetch connection closed");
}

void fetch_cancel() {
    s_cancel = true;
    dbg("Fetch cancel requested");
}

bool fetch_cancelled() {
    return s_cancel;
}

char *fetch_buf_ptr() {
    return fetch_buf;
}

// Read exactly `len` bytes into fetch_buf starting at `offset`.
// Returns bytes actually read.
static size_t read_exact(size_t offset, size_t len) {
    size_t got = 0;
    size_t cap = FETCH_BUF_SIZE - 1;
    if (offset + len > cap) len = cap - offset;
    uint32_t idle = millis();
    while (got < len && !s_cancel && (s_client->connected() || s_client->available())) {
        int av = s_client->available();
        if (av > 0) {
            size_t want = min((size_t)av, len - got);
            got += s_client->readBytes(fetch_buf + offset + got, want);
            g_fetch_kb = (int)((offset + got) / 1024);
            idle = millis();
        } else if (millis() - idle > 10000) {
            dbg("Read timeout at %zu/%zu", got, len);
            break;
        } else {
            delay(1);
        }
    }
    return got;
}

// Read until connection closes or buffer full. For fallback / error cases.
static size_t read_until_close(size_t offset) {
    size_t total = 0;
    size_t cap = FETCH_BUF_SIZE - 1 - offset;
    uint32_t idle = millis();
    while (!s_cancel && (s_client->connected() || s_client->available()) && total < cap) {
        int av = s_client->available();
        if (av > 0) {
            total += s_client->readBytes(fetch_buf + offset + total, min((size_t)av, cap - total));
            g_fetch_kb = (int)((offset + total) / 1024);
            idle = millis();
        } else if (millis() - idle > 10000) {
            dbg("Read timeout");
            break;
        } else {
            delay(1);
        }
    }
    return total;
}

// Read chunked transfer encoding body into fetch_buf starting at body_start.
// Returns total decoded body bytes, or -1 on error.
static int read_chunked_body(size_t body_start) {
    size_t out = body_start;
    size_t cap = FETCH_BUF_SIZE - 1;

    while (!s_cancel) {
        // Read chunk size line
        char line[16];
        size_t li = 0;
        uint32_t idle = millis();
        while (li < sizeof(line) - 1 && !s_cancel) {
            if (s_client->available()) {
                char c;
                s_client->readBytes((uint8_t *)&c, 1);
                idle = millis();
                if (c == '\n') break;
                if (c != '\r') line[li++] = c;
            } else if (!s_client->connected()) {
                break;
            } else if (millis() - idle > 10000) {
                dbg("Chunked: timeout reading size");
                return (int)(out - body_start);  // return partial on timeout
            } else {
                delay(1);
            }
        }
        if (s_cancel) break;  // return partial data
        line[li] = '\0';

        unsigned long chunk_size = strtoul(line, nullptr, 16);
        if (chunk_size == 0) break;  // final chunk

        if (out + chunk_size > cap) {
            dbg("Chunked: body exceeds buffer");
            chunk_size = cap - out;
        }

        size_t got = read_exact(out, chunk_size);
        out += got;
        if (got < chunk_size) {
            dbg("Chunked: short read %zu/%lu", got, chunk_size);
            break;
        }

        // Read trailing \r\n after chunk data
        char crlf[2];
        s_client->readBytes((uint8_t *)crlf, 2);

        g_fetch_kb = (int)(out / 1024);
        if (out >= cap) break;
    }

    return (int)(out - body_start);
}

// Extract host and path from URL
static void parse_url(const char *url, char *host, size_t host_len,
                      const char **path_out) {
    const char *hs = strstr(url, "://");
    hs = hs ? hs + 3 : url;
    const char *sl = strchr(hs, '/');
    size_t hl = sl ? (size_t)(sl - hs) : strlen(hs);
    if (hl >= host_len) hl = host_len - 1;
    memcpy(host, hs, hl);
    host[hl] = '\0';
    *path_out = sl ? sl : "/";
}

static int do_request(const char *url, const char *post_body, size_t *total_out) {
    *total_out = 0;

    // s_client must already be connected (proxy or direct)
    if (!s_client || !s_client->connected()) return 0;

    char host[128] = {};
    const char *path;
    parse_url(url, host, sizeof(host), &path);

    // Build Cookie header if we have cookies for this host
    char cookie_hdr[600] = "";
    CookieEntry *ce = cookie_find(host);
    if (ce && ce->data[0]) {
        snprintf(cookie_hdr, sizeof(cookie_hdr), "Cookie: %s\r\n", ce->data);
    }

    // Batch entire HTTP request into single write
    char req[2048];
    int req_len;
    if (post_body) {
        // Direct connection: use path only, no proxy auth
        req_len = snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
            "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.8\r\n"
            "Accept-Language: en-US,en;q=0.5\r\n"
            "Accept-Encoding: identity\r\n"
            "%s"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n"
            "%s",
            path, host, cookie_hdr, strlen(post_body), post_body);
    } else {
        // Proxy connection: use full URL, include proxy auth
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Proxy-Authorization: %s\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
            "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.8\r\n"
            "Accept-Language: en-US,en;q=0.5\r\n"
            "Accept-Encoding: identity\r\n"
            "%s"
            "Connection: keep-alive\r\n\r\n",
            url, host, PROXY_AUTH, cookie_hdr);
    }

    if (req_len >= (int)sizeof(req)) {
        dbg("Request too large for buffer");
        return 0;
    }

    s_client->write((const uint8_t *)req, req_len);
    dbg("%s sent (%d bytes, 1 write)", post_body ? "POST" : "GET", req_len);
    uint32_t t0 = millis();

    // Read response headers into fetch_buf
    // We read byte-by-byte until we find \r\n\r\n, then handle body separately
    size_t hdr_len = 0;
    size_t cap = FETCH_BUF_SIZE - 1;
    uint32_t idle = millis();
    bool found_end = false;
    while (hdr_len < cap && hdr_len < 8192 && !s_cancel) {
        if (s_client->available()) {
            fetch_buf[hdr_len] = s_client->read();
            hdr_len++;
            idle = millis();
            if (hdr_len >= 4 &&
                fetch_buf[hdr_len - 4] == '\r' && fetch_buf[hdr_len - 3] == '\n' &&
                fetch_buf[hdr_len - 2] == '\r' && fetch_buf[hdr_len - 1] == '\n') {
                found_end = true;
                break;
            }
        } else if (!s_client->connected()) {
            break;
        } else if (millis() - idle > 10000) {
            dbg("Header read timeout");
            break;
        } else {
            delay(1);
        }
    }
    fetch_buf[hdr_len] = '\0';

    if (!found_end) {
        dbg("No header terminator found");
        if (!s_cancel) s_client->stop();
        return 0;
    }

    // Parse status
    int status = 0;
    if (hdr_len > 12) sscanf(fetch_buf, "HTTP/%*d.%*d %d", &status);
    dbg("HTTP %d (headers %zu bytes, %lums)", status, hdr_len, millis() - t0);

    // Parse Set-Cookie headers and store cookies for this host
    {
        char req_host[128] = {};
        const char *dummy;
        parse_url(url, req_host, sizeof(req_host), &dummy);
        char *pos = fetch_buf;
        while ((pos = strcasestr(pos, "\r\nSet-Cookie:")) != nullptr) {
            pos += 13;
            while (*pos == ' ') pos++;
            char *end = pos;
            while (*end && *end != ';' && *end != '\r' && *end != '\n') end++;
            size_t nv_len = (size_t)(end - pos);
            if (nv_len > 0 && nv_len < 200) {
                char nv[200];
                memcpy(nv, pos, nv_len);
                nv[nv_len] = '\0';
                CookieEntry *ce = cookie_alloc(req_host);
                // Append or replace cookie in this entry
                size_t cur_len = strlen(ce->data);
                if (cur_len > 0 && cur_len + 2 + nv_len < sizeof(ce->data)) {
                    strcat(ce->data, "; ");
                    strncat(ce->data, nv, sizeof(ce->data) - strlen(ce->data) - 1);
                } else if (cur_len == 0 && nv_len < sizeof(ce->data)) {
                    strncpy(ce->data, nv, sizeof(ce->data) - 1);
                }
                dbg("Cookie [%s]: %s", req_host, nv);
            }
        }
    }

    // Parse Content-Length and Transfer-Encoding from headers
    long content_length = -1;
    bool chunked = false;
    {
        char *cl = strcasestr(fetch_buf, "\r\nContent-Length:");
        if (cl) {
            cl += 17;
            while (*cl == ' ') cl++;
            content_length = strtol(cl, nullptr, 10);
        }
        char *te = strcasestr(fetch_buf, "\r\nTransfer-Encoding:");
        if (te) {
            te += 20;
            while (*te == ' ') te++;
            if (strncasecmp(te, "chunked", 7) == 0) chunked = true;
        }
    }

    // For redirects, we need headers in fetch_buf — read body but we only need the headers
    if (status >= 301 && status <= 308) {
        // Drain body so connection stays clean for reuse
        if (content_length > 0) {
            // Read and discard body
            size_t to_drain = (size_t)content_length;
            size_t drained = 0;
            uint8_t discard[512];
            uint32_t drain_idle = millis();
            while (drained < to_drain && (s_client->connected() || s_client->available())) {
                int av = s_client->available();
                if (av > 0) {
                    size_t want = min((size_t)av, min(sizeof(discard), to_drain - drained));
                    drained += s_client->readBytes(discard, want);
                    drain_idle = millis();
                } else if (millis() - drain_idle > 5000) {
                    break;
                } else {
                    delay(1);
                }
            }
        } else if (chunked) {
            // Read and discard chunked body
            // Simple approach: read chunks until 0-length
            while (s_client->connected() || s_client->available()) {
                char line[16];
                size_t li = 0;
                uint32_t ci = millis();
                while (li < sizeof(line) - 1) {
                    if (s_client->available()) {
                        char c = s_client->read();
                        ci = millis();
                        if (c == '\n') break;
                        if (c != '\r') line[li++] = c;
                    } else if (!s_client->connected() || millis() - ci > 5000) {
                        goto drain_done;
                    } else {
                        delay(1);
                    }
                }
                line[li] = '\0';
                unsigned long cs = strtoul(line, nullptr, 16);
                if (cs == 0) break;
                uint8_t discard[512];
                size_t left = cs;
                while (left > 0 && (s_client->connected() || s_client->available())) {
                    int av = s_client->available();
                    if (av > 0) {
                        size_t want = min((size_t)av, min(sizeof(discard), left));
                        left -= s_client->readBytes(discard, want);
                    } else {
                        delay(1);
                    }
                }
                // Consume trailing \r\n
                char crlf[2];
                s_client->readBytes((uint8_t *)crlf, 2);
            }
            drain_done:;
        }
        *total_out = hdr_len;
        return status;
    }

    // For 200 responses, read body into fetch_buf after headers
    if (status == 200) {
        size_t body_bytes = 0;
        if (content_length >= 0) {
            dbg("Reading %ld bytes (Content-Length)", content_length);
            body_bytes = read_exact(hdr_len, (size_t)content_length);
        } else if (chunked) {
            dbg("Reading chunked body");
            int cb = read_chunked_body(hdr_len);
            if (cb < 0) cb = 0;
            body_bytes = (size_t)cb;
        } else {
            // No Content-Length, not chunked — read until close (connection won't be reusable)
            dbg("No Content-Length/chunked — reading until close");
            body_bytes = read_until_close(hdr_len);
        }
        *total_out = hdr_len + body_bytes;
        fetch_buf[*total_out] = '\0';
        dbg("Body: %zu bytes in %lums", body_bytes, millis() - t0);
        return status;
    }

    // Other status codes — read body if Content-Length known, else drain
    if (content_length > 0) {
        size_t body_bytes = read_exact(hdr_len, (size_t)content_length);
        *total_out = hdr_len + body_bytes;
    } else {
        *total_out = hdr_len;
    }
    fetch_buf[*total_out] = '\0';
    return status;
}

// Connect directly to target host (for POST — proxy doesn't support it)
static bool ensure_direct(const char *url) {
    char host[128] = {};
    const char *path;
    parse_url(url, host, sizeof(host), &path);

    close_client();

    s_client = new WiFiClientSecure();
    s_client->setInsecure();
    s_client->setTimeout(10);
    dbg("Direct TLS connect %s:443...", host);
    uint32_t t0 = millis();
    if (!s_client->connect(host, 443)) {
        dbg("Direct connect FAIL after %lums", millis() - t0);
        delete s_client;
        s_client = nullptr;
        return false;
    }
    s_conn_type = CONN_NONE;  // direct — not a named proxy
    dbg("Direct TLS connected in %lums", millis() - t0);
    return true;
}

// Send request via own PHP proxy. s_client must already be connected to PHP host.
// Returns HTTP status, sets *total_out.
static int do_php_request(const char *url, size_t *total_out) {
    *total_out = 0;
    if (!s_client || !s_client->connected()) return 0;

    char encoded[768];
    url_encode(url, encoded, sizeof(encoded));

    char req[1024];
    int req_len = snprintf(req, sizeof(req),
        "GET %s?url=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
        "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.8\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: keep-alive\r\n\r\n",
        PHP_PATH, encoded, PHP_HOST);

    if (req_len >= (int)sizeof(req)) { dbg("PHP req too large"); return 0; }

    s_client->write((const uint8_t *)req, req_len);
    dbg("PHP GET sent (%d bytes)", req_len);
    uint32_t t0 = millis();

    // Read response headers
    size_t hdr_len = 0;
    size_t cap = FETCH_BUF_SIZE - 1;
    uint32_t idle = millis();
    bool found_end = false;
    while (hdr_len < cap && hdr_len < 8192 && !s_cancel) {
        if (s_client->available()) {
            fetch_buf[hdr_len] = s_client->read();
            hdr_len++;
            idle = millis();
            if (hdr_len >= 4 &&
                fetch_buf[hdr_len-4] == '\r' && fetch_buf[hdr_len-3] == '\n' &&
                fetch_buf[hdr_len-2] == '\r' && fetch_buf[hdr_len-1] == '\n') {
                found_end = true;
                break;
            }
        } else if (!s_client->connected()) {
            break;
        } else if (millis() - idle > 10000) {
            dbg("PHP header timeout");
            break;
        } else {
            delay(1);
        }
    }
    fetch_buf[hdr_len] = '\0';

    int status = 0;
    if (found_end && hdr_len > 12)
        sscanf(fetch_buf, "HTTP/%*d.%*d %d", &status);
    dbg("PHP HTTP %d (%zu hdr, %lums)", status, hdr_len, millis() - t0);

    if (!found_end) {
        return 0;
    }

    long content_length = -1;
    bool chunked = false;
    {
        char *cl = strcasestr(fetch_buf, "\r\nContent-Length:");
        if (cl) { cl += 17; while (*cl == ' ') cl++; content_length = strtol(cl, nullptr, 10); }
        char *te = strcasestr(fetch_buf, "\r\nTransfer-Encoding:");
        if (te) { te += 20; while (*te == ' ') te++; if (strncasecmp(te, "chunked", 7) == 0) chunked = true; }
    }

    if (status == 200) {
        size_t body_bytes = 0;
        if (content_length >= 0) {
            body_bytes = read_exact(hdr_len, (size_t)content_length);
        } else if (chunked) {
            int cb = read_chunked_body(hdr_len);
            body_bytes = (cb >= 0) ? (size_t)cb : 0;
        } else {
            body_bytes = read_until_close(hdr_len);
        }
        *total_out = hdr_len + body_bytes;
        fetch_buf[*total_out] = '\0';
        dbg("PHP body: %zu bytes in %lums", body_bytes, millis() - t0);
    } else {
        *total_out = hdr_len;
        fetch_buf[hdr_len] = '\0';
    }

    return status;
}

static int fetch_impl(const char *url, const char *post_body, char **buf_out) {
    s_cancel = false;
    dbg("fetch: alloc buf");
    ensure_buf();
    if (!fetch_buf) {
        dbg("PSRAM alloc FAILED!");
        *buf_out = nullptr;
        return -1;
    }
    dbg("PSRAM buf OK (%dKB)", FETCH_BUF_SIZE / 1024);
    *buf_out = fetch_buf;

    char cur_url[512];
    strncpy(cur_url, url, sizeof(cur_url) - 1);
    cur_url[sizeof(cur_url) - 1] = '\0';

    // Only send POST body on the first request; redirects become GET
    const char *cur_body = post_body;

    // Try PHP proxy first for GET requests (follows redirects server-side)
    if (!cur_body) {
        if (ensure_php_connected()) {
            size_t total = 0;
            int status = do_php_request(cur_url, &total);
            if (!s_cancel && status == 200 && total > 0) {
                char *body = strstr(fetch_buf, "\r\n\r\n");
                if (body) {
                    body += 4;
                    size_t body_len = total - (size_t)(body - fetch_buf);
                    memmove(fetch_buf, body, body_len);
                    fetch_buf[body_len] = '\0';
                    dbg("PHP proxy OK: %zu bytes", body_len);
                    return (int)body_len;
                }
            }
            dbg("PHP proxy failed (status=%d), falling back to Brightdata", status);
            // close_client() will be called inside ensure_connected() on next attempt
        }
    }

    for (int attempt = 0; attempt < 5; attempt++) {
        if (s_cancel) break;
        dbg("Attempt %d: %.40s", attempt + 1, cur_url);

        // Connect: direct for POST, proxy for GET
        if (cur_body) {
            if (!ensure_direct(cur_url)) return -1;
        } else {
            if (!ensure_connected()) return -1;
        }

        size_t total = 0;
        int status = do_request(cur_url, cur_body, &total);

        // Clean up direct connection after use
        if (cur_body && s_client) {
            s_client->stop();
            delete s_client;
            s_client = nullptr;
        }

        if (s_cancel) {
            // Cancelled — return whatever body we have
            dbg("CANCEL: status=%d total=%zu", status, total);
            char *body = strstr(fetch_buf, "\r\n\r\n");
            dbg("CANCEL: header_end=%s", body ? "found" : "NOT found");
            if (body) {
                body += 4;
                size_t hdr_sz = (size_t)(body - fetch_buf);
                dbg("CANCEL: hdr_sz=%zu total=%zu body_avail=%zu", hdr_sz, total, total > hdr_sz ? total - hdr_sz : 0);
                if (total > hdr_sz) {
                    size_t body_len = total - hdr_sz;
                    memmove(fetch_buf, body, body_len);
                    fetch_buf[body_len] = '\0';
                    dbg("CANCEL: returning %zu bytes partial body", body_len);
                    return (int)body_len;
                }
            }
            // Clean up connection
            if (s_client) { s_client->stop(); delete s_client; s_client = nullptr; }
            dbg("CANCEL: no body data, returning -1");
            return -1;
        }

        if (status == 0 || total == 0) {
            if (attempt == 0) {
                dbg("Reconnecting after failure...");
                continue;  // retry via loop
            }
            dbg("No response");
            return -1;
        }

        if (status == 200) {
            char *body = strstr(fetch_buf, "\r\n\r\n");
            if (!body) { dbg("No header separator"); return -1; }
            body += 4;
            size_t body_len = total - (size_t)(body - fetch_buf);
            memmove(fetch_buf, body, body_len);
            fetch_buf[body_len] = '\0';
            dbg("HTML body: %zu bytes", body_len);
            return (int)body_len;
        }

        if (status >= 301 && status <= 308) {
            char *loc = strcasestr(fetch_buf, "\r\nLocation:");
            if (!loc) { dbg("Redirect no Location"); return -1; }
            loc += 11;
            while (*loc == ' ') loc++;
            char *end = strstr(loc, "\r\n");
            if (!end) { dbg("Malformed Location"); return -1; }
            size_t len = (size_t)(end - loc);
            char loc_str[512];
            if (len >= sizeof(loc_str)) len = sizeof(loc_str) - 1;
            strncpy(loc_str, loc, len);
            loc_str[len] = '\0';
            // Resolve relative redirects against current URL
            char resolved[512];
            if (!url_resolve(cur_url, loc_str, resolved, sizeof(resolved))) {
                dbg("Cannot resolve redirect URL");
                return -1;
            }
            strncpy(cur_url, resolved, sizeof(cur_url) - 1);
            cur_url[sizeof(cur_url) - 1] = '\0';
            cur_body = nullptr;  // redirects become GET
            dbg("Redirect -> %.40s", cur_url);
            continue;
        }

        if (status == 402) {
            // Brightdata rate-limit on this exit node — pause briefly then retry
            // (lwIP DNS cache means we usually reconnect to the same superproxy IP,
            //  but Brightdata rotates the residential exit node on each new connection)
            close_client();
            s_dns_cached = false;
            s_proxy_ip = IPAddress();
            dbg("402 rate limit, pausing before retry (attempt %d)...", attempt + 1);
            delay(1000);
            continue;
        }

        dbg("HTTP error %d", status);
        return -status;
    }

    dbg("Too many redirects");
    return -1;
}

int fetch_page(const char *url, char **buf_out) {
    return fetch_impl(url, nullptr, buf_out);
}

int fetch_page_post(const char *url, const char *post_body, char **buf_out) {
    return fetch_impl(url, post_body, buf_out);
}
