// shell.c - screens, the Tulip-style app squares and the Studio home page.
//
// Each app gets its own LVGL screen (in PSRAM), built at start for the
// running apps and kept from then on, so switching is instant and an app's
// page state is still there when you come back. Hidden apps keep playing:
// only their screen is off.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "studio/widgets.h"
#include "studio_priv.h"
#include "tulip/display.h"
#include "tulip/settings.h"
#include "tulip/sys.h"
#include "tulip/touchcal_ui.h"

static lv_obj_t *s_scr[STUDIO_MAX_MODULES];
static lv_obj_t *s_home;
static int s_cur = -2;   // shown: module index, -1 = home
static bool s_leaving;

// --- switching ----------------------------------------------------------------------

static void build(int i);

// ON starts the app the first time (studio.c), and builds its screen: an
// app's screen polls its MIDI / CCs even while hidden
static void home_refresh(void);

static void set_on(int i, bool on) {
    studio_module_set_on(i, on);
    if (on) build(i);
}

void shell_set_open(int i, bool on) {
    if (i < 0 || i >= g_nmods) return;
    set_on(i, on);
    home_refresh();
}

static lv_obj_t *new_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, sw_c(SW_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, sw_c(SW_TEXT), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}


static void build(int i) {
    if (s_scr[i]) return;
    s_scr[i] = new_screen();
    if (g_mods[i]->ui_create) g_mods[i]->ui_create(s_scr[i]);
}

static void show(int i) {
    if (i == s_cur) return;
    int old = s_cur;
    if (old >= 0 && g_mods[old]->ui_show) g_mods[old]->ui_show(false);
    s_cur = i;
    if (i < 0) {
        home_refresh();
        lv_screen_load(s_home);
    } else {
        if (!studio_module_on(i)) set_on(i, true);
        build(i);
        lv_screen_load(s_scr[i]);
        if (g_mods[i]->ui_show) g_mods[i]->ui_show(true);
    }
    if (g_cfg.shown != i) {
        g_cfg.shown = (int8_t)i;
        cfg_changed();
    }
}

void studio_show_module(int i) {
    if (i >= 0 && i < g_nmods) show(i);
}

void studio_show_home(void) {
    if (studio_multi()) show(-1);
}

void studio_next_app(void) {
    // the running apps in order, then the home page, round and round
    int n = g_nmods;
    for (int step = 1; step <= n + 1; step++) {
        int k = s_cur + step;   // -1 (home) .. n-1
        k = ((k + 1) % (n + 1)) - 1;
        if (k == -1) {
            if (studio_multi()) {
                show(-1);
                return;
            }
            continue;
        }
        if (studio_module_on(k)) {
            show(k);
            return;
        }
    }
}

// --- leaving for the launcher ------------------------------------------------------------

static void leave_wait(lv_timer_t *t) {
    static int ticks;
    studio_clock_status_t st;
    studio_clock_status(&st);
    bool busy = st.playing;
    for (int i = 0; i < g_nmods; i++)
        if (g_started[i] && g_mods[i]->busy && g_mods[i]->busy()) busy = true;
    if (!busy || ++ticks > 50) tulip_exit_to_launcher();
}

void studio_exit_to_launcher(void) {
    if (s_leaving) return;
    s_leaving = true;
    studio_stop();
    lv_timer_create(leave_wait, 100, NULL);
}

// --- the app squares -----------------------------------------------------------------------

static void sq_next(void *ud) { studio_next_app(); }

static void sq_red(void *ud) {
    if (studio_multi() && s_cur >= 0)
        studio_show_home();
    else
        studio_exit_to_launcher();
}

static void sq_blue_long(lv_event_t *e) { studio_show_home(); }

lv_obj_t *studio_chrome(lv_obj_t *parent) {
    const int h = STUDIO_SQ, x = STUDIO_SQ_X, y = STUDIO_HDR_Y;
    int w = studio_multi() ? 2 * h + 6 : h;
    lv_obj_t *box = sw_box(lv_obj_get_screen(parent), x + (studio_multi() ? 0 : h + 6), y, w, h,
                           -1, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   // (children take the taps)
    if (studio_multi()) {
        lv_obj_t *b = sw_square(box, 0, 0, h, SW_SQ_BLUE, LV_SYMBOL_NEXT, sq_next, NULL);
        lv_obj_add_event_cb(b, sq_blue_long, LV_EVENT_LONG_PRESSED, NULL);
        sw_square(box, h + 6, 0, h, SW_SQ_RED, LV_SYMBOL_HOME, sq_red, NULL);
    } else if (tulip_under_launcher()) {
        sw_square(box, 0, 0, h, SW_SQ_RED, LV_SYMBOL_HOME, sq_red, NULL);
    } else {
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);   // flashed on its own: nowhere to go
    }
    return box;
}

