/*
 * Browser entry point: WiFi first, then UI task
 * Written by Alun Morris and Claude Code
 *
 * 060326 Browser entry point: WiFi first, then UI task
 * 110326 Force WiFi PHY init before display PSRAM DMA to avoid MMU fault
 * 120326 NTP time sync after WiFi connect
 * 190326 Remove portal fallback — WiFi setup handled by UI
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>
#include "wifi_mgr.h"
#include "ui_task.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("ESP32 Text Browser v1.0");

    // ESP32-S3 cache workaround: esp_wifi_start() → esp_phy_enable() flushes the
    // cache bus.  If PSRAM DMA is in-flight at that moment the MMU faults.
    // Fix: complete PHY init synchronously BEFORE any PSRAM DMA starts.
    WiFi.mode(WIFI_STA);
    esp_wifi_start();   // synchronous; PHY enabled on return
    Serial.println("WiFi PHY init done");

    // Safe to start display DMA now — PHY is already up
    ui_task_start();

    wifi_mgr_init();
    if (!wifi_mgr_connect()) {
        Serial.println("No known AP — use WiFi setup in the browser UI");
    }

    Serial.printf("WiFi: %s\n",
                  wifi_mgr_is_connected() ? "connected" : "failed");

    // NTP time sync — helps TLS certificate validation and general sanity
    if (wifi_mgr_is_connected()) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        Serial.print("NTP sync");
        struct tm ti;
        for (int i = 0; i < 10; i++) {
            if (getLocalTime(&ti, 500)) {
                Serial.printf(" OK (%04d-%02d-%02d %02d:%02d)\n",
                              ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                              ti.tm_hour, ti.tm_min);
                break;
            }
            Serial.print(".");
        }
    }
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
