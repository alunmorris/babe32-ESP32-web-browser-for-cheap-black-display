/*
 * Swipe gesture: >50px horizontal, <30px vertical drift
 * Written by Alun Morris and Claude Code
 *
 * 060326 Swipe gesture: >50px horizontal, <30px vertical drift
 */
#include "gesture.h"
#include <Arduino.h>

#define SWIPE_MIN_DIST  50
#define SWIPE_MAX_DRIFT 30

static swipe_left_cb_t  s_left_cb  = nullptr;
static swipe_right_cb_t s_right_cb = nullptr;
static lv_coord_t s_press_x = 0, s_press_y = 0;
static bool s_pressing = false;

static void gesture_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        s_press_x  = pt.x;
        s_press_y  = pt.y;
        s_pressing = true;
    } else if (code == LV_EVENT_RELEASED && s_pressing) {
        s_pressing = false;
        lv_coord_t dx = pt.x - s_press_x;
        lv_coord_t dy = pt.y - s_press_y;
        if (abs(dx) >= SWIPE_MIN_DIST && abs(dy) <= SWIPE_MAX_DRIFT) {
            if (dx < 0 && s_left_cb)  s_left_cb();
            if (dx > 0 && s_right_cb) s_right_cb();
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        s_pressing = false;
    }
}

void gesture_attach(lv_obj_t *obj,
                    swipe_left_cb_t on_swipe_left,
                    swipe_right_cb_t on_swipe_right) {
    s_left_cb  = on_swipe_left;
    s_right_cb = on_swipe_right;
    lv_obj_add_event_cb(obj, gesture_event_cb, LV_EVENT_ALL, NULL);
}
