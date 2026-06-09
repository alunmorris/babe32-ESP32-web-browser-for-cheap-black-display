/*
 * LVGL page renderer
 * Written by Alun Morris and Claude Code
 *
 * 060326 LVGL page renderer
 * 130326 Add form widget rendering (input, select, submit)
 * 160326 Add inline image rendering via proxy
 */
#include "page_renderer.h"
#include "image_fetch.h"
#include "url_utils.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

static bool s_inverted = false;

#define COLOUR_BG      lv_color_hex(s_inverted ? 0xF0F0F0 : 0x1A1A2E)
#define COLOUR_TEXT    lv_color_hex(s_inverted ? 0x1A1A1A : 0xE0E0E0)
#define COLOUR_HEADING lv_color_hex(s_inverted ? 0x000000 : 0xFFFFFF)
#define COLOUR_LINK    lv_color_hex(s_inverted ? 0x0066CC : 0x4FC3F7)
#define COLOUR_LINK_BG lv_color_hex(s_inverted ? 0xE0E8F0 : 0x16213E)
#define COLOUR_FIELD   lv_color_hex(s_inverted ? 0xFFFFFF : 0x16213E)
#define COLOUR_BORDER  lv_color_hex(s_inverted ? 0xCCCCCC : 0x0F3460)
#define COLOUR_BTN_BG  lv_color_hex(s_inverted ? 0xE0E0E0 : 0x0F3460)
#define COLOUR_BTN_TXT lv_color_hex(s_inverted ? 0x0066CC : 0x4FC3F7)
#define COLOUR_ITALIC  lv_color_hex(s_inverted ? 0x555555 : 0xA0A0A0)

void page_set_inverted(bool inv) { s_inverted = inv; }
bool page_is_inverted() { return s_inverted; }

LV_FONT_DECLARE(lv_font_montserrat_bold_14);
LV_FONT_DECLARE(lv_font_dejavu_mono_14);

static const lv_font_t *font_for_size(uint8_t sz) {
    if (sz >= 22) return &lv_font_montserrat_24;
    if (sz >= 19) return &lv_font_montserrat_20;
    if (sz >= 17) return &lv_font_montserrat_18;
    if (sz >= 15) return &lv_font_montserrat_16;
    return &lv_font_montserrat_14;
}

static link_tap_cb_t        s_link_cb       = nullptr;
static form_submit_cb_t     s_submit_cb     = nullptr;
static form_field_focus_cb_t s_focus_cb     = nullptr;
static const ParseResult   *s_cur_result    = nullptr;
static lv_obj_t            *s_cur_container = nullptr;

// Image data cache — persists decoded image data while page is displayed
#define MAX_PAGE_IMAGES 16
struct ImgSlot {
    lv_img_dsc_t dsc;
    uint8_t *data;      // PSRAM allocation, freed on page_clear
    lv_obj_t *widget;   // lv_img widget to update after fetch
    const char *url;    // src URL (points into ParseResult text_pool)
};
static ImgSlot s_img_slots[MAX_PAGE_IMAGES];
static int     s_img_count = 0;
static int     s_img_fetch_idx = 0;

// Full-size image overlay (async)
static lv_obj_t    *s_overlay = nullptr;
static lv_img_dsc_t s_full_dsc;
static uint8_t     *s_full_data = nullptr;
static const char  *s_full_pending_url = nullptr;  // set by click, cleared by main loop

static void overlay_close_cb(lv_event_t *e) {
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = nullptr;
    }
    if (s_full_data) {
        heap_caps_free(s_full_data);
        s_full_data = nullptr;
    }
    s_full_pending_url = nullptr;
}

static void create_overlay_base() {
    if (s_overlay) lv_obj_del(s_overlay);
    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_close_cb, LV_EVENT_CLICKED, NULL);
}

static void img_click_cb(lv_event_t *e) {
    const char *url = (const char *)lv_event_get_user_data(e);
    if (!url || s_full_pending_url) return;  // ignore if already fetching

    s_full_pending_url = url;

    // Show loading overlay immediately
    create_overlay_base();
    lv_obj_t *lbl = lv_label_create(s_overlay);
    lv_label_set_text(lbl, "Loading...");
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, 0);
    lv_obj_center(lbl);
}

static void free_img_slots() {
    for (int i = 0; i < s_img_count; i++) {
        if (s_img_slots[i].data) {
            heap_caps_free(s_img_slots[i].data);
            s_img_slots[i].data = nullptr;
        }
    }
    s_img_count = 0;
}

