/*
 * NimBLE BLE HID keyboard host for ESP32-S3 browser
 * Written by Alun Morris and Claude Code
 *
 * 160426 Initial port from ai-chatbot/SLUG/src/hal_c3.cpp
 * 170426 SLUG halBeforeApiCall/halAfterApiCall pattern: coex not WiFi stop
 * 170426 Redirect mbedTLS alloc to PSRAM after NimBLE consumes ~62KB internal RAM
 * 170426 Disconnect BLE before each fetch (vTaskSuspend reconnect task) for full WiFi radio
 * STATUS: NOT WORKING RELIABLY — WiFi/BLE radio conflicts on ESP32-S3 cause significant
 * page load slowdown even when BLE is disconnected during fetches. NimBLE host stack
 * consumes ~62KB internal SRAM, forcing mbedTLS SSL buffers into PSRAM and degrading
 * TLS throughput. BLE keyboard reconnect after each page load adds further latency.
 * The non-BLE build is substantially faster. Retained for future investigation.
 */
#ifdef BLE_KEYBOARD
#include "ble_kb.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp_coexist.h"
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>

// NimBLE init consumes ~62KB of internal SRAM for its host stack and HCI buffers.
// This leaves insufficient contiguous internal RAM for mbedTLS's ~40KB SSL buffers.
// Fix: redirect all mbedTLS allocs to PSRAM. The ESP32-S3 AES hardware DMA then
// copies data between PSRAM and its ~16KB internal DMA buffer automatically.
// Keeping internal SRAM clear (do NOT put mbedTLS structures there) is required
// because the AES DMA buffer itself needs the remaining ~18KB of internal SRAM.
static void* tls_psram_calloc(size_t n, size_t size) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    return p;
}
static void tls_psram_free(void* ptr) { heap_caps_free(ptr); }

static const NimBLEUUID HID_SVC_UUID("1812");
static const NimBLEUUID HID_RPT_UUID("2A4D");

// --- Ring buffer ---
#define RB_SIZE 16
static BleKbEvent   rb[RB_SIZE];
static volatile int rb_head = 0;
static volatile int rb_tail = 0;
static SemaphoreHandle_t rb_mutex = nullptr;

static void rb_push(BleKbEvent ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    int next = (rb_head + 1) % RB_SIZE;
    if (next != rb_tail) {
        rb[rb_head] = ev;
        rb_head = next;
    }
    xSemaphoreGive(rb_mutex);
}

bool ble_kb_poll(BleKbEvent* ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    bool has = (rb_tail != rb_head);
    if (has) {
        *ev = rb[rb_tail];
        rb_tail = (rb_tail + 1) % RB_SIZE;
    }
    xSemaphoreGive(rb_mutex);
    return has;
}

// --- NVS bonded address store ---
static NimBLEAddress loadBondedAddress(bool& found) {
    Preferences p;
    p.begin("ble_kb", true);
    found = p.getBool("bonded", false);
    if (!found) { p.end(); return NimBLEAddress(); }
    String addrStr = p.getString("addr", "");
    uint8_t type   = p.getUChar("type", 0);
    p.end();
    return NimBLEAddress(addrStr.c_str(), type);
}

static void saveBondedAddress(const NimBLEAddress& addr) {
    Preferences p;
    p.begin("ble_kb", false);
    p.putBool("bonded", true);
    p.putString("addr", addr.toString().c_str());
    p.putUChar("type", addr.getType());
    p.end();
}

