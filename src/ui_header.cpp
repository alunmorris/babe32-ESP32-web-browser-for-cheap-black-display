/*
 * Header bar implementation
 * Written by Alun Morris and Claude Code
 *
 * 060326 Header bar implementation
 * 120326 Remove forward button; enlarge back button hit area
 * 120326 URL bar is now a textarea for keyboard input
 * 190326 WiFi signal strength indicator below Home button
 */
#include "ui_header.h"
#include <Arduino.h>
#include <WiFi.h>

static lv_obj_t *s_hdr       = nullptr;
static lv_obj_t *s_back_btn  = nullptr;
static lv_obj_t *s_url_ta    = nullptr;
static lv_obj_t *s_home_btn  = nullptr;
static lv_obj_t *s_wifi_icon = nullptr;

static navigate_cb_t s_nav_cb  = nullptr;
static back_cb_t     s_back_cb = nullptr;
static home_cb_t     s_home_cb = nullptr;

static void back_btn_cb(lv_event_t *e) {
    if (s_back_cb) s_back_cb();
}

static void home_btn_cb(lv_event_t *e) {
    if (s_home_cb) s_home_cb();
}

lv_obj_t *header_create(lv_obj_t *parent,
                          navigate_cb_t on_navigate,
                          back_cb_t on_back,
                          forward_cb_t on_forward,
                          home_cb_t on_home) {
    s_nav_cb  = on_navigate;
    s_back_cb = on_back;
    s_home_cb = on_home;

    s_hdr = lv_obj_create(parent);
    lv_obj_t *hdr = s_hdr;
    lv_obj_set_size(hdr, LV_HOR_RES, 30);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0F3460), 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_set_style_shadow_width(hdr, 0, 0);
    lv_obj_set_style_outline_width(hdr, 0, 0);
    lv_obj_set_scrollbar_mode(hdr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Back button — 30x26 hit area
    s_back_btn = lv_label_create(hdr);
    lv_obj_set_size(s_back_btn, 30, 26);
    lv_obj_set_pos(s_back_btn, 0, 0);
    lv_label_set_text(s_back_btn, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_back_btn, &lv_font_montserrat_18, 0);
    lv_obj_set_style_pad_top(s_back_btn, 3, 0);
    lv_obj_set_style_pad_left(s_back_btn, 4, 0);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    // URL textarea (single line) — leave room for home button on RHS
    s_url_ta = lv_textarea_create(hdr);
    lv_obj_set_size(s_url_ta, LV_HOR_RES - 38 - 30, 26);
    lv_obj_set_pos(s_url_ta, 34, 0);
    lv_textarea_set_one_line(s_url_ta, true);
    lv_textarea_set_text(s_url_ta, "");
    lv_obj_set_style_bg_color(s_url_ta, lv_color_hex(0x16213E), 0);
    lv_obj_set_style_text_color(s_url_ta, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_url_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_width(s_url_ta, 0, 0);
    lv_obj_set_style_radius(s_url_ta, 0, 0);
    lv_obj_set_style_pad_top(s_url_ta, 4, 0);
    lv_obj_set_style_pad_bottom(s_url_ta, 2, 0);
    lv_obj_set_style_pad_left(s_url_ta, 4, 0);
    lv_obj_set_style_shadow_width(s_url_ta, 0, 0);
    lv_obj_set_style_outline_width(s_url_ta, 0, 0);
    lv_obj_set_style_anim_time(s_url_ta, 0, 0);
    // Cursor: bright block, no border/radius (renders as noise on this display)
    lv_obj_set_style_bg_color(s_url_ta, lv_color_hex(0x4FC3F7), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_url_ta, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_url_ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_radius(s_url_ta, 0, LV_PART_CURSOR);
    // No cursor blink — reduces redraws
    lv_obj_set_style_anim_time(s_url_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    // Home button — RHS of URL bar
    s_home_btn = lv_label_create(hdr);
    lv_obj_set_size(s_home_btn, 30, 26);
    lv_obj_set_pos(s_home_btn, LV_HOR_RES - 32, 0);
    lv_label_set_text(s_home_btn, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(s_home_btn, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_home_btn, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(s_home_btn, 4, 0);
    lv_obj_set_style_text_align(s_home_btn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_home_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home_btn, home_btn_cb, LV_EVENT_CLICKED, NULL);

    // WiFi icon — floating on screen, below Home button
    s_wifi_icon = lv_label_create(parent);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_size(s_wifi_icon, 30, 16);
    lv_obj_set_pos(s_wifi_icon, LV_HOR_RES - 31, 30);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(s_wifi_icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_FLOATING);

    return hdr;
}

void header_set_url(const char *url) {
    if (s_url_ta && url) lv_textarea_set_text(s_url_ta, url);
}

const char *header_get_url_text() {
    if (s_url_ta) return lv_textarea_get_text(s_url_ta);
    return "";
}

lv_obj_t *header_get_url_ta() {
    return s_url_ta;
}

void header_set_back_enabled(bool en) {
    if (!s_back_btn) return;
    if (en) {
        lv_obj_clear_state(s_back_btn, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xE0E0E0), 0);
    } else {
        lv_obj_add_state(s_back_btn, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0x555555), 0);
    }
}

void header_set_forward_enabled(bool en) {
    // Forward button removed
}

void header_set_loading(bool loading) {
    header_set_back_enabled(!loading);
    if (s_url_ta) {
        if (loading) lv_obj_add_state(s_url_ta, LV_STATE_DISABLED);
        else         lv_obj_clear_state(s_url_ta, LV_STATE_DISABLED);
    }
}

void header_set_visible(bool visible) {
    if (!s_hdr) return;
    if (visible) lv_obj_clear_flag(s_hdr, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_hdr, LV_OBJ_FLAG_HIDDEN);
    // WiFi icon stays visible regardless of header state
}

void header_wifi_foreground() {
    if (s_wifi_icon) lv_obj_move_foreground(s_wifi_icon);
}

void header_set_wifi_rssi(int rssi) {
    if (!s_wifi_icon) return;
    lv_color_t color;
    if (rssi <= -80)       color = lv_color_hex(0xFF4444);  // red   — very weak / nil
    else if (rssi <= -70)  color = lv_color_hex(0xFF8800);  // orange — weak
    else if (rssi <= -60)  color = lv_color_hex(0xFFCC00);  // yellow — good
    else                   color = lv_color_hex(0x00C853);  // green  — strong
    lv_obj_set_style_text_color(s_wifi_icon, color, 0);
}
