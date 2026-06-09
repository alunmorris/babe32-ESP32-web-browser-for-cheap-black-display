/*
 * PSRAM-backed navigation history
 * Written by Alun Morris and Claude Code
 *
 * 060326 PSRAM-backed navigation history
 */
#include "history.h"
#include <Arduino.h>
#include <string.h>

static char (*urls)[HISTORY_URL_LEN] = nullptr;
static int head    = -1;
static int current = -1;

void history_init() {
    urls = (char (*)[HISTORY_URL_LEN])
        heap_caps_malloc(HISTORY_MAX * HISTORY_URL_LEN, MALLOC_CAP_SPIRAM);
    assert(urls);
    head = current = -1;
}

void history_push(const char *url) {
    head = current; // clear forward
    if (head < HISTORY_MAX - 1) {
        head++;
    } else {
        memmove(urls[0], urls[1], (HISTORY_MAX - 1) * HISTORY_URL_LEN);
    }
    strncpy(urls[head], url, HISTORY_URL_LEN - 1);
    urls[head][HISTORY_URL_LEN - 1] = '\0';
    current = head;
}

const char *history_current() {
    if (current < 0) return nullptr;
    return urls[current];
}

bool history_can_back()    { return current > 0; }
bool history_can_forward() { return current < head; }

const char *history_back() {
    if (!history_can_back()) return nullptr;
    current--;
    return urls[current];
}

const char *history_forward() {
    if (!history_can_forward()) return nullptr;
    current++;
    return urls[current];
}
