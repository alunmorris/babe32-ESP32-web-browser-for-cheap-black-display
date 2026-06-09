/*
 * Swipe gesture detection
 * Written by Alun Morris and Claude Code
 *
 * 060326 Swipe gesture detection
 */
#pragma once
#include <lvgl.h>

typedef void (*swipe_left_cb_t)();
typedef void (*swipe_right_cb_t)();

void gesture_attach(lv_obj_t *obj,
                    swipe_left_cb_t  on_swipe_left,
                    swipe_right_cb_t on_swipe_right);