static void link_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_link_cb) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev && lv_indev_get_scroll_obj(indev) != NULL) return;
        const char *url = (const char *)lv_event_get_user_data(e);
        if (url) s_link_cb(url);
    }
}

static void form_ta_click_cb(lv_event_t *e) {
    if (s_focus_cb) s_focus_cb(lv_event_get_target(e));
}

// Collect form data from container's textareas/dropdowns + hidden fields
// If form_id >= 0, only include hidden fields belonging to that form.
void collect_form_data(lv_obj_t *container, const ParseResult *result,
                               char *out, size_t out_len, int form_id) {
    size_t w = 0;
    // Hidden fields first (scoped to form_id)
    for (int i = 0; i < result->count && w < out_len - 1; i++) {
        const PageElement *e = &result->elems[i];
        if (e->type != ELEM_HIDDEN || !e->name) continue;
        if (form_id >= 0 && e->form_id != (uint8_t)form_id) continue;
        if (w > 0) out[w++] = '&';
        w += url_encode(e->name, out + w, out_len - w);
        out[w++] = '=';
        if (e->value) w += url_encode(e->value, out + w, out_len - w);
    }
    // Walk children for textareas and dropdowns
    uint32_t child_cnt = lv_obj_get_child_cnt(container);
    for (uint32_t ci = 0; ci < child_cnt && w < out_len - 1; ci++) {
        lv_obj_t *child = lv_obj_get_child(container, ci);
        const char *fname = (const char *)lv_obj_get_user_data(child);
        if (!fname || fname[0] == '\0') continue;

        const char *val = nullptr;
        char dd_buf[128];
        if (lv_obj_check_type(child, &lv_textarea_class)) {
            val = lv_textarea_get_text(child);
        } else if (lv_obj_check_type(child, &lv_dropdown_class)) {
            lv_dropdown_get_selected_str(child, dd_buf, sizeof(dd_buf));
            val = dd_buf;
        }
        if (!val) continue;

        if (w > 0) out[w++] = '&';
        w += url_encode(fname, out + w, out_len - w);
        out[w++] = '=';
        w += url_encode(val, out + w, out_len - w);
    }
    out[w] = '\0';
}

static void form_submit_click_cb(lv_event_t *e) {
    if (!s_submit_cb || !s_cur_result || !s_cur_container) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_scroll_obj(indev) != NULL) return;

    const PageElement *btn_elem = (const PageElement *)lv_event_get_user_data(e);
    int fid = btn_elem ? (int)btn_elem->form_id : -1;

    static char form_data[4096];
    collect_form_data(s_cur_container, s_cur_result, form_data, sizeof(form_data), fid);

    // Append clicked submit button's name=value if it has a name
    if (btn_elem && btn_elem->name) {
        size_t w = strlen(form_data);
        if (w > 0 && w < sizeof(form_data) - 1) form_data[w++] = '&';
        w += url_encode(btn_elem->name, form_data + w, sizeof(form_data) - w);
        if (w < sizeof(form_data) - 1) form_data[w++] = '=';
        if (btn_elem->value)
            w += url_encode(btn_elem->value, form_data + w, sizeof(form_data) - w);
        form_data[w] = '\0';
    }

    // Use per-form action/method if available
    const char *action = s_cur_result->form_action;
    bool is_post = s_cur_result->form_is_post;
    if (fid >= 0 && fid < s_cur_result->form_count) {
        action = s_cur_result->forms[fid].action;
        is_post = s_cur_result->forms[fid].is_post;
    }
    s_submit_cb(action, is_post, form_data);
}

void page_clear(lv_obj_t *container) {
    free_img_slots();
    s_img_fetch_idx = 0;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = nullptr; }
    if (s_full_data) { heap_caps_free(s_full_data); s_full_data = nullptr; }
    lv_obj_clean(container);
}

void page_show_spinner(lv_obj_t *container) {
    page_clear(container);
    lv_obj_set_style_bg_color(container, COLOUR_BG, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl = lv_label_create(container);
    lv_label_set_text(lbl, "Loading...");
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, 0);
}