// --- the Studio home page ------------------------------------------------------------------

// The apps, 4 tiles to a page (the arrows page through more): each says
// what it is and what it takes from MIDI IN, has its ON / OFF and SHOW, and
// its mixer strip. Every app is OFF when the Studio starts.
#define TILE_GAP 8
#define TILES_Y 86
#define TILE_COLS 4
#define TILE_W ((1000 - (TILE_COLS - 1) * TILE_GAP) / TILE_COLS)
#define TILE_H 250
#define ROUTE_Y 350
#define ROUTE_ROWS 4   // routing rows to a page
#define MAX_ROWS (STUDIO_MAX_MODULES * 2)

static struct {
    lv_obj_t *play, *bpm, *clk_int, *clk_midi, *clk_out, *mmc, *status;
    lv_obj_t *tile[STUDIO_MAX_MODULES], *on_b[STUDIO_MAX_MODULES], *show_b[STUDIO_MAX_MODULES];
    lv_obj_t *state[STUDIO_MAX_MODULES], *cv;
    lv_obj_t *app_prev, *app_next, *app_page, *route_prev, *route_next, *route_none;
    int page, rpage;
    // routing rows: every sequencer's tracks; shown while it's on
    int nrows;
    struct {
        int mod, track;
        lv_obj_t *name, *arrow, *dest, *ch, *chm, *chp;
    } row[MAX_ROWS];
    lv_obj_t *vol, *cpu, *midi_mon;
    lv_obj_t *midi_rows[STUDIO_MAX_MODULES];
    // mixer
    lv_obj_t *mute[STUDIO_MAX_MODULES], *meter[STUDIO_MAX_MODULES];
    sw_slider_t *fader[STUDIO_MAX_MODULES];
    lv_obj_t *strip[STUDIO_MAX_MODULES];   // the mixer strip: hidden while the app is off
    int meter_px[STUDIO_MAX_MODULES];
    int play_shown, bpm_shown;
    uint32_t mon_seq;
    // patches
    lv_obj_t *pname, *pmsg, *pdel;
    char (*plist)[STUDIO_PATCH_NAME + 1];
    int pn, psel;
    char pcur[STUDIO_PATCH_NAME + 1];   // the one loaded / saved last
    bool pdel_armed;
} H = {.play_shown = -1, .bpm_shown = -1, .psel = -1};

static void h_play(void *ud) { studio_toggle_play(); }

// --- mixer ---------------------------------------------------------------------------------

static int s_meter_w;

static float mix_get(void *ud) { return studio_mix_level((int)(intptr_t)ud) / 100.0f; }
static void mix_set(void *ud, float v) { studio_mix_set_level((int)(intptr_t)ud, (int)(v * 100 + 0.5f)); }
static void mix_text(void *ud, char *b, int n) {
    int v = studio_mix_level((int)(intptr_t)ud);
    if (v <= 0) {
        snprintf(b, n, "OFF");
        return;
    }
    float db = v >= STUDIO_MIX_UNITY
                   ? 2.0f * (v - STUDIO_MIX_UNITY) / (100 - STUDIO_MIX_UNITY)
                   : -40.0f * (float)(STUDIO_MIX_UNITY - v) / (STUDIO_MIX_UNITY - 1);
    snprintf(b, n, "%+.1f dB", (double)db);
}
static const sw_slider_ops_t MIX_OPS = {mix_get, mix_set, mix_text};

static void h_mute(void *ud) {
    int m = (int)(intptr_t)ud;
    studio_mix_set_mute(m, !studio_mix_muted(m));
    sw_btn_set(H.mute[m], studio_mix_muted(m));
}

// an app's mixer strip, inside its tile: M, the level, a peak meter
static void mixer_strip(lv_obj_t *t, int i, int y) {
    int w = TILE_W - 2 * 10 - 44;
    s_meter_w = w;
    t = H.strip[i] = sw_box(t, 0, y, TILE_W - 4, 52, -1, 0);
    y = 0;
    H.mute[i] = sw_btn(t, "M", 10, y + 17, 36, 30, SW_RED, h_mute, (void *)(intptr_t)i, NULL);
    sw_btn_set(H.mute[i], studio_mix_muted(i));
    H.fader[i] = sw_slider(t, "LEVEL", 54, y, w, 44, SW_GREEN, &MIX_OPS, (void *)(intptr_t)i);
    sw_box(t, 54, y + 47, w, 4, 0x242424, 0);
    H.meter[i] = sw_box(t, 54, y + 47, 1, 4, SW_GREEN, 0);
    H.meter_px[i] = -1;
}

