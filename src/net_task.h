/*
 * Network task: async fetch + parse on core 0
 * Written by Alun Morris and Claude Code
 *
 * 060326 Network task: async fetch + parse on core 0
 */
#pragma once
#include "html_parser.h"

typedef void (*page_ready_cb_t)(ParseResult *result, const char *url);

// Start the network task. page_ready_cb called from net task when page loaded.
// WARNING: runs on core 0 — use lvgl_lock() before touching LVGL.
void net_task_start(page_ready_cb_t on_page_ready);

// Request a page load (non-blocking, posts to queue)
void net_task_load(const char *url);

// Request a POST page load (non-blocking)
void net_task_load_post(const char *url, const char *post_body);

// Cancel in-progress fetch
void net_task_cancel();
