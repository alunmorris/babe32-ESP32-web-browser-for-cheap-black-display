/*
 * USB HID keyboard host for ESP32-S3 browser
 * Written by Alun Morris and Claude Code
 *
 * 170426 Initial implementation, ported from SLUG/src/hal_s2.cpp
 */
#pragma once
#include "ble_kb.h"  // reuse BleKbEvent / BleKbEventType

#ifdef USB_KEYBOARD
void usb_kb_init();           // start USB host tasks; waits up to 5s for device
bool usb_kb_connected();
bool usb_kb_poll(BleKbEvent* ev);
#else
inline void usb_kb_init() {}
inline bool usb_kb_connected() { return false; }
inline bool usb_kb_poll(BleKbEvent*) { return false; }
#endif
