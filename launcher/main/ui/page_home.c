// page_home.c - the start page: Tulip Studio's apps (each on its own, or
// all together in the STUDIO), other app images, and how to transfer files.
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
#define COL(i) ((i) * (TILE_W + GAP))

// a tile: what it launches
typedef struct {
    const char *path;   // the .bin
    const char *mode;   // Tulip Studio's boot mode: "" the Studio, or one app; NULL another image
    const char *title;
} tile_t;

static struct {
    lv_obj_t *grid, *empty, *xfer1, *xfer2, *pin;
    app_entry_t apps[MAX_APPS];
    int n;
    app_manifest_t man;   // the apps in studio.bin
    app_manifest_entry_t loaded[APP_MANIFEST_MAX];   // ... and those it loads from /files
    int nloaded;
    int studio;           // its index in apps[], -1 none
    tile_t tiles[APP_MANIFEST_MAX + MAX_APPS + 1];
    int ntiles;
} H;

static void tile_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= H.ntiles) return;
    const tile_t *t = &H.tiles[i];
    if (t->mode)
        ui_launch_as(t->path, t->mode, t->title);
    else
        ui_launch(t->path);
}

static const char *const ROLE[] = {"SEQUENCER", "DRUM MACHINE", "SYNTH"};
static const uint8_t ROLE_C[] = {TH_CYAN, TH_ORANGE, TH_GREEN};

// Tiles sit in a scrolling grid, so they act on a completed tap
// (SHORT_CLICKED), never on the press that might start a scroll.
static lv_obj_t *tile(int x, int y, int w, int h, const char *path, const char *mode,
                      const char *title) {
    int i = H.ntiles++;
    H.tiles[i] = (tile_t){path, mode, title};
    lv_obj_t *t = th_btn(H.grid, "", x, y, w, h, NULL, NULL);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_delete(lv_obj_get_child(t, 0));   // tiles lay out their own labels
    return t;
}

static void name_text(lv_obj_t *t, const char *s) {
    lv_obj_t *nm = th_text(t, s, 14, 10, TH_WHITE, &lv_font_montserrat_20);
    lv_obj_set_width(nm, TILE_W - 28);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
}

// the Studio's apps: 3 cells beside the STUDIO tile for two rows, then 4 a row
static void cell_pos(int cell, int *x, int *y) {
    int c, r;
    if (cell < 6) {
        c = 1 + cell % 3;
        r = cell / 3;
    } else {
        c = (cell - 6) % 4;
        r = 2 + (cell - 6) / 4;
    }
    *x = COL(c);
    *y = r * (TILE_H + GAP);
}

// returns the height used
static int studio_tiles(void) {
    const app_entry_t *st = &H.apps[H.studio];
    lv_obj_t *t = tile(0, 0, TILE_W, 2 * TILE_H + GAP, st->path, "", "TULIP STUDIO");
    lv_obj_set_style_border_color(t, th_c(TH_ORANGE), 0);
    name_text(t, "STUDIO");
    th_text(t, "ALL THE APPS TOGETHER", 14, 42, TH_ORANGE, &lv_font_montserrat_12);
    th_text(t,
            "on one clock: switch on the\napps you want, route the\nsequencers to the synths,\n"
            "mix them, save it as a patch",
            14, 70, TH_FG, &lv_font_montserrat_12);
    char b[64];
    snprintf(b, sizeof b, "v%s   %lu KB", st->info.version, (unsigned long)(st->size / 1024));
    th_text(t, b, 14, 2 * TILE_H + GAP - 28, TH_DIM, &lv_font_montserrat_12);
    // each app on its own: sequencers, then drum machines, then synths
    int cell = 0;
    for (int role = APP_ROLE_SEQUENCER; role <= APP_ROLE_SYNTH; role++)
        for (int i = 0; i < H.man.count + H.nloaded; i++) {
            bool loaded = i >= H.man.count;
            const app_manifest_entry_t *e = loaded ? &H.loaded[i - H.man.count] : &H.man.app[i];
            if (e->role != role) continue;
            int x, y;
            cell_pos(cell++, &x, &y);
            t = tile(x, y, TILE_W, TILE_H, st->path, e->name, e->title);
            name_text(t, e->title);
            th_text(t, ROLE[role], 14, 38, ROLE_C[role], &lv_font_montserrat_12);
            // from /files/studio/apps, not built in: an experiment
            if (loaded) th_text(t, "LOADED", TILE_W - 66, 14, TH_DIM, &lv_font_montserrat_12);
            lv_obj_t *bl = th_text(t, e->blurb, 14, 54, TH_DIM, &lv_font_montserrat_12);
            lv_obj_set_size(bl, TILE_W - 24, 46);
            lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
        }
    int rows = cell <= 6 ? 2 : 2 + (cell - 6 + 3) / 4;
    return rows * (TILE_H + GAP);
}

