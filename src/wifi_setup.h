/*
 * Self-contained WiFi setup: AP scan, password entry, NVS credential store
 * Written by Alun Morris and Claude Code
 *
 * 190326 Self-contained WiFi setup: AP scan, password entry, NVS credential store
 */
#pragma once
#include <lvgl.h>

typedef void (*wifi_setup_done_cb_t)();

void wifi_setup_init(lv_obj_t *content, wifi_setup_done_cb_t on_done,
                     void (*field_focus_cb)(lv_obj_t *ta));
void wifi_setup_show();          // begin: show scan screen; call inside lvgl_lock
void wifi_setup_tick();          // drive state machine; call outside lvgl_lock
bool wifi_setup_active();        // true while setup flow is running
lv_obj_t *wifi_setup_get_pass_ta();  // for kb_event_cb integration
void wifi_setup_kb_submit();     // called when Enter pressed on password textarea
