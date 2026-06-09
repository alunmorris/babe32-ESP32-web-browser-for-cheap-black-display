/*
 * NVS-backed multi-AP WiFi with scan-and-match
 * Written by Alun Morris and Claude Code
 *
 * 060326 NVS-backed multi-AP WiFi with scan-and-match
 * 190326 Remove WiFiManager portal (replaced by wifi_setup.cpp)
 */
#include "wifi_mgr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

static Preferences prefs;
static char ssids[WIFI_MAX_APS][WIFI_SSID_LEN];
static char passes[WIFI_MAX_APS][WIFI_PASS_LEN];
static int  ap_count = 0;

static const char *NVS_NS   = "wifimgr";
static const char *NVS_CNT  = "count";

static String ssid_key(int i) { return "ssid" + String(i); }
static String pass_key(int i) { return "pass" + String(i); }

void wifi_mgr_init() {
    prefs.begin(NVS_NS, false);
    ap_count = prefs.getInt(NVS_CNT, 0);
    for (int i = 0; i < ap_count; i++) {
        String s = prefs.getString(ssid_key(i).c_str(), "");
        String p = prefs.getString(pass_key(i).c_str(), "");
        strncpy(ssids[i], s.c_str(), WIFI_SSID_LEN - 1);
        strncpy(passes[i], p.c_str(), WIFI_PASS_LEN - 1);
    }
    prefs.end();
    Serial.printf("WiFi: loaded %d saved APs\n", ap_count);
}

bool wifi_mgr_connect() {
    if (ap_count == 0) return false;

    Serial.println("WiFi: scanning...");
    int n = WiFi.scanNetworks();
    Serial.printf("WiFi: found %d networks\n", n);

    for (int s = 0; s < n; s++) {
        String found_ssid = WiFi.SSID(s);
        for (int k = 0; k < ap_count; k++) {
            if (found_ssid == ssids[k]) {
                Serial.printf("WiFi: matching AP '%s', connecting...\n", ssids[k]);
                WiFi.begin(ssids[k], passes[k]);
                uint32_t t = millis();
                while (WiFi.status() != WL_CONNECTED) {
                    if (millis() - t > WIFI_CONNECT_TIMEOUT_MS) {
                        Serial.println("WiFi: connect timeout");
                        WiFi.disconnect();
                        break;
                    }
                    delay(100);
                }
                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf("WiFi: connected, IP=%s\n",
                                  WiFi.localIP().toString().c_str());
                    WiFi.scanDelete();
                    return true;
                }
            }
        }
    }
    WiFi.scanDelete();
    return false;
}

void wifi_mgr_add_ap(const char *ssid, const char *pass) {
    // Check if SSID already exists — update password
    for (int i = 0; i < ap_count; i++) {
        if (strcmp(ssids[i], ssid) == 0) {
            strncpy(passes[i], pass, WIFI_PASS_LEN - 1);
            prefs.begin(NVS_NS, false);
            prefs.putString(pass_key(i).c_str(), pass);
            prefs.end();
            Serial.printf("WiFi: updated password for '%s'\n", ssid);
            return;
        }
    }
    // New AP — evict oldest if full
    if (ap_count >= WIFI_MAX_APS) {
        for (int i = 0; i < WIFI_MAX_APS - 1; i++) {
            memcpy(ssids[i], ssids[i+1], WIFI_SSID_LEN);
            memcpy(passes[i], passes[i+1], WIFI_PASS_LEN);
        }
        ap_count = WIFI_MAX_APS - 1;
    }
    strncpy(ssids[ap_count], ssid, WIFI_SSID_LEN - 1);
    strncpy(passes[ap_count], pass, WIFI_PASS_LEN - 1);
    ap_count++;

    prefs.begin(NVS_NS, false);
    prefs.putInt(NVS_CNT, ap_count);
    for (int i = 0; i < ap_count; i++) {
        prefs.putString(ssid_key(i).c_str(), ssids[i]);
        prefs.putString(pass_key(i).c_str(), passes[i]);
    }
    prefs.end();
    Serial.printf("WiFi: saved AP '%s' (total: %d)\n", ssid, ap_count);
}


bool wifi_mgr_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}