void page_render(lv_obj_t *container, const ParseResult *result,
                 link_tap_cb_t on_link_tap,
                 form_submit_cb_t on_form_submit,
                 form_field_focus_cb_t on_field_focus,
                 bool show_links,
                 bool show_images) {
    page_clear(container);
    s_link_cb       = on_link_tap;
    s_submit_cb     = on_form_submit;
    s_focus_cb      = on_field_focus;
    s_cur_result    = result;
    s_cur_container = container;

    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(container, COLOUR_BG, 0);
    lv_obj_set_style_pad_all(container, 8, 0);
    lv_obj_set_style_pad_gap(container, 4, 0);

    // Helper: append text to the last child label (for merging around hidden links)
    auto append_to_prev = [&](const char *txt, bool add_space) -> bool {
        uint32_t cc = lv_obj_get_child_cnt(container);
        if (cc == 0) return false;
        lv_obj_t *prev = lv_obj_get_child(container, cc - 1);
        if (!lv_obj_check_type(prev, &lv_label_class)) return false;
        const char *old_txt = lv_label_get_text(prev);
        if (!old_txt) return false;
        size_t olen = strlen(old_txt);
        size_t nlen = strlen(txt);
        // Check if we need to insert a space
        bool need_sp = add_space && olen > 0 && old_txt[olen - 1] != ' ' && txt[0] != ' ';
        size_t total = olen + (need_sp ? 1 : 0) + nlen;
        char *merged = (char *)lv_mem_alloc(total + 1);
        if (!merged) return false;
        memcpy(merged, old_txt, olen);
        if (need_sp) merged[olen++] = ' ';
        memcpy(merged + olen, txt, nlen);
        merged[olen + nlen] = '\0';
        lv_label_set_text(prev, merged);
        lv_mem_free(merged);
        return true;
    };

    bool merge_next = false;  // true after a hidden link — next text appends too
    for (int i = 0; i < result->count; i++) {
        const PageElement *e = &result->elems[i];
        if (e->type == ELEM_HIDDEN) continue;

        // Links with show_links off: merge text inline with > suffix
        if (e->type == ELEM_LINK && !show_links) {
            if (!e->text) continue;
            // Build "[text]" string
            size_t tlen = strlen(e->text);
            char *marked = (char *)lv_mem_alloc(tlen + 3);
            if (marked) {
                marked[0] = '[';
                memcpy(marked + 1, e->text, tlen);
                marked[tlen + 1] = ']';
                marked[tlen + 2] = '\0';
                if (!append_to_prev(marked, true)) {
                    lv_obj_t *lbl = lv_label_create(container);
                    lv_label_set_text(lbl, marked);
                    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(lbl, LV_PCT(100));
                    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, 0);
                    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
                }
                lv_mem_free(marked);
            }
            merge_next = true;
            continue;
        }

        // After a hidden link, merge the next paragraph into the same label
        if (merge_next && e->type == ELEM_PARAGRAPH && e->text) {
            merge_next = false;
            if (append_to_prev(e->text, true)) continue;
        }
        merge_next = false;

        if (!e->text && e->type != ELEM_LINEBREAK &&
            e->type != ELEM_HR &&
            e->type != ELEM_INPUT && e->type != ELEM_SELECT &&
            e->type != ELEM_IMAGE) continue;

        switch (e->type) {
            case ELEM_HEADING: {
                lv_obj_t *lbl = lv_label_create(container);
                lv_label_set_text(lbl, e->text);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(lbl, LV_PCT(100));
                lv_obj_set_style_text_color(lbl, COLOUR_HEADING, 0);
                const lv_font_t *font = &lv_font_montserrat_16;
                if      (e->level == 1) font = &lv_font_montserrat_18;
                else if (e->level == 2) font = &lv_font_montserrat_18;
                lv_obj_set_style_text_font(lbl, font, 0);
                lv_obj_set_style_pad_top(lbl, 6, 0);
                break;
            }
            case ELEM_PARAGRAPH: {
                lv_obj_t *lbl = lv_label_create(container);
                lv_label_set_text(lbl, e->text);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(lbl, LV_PCT(100));
                lv_color_t tc = e->color ? lv_color_hex(e->color & 0x00FFFFFF)
                              : e->italic ? COLOUR_ITALIC : COLOUR_TEXT;
                lv_obj_set_style_text_color(lbl, tc, 0);
                const lv_font_t *f = e->monospace  ? &lv_font_dejavu_mono_14
                                   : e->font_size ? font_for_size(e->font_size)
                                   : e->bold      ? &lv_font_montserrat_bold_14
                                                  : &lv_font_montserrat_14;
                lv_obj_set_style_text_font(lbl, f, 0);
                break;
            }
            case ELEM_LINK: {
                lv_obj_t *lbl = lv_label_create(container);
                lv_label_set_text(lbl, e->text);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(lbl, LV_PCT(100));
                lv_color_t tc = e->color ? lv_color_hex(e->color & 0x00FFFFFF)
                              : COLOUR_LINK;
                lv_obj_set_style_text_color(lbl, tc, 0);
                const lv_font_t *f = e->monospace  ? &lv_font_dejavu_mono_14
                                   : e->font_size  ? font_for_size(e->font_size)
                                   : e->bold       ? &lv_font_montserrat_bold_14
                                                  : &lv_font_montserrat_14;
                lv_obj_set_style_text_font(lbl, f, 0);
                lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(lbl, link_event_cb, LV_EVENT_CLICKED,
                                    (void *)e->href);
                break;
            }
            case ELEM_LINEBREAK: {
                lv_obj_t *sp = lv_label_create(container);
                lv_obj_set_size(sp, LV_PCT(100), 4);
                lv_label_set_text(sp, "");
                break;
            }
            case ELEM_HR: {
                lv_obj_t *line = lv_obj_create(container);
                lv_obj_set_size(line, LV_PCT(100), 1);
                lv_obj_set_style_bg_color(line, COLOUR_BORDER, 0);
                lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(line, 0, 0);
                lv_obj_set_style_radius(line, 0, 0);
                lv_obj_set_style_pad_all(line, 0, 0);
                lv_obj_set_style_shadow_width(line, 0, 0);
                lv_obj_set_style_outline_width(line, 0, 0);
                break;
            }
            case ELEM_INPUT: {
                lv_obj_t *ta = lv_textarea_create(container);
                lv_obj_set_width(ta, LV_PCT(100));
                lv_obj_set_height(ta, e->multiline ? 80 : 32);
                lv_textarea_set_one_line(ta, !e->multiline);
                if (e->value) lv_textarea_set_text(ta, e->value);
                if (e->text) lv_textarea_set_placeholder_text(ta, e->text);
                lv_obj_set_style_bg_color(ta, COLOUR_FIELD, 0);
                lv_obj_set_style_text_color(ta, COLOUR_TEXT, 0);
                lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
                lv_obj_set_style_border_width(ta, 1, 0);
                lv_obj_set_style_border_color(ta, COLOUR_BORDER, 0);
                lv_obj_set_style_radius(ta, 0, 0);
                lv_obj_set_style_shadow_width(ta, 0, 0);
                lv_obj_set_style_outline_width(ta, 0, 0);
                lv_obj_set_style_pad_all(ta, 4, 0);
                lv_obj_set_style_anim_time(ta, 0, 0);
                lv_obj_set_style_bg_color(ta, lv_color_hex(0x4FC3F7),
                                          LV_PART_CURSOR | LV_STATE_FOCUSED);
                lv_obj_set_style_bg_opa(ta, LV_OPA_COVER,
                                        LV_PART_CURSOR | LV_STATE_FOCUSED);
                lv_obj_set_style_border_width(ta, 0, LV_PART_CURSOR);
                lv_obj_set_style_radius(ta, 0, LV_PART_CURSOR);
                lv_obj_set_style_anim_time(ta, 0,
                                           LV_PART_CURSOR | LV_STATE_FOCUSED);
                lv_obj_set_user_data(ta, (void *)e->name);
                lv_obj_add_event_cb(ta, form_ta_click_cb, LV_EVENT_CLICKED, NULL);
                break;
            }
            case ELEM_SUBMIT: {
                lv_obj_t *btn = lv_label_create(container);
                lv_label_set_text(btn, e->text ? e->text : "Submit");
                lv_obj_set_width(btn, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(btn, COLOUR_BTN_BG, 0);
                lv_obj_set_style_text_color(btn, COLOUR_BTN_TXT, 0);
                lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
                lv_obj_set_style_pad_all(btn, 8, 0);
                lv_obj_set_style_radius(btn, 0, 0);
                lv_obj_set_style_shadow_width(btn, 0, 0);
                lv_obj_set_style_border_width(btn, 0, 0);
                lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(btn, form_submit_click_cb,
                                    LV_EVENT_CLICKED, (void *)e);
                break;
            }
            case ELEM_SELECT: {
                lv_obj_t *dd = lv_dropdown_create(container);
                lv_obj_set_width(dd, LV_PCT(100));
                if (e->value) lv_dropdown_set_options(dd, e->value);
                lv_obj_set_style_bg_color(dd, COLOUR_FIELD, 0);
                lv_obj_set_style_text_color(dd, COLOUR_TEXT, 0);
                lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, 0);
                lv_obj_set_style_border_width(dd, 1, 0);
                lv_obj_set_style_border_color(dd, COLOUR_BORDER, 0);
                lv_obj_set_style_radius(dd, 0, 0);
                lv_obj_set_style_shadow_width(dd, 0, 0);
                lv_obj_set_style_pad_all(dd, 4, 0);
                lv_obj_set_user_data(dd, (void *)e->name);
                break;
            }
            case ELEM_IMAGE: {
                if (!show_images) {
                    break; // skip images entirely when disabled
                } else if (e->href && s_img_count < MAX_PAGE_IMAGES) {
                    // Create placeholder image widget
                    lv_obj_t *img = lv_img_create(container);
                    lv_obj_set_size(img, IMAGE_THUMB_W, 10); // placeholder height
                    lv_obj_set_style_bg_opa(img, LV_OPA_50, 0);
                    lv_obj_set_style_bg_color(img, lv_color_hex(0x333333), 0);
                    // Make clickable for full-size view
                    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_add_event_cb(img, img_click_cb, LV_EVENT_CLICKED,
                                        (void *)e->href);
                    // Queue for async fetch
                    ImgSlot *slot = &s_img_slots[s_img_count++];
                    memset(slot, 0, sizeof(*slot));
                    slot->widget = img;
                    slot->url = e->href;
                }
                break;
            }
            default:
                break;
        }
    }
}

