/*
 * USB HID keyboard host for ESP32-S3 browser
 * Written by Alun Morris and Claude Code
 *
 * 170426 Ported from SLUG/src/hal_s2.cpp (ESP32-S2 Mini, proven working)
 * GPIO 19/20: D-/D+ USB OTG. Keyboard must be self-powered or VBUS provided.
 * ARDUINO_USB_CDC_ON_BOOT=0 required to free OTG from CDC device mode.
 * No WiFi/BLE radio conflicts — no before/after fetch hooks needed.
 */
#ifdef USB_KEYBOARD
#include "usb_kb.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "usb/usb_host.h"

// --- Ring buffer ---
#define RB_SIZE 16
static BleKbEvent        rb[RB_SIZE];
static volatile int      rb_head  = 0;
static volatile int      rb_tail  = 0;
static SemaphoreHandle_t rb_mutex = nullptr;

static void rb_push(BleKbEvent ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    int next = (rb_head + 1) % RB_SIZE;
    if (next != rb_tail) { rb[rb_head] = ev; rb_head = next; }
    xSemaphoreGive(rb_mutex);
}

bool usb_kb_poll(BleKbEvent* ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    bool has = (rb_tail != rb_head);
    if (has) { *ev = rb[rb_tail]; rb_tail = (rb_tail + 1) % RB_SIZE; }
    xSemaphoreGive(rb_mutex);
    return has;
}

// --- HID scan code → ASCII ---
static char hidToAscii(uint8_t code, bool shifted, bool capsLock) {
    if (code >= 0x04 && code <= 0x1D) {
        char c = 'a' + (code - 0x04);
        return (shifted ^ capsLock) ? (c - 32) : c;
    }
    if (code >= 0x1E && code <= 0x27) {
        static const char num[]   = "1234567890";
        static const char numSh[] = "!@#$%^&*()";
        return shifted ? numSh[code - 0x1E] : num[code - 0x1E];
    }
    switch (code) {
        case 0x2C: return ' ';
        case 0x2D: return shifted ? '_' : '-';
        case 0x2E: return shifted ? '+' : '=';
        case 0x2F: return shifted ? '{' : '[';
        case 0x30: return shifted ? '}' : ']';
        case 0x31: return shifted ? '|' : '\\';
        case 0x33: return shifted ? ':' : ';';
        case 0x34: return shifted ? '"' : '\'';
        case 0x35: return shifted ? '~' : '`';
        case 0x36: return shifted ? '<' : ',';
        case 0x37: return shifted ? '>' : '.';
        case 0x38: return shifted ? '?' : '/';
        default:   return 0;
    }
}

// --- HID report parser ---
// Standard USB HID boot-protocol keyboard report: 8 bytes.
// byte 0: modifier  byte 1: reserved  bytes 2-7: up to 6 keycodes
// Some wireless dongles prefix with a report ID byte despite SET_PROTOCOL(Boot);
// detect by: byte 0 is 1-4 AND byte 2 is 0x00 (boot-protocol reserved field).
static uint8_t s_lastKeycodes[6] = {};
static bool    s_capsLock        = false;
static volatile bool s_ledsDirty = false;

static void parseHidReport(const uint8_t* data, size_t len) {
    if (len < 3) return;
    if (len >= 4 && data[0] >= 1 && data[0] <= 4 && data[2] == 0x00) { data++; len--; }
    uint8_t modifier = data[0];
    bool ctrl    = (modifier & 0x11) != 0;
    bool shifted = (modifier & 0x22) != 0;

    bool allZero = true;
    for (size_t i = 2; i < len && i < 8; i++) if (data[i]) { allZero = false; break; }
    if (allZero) { memset(s_lastKeycodes, 0, sizeof(s_lastKeycodes)); return; }

    // Key-repeat suppression: ignore identical reports
    uint8_t keycodes[6] = {};
    for (size_t i = 2, k = 0; i < len && i < 8 && k < 6; i++, k++) keycodes[k] = data[i];
    if (memcmp(keycodes, s_lastKeycodes, 6) == 0) return;
    memcpy(s_lastKeycodes, keycodes, 6);

    for (size_t i = 2; i < len && i < 8; i++) {
        uint8_t code = data[i];
        if (!code) continue;
        BleKbEvent ev = { BLE_KB_NONE, 0 };
        if      (code == 0x39)         { s_capsLock = !s_capsLock; s_ledsDirty = true; }
        else if (code == 0x29)         ev.type = BLE_KB_ESCAPE;
        else if (ctrl && code == 0x0F) ev.type = BLE_KB_URL_FOCUS;   // Ctrl+L
        else if (code == 0x52)         ev.type = BLE_KB_SCROLL_UP;
        else if (code == 0x51)         ev.type = BLE_KB_SCROLL_DOWN;
        else if (code == 0x50)         ev.type = BLE_KB_CURSOR_LEFT;
        else if (code == 0x4F)         ev.type = BLE_KB_CURSOR_RIGHT;
        else if (code == 0x28)         ev.type = BLE_KB_ENTER;
        else if (code == 0x2A)         ev.type = BLE_KB_BACKSPACE;
        else {
            char c = hidToAscii(code, shifted, s_capsLock);
            if (c) { ev.type = BLE_KB_CHAR; ev.ch = c; }
        }
        if (ev.type != BLE_KB_NONE) rb_push(ev);
    }
}

