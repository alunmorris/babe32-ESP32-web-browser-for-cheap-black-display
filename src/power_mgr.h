/*
 * Battery power management — dim, backlight off, light sleep
 * Written by Alun Morris and Claude Code
 *
 * 190326 Battery power management — dim, backlight off, light sleep
 */
#pragma once
#ifdef BATTERY_MODE
void power_mgr_init();   // call once at startup; enables WiFi modem sleep
void power_mgr_tick();   // drive state machine; call outside lvgl_lock each loop
#endif