bool page_img_next(int *index, const char **url) {
    while (s_img_fetch_idx < s_img_count) {
        ImgSlot *slot = &s_img_slots[s_img_fetch_idx];
        if (slot->url && slot->widget && !slot->data) {
            *index = s_img_fetch_idx;
            *url = slot->url;
            s_img_fetch_idx++;
            return true;
        }
        s_img_fetch_idx++;
    }
    return false;
}

void page_img_set(int index, uint8_t *data, size_t len) {
    if (index < 0 || index >= s_img_count) return;
    ImgSlot *slot = &s_img_slots[index];
    if (!slot->widget || !data || len == 0) return;
    slot->data = data;
    memset(&slot->dsc, 0, sizeof(slot->dsc));
    slot->dsc.header.cf = LV_IMG_CF_RAW;
    slot->dsc.data_size = len;
    slot->dsc.data = data;

    // Get actual decoded dimensions from LVGL decoder
    lv_img_header_t hdr;
    if (lv_img_decoder_get_info(&slot->dsc, &hdr) == LV_RES_OK) {
        lv_obj_set_size(slot->widget, hdr.w, hdr.h);
    } else {
        lv_obj_set_size(slot->widget, IMAGE_THUMB_W, IMAGE_THUMB_H);
    }
    lv_img_set_src(slot->widget, &slot->dsc);
}