// --- HID scan code → ASCII ---
static char hidToAscii(uint8_t code, bool shifted, bool capsLock) {
    if (code >= 0x04 && code <= 0x1D) {
        char c = 'a' + (code - 0x04);
        bool upper = shifted ^ capsLock;
        return upper ? (c - 32) : c;
    }
    if (code >= 0x1E && code <= 0x27) {
        static const char num[]   = "1234567890";
        static const char numSh[] = "!@#$%^&*()";
        int i = code - 0x1E;
        return shifted ? numSh[i] : num[i];
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

// --- HID notify callback ---
static bool capsLock = false;

static void hidNotifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 2) return;
    uint8_t modifier = data[0];
    bool ctrl    = (modifier & 0x11) != 0;
    bool shifted = (modifier & 0x22) != 0;

    bool allZero = true;
    for (size_t i = 2; i < len && i < 8; i++) {
        if (data[i] != 0) { allZero = false; break; }
    }
    if (allZero) return;

    for (size_t i = 2; i < len && i < 8; i++) {
        uint8_t code = data[i];
        if (code == 0) continue;

        BleKbEvent ev = { BLE_KB_NONE, 0 };

        if (code == 0x39) { capsLock = !capsLock; }
        else if (code == 0x29) { ev.type = BLE_KB_ESCAPE; }
        else if (ctrl && code == 0x0F) { ev.type = BLE_KB_URL_FOCUS; }  // Ctrl+L
        else if (code == 0x52) { ev.type = BLE_KB_SCROLL_UP; }
        else if (code == 0x51) { ev.type = BLE_KB_SCROLL_DOWN; }
        else if (code == 0x50) { ev.type = BLE_KB_CURSOR_LEFT; }
        else if (code == 0x4F) { ev.type = BLE_KB_CURSOR_RIGHT; }
        else if (code == 0x28) { ev.type = BLE_KB_ENTER; }
        else if (code == 0x2A) { ev.type = BLE_KB_BACKSPACE; }
        else {
            char c = hidToAscii(code, shifted, capsLock);
            if (c) { ev.type = BLE_KB_CHAR; ev.ch = c; }
        }

        if (ev.type != BLE_KB_NONE) rb_push(ev);
    }
}

// --- BLE client + reconnect ---
static NimBLEClient*  bleClient   = nullptr;
static NimBLEAddress  bondedAddr;
static bool           hasBonded   = false;
static volatile bool  connected   = false;
static volatile bool  wantConnect = false;

volatile bool g_ble_kb_scanning = false;

static bool doConnect(const NimBLEAddress& addr, uint8_t connectTimeoutSec = 15);
static void setupScan();

static TaskHandle_t reconnectTaskHandle = nullptr;

static void reconnectTask(void*) {
    // Wait for WiFi to settle and initial page loads to complete before scanning
    vTaskDelay(pdMS_TO_TICKS(30000));

    for (;;) {
        if (connected) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        // Scan with BT coex priority.
        // g_ble_kb_scanning suppresses the WiFi-lost banner in ui_task.
        g_ble_kb_scanning = true;
        esp_coex_preference_set(ESP_COEX_PREFER_BT);
        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        wantConnect = false;
        scan->start(10, false);
        for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        scan->stop();
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        g_ble_kb_scanning = false;

        if (wantConnect && !connected) {
            wantConnect = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            doConnect(bondedAddr, 15);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));  // 30s between attempts
    }
}

class ClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int) override {
        connected = false;
        Serial.println("[BLE KB] disconnected");
    }
    void onPassKeyEntry(NimBLEConnInfo&) override {}
    void onAuthenticationComplete(NimBLEConnInfo&) override {}
    void onConfirmPasskey(NimBLEConnInfo& info, uint32_t) override {
        NimBLEDevice::injectConfirmPasskey(info, true);
    }
};

static bool subscribeHID(NimBLEClient* client) {
    NimBLERemoteService* svc = client->getService(HID_SVC_UUID);
    if (!svc) {
        vTaskDelay(pdMS_TO_TICKS(600));
        client->getServices(true);
        svc = client->getService(HID_SVC_UUID);
    }
    if (!svc) { Serial.println("[BLE KB] ERR: HID service not found"); return false; }

    const auto& chars = svc->getCharacteristics(true);
    bool subscribedAny = false;
    for (auto* c : chars) {
        if (c->getUUID() == HID_RPT_UUID) {
            bool subOk = false;
            for (int t = 0; t < 5; t++) {
                if (c->subscribe(true, hidNotifyCB, true)) {
                    Serial.println("[BLE KB] subscribed to HID report");
                    subOk = true; subscribedAny = true; break;
                }
                Serial.printf("[BLE KB] subscribe rejected, retry %d/5\n", t + 1);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            if (!subOk) Serial.println("[BLE KB] ERR: gave up subscribing");
        }
    }
    return subscribedAny;
}

static bool doConnect(const NimBLEAddress& addr, uint8_t connectTimeoutSec) {
    if (bleClient) {
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
    }
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCB(), false);
    bleClient->setConnectTimeout(connectTimeoutSec);
    Serial.printf("[BLE KB] connecting to %s...\n", addr.toString().c_str());

    bool connectedOk = false;
    for (int i = 0; i < 3; i++) {
        if (bleClient->connect(addr)) { connectedOk = true; break; }
        Serial.printf("[BLE KB] attempt %d failed, retrying...\n", i + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!connectedOk) {
        Serial.println("[BLE KB] all connect attempts failed");
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
        return false;
    }

    bleClient->secureConnection();
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!subscribeHID(bleClient)) {
        bleClient->disconnect();
        return false;
    }

    bleClient->setConnectionParams(80, 80, 30, 1000);
    connected = true;
    Serial.println("[BLE KB] connected OK");
    return true;
}