// peak meters, ~15 Hz: a quick rise, a slow fall
static void mixer_poll(void) {
    static float hold[STUDIO_MAX_MODULES];
    for (int i = 0; i < g_nmods; i++) {
        if (!H.meter[i]) continue;
        float p = studio_mix_peak(i);
        hold[i] = p > hold[i] ? p : hold[i] * 0.8f;
        // -48 .. 0 dB across the bar
        float db = hold[i] > 1e-5f ? 20.0f * log10f(hold[i]) : -96.0f;
        float f = (db + 48.0f) / 48.0f;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        int px = (int)(f * s_meter_w);
        if (px < 1) px = 1;
        if (px == H.meter_px[i]) continue;
        H.meter_px[i] = px;
        lv_obj_set_width(H.meter[i], px);
        lv_obj_set_style_bg_color(H.meter[i], sw_c(db > -1.0f ? SW_RED : db > -9.0f ? SW_YELLOW : SW_GREEN), 0);
    }
}
static void h_rst(void *ud) { studio_restart(); }

static void h_bpm(void *ud) {
    int d = (int)(intptr_t)ud;
    int32_t v = (studio_bpm_x100() + 50) / 100 * 100 + d * 100;
    studio_set_bpm_x100(v);
}

static void h_clock(void *ud) {
    studio_set_clock_src((int)(intptr_t)ud);
    home_refresh();
}

static void h_clock_out(void *ud) {
    studio_set_clock_out(!studio_clock_out());
    home_refresh();
}

static void h_mmc(void *ud) {
    studio_set_mmc(!studio_mmc());
    home_refresh();
}

static void h_show(void *ud) { show((int)(intptr_t)ud); }

static void h_onoff(void *ud) {
    int i = (int)(intptr_t)ud;
    set_on(i, !studio_module_on(i));
    home_refresh();
}

static void h_page(void *ud) {
    int pages = (g_nmods + TILE_COLS - 1) / TILE_COLS;
    H.page = (H.page + (int)(intptr_t)ud + pages) % pages;
    home_refresh();
}

static void h_rpage(void *ud) {
    H.rpage += (int)(intptr_t)ud;
    home_refresh();
}

static void h_cv(void *ud) {
    studio_set_cv_clock(!studio_cv_clock(), -1, -1);
    home_refresh();
}

// destinations a track can go to: preview (none), each synth that's on, MIDI OUT
static int dest_next(int cur) {
    int opts[STUDIO_MAX_MODULES + 2], n = 0;
    opts[n++] = ROUTE_NONE;
    for (int i = 0; i < g_nmods; i++)
        if ((g_mods[i]->flags & TMOD_F_NOTES_IN) && studio_module_on(i)) opts[n++] = i;
    opts[n++] = ROUTE_MIDI_OUT;
    for (int k = 0; k < n; k++)
        if (opts[k] == cur) return opts[(k + 1) % n];
    return ROUTE_NONE;
}

static void dest_text(char *buf, int len, int d) {
    if (d == ROUTE_NONE)
        snprintf(buf, len, "OWN SOUND");
    else if (d == ROUTE_MIDI_OUT)
        snprintf(buf, len, "MIDI OUT");
    else if (d >= 0 && d < g_nmods)
        snprintf(buf, len, "%s", g_mods[d]->title);
    else
        snprintf(buf, len, "?");
}

static void h_dest(void *ud) {
    int r = (int)(intptr_t)ud;
    int m = H.row[r].mod, k = H.row[r].track;
    studio_route_set(m, k, dest_next(studio_route_get(m, k)));
    home_refresh();
}

static void h_ch(void *ud) {
    int r = (int)(intptr_t)ud >> 1, d = ((int)(intptr_t)ud & 1) ? 1 : -1;
    int m = H.row[r].mod, k = H.row[r].track;
    studio_route_set_midi_ch(m, k, studio_route_midi_ch(m, k) + d);
    home_refresh();
}

static void h_vol(void *ud) {
    studio_set_volume(studio_volume() + (int)(intptr_t)ud);
    home_refresh();
}

static void h_cal(void *ud) { tulip_touchcal_start(NULL); }

// --- Studio patches ------------------------------------------------------------------------

static void patch_say(const char *t) { lv_label_set_text(H.pmsg, t); }

static void patch_disarm(void) {
    H.pdel_armed = false;
    sw_btn_text(H.pdel, "DELETE");
    sw_btn_set(H.pdel, false);
}

