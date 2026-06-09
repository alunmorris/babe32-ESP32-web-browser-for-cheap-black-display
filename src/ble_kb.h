/*
 * NimBLE HID keyboard host for ESP32-S3 browser
 * Written by Alun Morris and Claude Code
 */
#pragma once
#include <stdbool.h>

enum BleKbEventType {
    BLE_KB_NONE,
    BLE_KB_CHAR,
    BLE_KB_ENTER,
    BLE_KB_BACKSPACE,
    BLE_KB_ESCAPE,
    BLE_KB_CURSOR_LEFT,
    BLE_KB_CURSOR_RIGHT,
    BLE_KB_SCROLL_UP,
    BLE_KB_SCROLL_DOWN,
    BLE_KB_URL_FOCUS,  // Ctrl+L
};

struct BleKbEvent {
    BleKbEventType type;
    char ch;
};

#ifdef BLE_KEYBOARD
extern volatile bool g_ble_kb_scanning;  // true during WiFi-off BLE scan windows
void ble_kb_init();
void ble_kb_start_reconnect();  // call after WiFi connects
bool ble_kb_poll(BleKbEvent* ev);
bool ble_kb_connected();
void ble_kb_before_fetch();
void ble_kb_after_fetch();
#else
static volatile bool g_ble_kb_scanning = false;
inline void ble_kb_init() {}
inline void ble_kb_start_reconnect() {}
inline bool ble_kb_poll(BleKbEvent*) { return false; }
inline bool ble_kb_connected() { return false; }
inline void ble_kb_before_fetch() {}
inline void ble_kb_after_fetch() {}
#endif