// an app Tulip Studio holds now (an older install's metroseq.bin, gintonix.bin
// ... are superseded by it)
static bool in_studio(const char *name) {
    if (!strcmp(name, "studio")) return true;
    for (int j = 0; H.studio >= 0 && j < H.man.count; j++)
        if (!strcmp(name, H.man.app[j].name)) return true;
    return false;
}

static void build_tiles(void) {
    lv_obj_clean(H.grid);
    H.ntiles = 0;
    H.n = apps_scan(H.apps, MAX_APPS);
    H.studio = -1;
    for (int i = 0; i < H.n && H.studio < 0; i++)
        if (!strcmp(H.apps[i].info.name, "studio") && apps_read_manifest(H.apps[i].path, &H.man))
            H.studio = i;
    H.nloaded = 0;
    if (H.studio >= 0) {
        app_manifest_entry_t found[APP_MANIFEST_MAX];
        int nf = apps_loaded(found, APP_MANIFEST_MAX);
        // the Studio holds 16 apps at most, and skips a name it has built in
        for (int i = 0; i < nf && H.man.count + H.nloaded < APP_MANIFEST_MAX; i++) {
            bool dup = false;
            for (int j = 0; j < H.man.count; j++) dup |= !strcmp(found[i].name, H.man.app[j].name);
            if (!dup) H.loaded[H.nloaded++] = found[i];
        }
    }
    int y = H.studio >= 0 ? studio_tiles() : 0;
    // anything else in /files/apps
    int k = 0;
    for (int i = 0; i < H.n; i++) {
        const app_entry_t *a = &H.apps[i];
        if (i == H.studio || (H.studio >= 0 && in_studio(a->info.name))) continue;
        if (k == 0 && H.studio >= 0) {
            th_text(H.grid, "OTHER APPS", 2, y + 4, TH_DIM, &lv_font_montserrat_14);
            y += 28;
        }
        lv_obj_t *t = tile(COL(k % 4), y + (k / 4) * (TILE_H + GAP), TILE_W, TILE_H, a->path, NULL,
                           NULL);
        name_text(t, a->info.name);
        char b[64];
        snprintf(b, sizeof b, "v%s   %lu KB", a->info.version, (unsigned long)(a->size / 1024));
        th_text(t, b, 14, 76, TH_DIM, &lv_font_montserrat_12);
        th_text(t, a->slot >= 0 ? "READY" : "INSTALLS FIRST", TILE_W - 120, 76,
                a->slot >= 0 ? TH_GREEN : TH_ORANGE, &lv_font_montserrat_12);
        k++;
    }
    if (H.ntiles)
        lv_obj_add_flag(H.empty, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(H.empty, LV_OBJ_FLAG_HIDDEN);
}

void home_create(lv_obj_t *p) {
    th_title(p, "APPS", 16, 14, TH_ORANGE);
    th_text(p, "tap an app to run it on its own - or STUDIO to run them together", 96, 19, TH_DIM,
            &lv_font_montserrat_14);
    H.grid = th_box(p, 16, 52, SCR_W - 32, 336);
    lv_obj_add_flag(H.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(H.grid, LV_DIR_VER);
    H.empty = th_text(p,
                      "No apps yet.\n\nUpload studio.bin (or any app .bin) from a browser - see "
                      "TRANSFER below -\nor copy it into /files/apps.",
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
                 "tools/tulip.py push studio.bin --launch",
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