static void patch_show(void) {
    if (H.pn == 0)
        lv_label_set_text(H.pname, "-- no patches yet --");
    else if (H.psel < 0 || H.psel >= H.pn)
        lv_label_set_text(H.pname, "-- pick one --");
    else
        lv_label_set_text_fmt(H.pname, "%d/%d  %s", H.psel + 1, H.pn, H.plist[H.psel]);
}

// re-read the folder; keep `keep` selected if it's there
static void patch_rescan(const char *keep) {
    if (!H.plist) H.plist = malloc(STUDIO_MAX_PATCHES * sizeof H.plist[0]);
    H.pn = H.plist ? studio_patch_list(H.plist, STUDIO_MAX_PATCHES) : 0;
    if (keep && keep[0]) {
        H.psel = -1;
        for (int i = 0; i < H.pn; i++)
            if (!strcmp(H.plist[i], keep)) H.psel = i;
    }
    if (H.psel >= H.pn) H.psel = H.pn - 1;
    patch_show();
}

static void h_pstep(void *ud) {
    patch_disarm();
    if (!H.pn) return;
    H.psel = (H.psel + (int)(intptr_t)ud + H.pn) % H.pn;
    if (H.psel < 0) H.psel = 0;
    patch_show();
}

static void h_pload(void *ud) {
    patch_disarm();
    if (H.psel < 0 || H.psel >= H.pn) return patch_say("pick a patch first");
    char err[64];
    char nm[STUDIO_PATCH_NAME + 1];
    snprintf(nm, sizeof nm, "%s", H.plist[H.psel]);
    if (!studio_patch_load(nm, err, sizeof err)) return patch_say(err);
    snprintf(H.pcur, sizeof H.pcur, "%s", nm);
    char t[48];
    snprintf(t, sizeof t, "loaded %s", nm);
    patch_say(t);
    home_refresh();
    for (int i = 0; i < g_nmods; i++)
        if (H.fader[i]) sw_slider_refresh(H.fader[i]);
}

static void patch_saved(const char *name, void *ud) {
    char t[48];
    if (!studio_patch_save(name)) {
        patch_say(studio_files_root() ? "couldn't write the file" : "no /files to save to");
        return;
    }
    snprintf(H.pcur, sizeof H.pcur, "%.*s", STUDIO_PATCH_NAME, name);
    patch_rescan(H.pcur);
    snprintf(t, sizeof t, "saved %s", H.pcur);
    patch_say(t);
}

static void h_psave_as(void *ud) {
    patch_disarm();
    char def[STUDIO_PATCH_NAME + 1];
    if (H.pcur[0])
        snprintf(def, sizeof def, "%s", H.pcur);
    else
        snprintf(def, sizeof def, "PATCH %d", H.pn + 1);
    sw_text_entry("SAVE STUDIO PATCH", def, STUDIO_PATCH_NAME, SW_ORANGE, patch_saved, NULL);
}

// SAVE: over the one picked (or the last one loaded); a new name otherwise
static void h_psave(void *ud) {
    patch_disarm();
    if (H.psel >= 0 && H.psel < H.pn) {
        char nm[STUDIO_PATCH_NAME + 1];
        snprintf(nm, sizeof nm, "%s", H.plist[H.psel]);
        patch_saved(nm, NULL);
    } else {
        h_psave_as(NULL);
    }
}

static void h_pdel(void *ud) {
    if (H.psel < 0 || H.psel >= H.pn) return patch_say("pick a patch first");
    if (!H.pdel_armed) {   // tap twice
        H.pdel_armed = true;
        sw_btn_text(H.pdel, "SURE?");
        sw_btn_set(H.pdel, true);
        return;
    }
    patch_disarm();
    char t[48];
    snprintf(t, sizeof t, "%s %s", studio_patch_delete(H.plist[H.psel]) ? "deleted" : "couldn't delete",
             H.plist[H.psel]);
    patch_rescan(NULL);
    patch_say(t);
}

