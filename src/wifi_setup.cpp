/*
 * Self-contained WiFi setup: AP scan, password entry, NVS credential store
 * Written by Alun Morris and Claude Code
 *
 * 190326 Self-contained WiFi setup: AP scan, password entry, NVS credential store
 * NVS namespace "wifimgr" matches wifi_mgr.cpp — existing saved APs are preserved.
 */
#include "wifi_setup.h"
#include "ui_header.h"
#include "ui_buttons.h"
#include "page_renderer.h"
#include "dbglog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <lvgl.h>

extern bool lvgl_lock(uint32_t ms);
extern void lvgl_unlock();

// --- NVS credential store ---
#define NVS_NS       "wifimgr"
#define MAX_CREDS    10
#define CONNECT_MS   12000

static char s_cred_ssid[MAX_CREDS][33];
static char s_cred_pass[MAX_CREDS][64];
static int  s_cred_n = 0;

static void creds_load() {
    Preferences p; p.begin(NVS_NS, true);
    s_cred_n = p.getInt("count", 0);
    if (s_cred_n > MAX_CREDS) s_cred_n = MAX_CREDS;
    for (int i = 0; i < s_cred_n; i++) {
        char sk[8], pk[8];
        snprintf(sk, 8, "ssid%d", i); snprintf(pk, 8, "pass%d", i);
        String ss = p.getString(sk, ""), ps = p.getString(pk, "");
        strncpy(s_cred_ssid[i], ss.c_str(), 32); s_cred_ssid[i][32] = 0;
        strncpy(s_cred_pass[i], ps.c_str(), 63); s_cred_pass[i][63] = 0;
    }
    p.end();
    dbg("wifi_setup: loaded %d saved APs", s_cred_n);
}

static void creds_save() {
    Preferences p; p.begin(NVS_NS, false);
    p.putInt("count", s_cred_n);
    for (int i = 0; i < s_cred_n; i++) {
        char sk[8], pk[8];
        snprintf(sk, 8, "ssid%d", i); snprintf(pk, 8, "pass%d", i);
        p.putString(sk, s_cred_ssid[i]); p.putString(pk, s_cred_pass[i]);
    }
    p.end();
}

// Insert/update credential at slot 0 (MRU). Shifts others down, caps at MAX_CREDS.
static void creds_insert(const char *ssid, const char *pass) {
    for (int i = 0; i < s_cred_n; i++) {
        if (strcmp(s_cred_ssid[i], ssid) == 0) {
            for (int j = i; j < s_cred_n - 1; j++) {
                memcpy(s_cred_ssid[j], s_cred_ssid[j+1], 33);
                memcpy(s_cred_pass[j], s_cred_pass[j+1], 64);
            }
            s_cred_n--;
            break;
        }
    }
    int cap = s_cred_n < MAX_CREDS ? s_cred_n : MAX_CREDS - 1;
    for (int i = cap; i > 0; i--) {
        memcpy(s_cred_ssid[i], s_cred_ssid[i-1], 33);
        memcpy(s_cred_pass[i], s_cred_pass[i-1], 64);
    }
    strncpy(s_cred_ssid[0], ssid, 32); s_cred_ssid[0][32] = 0;
    strncpy(s_cred_pass[0], pass, 63); s_cred_pass[0][63] = 0;
    s_cred_n = s_cred_n < MAX_CREDS ? s_cred_n + 1 : MAX_CREDS;
    creds_save();
}

// Returns true if SSID found. Fills passOut (64 bytes) if non-null.
static bool creds_find(const char *ssid, char *passOut) {
    for (int i = 0; i < s_cred_n; i++) {
        if (strcmp(s_cred_ssid[i], ssid) == 0) {
            if (passOut) { strncpy(passOut, s_cred_pass[i], 63); passOut[63] = 0; }
            return true;
        }
    }
    return false;
}

// --- AP scan results ---
#define MAX_APS 12
static char s_ap_ssid[MAX_APS][33];
static int  s_ap_rssi[MAX_APS];
static int  s_ap_n = 0;

// --- State machine ---
// WS_PRE_SCAN / WS_PRE_PASS: set by callbacks; tick() rebuilds UI (safe outside callback)
enum WsState { WS_IDLE, WS_PRE_SCAN, WS_SCANNING, WS_SHOW_LIST,
               WS_PRE_PASS, WS_ENTER_PASS, WS_PRE_CONNECT, WS_CONNECTING, WS_FAILED };

static WsState  s_state        = WS_IDLE;
static bool     s_scan_started = false;
static char     s_sel_ssid[33] = "";
static char     s_sel_pass[64] = "";
static uint32_t s_connect_ms   = 0;

static lv_obj_t *s_content         = nullptr;
static lv_obj_t *s_pass_ta         = nullptr;
static wifi_setup_done_cb_t s_done_cb          = nullptr;
static void (*s_field_focus_cb)(lv_obj_t *ta)  = nullptr;

// --- Forward declarations ---
static void build_scan_screen();
static void build_ap_list();
static void build_password_screen();
static void build_connecting_screen();
static void build_failed_screen();

