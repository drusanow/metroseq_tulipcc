// page_wifi.c - Wi-Fi on/off, scan + join, web transfer on/off + PIN.
#include <stdio.h>
#include <string.h>

#include "dialog.h"
#include "lsettings.h"
#include "theme.h"
#include "ui.h"
#include "web.h"
#include "wifi.h"

static struct {
    lv_obj_t *b_wifi, *status, *saved, *b_forget, *b_scan, *list, *scan_msg;
    lv_obj_t *b_web, *url1, *url2, *pin;
    wifi_ap_t aps[20];
    int naps;
    bool was_scanning;
    char join_ssid[33];
} W;

static void join_with(const char *pass, void *ud) {
    wifi_connect(W.join_ssid, pass);
    ui_toast("connecting...");
}

static void ap_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= W.naps) return;
    snprintf(W.join_ssid, sizeof W.join_ssid, "%s", W.aps[i].ssid);
    if (!W.aps[i].secure) {
        join_with("", NULL);
        return;
    }
    char t[64];
    snprintf(t, sizeof t, "PASSWORD FOR %s", W.join_ssid);
    dlg_prompt(t, "", "JOIN", true, join_with, NULL);
}

static void draw_list(void) {
    lv_obj_clean(W.list);
    for (int i = 0; i < W.naps; i++) {
        lv_obj_t *r = th_btn(W.list, "", 0, i * 48, 560, 44, NULL, NULL);
        lv_obj_add_event_cb(r, ap_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_delete(lv_obj_get_child(r, 0));
        lv_obj_t *n = th_text(r, W.aps[i].ssid, 14, 12, TH_WHITE, &lv_font_montserrat_16);
        lv_obj_set_width(n, 380);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        int bars = W.aps[i].rssi > -55 ? 4 : W.aps[i].rssi > -65 ? 3 : W.aps[i].rssi > -75 ? 2 : 1;
        static const char *const B[5] = {"", "|", "||", "|||", "||||"};
        char right[32];
        snprintf(right, sizeof right, "%s  %-4s %d dBm", W.aps[i].secure ? LV_SYMBOL_EYE_CLOSE : " ",
                 B[bars], W.aps[i].rssi);
        lv_obj_t *rl = th_text(r, right, 0, 14, TH_DIM, &lv_font_montserrat_14);
        lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -14, 0);
    }
}

static void scan_cb(lv_event_t *e) {
    if (wifi_scan_start()) {
        lv_label_set_text(W.scan_msg, "scanning...");
        W.was_scanning = true;
    } else {
        ui_toast("can't scan right now - try again");
    }
}

static void wifi_toggle(lv_event_t *e) { wifi_set_enabled(!g_ls.wifi_on); }

static void forget_ok(void *ud) { wifi_forget(); }
static void forget_cb(lv_event_t *e) {
    if (!g_ls.ssid[0]) return;
    dlg_confirm("FORGET NETWORK", "Forget the saved network and its password?", "FORGET", true,
                forget_ok, NULL);
}

static void web_toggle(lv_event_t *e) {
    g_ls.web_on = !g_ls.web_on;
    lsettings_save();
    if (g_ls.web_on)
        web_start();
    else
        web_stop();
}

static void pin_cb(lv_event_t *e) {
    lsettings_new_pin();
    lsettings_save();
    ui_toast("new PIN set");
}

