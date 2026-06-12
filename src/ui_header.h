/*
 * Header bar: back/forward buttons + URL textarea
 * Written by Alun Morris and Claude Code
 *
 * 060326 Header bar: back/forward buttons + URL textarea
 * 120326 Add keyboard support for URL entry
 */
#pragma once
#include <lvgl.h>

typedef void (*navigate_cb_t)(const char *url);
typedef void (*back_cb_t)();
typedef void (*forward_cb_t)();
typedef void (*home_cb_t)();

lv_obj_t *header_create(lv_obj_t *parent,
                         navigate_cb_t on_navigate,
                         back_cb_t on_back,
                         forward_cb_t on_forward,
                         home_cb_t on_home);

void header_set_url(const char *url);
void header_set_back_enabled(bool en);
void header_set_forward_enabled(bool en);
void header_set_loading(bool loading);

// Get the URL textarea object (for keyboard association)
lv_obj_t *header_get_url_ta();

// Get current text from URL bar
const char *header_get_url_text();

// Show/hide the header bar
void header_set_visible(bool visible);

// Update WiFi signal icon colour (rssi in dBm; pass -100 when disconnected)
void header_set_wifi_rssi(int rssi);

// Move WiFi icon to foreground — call once after all screen widgets are created
void header_wifi_foreground();

// Height of the header bar in pixels — used to position content below it
#define HEADER_HEIGHT 30
