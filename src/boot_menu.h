/*
 * Boot menu and wiki search screens (extracted from ui_task.cpp)
 * Written by Alun Morris and Claude Code
 *
 * 180326 Boot menu and wiki search screens (extracted from ui_task.cpp)
 */
#pragma once
#include <lvgl.h>

typedef void (*navigate_cb_t)(const char *url);
typedef void (*field_focus_cb_t)(lv_obj_t *ta);
typedef void (*urls_mode_cb_t)();

void boot_menu_init(lv_obj_t *content, navigate_cb_t nav,
                    field_focus_cb_t focus, urls_mode_cb_t urls_mode);
void boot_menu_hide_buttons();
void show_boot_menu();
void show_wiki_search();
void boot_menu_wiki_submit();
lv_obj_t *boot_menu_get_wiki_ta();
