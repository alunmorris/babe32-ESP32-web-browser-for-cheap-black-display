/*
 * Flat button helper for consistent UI styling
 * Written by Alun Morris and Claude Code
 *
 * 180326 Flat button helper for consistent UI styling
 */
#pragma once
#include <lvgl.h>

// Create a flat-styled clickable label button.
// Applies: bg_opa cover, bg_color 0x0F3460, text_color 0x4FC3F7,
// text_align center, pad_top 8, radius 0, shadow 0, border 0, clickable.
lv_obj_t *create_flat_btn(lv_obj_t *parent, const char *text,
                           int w, int h,
                           lv_event_cb_t cb, void *user_data);
