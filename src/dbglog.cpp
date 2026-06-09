/*
 * On-screen debug log
 * Written by Alun Morris and Claude Code
 *
 * 120326 On-screen debug log
 */
#include "dbglog.h"
#include <Arduino.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define LOG_BUF_SIZE 2048

static char s_buf[LOG_BUF_SIZE];
static size_t s_used = 0;
static SemaphoreHandle_t s_mutex = nullptr;

void dbglog_init() {
    s_mutex = xSemaphoreCreateMutex();
    s_buf[0] = '\0';
}

void dbg(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    // Add newline if not present
    if (line[n - 1] != '\n') {
        if (n < (int)sizeof(line) - 1) { line[n++] = '\n'; line[n] = '\0'; }
    }

    Serial.print(line);

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    // If buffer would overflow, discard first half
    if (s_used + n >= LOG_BUF_SIZE - 1) {
        size_t half = s_used / 2;
        // Find newline after half point
        char *nl = strchr(s_buf + half, '\n');
        if (nl) {
            size_t skip = (size_t)(nl + 1 - s_buf);
            memmove(s_buf, s_buf + skip, s_used - skip);
            s_used -= skip;
        } else {
            s_used = 0;
        }
    }

    memcpy(s_buf + s_used, line, n);
    s_used += n;
    s_buf[s_used] = '\0';

    if (s_mutex) xSemaphoreGive(s_mutex);
}

// Return last 14 lines only
static char s_view[800];

const char *dbglog_text() {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Count newlines from end to find start of last 14 lines
    int lines = 0;
    const char *p = s_buf + s_used;
    while (p > s_buf && lines < 14) {
        p--;
        if (*p == '\n') lines++;
    }
    if (p > s_buf) p++; // skip past the newline

    size_t len = s_used - (size_t)(p - s_buf);
    if (len >= sizeof(s_view)) len = sizeof(s_view) - 1;
    memcpy(s_view, p, len);
    s_view[len] = '\0';

    if (s_mutex) xSemaphoreGive(s_mutex);
    return s_view;
}
