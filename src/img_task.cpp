/*
 * Background image fetch task
 * Written by Alun Morris and Claude Code
 *
 * 180326 Background image fetch task
 */
#include "img_task.h"
#include "image_fetch.h"
#include "dbglog.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static QueueHandle_t s_req_queue  = nullptr;
static QueueHandle_t s_res_queue  = nullptr;

static void img_task_fn(void *arg) {
    for (;;) {
        ImgRequest req;
        if (xQueueReceive(s_req_queue, &req, portMAX_DELAY) == pdTRUE) {
            size_t len = 0;
            uint8_t *data;
            if (req.full_size)
                data = image_fetch_full(req.url, &len);
            else
                data = image_fetch(req.url, &len);

            ImgResult res;
            res.index = req.index;
            res.data = data;
            res.len = len;
            res.full_size = req.full_size;
            // Post result — if queue full, free data to avoid leak
            if (xQueueSend(s_res_queue, &res, 0) != pdTRUE) {
                if (data) heap_caps_free(data);
                dbg("img_task: result queue full, dropped");
            }
        }
    }
}

void img_task_start() {
    s_req_queue = xQueueCreate(16, sizeof(ImgRequest));
    s_res_queue = xQueueCreate(4, sizeof(ImgResult));
    xTaskCreatePinnedToCore(img_task_fn, "img_task", 16384, nullptr, 3, nullptr, 0);
}

void img_task_post(const ImgRequest *req) {
    if (s_req_queue)
        xQueueSend(s_req_queue, req, 0);  // non-blocking
}

bool img_task_poll(ImgResult *out) {
    if (!s_res_queue) return false;
    return xQueueReceive(s_res_queue, out, 0) == pdTRUE;
}

void img_task_flush() {
    if (!s_req_queue) return;
    ImgRequest discard;
    while (xQueueReceive(s_req_queue, &discard, 0) == pdTRUE) {}
    // Also drain results and free any allocated data
    ImgResult res;
    while (xQueueReceive(s_res_queue, &res, 0) == pdTRUE) {
        if (res.data) heap_caps_free(res.data);
    }
}