// --- USB Host ---
static usb_host_client_handle_t s_client   = nullptr;
static usb_device_handle_t      s_dev      = nullptr;
static usb_transfer_t*          s_xfer     = nullptr;
static uint8_t                  s_ifaceNum = 0;
static volatile bool            s_devOpen  = false;
static volatile uint8_t         s_newAddr  = 0;
static volatile bool            s_newDev   = false;
static volatile bool            s_devGone  = false;
static volatile bool            s_usbInitErr = false;

bool usb_kb_connected() { return s_devOpen; }

static void transferCb(usb_transfer_t* xfer) {
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0)
        parseHidReport(xfer->data_buffer, xfer->actual_num_bytes);
    if (s_devOpen && xfer->status == USB_TRANSFER_STATUS_COMPLETED)
        usb_host_transfer_submit(xfer);
}

static void clientEventCb(const usb_host_client_event_msg_t* msg, void*) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_newAddr = msg->new_dev.address;
        s_newDev  = true;
    } else {
        s_devGone = true;
    }
}

// Find first HID interrupt-IN endpoint; skip mouse (protocol 2).
static bool findHidEndpoint(usb_device_handle_t dev,
                            uint8_t* epAddr, uint16_t* maxPkt, uint8_t* ifNum) {
    const usb_config_desc_t* cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return false;
    bool inHid = false;
    int  offset = 0;
    const uint8_t* p = (const uint8_t*)cfg;
    while (offset + 2 <= cfg->wTotalLength) {
        uint8_t dLen = p[offset], dType = p[offset + 1];
        if (!dLen || offset + dLen > cfg->wTotalLength) break;
        if (dType == 0x04) {
            const usb_intf_desc_t* intf = (const usb_intf_desc_t*)(p + offset);
            inHid = (intf->bInterfaceClass == 0x03 && intf->bInterfaceProtocol != 2);
            if (inHid) *ifNum = intf->bInterfaceNumber;
        } else if (dType == 0x05 && inHid) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(p + offset);
            if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 0x03) == 0x03) {
                *epAddr = ep->bEndpointAddress;
                *maxPkt = ep->wMaxPacketSize;
                return true;
            }
        }
        offset += dLen;
    }
    return false;
}

static void sendCtrl(uint8_t bmReqType, uint8_t bReq,
                     uint16_t wValue, uint8_t wIndexIface, uint16_t wLength,
                     const uint8_t* outData, SemaphoreHandle_t* doneSem) {
    usb_transfer_t* ctrl = nullptr;
    if (usb_host_transfer_alloc(8 + wLength, 0, &ctrl) != ESP_OK) return;
    ctrl->data_buffer[0] = bmReqType;
    ctrl->data_buffer[1] = bReq;
    ctrl->data_buffer[2] = wValue & 0xFF;
    ctrl->data_buffer[3] = wValue >> 8;
    ctrl->data_buffer[4] = wIndexIface;
    ctrl->data_buffer[5] = 0x00;
    ctrl->data_buffer[6] = wLength & 0xFF;
    ctrl->data_buffer[7] = wLength >> 8;
    if (outData && wLength) memcpy(ctrl->data_buffer + 8, outData, wLength);
    ctrl->num_bytes        = 8 + wLength;
    ctrl->device_handle    = s_dev;
    ctrl->bEndpointAddress = 0x00;
    ctrl->timeout_ms       = 1000;
    ctrl->callback = [](usb_transfer_t* t) { xSemaphoreGive((SemaphoreHandle_t)t->context); };
    ctrl->context  = *doneSem;
    if (usb_host_transfer_submit_control(s_client, ctrl) == ESP_OK)
        xSemaphoreTake(*doneSem, pdMS_TO_TICKS(1000));
    usb_host_transfer_free(ctrl);
}

static void sendLedReport() {
    if (!s_devOpen || !s_client || !s_dev) return;
    static SemaphoreHandle_t ledDone = nullptr;
    if (!ledDone) ledDone = xSemaphoreCreateBinary();
    uint8_t leds = s_capsLock ? 0x02 : 0x00;
    // SET_REPORT (Output, report type 2, report ID 0), wValue = 0x0200
    sendCtrl(0x21, 0x09, 0x0200, s_ifaceNum, 1, &leds, &ledDone);
}

