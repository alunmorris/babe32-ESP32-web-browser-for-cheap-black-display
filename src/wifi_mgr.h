/*
 * Multi-AP WiFi manager: 10 slots in NVS, scan-and-match
 * Written by Alun Morris and Claude Code
 *
 * 060326 Multi-AP WiFi manager: 10 slots in NVS, scan-and-match
 */
#pragma once
#include <stdbool.h>

#define WIFI_MAX_APS    10
#define WIFI_SSID_LEN   64
#define WIFI_PASS_LEN   64
#define WIFI_CONNECT_TIMEOUT_MS 10000

void wifi_mgr_init();            // load APs from NVS
bool wifi_mgr_connect();         // scan + connect to first known AP
void wifi_mgr_add_ap(const char *ssid, const char *pass); // save new AP
bool wifi_mgr_is_connected();
