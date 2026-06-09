/*
 * Custom 5-row keyboard maps (extracted from ui_task.cpp)
 * Written by Alun Morris and Claude Code
 *
 * 180326 Custom 5-row keyboard maps (extracted from ui_task.cpp)
 */
#pragma once
#include <lvgl.h>

#define KB_BTN(w) (LV_BTNMATRIX_CTRL_POPOVER | (w))
#define KB_SPEC   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)

static const char * const kb_map_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_UP, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "1#", "z", "x", "c", "v", "b", "n", "m", ".", "/", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_SPEC | 4,
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 6,
    KB_SPEC | 6, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 6,
    KB_SPEC | 5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_SPEC | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, KB_SPEC | 2
};

static const char * const kb_map_uc[] = {
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", LV_SYMBOL_UP, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "1#", "Z", "X", "C", "V", "B", "N", "M", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_uc[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_SPEC | 4,
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 6,
    KB_SPEC | 6, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 6,
    KB_SPEC | 5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_SPEC | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, KB_SPEC | 2
};

static const char * const kb_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_UP, "\n",
    "+", "-", "*", "/", "=", "%", "!", "?", "#", "@", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "&", "_", "\\", "|", "(", ")", "[", "]", ";", LV_SYMBOL_NEW_LINE, "\n",
    "<", ">", "{", "}", "\"", "'", "~", "`", "$", "^", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_spec[] = {
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_SPEC | 4,
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 6,
    KB_SPEC | 6, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 6,
    KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4),
    KB_SPEC | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, KB_SPEC | 2
};
