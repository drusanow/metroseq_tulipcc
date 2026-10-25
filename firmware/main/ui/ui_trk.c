// ui_trk.c - TRK page: per-track transforms for TRK 1 and TRK 2.
#include <stdio.h>

#include "ui_common.h"

enum { R_ORDER, R_LEN, R_DIV, R_SWING, R_GATE, R_TRANS, R_OCT, R_INV, R_AUDIO, R_COUNT };

static const char *const ROW_NAMES[R_COUNT] = {"Order",    "Length",    "Clock div",
                                               "Swing",    "Gate len",  "Transpose",
                                               "Octave",   "Invert",    "Audio"};

static lv_obj_t *s_panel;
static lv_obj_t *s_vals[NUM_TRACKS][R_COUNT];

static void refresh_track(int t) {
    const track_cfg_t *tr = &g_pat.tracks[t];
    lv_label_set_text(s_vals[t][R_ORDER], ORDER_NAMES[tr->order]);
    lv_label_set_text_fmt(s_vals[t][R_LEN], "%d", tr->length);
    lv_label_set_text(s_vals[t][R_DIV], DIV_NAMES[tr->div_i]);
    lv_label_set_text_fmt(s_vals[t][R_SWING], "%d%%", tr->swing);
    lv_label_set_text_fmt(s_vals[t][R_GATE], "%d%%", tr->gate_pct);
    lv_label_set_text_fmt(s_vals[t][R_TRANS], "%+d", tr->transpose);
    lv_label_set_text_fmt(s_vals[t][R_OCT], "%+d", tr->octave);
    lv_label_set_text(s_vals[t][R_INV], tr->invert ? "ON" : "off");
    lv_label_set_text(s_vals[t][R_AUDIO], tr->mute_audio ? "off" : "ON");
}

// arg: (track * R_COUNT + row) * 2 + (0 = minus, 1 = plus)
static void step(intptr_t arg) {
    int up = arg & 1;
    int row = (arg >> 1) % R_COUNT;
    int t = (arg >> 1) / R_COUNT;
    int d = up ? 1 : -1;
    track_cfg_t *tr = &g_pat.tracks[t];
    switch (row) {
    case R_ORDER: tr->order = pmod(tr->order + d, ORD_COUNT); break;
    case R_LEN: tr->length = clampi(tr->length + d, 1, NUM_STAGES); break;
    case R_DIV: tr->div_i = clampi(tr->div_i + d, 0, NUM_DIVS - 1); break;
    case R_SWING: tr->swing = clampi(tr->swing + 5 * d, 0, 60); break;
    case R_GATE: tr->gate_pct = clampi(tr->gate_pct + 5 * d, 5, 100); break;
    case R_TRANS: tr->transpose = clampi(tr->transpose + d, -24, 24); break;
    case R_OCT: tr->octave = clampi(tr->octave + d, -3, 3); break;
    case R_INV: tr->invert = !tr->invert; break;
    case R_AUDIO: tr->mute_audio = !tr->mute_audio; break;
    }
    refresh_track(t);
}

static void column_create(int t, int x0) {
    lv_obj_t *head = ui_label(s_panel, t == 0 ? "TRACK 1" : "TRACK 2", x0 + 8, 10);
    lv_obj_set_style_text_color(head, c332(C_TRK[t]), 0);
    for (int r = 0; r < R_COUNT; r++) {
        int y = 46 + r * 44;
        intptr_t base = (t * R_COUNT + r) * 2;
        ui_label(s_panel, ROW_NAMES[r], x0 + 12, y + 8);
        ui_btn(s_panel, "-", x0 + 190, 46, 34, step, base, C_BTN, y, NULL);
        s_vals[t][r] = ui_label(s_panel, "", x0 + 250, y + 8);
        ui_btn(s_panel, "+", x0 + 360, 46, 34, step, base + 1, C_BTN, y, NULL);
    }
}

void trk_page_refresh(void) {
    for (int t = 0; t < NUM_TRACKS; t++) refresh_track(t);
}

void trk_page_show(bool on) {
    if (on)
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void trk_page_create(lv_obj_t *parent) {
    s_panel = ui_panel(parent, 0, PAGE_Y, 1024, PAGE_H, C_PANEL, 0);
    lv_obj_t *div = ui_panel(s_panel, 0, 10, 2, PAGE_H - 20, C_PANEL2, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 10);
    column_create(0, 30);
    column_create(1, 530);
    trk_page_refresh();
}