bool page_img_full_pending(const char **url) {
    if (s_full_pending_url) {
        *url = s_full_pending_url;
        s_full_pending_url = nullptr;  // consume — prevents re-posting every tick
        return true;
    }
    return false;
}

void page_img_full_set(uint8_t *data, size_t len) {
    s_full_pending_url = nullptr;
    if (!data || len == 0 || !s_overlay) return;

    if (s_full_data) heap_caps_free(s_full_data);
    s_full_data = data;

    memset(&s_full_dsc, 0, sizeof(s_full_dsc));
    s_full_dsc.header.cf = LV_IMG_CF_RAW;
    s_full_dsc.data_size = len;
    s_full_dsc.data = data;

    lv_img_header_t hdr;
    int img_w = IMAGE_FULL_W, img_h = IMAGE_FULL_H;
    if (lv_img_decoder_get_info(&s_full_dsc, &hdr) == LV_RES_OK) {
        img_w = hdr.w;
        img_h = hdr.h;
    }

    // Replace loading text with image
    lv_obj_clean(s_overlay);
    lv_obj_add_event_cb(s_overlay, overlay_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *img = lv_img_create(s_overlay);
    lv_obj_set_size(img, img_w, img_h);
    lv_img_set_src(img, &s_full_dsc);
    lv_obj_center(img);
}

void page_img_full_fail() {
    s_full_pending_url = nullptr;
    if (!s_overlay) return;

    // Replace loading text with error
    lv_obj_clean(s_overlay);
    lv_obj_add_event_cb(s_overlay, overlay_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_overlay);
    lv_label_set_text(lbl, "Failed to load image\nTap to close");
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
}
