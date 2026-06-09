/*
 * Background image fetch task — fetches images on core 0 via queues
 * Written by Alun Morris and Claude Code
 *
 * 180326 Background image fetch task — fetches images on core 0 via queues
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct ImgRequest {
    int index;              // slot index, or -1 for full-size
    char url[512];          // copied URL (safe if page navigates during fetch)
    bool full_size;
};

struct ImgResult {
    int index;
    uint8_t *data;          // PSRAM allocation, receiver frees. nullptr on failure.
    size_t len;
    bool full_size;
};

// Start the background image fetch task (call once at startup).
void img_task_start();

// Post a fetch request (non-blocking, drops if queue full).
void img_task_post(const ImgRequest *req);

// Poll for a completed result (non-blocking).
// Returns true and fills *out if a result is available.
bool img_task_poll(ImgResult *out);

// Flush pending requests (call on page navigation to cancel queued fetches).
void img_task_flush();