void wifi_page_create(lv_obj_t *p) {
    th_title(p, "WI-FI", 16, 14, TH_ORANGE);
    W.b_wifi = th_btn(p, "WI-FI ON", 16, 52, 150, 42, wifi_toggle, NULL);
    W.status = th_text(p, "", 182, 64, TH_FG, &lv_font_montserrat_16);
    W.saved = th_text(p, "", 16, 110, TH_DIM, &lv_font_montserrat_14);
    W.b_forget = th_btn(p, "FORGET", 456, 102, 120, 34, forget_cb, NULL);

    th_hline(p, 16, 148, 560);
    th_title(p, "NETWORKS", 16, 160, TH_ORANGE);
    W.b_scan = th_btn(p, LV_SYMBOL_REFRESH "  SCAN", 456, 156, 120, 36, scan_cb, NULL);
    W.scan_msg = th_text(p, "tap SCAN, then tap your network", 160, 166, TH_DIM, &lv_font_montserrat_14);
    W.list = th_box(p, 16, 204, 576, PAGE_H - 204 - 12);
    lv_obj_add_flag(W.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(W.list, LV_DIR_VER);

    int x = 640;
    th_title(p, "WEB TRANSFER", x, 14, TH_CYAN);
    W.b_web = th_btn(p, "WEB ON", x, 52, 150, 42, web_toggle, NULL);
    W.url1 = th_text(p, "", x, 110, TH_WHITE, &lv_font_montserrat_16);
    W.url2 = th_text(p, "", x, 136, TH_DIM, &lv_font_montserrat_14);
    th_text(p, "PIN", x, 176, TH_DIM, &lv_font_montserrat_14);
    W.pin = th_text(p, "", x, 196, TH_ORANGE, &lv_font_montserrat_28);
    lv_obj_set_style_text_letter_space(W.pin, 6, 0);
    th_btn(p, "NEW PIN", x + 180, 196, 120, 36, pin_cb, NULL);
    lv_obj_t *help = th_text(p,
                             "Open the address in a browser on the same network and enter the "
                             "PIN. Drag .bin apps or any files onto the page; apps go in "
                             "/files/apps.\n\nFrom a terminal:\ntools/tulip.py --pin PIN push "
                             "build/app.bin --launch",
                             x, 252, TH_DIM, &lv_font_montserrat_14);
    lv_obj_set_width(help, SCR_W - x - 16);
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
}

void wifi_page_show(void) { wifi_page_poll(); }

void wifi_page_poll(void) {
    static const char *const ST[] = {"off", "on - pick a network below", "connecting to %s...",
                                     "connected to %s", "can't reach %s - retrying"};
    char b[96];
    snprintf(b, sizeof b, ST[g_wifi.state], g_wifi.ssid);
    if (g_wifi.state == WIFI_ST_CONNECTED) {
        char c[128];
        snprintf(c, sizeof c, "%s   %d dBm", b, g_wifi.rssi);
        lv_label_set_text(W.status, c);
    } else {
        lv_label_set_text(W.status, b);
    }
    th_btn_set_text(W.b_wifi, g_ls.wifi_on ? "WI-FI ON" : "WI-FI OFF");
    th_btn_set_active(W.b_wifi, g_ls.wifi_on);
    snprintf(b, sizeof b, g_ls.ssid[0] ? "saved network:  %s" : "no network saved", g_ls.ssid);
    lv_label_set_text(W.saved, b);
    th_btn_set_enabled(W.b_forget, g_ls.ssid[0] != 0);

    bool busy;
    W.naps = wifi_scan_results(W.aps, 20, &busy);
    if (W.was_scanning && !busy) {
        W.was_scanning = false;
        lv_label_set_text(W.scan_msg, W.naps ? "tap a network to join it" : "no networks found");
        draw_list();
    }

    bool web = web_running();
    th_btn_set_text(W.b_web, web ? "WEB ON" : "WEB OFF");
    th_btn_set_active(W.b_web, web);
    if (web && g_wifi.state == WIFI_ST_CONNECTED) {
        lv_label_set_text(W.url1, "http://tulip.local");
        snprintf(b, sizeof b, "or  http://%s", g_wifi.ip);
        lv_label_set_text(W.url2, b);
    } else {
        lv_label_set_text(W.url1, web ? "waiting for Wi-Fi" : "web transfer is off");
        lv_label_set_text(W.url2, "");
    }
    lv_label_set_text(W.pin, g_ls.pin);
}