class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        bool isHID = dev->isAdvertisingService(HID_SVC_UUID);
        bool isKB  = dev->getName().find("Keyboard") != std::string::npos ||
                     dev->getName().find("keyboard") != std::string::npos;
        uint8_t aType = dev->getAdvType();
        bool isDirect = (aType == 1 || aType == 5);

        if (!isHID && !isKB && !isDirect) return;

        Serial.printf("[BLE scan] %s name='%s' HID=%d KB=%d isDirect=%d\n",
            dev->getAddress().toString().c_str(), dev->getName().c_str(),
            isHID, isKB, isDirect);

        NimBLEDevice::getScan()->stop();
        bondedAddr = dev->getAddress();
        if (!hasBonded) {
            hasBonded = true;
            saveBondedAddress(bondedAddr);
            Serial.println("[BLE scan] new keyboard — address saved");
        }
        wantConnect = true;
    }
};
static ScanCB gScanCB;

static void setupScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&gScanCB, false);
    scan->setActiveScan(true);
    scan->setInterval(160);  // 100ms
    scan->setWindow(160);    // 100% duty — PREFER_BT coex gives us the radio
}

void ble_kb_init() {
    rb_mutex = xSemaphoreCreateMutex();

    Serial.printf("[BLE KB] free heap before NimBLE: %u internal\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    NimBLEDevice::init("");
    NimBLEDevice::setSecurityAuth(true, false, false);
    Serial.printf("[BLE KB] NimBLE init done, free heap after: %u internal\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    mbedtls_platform_set_calloc_free(tls_psram_calloc, tls_psram_free);

    bondedAddr = loadBondedAddress(hasBonded);
    Serial.printf("[BLE KB] hasBonded=%d\n", hasBonded);
    if (!hasBonded) Serial.println("[BLE KB] no bonded keyboard — put keyboard in pairing mode");

    // Initial scan with BT coex priority (WiFi idle at this point — not yet associated)
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    setupScan();
    NimBLEScan* scan = NimBLEDevice::getScan();
    wantConnect = false;
    scan->start(0, false);
    for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    scan->stop();
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (wantConnect && !connected) {
        wantConnect = false;
        doConnect(bondedAddr, 15);
    }
    // Reconnect task started separately after WiFi connects (ble_kb_start_reconnect)
}

void ble_kb_start_reconnect() {
    xTaskCreate(reconnectTask, "ble_recon", 2048, nullptr, 1, &reconnectTaskHandle);
}

bool ble_kb_connected() {
    return connected;
}

// Disconnect BLE keyboard before each fetch so WiFi gets 100% radio.
// Suspend the reconnect task immediately (wherever it is), stop any active scan,
// then drop the BLE connection. vTaskResume on after_fetch wakes the task from
// whatever delay it was in so it reconnects promptly once the fetch completes.
void ble_kb_before_fetch() {
    if (reconnectTaskHandle) vTaskSuspend(reconnectTaskHandle);
    NimBLEDevice::getScan()->stop();
    g_ble_kb_scanning = false;
    if (connected && bleClient) {
        bleClient->disconnect();
        connected = false;
    }
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
}

void ble_kb_after_fetch() {
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (reconnectTaskHandle) vTaskResume(reconnectTaskHandle);
}

#endif // BLE_KEYBOARD
