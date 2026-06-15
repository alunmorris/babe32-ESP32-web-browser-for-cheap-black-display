/*
 * Custom 4-state keyboard maps
 * Written by Alun Morris and Claude Code
 *
 * 180326 Custom 5-row keyboard maps (extracted from ui_task.cpp)
 * 140626 4-state keyboard: ABC (shift toggle) + 1# (num/sym layer toggle)
 */
#pragma once
#include <lvgl.h>

#define KB_BTN(w)  (LV_BTNMATRIX_CTRL_POPOVER | (w))
#define KB_FUNC    (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG)
#define KB_SPEC    (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)

// State 1: letters lowercase — ABC=dark, 1#=dark
static const char * const kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_BACKSPACE, "\n",
    "SFT", "z", "x", "c", "v", "b", "n", "m", "\n",
    LV_SYMBOL_KEYBOARD, "1#", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "ENT", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED|4,
    KB_FUNC|5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_SPEC|2, KB_FUNC|2, LV_BTNMATRIX_CTRL_CHECKED|1, 6, LV_BTNMATRIX_CTRL_CHECKED|1, KB_SPEC|2
};

// State 2: letters uppercase — ABC=bright, 1#=dark
static const char * const kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_BACKSPACE, "\n",
    "SFT", "Z", "X", "C", "V", "B", "N", "M", "\n",
    LV_SYMBOL_KEYBOARD, "1#", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "ENT", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_uc[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED|4,
    KB_SPEC|5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_SPEC|2, KB_FUNC|2, LV_BTNMATRIX_CTRL_CHECKED|1, 6, LV_BTNMATRIX_CTRL_CHECKED|1, KB_SPEC|2
};

// State 3: numbers + lowercase — ABC=dark, 1#=bright
static const char * const kb_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\n",
    "SFT", "!", "'", "\"", "`", ",", ".", "?", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, "1#", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "ENT", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_spec[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_FUNC|5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED|4,
    KB_SPEC|2, KB_SPEC|2, LV_BTNMATRIX_CTRL_CHECKED|1, 6, LV_BTNMATRIX_CTRL_CHECKED|1, KB_SPEC|2
};

// State 4: symbols + uppercase — ABC=bright, 1#=bright
static const char * const kb_map_sym[] = {
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\n",
    "SFT", "~", "'", "\"", "`", ",", ".", "?", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, "1#", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "ENT", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_sym[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_SPEC|5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED|4,
    KB_SPEC|2, KB_SPEC|2, LV_BTNMATRIX_CTRL_CHECKED|1, 6, LV_BTNMATRIX_CTRL_CHECKED|1, KB_SPEC|2
};
