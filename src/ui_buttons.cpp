/*
 * Flat button helper
 * Written by Alun Morris and Claude Code
 *
 * 180326 Flat button helper
 */
#include "ui_buttons.h"

lv_obj_t *create_flat_btn(lv_obj_t *parent, const char *text,
                           int w, int h,
                           lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_label_create(parent);
    lv_label_set_text(btn, text);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0F3460), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(btn, 8, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}
