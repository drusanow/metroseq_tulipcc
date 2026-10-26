// page_home.c - app tiles + how to transfer files.
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "lsettings.h"
#include "theme.h"
#include "ui.h"
#include "web.h"
#include "wifi.h"

#define TILE_W 236
#define TILE_H 104
#define GAP 16

static struct {
    lv_obj_t *grid, *empty, *xfer1, *xfer2, *pin;
    app_entry_t apps[MAX_APPS];
    int n;
} H;

static void tile_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < H.n) ui_launch(H.apps[i].path);
}

static void build_tiles(void) {
    lv_obj_clean(H.grid);
    H.n = apps_scan(H.apps, MAX_APPS);
    if (H.n == 0) {
        lv_obj_remove_flag(H.empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(H.empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < H.n; i++) {
        int x = (i % 4) * (TILE_W + GAP), y = (i / 4) * (TILE_H + GAP);
        // Tiles sit in a scrolling grid, so they act on a completed tap
        // (SHORT_CLICKED), never on the press that might start a scroll.
        lv_obj_t *t = th_btn(H.grid, "", x, y, TILE_W, TILE_H, NULL, NULL);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_obj_get_child(t, 0);
        lv_obj_delete(l);   // tiles lay out their own labels
        const app_entry_t *a = &H.apps[i];
        lv_obj_t *nm = th_text(t, a->info.name, 14, 12, TH_WHITE, &lv_font_montserrat_20);
        lv_obj_set_width(nm, TILE_W - 28);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        char b[64];
        snprintf(b, sizeof b, "v%s   %lu KB", a->info.version, (unsigned long)(a->size / 1024));
        th_text(t, b, 14, 44, TH_DIM, &lv_font_montserrat_14);
        th_text(t, a->slot >= 0 ? "READY" : "INSTALLS ON FIRST LAUNCH", 14, 72,
                a->slot >= 0 ? TH_GREEN : TH_ORANGE, &lv_font_montserrat_12);
    }
}

void home_create(lv_obj_t *p) {
    th_title(p, "APPS", 16, 14, TH_ORANGE);
    th_text(p, "tap an app to run it  -  apps are the .bin files in /files/apps", 96, 19, TH_DIM,
            &lv_font_montserrat_14);
    H.grid = th_box(p, 16, 52, SCR_W - 32, 336);
    lv_obj_add_flag(H.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(H.grid, LV_DIR_VER);
    H.empty = th_text(p,
                      "No apps yet.\n\nUpload a .bin from a browser (see TRANSFER below), or copy one "
                      "into /files/apps.",
                      16, 120, TH_DIM, &lv_font_montserrat_16);
    th_hline(p, 16, 398, SCR_W - 32);
    th_title(p, "TRANSFER", 16, 412, TH_CYAN);
    H.xfer1 = th_text(p, "", 16, 448, TH_FG, &lv_font_montserrat_16);
    H.xfer2 = th_text(p, "", 16, 476, TH_DIM, &lv_font_montserrat_14);
    th_text(p, "PIN", 800, 420, TH_DIM, &lv_font_montserrat_14);
    H.pin = th_text(p, "", 800, 440, TH_ORANGE, &lv_font_montserrat_28);
    lv_obj_set_style_text_letter_space(H.pin, 6, 0);
}

void home_show(void) {
    build_tiles();
    home_poll();
}

void home_poll(void) {
    char a[160], b[160];
    if (g_wifi.state == WIFI_ST_CONNECTED && web_running()) {
        snprintf(a, sizeof a, "Open  http://tulip.local  or  http://%s  in a browser", g_wifi.ip);
        snprintf(b, sizeof b,
                 "on the same network as the Tulip (%s). Or from a terminal:  "
                 "tools/tulip.py push app.bin --launch",
                 g_wifi.ssid);
    } else if (g_wifi.state == WIFI_ST_CONNECTED) {
        snprintf(a, sizeof a, "Wi-Fi is connected but web transfer is off.");
        snprintf(b, sizeof b, "Turn it on in the WI-FI page.");
    } else if (g_wifi.state == WIFI_ST_OFF || g_wifi.state == WIFI_ST_NO_NETWORK) {
        snprintf(a, sizeof a, "Join a Wi-Fi network to transfer apps and files from a browser.");
        snprintf(b, sizeof b, "Go to the WI-FI page, tap SCAN and pick your network.");
    } else {
        snprintf(a, sizeof a, "Connecting to %s ...", g_wifi.ssid);
        b[0] = 0;
    }
    lv_label_set_text(H.xfer1, a);
    lv_label_set_text(H.xfer2, b);
    lv_label_set_text(H.pin, g_ls.pin);
}
