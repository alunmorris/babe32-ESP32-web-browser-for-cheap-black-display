/*
 * LVGL UI task pinned to core 1
 * Written by Alun Morris and Claude Code
 *
 * 060326 LVGL UI task pinned to core 1
 */
#pragma once
#include <stdint.h>

void ui_task_start();
// Called from UI task context to build the root screen
void ui_build_root();

// LVGL mutex — use from other tasks before touching LVGL objects
bool lvgl_lock(uint32_t timeout_ms);
void lvgl_unlock();
