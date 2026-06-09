/*
 * AXS15231B QSPI display driver for JC3248W535C
 * Written by Alun Morris and Claude Code
 *
 * 110326 AXS15231B QSPI display driver for JC3248W535C
 * 130326 Landscape 480x320 via software rotation in flush callback
 */
#pragma once
#include <lvgl.h>

void display_init();
// Clear screen to black — call after display_init(), before LVGL
void display_clear();
// Called after display_init() — registers LVGL display driver
void display_lvgl_init();
// Hardware test: fills four quadrants with R/G/B/W. Call after display_init(), no LVGL needed.
void display_test_quadrants();
// Hardware test: cycles through solid R/G/B/W/Cyan/Magenta every 2 seconds.
void display_test_solid_cycle();
// Set backlight brightness 0-100%. Safe to call outside LVGL lock (LEDC, no LVGL).
void display_backlight_set(int percent);