// --- UI helpers ---
static lv_color_t tc() { return lv_color_hex(page_is_inverted() ? 0x1A1A1A : 0xE0E0E0); }
static lv_color_t gc() { return lv_color_hex(page_is_inverted() ? 0x008800 : 0x00C853); }
static lv_color_t rc() { return lv_color_hex(page_is_inverted() ? 0xCC0000 : 0xFF4444); }
static lv_color_t bc() { return lv_color_hex(page_is_inverted() ? 0xCCCCCC : 0x0F3460); }

static void reset_content() {
    lv_obj_clean(s_content);
    s_pass_ta = nullptr;
    lv_obj_set_style_bg_color(s_content,
        lv_color_hex(page_is_inverted() ? 0xF0F0F0 : 0x1A1A2E), 0);
    lv_obj_set_style_pad_all(s_content, 8, 0);
    lv_obj_set_style_pad_gap(s_content, 5, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *add_label(const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *lbl = lv_label_create(s_content);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

// --- Callbacks ---
// Callbacks only set state — tick() does UI rebuild to avoid
// modifying LVGL objects while inside their own callbacks.
static void rescan_cb(lv_event_t *) {
    s_state = WS_PRE_SCAN;
    s_scan_started = false;
}

static void ap_select_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_n) return;
    strncpy(s_sel_ssid, s_ap_ssid[idx], 32); s_sel_ssid[32] = 0;
    s_sel_pass[0] = 0;
    creds_find(s_sel_ssid, s_sel_pass);  // pre-fill saved password if any
    s_state = WS_PRE_PASS;
}

static void connect_cb(lv_event_t *) {
    if (!s_pass_ta) return;
    const char *pw = lv_textarea_get_text(s_pass_ta);
    strncpy(s_sel_pass, pw ? pw : "", 63); s_sel_pass[63] = 0;
    s_state = WS_PRE_CONNECT;
}

static void back_to_scan_cb(lv_event_t *) {
    s_state = WS_PRE_SCAN;
    s_scan_started = false;
}

static void retry_cb(lv_event_t *) {
    s_state = WS_PRE_PASS;
}

// --- Screen builders (all called inside lvgl_lock) ---

static void build_scan_screen() {
    header_set_visible(true);
    header_set_url("");
    lv_obj_set_pos(s_content, 0, 30);
    lv_obj_set_height(s_content, LV_VER_RES - 30);
    reset_content();
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    add_label("Scanning for WiFi networks...", &lv_font_montserrat_16, tc());
}

static void build_ap_list() {
    reset_content();

    lv_obj_t *title = add_label(
        s_ap_n > 0 ? "Select WiFi network:" : "No networks found",
        &lv_font_montserrat_16, gc());
    lv_obj_set_style_pad_bottom(title, 4, 0);

    bool inv = page_is_inverted();
    for (int i = 0; i < s_ap_n; i++) {
        bool saved = creds_find(s_ap_ssid[i], nullptr);

        // Row: "* SSID  -65dBm" (saved) or "  SSID  -65dBm"
        char label[64];
        snprintf(label, sizeof(label), "%s %-24.24s %ddBm",
                 saved ? "*" : " ", s_ap_ssid[i], s_ap_rssi[i]);

        lv_obj_t *row = lv_label_create(s_content);
        lv_label_set_text(row, label);
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_style_text_color(row, lv_color_hex(inv ? 0x0066CC : 0x4FC3F7), 0);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_16, 0);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, bc(), 0);
        lv_obj_set_style_radius(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ap_select_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    create_flat_btn(s_content, "Rescan", 90, 28, rescan_cb, nullptr);
}

static void build_password_screen() {
    reset_content();
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    bool inv = page_is_inverted();

    add_label("WiFi Password", &lv_font_montserrat_16, gc());
    add_label(s_sel_ssid, &lv_font_montserrat_16, tc());

    // Password textarea
    s_pass_ta = lv_textarea_create(s_content);
    lv_obj_set_width(s_pass_ta, LV_PCT(100));
    lv_obj_set_height(s_pass_ta, 36);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, "Password...");
    if (s_sel_pass[0])
        lv_textarea_set_text(s_pass_ta, s_sel_pass);
    lv_obj_set_style_bg_color(s_pass_ta, lv_color_hex(inv ? 0xFFFFFF : 0x16213E), 0);
    lv_obj_set_style_text_color(s_pass_ta, lv_color_hex(inv ? 0x1A1A1A : 0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_pass_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_width(s_pass_ta, 1, 0);
    lv_obj_set_style_border_color(s_pass_ta, lv_color_hex(inv ? 0xCCCCCC : 0x0F3460), 0);
    lv_obj_set_style_radius(s_pass_ta, 2, 0);
    lv_obj_set_style_shadow_width(s_pass_ta, 0, 0);
    lv_obj_set_style_pad_all(s_pass_ta, 5, 0);
    lv_obj_set_style_bg_color(s_pass_ta, lv_color_hex(0x4FC3F7), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_pass_ta, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_pass_ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_radius(s_pass_ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_time(s_pass_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    // Tap textarea → show keyboard
    lv_obj_add_event_cb(s_pass_ta, [](lv_event_t *e) {
        if (s_field_focus_cb) s_field_focus_cb(lv_event_get_target(e));
    }, LV_EVENT_CLICKED, nullptr);

    // Buttons row
    lv_obj_t *row = lv_obj_create(s_content);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 8, 0);

    create_flat_btn(row, "Connect", 90, 32, connect_cb, nullptr);
    create_flat_btn(row, "Back",    70, 32, back_to_scan_cb, nullptr);

    // Auto-focus textarea so keyboard appears immediately
    if (s_field_focus_cb) s_field_focus_cb(s_pass_ta);
}

static void build_connecting_screen() {
    reset_content();
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to:\n%s", s_sel_ssid);
    add_label(msg, &lv_font_montserrat_16, tc());
}

static void build_failed_screen() {
    reset_content();
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_label("Connection failed", &lv_font_montserrat_16, rc());
    add_label(s_sel_ssid, &lv_font_montserrat_14, tc());

    lv_obj_t *row = lv_obj_create(s_content);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 12, 0);

    create_flat_btn(row, "Retry",   80, 32, retry_cb,        nullptr);
    create_flat_btn(row, "Rescan",  80, 32, back_to_scan_cb, nullptr);
}

// --- Public API ---

void wifi_setup_init(lv_obj_t *content, wifi_setup_done_cb_t on_done,
                     void (*field_focus_cb)(lv_obj_t *ta)) {
    s_content       = content;
    s_done_cb       = on_done;
    s_field_focus_cb = field_focus_cb;
    creds_load();
}

void wifi_setup_show() {
    // Called inside lvgl_lock from boot_menu button callback
    s_state        = WS_SCANNING;
    s_scan_started = false;
    s_pass_ta      = nullptr;
    build_scan_screen();
}

void wifi_setup_tick() {
    if (s_state == WS_IDLE) return;

    if (s_state == WS_PRE_SCAN) {
        if (lvgl_lock(50)) { build_scan_screen(); lvgl_unlock(); }
        s_state = WS_SCANNING;
        return;
    }

    if (s_state == WS_PRE_PASS) {
        if (lvgl_lock(50)) { build_password_screen(); lvgl_unlock(); }
        s_state = WS_ENTER_PASS;
        return;
    }

    if (s_state == WS_SCANNING) {
        if (!s_scan_started) {
            WiFi.disconnect();
            WiFi.scanNetworks(/*async=*/true);
            s_scan_started = true;
            return;
        }
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) return;

        // Build sorted AP list (top MAX_APS by RSSI)
        int total = (n > 0) ? n : 0;
        int indices[40];
        int cnt = (total < 40) ? total : 40;
        for (int i = 0; i < cnt; i++) indices[i] = i;
        for (int i = 0; i < cnt - 1; i++)
            for (int j = i + 1; j < cnt; j++)
                if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
                    int t = indices[i]; indices[i] = indices[j]; indices[j] = t;
                }
        s_ap_n = (cnt < MAX_APS) ? cnt : MAX_APS;
        for (int i = 0; i < s_ap_n; i++) {
            strncpy(s_ap_ssid[i], WiFi.SSID(indices[i]).c_str(), 32);
            s_ap_ssid[i][32] = 0;
            s_ap_rssi[i] = WiFi.RSSI(indices[i]);
        }
        WiFi.scanDelete();

        if (lvgl_lock(50)) { build_ap_list(); lvgl_unlock(); }
        s_state = WS_SHOW_LIST;
        return;
    }

    if (s_state == WS_PRE_CONNECT) {
        WiFi.begin(s_sel_ssid, s_sel_pass);
        s_connect_ms = millis();
        s_state = WS_CONNECTING;
        if (lvgl_lock(50)) { build_connecting_screen(); lvgl_unlock(); }
        return;
    }

    if (s_state == WS_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            creds_insert(s_sel_ssid, s_sel_pass);
            dbg("WiFi setup: connected to %s", s_sel_ssid);
            s_state = WS_IDLE;
            if (lvgl_lock(50)) {
                if (s_done_cb) s_done_cb();
                lvgl_unlock();
            }
            return;
        }
        if (millis() - s_connect_ms > CONNECT_MS) {
            dbg("WiFi setup: connect timeout for %s", s_sel_ssid);
            s_state = WS_FAILED;
            if (lvgl_lock(50)) { build_failed_screen(); lvgl_unlock(); }
        }
    }
}

bool wifi_setup_active() {
    return s_state != WS_IDLE;
}

lv_obj_t *wifi_setup_get_pass_ta() {
    return (s_state == WS_ENTER_PASS) ? s_pass_ta : nullptr;
}

void wifi_setup_kb_submit() {
    if (s_state != WS_ENTER_PASS || !s_pass_ta) return;
    const char *pw = lv_textarea_get_text(s_pass_ta);
    strncpy(s_sel_pass, pw ? pw : "", 63); s_sel_pass[63] = 0;
    s_state = WS_PRE_CONNECT;
}