static void patch_row(lv_obj_t *s, int y) {
    sw_label(s, "STUDIO PATCH", SW_F_LABEL, SW_ORANGE, 14, y, 0, false);
    sw_label(s, "the whole session in one file: tempo, apps, mixer, routing and every app's "
                "sounds, patterns and presets",
             SW_F_SMALL, SW_DIM, 140, y + 2, 0, false);
    y += 24;
    sw_btn(s, "<", 14, y, 44, 40, SW_ORANGE, h_pstep, (void *)-1, SW_F_MID);
    H.pname = sw_label(s, "", SW_F_LABEL, SW_TEXT, 60, y + 11, 280, true);
    lv_label_set_long_mode(H.pname, LV_LABEL_LONG_CLIP);
    sw_btn(s, ">", 342, y, 44, 40, SW_ORANGE, h_pstep, (void *)1, SW_F_MID);
    sw_btn(s, "LOAD", 400, y, 100, 40, SW_ORANGE, h_pload, NULL, NULL);
    sw_btn(s, "SAVE", 506, y, 100, 40, SW_ORANGE, h_psave, NULL, NULL);
    sw_btn(s, "SAVE AS...", 612, y, 120, 40, SW_ORANGE, h_psave_as, NULL, NULL);
    H.pdel = sw_btn(s, "DELETE", 738, y, 100, 40, SW_RED, h_pdel, NULL, NULL);
    H.pmsg = sw_label(s, "", SW_F_SMALL, SW_DIM, 850, y + 13, 170, false);
    lv_label_set_long_mode(H.pmsg, LV_LABEL_LONG_CLIP);
    patch_rescan(NULL);
}

