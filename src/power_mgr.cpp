/*
 * Battery power management — 4-state machine + light sleep polling loop
 * Written by Alun Morris and Claude Code
 *
 * 190326 Battery power management — 4-state machine + light sleep polling loop
 */
#ifdef BATTERY_MODE

#include "power_mgr.h"
#include "display.h"
#include "touch.h"
#include "net_task.h"
#include "fetcher.h"
#include "image_fetch.h"
#include "img_task.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

// Thresholds — total elapsed ms since last touch
#define DIM_MS  (2UL  * 60UL * 1000UL)   // 2 min
#define OFF_MS  (5UL  * 60UL * 1000UL)   // 5 min
#define LOW_MS  (10UL * 60UL * 1000UL)   // 10 min

#define AXS_ADDR 0x3B

enum PowerState { ACTIVE, DIM, OFF, LOW_POWER };
static PowerState s_state         = ACTIVE;
static uint32_t   s_last_touch_ms = 0;

// Direct AXS15231B poll — used inside the light-sleep loop where LVGL is not running.
// Wire is safe immediately after esp_light_sleep_start() returns (I2C peripheral preserved).
static bool axs_poll_touch() {
    uint8_t cmd[8] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x08};
    Wire.beginTransmission(AXS_ADDR);
    Wire.write(cmd, 8);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)AXS_ADDR, (uint8_t)14) < 14) return false;
    uint8_t buf[14];
    for (int i = 0; i < 14; i++) buf[i] = Wire.read();
    return (buf[0] == 0 && buf[1] > 0 && buf[1] <= 2);
}

void power_mgr_init() {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_last_touch_ms = millis();
}

void power_mgr_tick() {
    // Reset timer if touch occurred (flag set in touch_read_cb, cleared here)
    if (touch_was_active()) {
        s_last_touch_ms = millis();
        if (s_state != ACTIVE) {
            s_state = ACTIVE;
            display_backlight_set(100);
        }
        return;
    }

    uint32_t elapsed = millis() - s_last_touch_ms;

    if (s_state == ACTIVE && elapsed >= DIM_MS) {
        s_state = DIM;
        display_backlight_set(20);

    } else if (s_state == DIM && elapsed >= OFF_MS) {
        s_state = OFF;
        display_backlight_set(0);

    } else if (s_state == OFF && elapsed >= LOW_MS) {
        s_state = LOW_POWER;

        // Cleanly shut down in-flight network activity before sleeping
        net_task_cancel();          // signal fetch to stop
        fetch_disconnect();         // close proxy TCP socket
        image_fetch_disconnect();   // close image TLS socket
        img_task_flush();           // drain image queues

        // Polling loop — wakes every 30s to check touch via I2C
        for (;;) {
            esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
            esp_light_sleep_start();
            // Both cores halted during sleep; I2C/LEDC state preserved on wake
            if (axs_poll_touch()) {
                display_backlight_set(100);
                touch_suppress_next();  // don't action the wake tap
                s_last_touch_ms = millis();
                s_state = ACTIVE;
                return;
            }
        }
    }
}

#endif // BATTERY_MODE
