/*
 * Build LVGL widgets from parsed PageElement list
 * Written by Alun Morris and Claude Code
 *
 * 060326 Build LVGL widgets from parsed PageElement list
 * 160326 Add show_images param for inline image rendering
 */
#pragma once
#include <lvgl.h>
#include "html_parser.h"

typedef void (*link_tap_cb_t)(const char *url);
typedef void (*form_submit_cb_t)(const char *action_url, bool is_post,
                                  const char *encoded_body);
typedef void (*form_field_focus_cb_t)(lv_obj_t *textarea);

// Render page into container. Clears existing children first.
void page_render(lv_obj_t *container, const ParseResult *result,
                 link_tap_cb_t on_link_tap,
                 form_submit_cb_t on_form_submit,
                 form_field_focus_cb_t on_field_focus,
                 bool show_links = true,
                 bool show_images = false);

// Collect form data from container into URL-encoded string
// form_id scopes hidden fields to a specific form (-1 = all)
void collect_form_data(lv_obj_t *container, const ParseResult *result,
                       char *out, size_t out_len, int form_id = -1);

// Toggle inverted colour scheme (light bg / dark text)
void page_set_inverted(bool inv);
bool page_is_inverted();

// Clear the page container
void page_clear(lv_obj_t *container);

// Show loading spinner in container
void page_show_spinner(lv_obj_t *container);

// Image fetch queue — call page_img_next() repeatedly from main loop
// Returns true if there's an image to fetch (sets *url and *index).
bool page_img_next(int *index, const char **url);
// Apply fetched image data to the widget at given index.
void page_img_set(int index, uint8_t *data, size_t len);

// Full-size image overlay (async) — check from main loop
bool page_img_full_pending(const char **url);
void page_img_full_posted();   // call after posting the request to img_task
void page_img_full_set(uint8_t *data, size_t len);
void page_img_full_fail();