static void home_create(void) {
    lv_obj_t *s = s_home = new_screen();
    // top bar
    sw_label(s, "TULIP STUDIO", SW_F_MID, SW_ORANGE, 14, 16, 0, false);
    H.play = sw_btn(s, "PLAY", 200, 8, 96, 40, SW_GREEN, h_play, NULL, SW_F_MID);
    sw_btn(s, "RST", 302, 8, 56, 40, SW_CYAN, h_rst, NULL, NULL);
    lv_obj_t *b = sw_btn(s, "-", 372, 8, 44, 40, SW_CYAN, h_bpm, (void *)-1, SW_F_MID);
    sw_btn_repeat(b);
    H.bpm = sw_label(s, "", SW_F_MID, SW_TEXT, 418, 18, 110, true);
    b = sw_btn(s, "+", 530, 8, 44, 40, SW_CYAN, h_bpm, (void *)1, SW_F_MID);
    sw_btn_repeat(b);
    H.clk_int = sw_btn(s, "INT", 590, 8, 58, 40, SW_CYAN, h_clock, (void *)CLOCK_SRC_INT, NULL);
    H.clk_midi = sw_btn(s, "MIDI", 652, 8, 64, 40, SW_CYAN, h_clock, (void *)CLOCK_SRC_MIDI, NULL);
    H.clk_out = sw_btn(s, "CLK OUT", 722, 8, 82, 40, SW_CYAN, h_clock_out, NULL, NULL);
    H.mmc = sw_btn(s, "MMC", 808, 8, 92, 40, SW_CYAN, h_mmc, NULL, NULL);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);

    // apps: a tile each, 4 to a page
    sw_label(s, "APPS", SW_F_LABEL, SW_ORANGE, 14, 64, 0, false);
    sw_label(s, "switch on the apps you want; the others stay off. LEVEL into the master VOLUME, M mutes",
             SW_F_SMALL, SW_DIM, 74, 66, 700, false);
    if (g_nmods > TILE_COLS) {
        H.app_prev = sw_btn(s, "<", 840, 58, 44, 26, SW_ORANGE, h_page, (void *)-1, NULL);
        H.app_page = sw_label(s, "", SW_F_SMALL, SW_TEXT, 886, 64, 76, true);
        H.app_next = sw_btn(s, ">", 966, 58, 44, 26, SW_ORANGE, h_page, (void *)1, NULL);
    }
    for (int i = 0; i < g_nmods; i++) {
        const tmod_t *m = g_mods[i];
        int x = 12 + (i % TILE_COLS) * (TILE_W + TILE_GAP);
        lv_obj_t *t = H.tile[i] = sw_box(s, x, TILES_Y, TILE_W, TILE_H, SW_BG, 4);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, sw_c(SW_LINE), 0);
        sw_label(t, m->title, SW_F_MID, SW_TEXT, 10, 8, 0, false);
        char tag[48];
        snprintf(tag, sizeof tag, "%s%s%s", studio_role_name(m->role),
                 (m->flags & TMOD_F_CV) ? "  CV" : "", (m->flags & TMOD_F_AUDIO) ? "  AUDIO" : "");
        uint32_t tc = m->role == TMOD_SEQUENCER ? SW_CYAN : m->role == TMOD_DRUMS ? SW_ORANGE
                                                                                 : SW_GREEN;
        sw_label(t, tag, SW_F_SMALL, tc, 10, 36, 0, false);
        H.state[i] = sw_label(t, "", SW_F_SMALL, SW_DIM, TILE_W - 90, 12, 78, false);
        lv_obj_set_style_text_align(H.state[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t *bl = sw_label(t, m->blurb ? m->blurb : "", SW_F_SMALL, SW_DIM, 10, 56, TILE_W - 20, false);
        lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(bl, 30);
        sw_label(t, "MIDI IN", SW_F_SMALL, SW_ORANGE, 10, 92, 0, false);
        H.midi_rows[i] = sw_label(t, "", SW_F_SMALL, SW_TEXT, 10, 108, TILE_W - 20, false);
        lv_label_set_long_mode(H.midi_rows[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_height(H.midi_rows[i], 46);
        H.on_b[i] = sw_btn(t, "OFF", 10, 156, 110, 36, SW_GREEN, h_onoff, (void *)(intptr_t)i, NULL);
        H.show_b[i] = sw_btn(t, "SHOW", TILE_W - 110, 156, 100, 36, SW_ORANGE, h_show, (void *)(intptr_t)i, NULL);
        if (m->flags & TMOD_F_AUDIO) mixer_strip(t, i, 196);
    }

    // routing: every sequencer's tracks (while it's on), 4 to a page
    sw_label(s, "ROUTING", SW_F_LABEL, SW_ORANGE, 14, ROUTE_Y, 0, false);
    sw_label(s, "a sequencer track plays its own sound, a synth on the Tulip, or MIDI OUT",
             SW_F_SMALL, SW_DIM, 110, ROUTE_Y + 2, 0, false);
    H.route_none = sw_label(s, "switch a sequencer on to route its tracks", SW_F_SMALL, SW_DIM, 14,
                            ROUTE_Y + 36, 0, false);
    H.route_prev = sw_btn(s, "<", 840, ROUTE_Y - 6, 44, 26, SW_CYAN, h_rpage, (void *)-1, NULL);
    H.route_next = sw_btn(s, ">", 966, ROUTE_Y - 6, 44, 26, SW_CYAN, h_rpage, (void *)1, NULL);
    H.nrows = 0;
    for (int i = 0; i < g_nmods; i++) {
        const tmod_t *m = g_mods[i];
        for (int k = 0; k < m->n_note_outs && k < 2 && H.nrows < MAX_ROWS; k++) {
            int r = H.nrows++;
            H.row[r].mod = i;
            H.row[r].track = k;
            char t[40];
            snprintf(t, sizeof t, "%s %s", m->title, m->note_out_names ? m->note_out_names[k] : "OUT");
            H.row[r].name = sw_label(s, t, SW_F_LABEL, SW_TEXT, 0, 0, 170, false);
            H.row[r].arrow = sw_label(s, LV_SYMBOL_RIGHT, SW_F_LABEL, SW_DIM, 0, 0, 0, false);
            H.row[r].dest = sw_btn(s, "", 0, 0, 140, 38, SW_CYAN, h_dest, (void *)(intptr_t)r, NULL);
            H.row[r].chm = sw_btn(s, "-", 0, 0, 38, 38, SW_CYAN, h_ch, (void *)(intptr_t)(r << 1), NULL);
            H.row[r].ch = sw_label(s, "", SW_F_LABEL, SW_TEXT, 0, 0, 70, true);
            H.row[r].chp = sw_btn(s, "+", 0, 0, 38, 38, SW_CYAN, h_ch, (void *)(intptr_t)((r << 1) | 1), NULL);
        }
    }

    // Studio patches, under the routing
    patch_row(s, 470);

    // bottom line: volume, load, MIDI monitor, touch calibration
    sw_line(s, 0, 540, 1024, 1);
    sw_label(s, "VOLUME", SW_F_SMALL, SW_DIM, 14, 562, 0, false);
    sw_btn(s, "-", 76, 550, 38, 38, SW_ORANGE, h_vol, (void *)-1, NULL);
    H.vol = sw_label(s, "", SW_F_LABEL, SW_TEXT, 116, 560, 40, true);
    sw_btn(s, "+", 158, 550, 38, 38, SW_ORANGE, h_vol, (void *)1, NULL);
    H.cpu = sw_label(s, "", SW_F_SMALL, SW_DIM, 214, 562, 170, false);
    sw_label(s, "MIDI IN", SW_F_SMALL, SW_DIM, 400, 562, 0, false);
    H.midi_mon = sw_label(s, "--", SW_F_SMALL, SW_TEXT, 460, 562, 270, false);
    H.cv = sw_btn(s, "CV CLOCK", 744, 550, 128, 38, SW_GREEN, h_cv, NULL, NULL);
    sw_btn(s, "TOUCH CAL", 880, 550, 130, 38, SW_ORANGE, h_cal, NULL, NULL);
    home_refresh();
}

static void home_refresh(void) {
    if (!s_home) return;
    int src = studio_clock_src();
    sw_btn_set(H.clk_int, src == CLOCK_SRC_INT);
    sw_btn_set(H.clk_midi, src == CLOCK_SRC_MIDI);
    sw_btn_set(H.clk_out, studio_clock_out() && src == CLOCK_SRC_INT);
    sw_btn_set(H.mmc, studio_mmc());
    sw_btn_text(H.mmc, src == CLOCK_SRC_MIDI ? "MMC IN" : "MMC OUT");
    int pages = (g_nmods + TILE_COLS - 1) / TILE_COLS;
    if (H.page >= pages) H.page = 0;
    for (int i = 0; i < g_nmods; i++) {
        bool on = studio_module_on(i), here = i / TILE_COLS == H.page;
        if (here)
            lv_obj_remove_flag(H.tile[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(H.tile[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(H.state[i], on ? "ON" : "OFF");
        lv_obj_set_style_text_color(H.state[i], sw_c(on ? SW_GREEN : SW_DIM), 0);
        lv_obj_set_style_border_color(H.tile[i], sw_c(on ? SW_EDGE : SW_LINE), 0);
        sw_btn_text(H.on_b[i], on ? "ON" : "OFF");
        sw_btn_set(H.on_b[i], on);
        lv_obj_t *only_on[2] = {H.show_b[i], H.strip[i]};
        for (int j = 0; j < 2; j++) {
            if (!only_on[j]) continue;
            if (on)
                lv_obj_remove_flag(only_on[j], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(only_on[j], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (H.app_page) lv_label_set_text_fmt(H.app_page, "%d / %d", H.page + 1, pages);
    // routing: the rows of the sequencers that are on, a page of 4
    int vis = 0;
    for (int r = 0; r < H.nrows; r++) vis += studio_module_on(H.row[r].mod);
    int rpages = (vis + ROUTE_ROWS - 1) / ROUTE_ROWS;
    if (H.rpage >= rpages) H.rpage = rpages ? rpages - 1 : 0;
    if (H.rpage < 0) H.rpage = 0;
    lv_obj_t *nav[2] = {H.route_prev, H.route_next};
    for (int j = 0; j < 2; j++) {
        if (rpages > 1)
            lv_obj_remove_flag(nav[j], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(nav[j], LV_OBJ_FLAG_HIDDEN);
    }
    if (vis)
        lv_obj_add_flag(H.route_none, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(H.route_none, LV_OBJ_FLAG_HIDDEN);
    for (int r = 0, v = 0; r < H.nrows; r++) {
        bool here = studio_module_on(H.row[r].mod) && v / ROUTE_ROWS == H.rpage;
        if (studio_module_on(H.row[r].mod)) v++;
        lv_obj_t *objs[6] = {H.row[r].name, H.row[r].arrow, H.row[r].dest, H.row[r].chm, H.row[r].ch, H.row[r].chp};
        if (!here) {
            for (int j = 0; j < 6; j++) lv_obj_add_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int slot = (v - 1) % ROUTE_ROWS;
        int x = 14 + (slot % 2) * 504, yy = ROUTE_Y + 24 + (slot / 2) * 46;
        lv_obj_set_pos(H.row[r].name, x, yy + 11);
        lv_obj_set_pos(H.row[r].arrow, x + 170, yy + 11);
        lv_obj_set_pos(H.row[r].dest, x + 194, yy);
        lv_obj_set_pos(H.row[r].chm, x + 342, yy);
        lv_obj_set_pos(H.row[r].ch, x + 382, yy + 11);
        lv_obj_set_pos(H.row[r].chp, x + 454, yy);
        for (int j = 0; j < 3; j++) lv_obj_remove_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
        int m = H.row[r].mod, k = H.row[r].track;
        int d = studio_route_get(m, k);
        char t[24];
        dest_text(t, sizeof t, d);
        sw_btn_text(H.row[r].dest, t);
        sw_btn_set(H.row[r].dest, d != ROUTE_NONE);
        bool midi = d == ROUTE_MIDI_OUT;
        snprintf(t, sizeof t, "CH %d", studio_route_midi_ch(m, k));
        lv_label_set_text(H.row[r].ch, t);
        for (int j = 3; j < 6; j++) {
            if (midi)
                lv_obj_remove_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
        }
    }
    bool cv = studio_cv_clock();
    sw_btn_set(H.cv, cv);
    sw_btn_text(H.cv, cv ? "CV CLOCK ON" : "CV CLOCK OFF");
    char v[8];
    snprintf(v, sizeof v, "%d", studio_volume());
    lv_label_set_text(H.vol, v);
    for (int i = 0; i < g_nmods; i++) {
        char d[96] = "-";
        if (g_mods[i]->midi_desc)
            g_mods[i]->midi_desc(d, sizeof d);
        else if (g_mods[i]->flags & TMOD_F_CLOCKED)
            snprintf(d, sizeof d, "clock and transport only");
        if (H.midi_rows[i]) lv_label_set_text(H.midi_rows[i], d);
    }
}

static void home_poll(void) {
    static int64_t next_meter;
    if (studio_now_us() > next_meter) {
        next_meter = studio_now_us() + 66000;
        mixer_poll();
    }
    studio_clock_status_t st;
    studio_clock_status(&st);
    int ps = st.playing ? 1 : st.armed ? 2 : 0;
    if (ps != H.play_shown) {
        H.play_shown = ps;
        static const char *const txt[3] = {"PLAY", "STOP", "ARMED"};
        sw_btn_text(H.play, txt[ps]);
        sw_btn_set(H.play, ps != 0);
        sw_btn_accent(H.play, ps == 2 ? SW_YELLOW : SW_GREEN);
    }
    int bv = st.ext ? -1 - st.ext_bpm_x10 : (int)studio_bpm_x100();
    if (bv != H.bpm_shown) {
        H.bpm_shown = bv;
        char t[24];
        if (bv >= 0) {
            int32_t b = studio_bpm_x100();
            if (b % 100)
                snprintf(t, sizeof t, "%d.%d BPM", (int)(b / 100), (int)(b % 100) / 10);
            else
                snprintf(t, sizeof t, "%d BPM", (int)(b / 100));
        } else if (st.ext_bpm_x10) {
            snprintf(t, sizeof t, "MIDI %d", (st.ext_bpm_x10 + 5) / 10);
        } else {
            snprintf(t, sizeof t, "MIDI --");
        }
        lv_label_set_text(H.bpm, t);
    }
    static int64_t next;
    int64_t now = studio_now_us();
    if (now > next) {
        next = now + 500000;
        char t[72];
        const char *bad = studio_audio_problem();
        if (bad)
            snprintf(t, sizeof t, "%s", bad);
        else
            snprintf(t, sizeof t, "AUDIO %d%%   XRUN %u", studio_audio_load_pct(),
                     (unsigned)studio_audio_underruns());
        lv_obj_set_style_text_color(H.cpu, sw_c(bad ? SW_RED : SW_DIM), 0);
        lv_label_set_text(H.cpu, t);
    }
    char mon[40];
    uint32_t seq = studio_midi_monitor(mon, sizeof mon);
    if (seq != H.mon_seq) {
        H.mon_seq = seq;
        lv_label_set_text(H.midi_mon, mon);
    }
}

// --- UI task entry points ---------------------------------------------------------------------

void shell_ui_create(void) {
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *t = lv_theme_default_init(d, sw_c(SW_ORANGE), sw_c(SW_CYAN), true,
                                          &lv_font_montserrat_14);
    lv_display_set_theme(d, t);
    lv_obj_t *boot = lv_screen_active();   // LVGL's default screen: replaced below
    if (studio_multi()) home_create();
    // every running app gets its screen now (its MIDI CC / program change
    // handling runs in its UI poll, shown or not); closed ones when opened
    for (int i = 0; i < g_nmods; i++)
        if (studio_module_on(i)) build(i);
    // Studio always opens on its home page; a single app on its own screen
    show(studio_multi() ? -1 : 0);
    if (boot != lv_screen_active()) lv_obj_delete(boot);
}

// No sound would otherwise fail silently: on every page, a red line at the
// bottom says why while audio is down.
static void audio_banner(void) {
    static lv_obj_t *lbl;
    static int64_t next;
    int64_t now = studio_now_us();
    if (now < next) return;
    next = now + 500000;
    const char *bad = studio_audio_problem();
    if (!bad) {
        if (lbl) lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!lbl) {
        lbl = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(lbl, sw_c(SW_RED), 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lbl, sw_c(0xffffff), 0);
        lv_obj_set_style_pad_hor(lbl, 8, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    lv_label_set_text(lbl, bad);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

void shell_ui_poll(void) {
    audio_banner();
    for (int i = 0; i < g_nmods; i++)
        if (s_scr[i] && g_mods[i]->ui_poll) g_mods[i]->ui_poll(s_cur == i);
    if (s_cur == -1) home_poll();
    tulip_touchcal_poll();
    cfg_poll();
}
