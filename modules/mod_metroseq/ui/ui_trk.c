// ui_trk.c - TRK page: per-track transforms for TRK 1 and TRK 2, and
// where each track's notes go (its own sound, or MIDI OUT on a channel).
#include <stdio.h>

#include "mod_metroseq.h"
#include "ui_common.h"

enum { R_ORDER, R_LEN, R_DIV, R_SWING, R_GATE, R_TRANS, R_OCT, R_INV, R_AUDIO, R_MIDI, R_COUNT };

static const char *const ROW_NAMES[R_COUNT] = {"Order",    "Length",    "Clock div",
                                               "Swing",    "Gate len",  "Transpose",
                                               "Octave",   "Invert",    "Notes",
                                               "MIDI out"};

// MIDI out is the Studio's routing for the track (the same setting as on
// the Studio home page): OFF = the track's own sound, CH n = MIDI OUT on
// channel n. In Studio a track routed to a synth shows the synth.
static int self(void) { return studio_module_index(&mod_metroseq); }

static void midi_text(int t, char *b, int n) {
    int d = studio_route_get(self(), t);
    if (d == ROUTE_MIDI_OUT)
        snprintf(b, n, "CH %d", studio_route_midi_ch(self(), t));
    else if (d >= 0 && studio_module(d))
        snprintf(b, n, "> %s", studio_module(d)->title);
    else
        snprintf(b, n, "OFF");
}

// OFF, CH 1 .. CH 16 (a routed synth steps to OFF / CH 1)
static void midi_step(int t, int d) {
    int r = studio_route_get(self(), t);
    int v = r == ROUTE_MIDI_OUT ? studio_route_midi_ch(self(), t) : 0;
    if (r >= 0 && r != ROUTE_MIDI_OUT) v = 0;
    v = clampi(v + d, 0, 16);
    if (v == 0) {
        studio_route_set(self(), t, ROUTE_NONE);
    } else {
        studio_route_set_midi_ch(self(), t, v);
        studio_route_set(self(), t, ROUTE_MIDI_OUT);
    }
}

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
    lv_label_set_text(s_vals[t][R_INV], tr->invert ? "ON" : "OFF");
    lv_label_set_text(s_vals[t][R_AUDIO], tr->mute_audio ? "OFF" : "ON");
    char m[24];
    midi_text(t, m, sizeof m);
    lv_label_set_text(s_vals[t][R_MIDI], m);
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
    case R_MIDI: midi_step(t, d); break;
    }
    refresh_track(t);
}

static void column_create(int t, int x0) {
    lv_obj_t *head = ui_label(s_panel, t == 0 ? "TRACK 1" : "TRACK 2", x0 + 8, 10);
    lv_obj_set_style_text_color(head, c332(C_TRK[t]), 0);
    for (int r = 0; r < R_COUNT; r++) {
        int y = 44 + r * 46 + (r == R_MIDI ? 10 : 0);
        intptr_t base = (t * R_COUNT + r) * 2;
        if (r == R_MIDI) ui_panel(s_panel, x0 + 12, y - 8, 430, 1, C_PANEL2, 0);
        ui_label(s_panel, ROW_NAMES[r], x0 + 12, y + 11);
        // - value + : the value centred between the buttons
        ui_btn(s_panel, "-", x0 + 190, 50, 38, step, base, C_BTN, y, NULL);
        lv_obj_t *v = s_vals[t][r] = ui_label(s_panel, "", x0 + 240, y + 11);
        lv_obj_set_width(v, 152);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
        ui_btn(s_panel, "+", x0 + 392, 50, 38, step, base + 1, C_BTN, y, NULL);
    }
}

void trk_page_refresh(void) {
    for (int t = 0; t < NUM_TRACKS; t++) refresh_track(t);
}

void trk_page_show(bool on) {
    if (on) trk_page_refresh();   // the routing may have changed on the Studio home page
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
