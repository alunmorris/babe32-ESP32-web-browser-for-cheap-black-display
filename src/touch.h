/*
 * GT911 touch via bb_captouch, registered as LVGL indev
 * Written by Alun Morris and Claude Code
 *
 * 060326 GT911 touch via bb_captouch, registered as LVGL indev
 */
#pragma once
#include <lvgl.h>

void touch_init();
void touch_lvgl_init();
// Returns true (and clears the flag) if touch occurred since last call.
bool touch_was_active();
// Suppress the next touch press until finger lifts (use after wake-from-sleep).
void touch_suppress_next();
