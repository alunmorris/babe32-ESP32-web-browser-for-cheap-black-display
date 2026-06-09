/*
 * AXS15231 touch on I2C SDA=4 SCL=8 for JC3248W535C
 * Written by Alun Morris and Claude Code
 *
 * 110326 AXS15231 touch on I2C SDA=4 SCL=8 for JC3248W535C
 * 120326 Direct I2C driver — bb_captouch has AXS15231 behind #ifdef FUTURE
 * 130326 Transform portrait touch → landscape coordinates (CW 90°)
 */
#include "touch.h"
#include "dbglog.h"
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define AXS_ADDR  0x3B

static lv_indev_drv_t indev_drv;
static bool s_touch_ok = false;
static volatile bool s_touch_active   = false;
static volatile bool s_suppress_touch = false;

volatile int g_touch_x = -1, g_touch_y = -1;
volatile bool g_touch_pressed = false;
volatile int g_touch_init_rc = -99;
volatile int g_touch_type = -99;

static bool axs_read(uint16_t *x, uint16_t *y) {
    uint8_t cmd[8] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x08};
    Wire.beginTransmission(AXS_ADDR);
    Wire.write(cmd, 8);
    if (Wire.endTransmission() != 0) return false;

    if (Wire.requestFrom((uint8_t)AXS_ADDR, (uint8_t)14) < 14) return false;
    uint8_t buf[14];
    for (int i = 0; i < 14; i++) buf[i] = Wire.read();

    uint8_t status = buf[0];
    uint8_t count  = buf[1];
    if (status != 0 || count == 0 || count > 2) return false;

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

static uint32_t s_touch_dbg_count = 0;

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint16_t tx, ty;
    if (s_touch_ok && axs_read(&tx, &ty)) {
        if (s_suppress_touch) {
            // Wake-from-sleep touch — report released until finger lifts
            data->state = LV_INDEV_STATE_REL;
            g_touch_pressed = false;
            return;
        }
        // Transform portrait touch → landscape (CW 90°)
        data->point.x = ty;           // landscape x = portrait y
        data->point.y = 319 - tx;     // landscape y = 319 - portrait x
        data->state   = LV_INDEV_STATE_PR;
        g_touch_x = data->point.x;
        g_touch_y = data->point.y;
        g_touch_pressed = true;
        s_touch_active = true;
        if (s_touch_dbg_count < 5) {
            dbg("TOUCH: raw(%d,%d) -> landscape(%d,%d)", tx, ty,
                data->point.x, data->point.y);
            s_touch_dbg_count++;
        }
    } else {
        s_suppress_touch = false;   // finger lifted — end suppression
        data->state = LV_INDEV_STATE_REL;
        g_touch_pressed = false;
    }
}

void touch_init() {
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);

    // Check AXS15231 is present
    Wire.beginTransmission(AXS_ADDR);
    int rc = Wire.endTransmission();
    g_touch_init_rc = rc;
    g_touch_type = (rc == 0) ? 0x3B : 0;

    if (rc == 0) {
        s_touch_ok = true;
        dbg("Touch OK: AXS15231 at 0x3B");
    } else {
        dbg("Touch FAIL: no device at 0x3B (rc=%d)", rc);
    }
}

void touch_lvgl_init() {
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
    dbg("LVGL touch indev registered");
}

bool touch_was_active() {
    if (!s_touch_active) return false;
    s_touch_active = false;
    return true;
}

void touch_suppress_next() {
    s_suppress_touch = true;
}
