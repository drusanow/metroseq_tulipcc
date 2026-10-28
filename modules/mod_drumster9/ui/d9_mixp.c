// d9_mixp.c - the MIX page: every lane's level, mute and solo side by side,
// the master, and a peak meter of what Drumster9 puts out (dBFS, before the
// Studio mixer).
#include <math.h>
#include <stdio.h>

#include "d9_ui_int.h"

#define ROW_Y0 14
#define ROW_DY 54
#define MET_X 912
#define MET_Y 20
#define MET_H 440
#define MET_W 40
#define MET_LO (-48.0f)

static struct {
    sw_slider_t *lvl[D9_LANES], *master;
    lv_obj_t *mute[D9_LANES], *solo[D9_LANES], *bus[D9_LANES], *meter, *db;
    float hold;
    int px;
    uint32_t last;
} M = {.px = -1};

static float get_v(void *ud) { return g_d9.vol[(intptr_t)ud]; }
static void set_v(void *ud, float v) {
    g_d9.vol[(intptr_t)ud] = v;
    d9_edit();
}
static void text_v(void *ud, char *b, int n) { snprintf(b, n, "%d%%", (int)(g_d9.vol[(intptr_t)ud] * 100 + 0.5f)); }
static const sw_slider_ops_t VOL = {get_v, set_v, text_v};

static float get_m(void *ud) { return g_d9.master / D9_MASTER_MAX; }
static void set_m(void *ud, float v) {
    g_d9.master = v * D9_MASTER_MAX;
    d9_edit();
}
static void text_m(void *ud, char *b, int n) {
    if (g_d9.master <= 0.01f)
        snprintf(b, n, "OFF");
    else
        snprintf(b, n, "%+.1f dB", (double)(20.0f * log10f(g_d9.master / D9_MASTER_DEF)));
}
static const sw_slider_ops_t MASTER = {get_m, set_m, text_m};

void d9_mix_page_refresh(void) {
    for (int l = 0; l < D9_LANES; l++) {
        sw_slider_refresh(M.lvl[l]);
        sw_btn_set(M.mute[l], g_d9.muted[l]);
        sw_btn_set(M.solo[l], g_d9.solo[l]);
        lv_label_set_text_fmt(M.bus[l], "BUS %d", g_d9.bus[l]);
        lv_obj_set_style_text_color(M.bus[l], sw_c(D9_BUS_COLOR[g_d9.bus[l]]), 0);
    }
    sw_slider_refresh(M.master);
}

static void mute_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_d9.muted[l] = !g_d9.muted[l];
    sw_btn_set(M.mute[l], g_d9.muted[l]);
    d9_edit();
}

static void solo_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_d9.solo[l] = !g_d9.solo[l];
    sw_btn_set(M.solo[l], g_d9.solo[l]);
    d9_edit();
}

// ~30 Hz: catch the peak, hold it a moment, fall at 60 dB/s (the Python's
// ballistics)
void d9_mix_page_tick(uint32_t ms) {
    if (ms - M.last < 33) return;
    float dt = (ms - M.last) / 1000.0f;
    M.last = ms;
    float pk = g_d9s.peak;
    g_d9s.peak = 0;
    float db = pk > 1e-5f ? 20.0f * log10f(pk) : -96.0f;
    M.hold = db >= M.hold ? db : M.hold - 60.0f * (dt > 0.2f ? 0.2f : dt);
    float f = (M.hold - MET_LO) / -MET_LO;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    int px = (int)(f * MET_H);
    if (px == M.px) return;
    M.px = px;
    lv_obj_set_height(M.meter, px < 2 ? 2 : px);
    lv_obj_set_y(M.meter, MET_Y + MET_H - (px < 2 ? 2 : px));
    lv_obj_set_style_bg_color(M.meter, sw_c(M.hold > -1 ? SW_RED : M.hold > -6 ? SW_YELLOW : SW_GREEN), 0);
    if (M.hold <= MET_LO)
        lv_label_set_text(M.db, "-inf");
    else
        lv_label_set_text_fmt(M.db, "%.1f", (double)M.hold);
}

void d9_mix_page_create(lv_obj_t *p) {
    for (int l = 0; l < D9_LANES; l++) {
        int y = ROW_Y0 + l * ROW_DY;
        sw_label(p, D9_ROLE[l], SW_F_LABEL, SW_TEXT, 12, y + 15, 80, false);
        M.mute[l] = sw_btn(p, "M", 96, y + 4, 40, 40, SW_RED, mute_cb, (void *)(intptr_t)l, NULL);
        M.solo[l] = sw_btn(p, "S", 140, y + 4, 40, 40, SW_YELLOW, solo_cb, (void *)(intptr_t)l, NULL);
        M.lvl[l] = sw_slider(p, "LEVEL", 192, y, 600, 44, ACC, &VOL, (void *)(intptr_t)l);
        M.bus[l] = sw_label(p, "", SW_F_SMALL, C_DIM, 808, y + 17, 0, false);
    }
    sw_line(p, 12, ROW_Y0 + D9_LANES * ROW_DY, 860, 1);
    int y = ROW_Y0 + D9_LANES * ROW_DY + 14;
    sw_label(p, "MASTER", SW_F_LABEL, SW_GREEN, 12, y + 15, 0, false);
    M.master = sw_slider(p, "OUT", 192, y, 600, 44, SW_GREEN, &MASTER, NULL);
    sw_label(p, "MASTER 0 dB is the Python's 15; the Studio mixer comes after", SW_F_SMALL, C_DIM, 192,
             y + 56, 0, false);

    // the meter
    sw_label(p, "OUT", SW_F_SMALL, C_DIM, MET_X + 6, 2, 0, false);
    sw_box(p, MET_X, MET_Y, MET_W, MET_H, 0x242424, 0);
    M.meter = sw_box(p, MET_X, MET_Y + MET_H - 2, MET_W, 2, SW_GREEN, 0);
    static const int TICKS[] = {0, -6, -12, -24, -36, -48};
    for (int i = 0; i < 6; i++) {
        int ty = MET_Y + (int)((TICKS[i] / MET_LO) * MET_H);
        sw_box(p, MET_X + MET_W, ty, 6, 1, SW_LINE, 0);
        char t[8];
        snprintf(t, sizeof t, "%d", TICKS[i]);
        sw_label(p, t, SW_F_SMALL, C_DIM, MET_X + MET_W + 10, ty - 7, 0, false);
    }
    M.db = sw_label(p, "-inf", SW_F_SMALL, SW_TEXT, MET_X - 4, MET_Y + MET_H + 8, 60, true);
    M.hold = -96;
    d9_mix_page_refresh();
}