static void openDevice(uint8_t addr) {
    Serial.printf("[USB KB] opening device addr=%d\n", addr);
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        Serial.println("[USB KB] FAIL: device_open");
        return;
    }
    uint8_t epAddr = 0; uint16_t maxPkt = 8;
    if (!findHidEndpoint(s_dev, &epAddr, &maxPkt, &s_ifaceNum)) {
        Serial.println("[USB KB] FAIL: no HID endpoint");
        usb_host_device_close(s_client, s_dev); s_dev = nullptr; return;
    }
    Serial.printf("[USB KB] ep=0x%02X pkt=%d iface=%d\n", epAddr, maxPkt, s_ifaceNum);
    if (usb_host_interface_claim(s_client, s_dev, s_ifaceNum, 0) != ESP_OK) {
        Serial.println("[USB KB] FAIL: iface_claim");
        usb_host_device_close(s_client, s_dev); s_dev = nullptr; return;
    }

    static SemaphoreHandle_t ctrlDone = nullptr;
    if (!ctrlDone) ctrlDone = xSemaphoreCreateBinary();

    // SET_PROTOCOL(Boot) — forces standard 8-byte report format, essential for wireless dongles
    sendCtrl(0x21, 0x0B, 0x0000, s_ifaceNum, 0, nullptr, &ctrlDone);
    Serial.println("[USB KB] SET_PROTOCOL(Boot) done");

    // SET_IDLE(0) — suppress repeated reports while key held (some devices ignore this)
    sendCtrl(0x21, 0x0A, 0x0000, s_ifaceNum, 0, nullptr, &ctrlDone);
    Serial.println("[USB KB] SET_IDLE(0) done");

    if (usb_host_transfer_alloc(maxPkt, 0, &s_xfer) != ESP_OK) {
        Serial.println("[USB KB] FAIL: xfer_alloc");
        usb_host_interface_release(s_client, s_dev, s_ifaceNum);
        usb_host_device_close(s_client, s_dev); s_dev = nullptr; return;
    }
    s_xfer->device_handle    = s_dev;
    s_xfer->bEndpointAddress = epAddr;
    s_xfer->callback         = transferCb;
    s_xfer->context          = nullptr;
    s_xfer->num_bytes        = maxPkt;
    s_xfer->timeout_ms       = 0;
    s_devOpen = true;
    usb_host_transfer_submit(s_xfer);
    Serial.println("[USB KB] keyboard ready");
}

static void closeDevice() {
    s_devOpen = false;
    if (s_xfer && s_dev) {
        usb_host_endpoint_halt(s_dev, s_xfer->bEndpointAddress);
        usb_host_endpoint_flush(s_dev, s_xfer->bEndpointAddress);
    }
    if (s_xfer) { usb_host_transfer_free(s_xfer); s_xfer = nullptr; }
    if (s_dev) {
        usb_host_interface_release(s_client, s_dev, s_ifaceNum);
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
    }
    Serial.println("[USB KB] keyboard closed");
}

// USB host lib daemon — must run at higher priority than the client task
static void usbHostDaemonTask(void*) {
    usb_host_config_t cfg = {};
    cfg.skip_phy_setup = false;
    cfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[USB KB] host_install failed: 0x%x\n", err);
        s_usbInitErr = true;
        vTaskDelete(nullptr);
        return;
    }
    Serial.println("[USB KB] host lib installed");
    uint32_t flags;
    for (;;) {
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void usbClientTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(100));  // ensure daemon has installed the host lib
    if (s_usbInitErr) { vTaskDelete(nullptr); return; }

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous              = false;
    clientCfg.max_num_event_msg           = 5;
    clientCfg.async.client_event_callback = clientEventCb;
    clientCfg.async.callback_arg          = nullptr;
    if (usb_host_client_register(&clientCfg, &s_client) != ESP_OK) {
        Serial.println("[USB KB] client_register failed");
        s_usbInitErr = true;
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        if (s_newDev)   { s_newDev = false;   openDevice(s_newAddr); }
        if (s_devGone)  { s_devGone = false;   closeDevice(); }
        if (s_ledsDirty){ s_ledsDirty = false; sendLedReport(); }
    }
}

void usb_kb_init() {
    rb_mutex = xSemaphoreCreateMutex();
    // Daemon at priority 2 so it processes the host-lib-installed signal before client task
    xTaskCreate(usbHostDaemonTask, "usb_host",   4096, nullptr, 2, nullptr);
    xTaskCreate(usbClientTask,     "usb_client", 4096, nullptr, 1, nullptr);
    // Wait up to 5s for keyboard to be detected
    for (int i = 0; i < 50 && !s_devOpen && !s_usbInitErr; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (s_usbInitErr) Serial.println("[USB KB] init failed — keyboard won't work");
    else if (!s_devOpen) Serial.println("[USB KB] no keyboard found yet (will auto-connect)");
}

#endif // USB_KEYBOARD
