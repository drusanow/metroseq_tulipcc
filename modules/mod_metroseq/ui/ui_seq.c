// ui_seq.c - SEQ page (the 8 shared stages) and the stage DETAIL popup.
#include <stdio.h>

#include "ui_common.h"

typedef struct {
    lv_obj_t *leds[NUM_TRACKS];
    ui_btn_t *num_btn;
    lv_obj_t *note_lbl;
    lv_obj_t *slider;
    ui_btn_t *pulse_btn, *gate_btn, *slide_btn, *skip_btn;
} col_t;

static lv_obj_t *s_panel;
static col_t s_cols[NUM_STAGES];

static void refresh_note(int i) {
    const stage_t *st = &g_pat.stages[i];
    int semi = quantise(stage_semi(&g_pat, st->pitch), &SCALES[g_pat.scale_idx], g_pat.root);
    char nm[8];
    note_name(eff_base_midi(&g_pat) + semi, nm, sizeof nm);
    lv_label_set_text(s_cols[i].note_lbl, nm);
}

static void slider_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    g_pat.stages[i].pitch = clampi(v, 0, PITCH_STEPS);
    refresh_note(i);
}

static void cycle_pulse(intptr_t i) {
    stage_t *st = &g_pat.stages[i];
    st->pulses = st->pulses % 8 + 1;
    seq_page_refresh_col(i);
}

static void cycle_gate(intptr_t i) {
    stage_t *st = &g_pat.stages[i];
    st->gate = (st->gate + 1) % GATE_COUNT;
    seq_page_refresh_col(i);
}

static void toggle_slide(intptr_t i) {
    g_pat.stages[i].slide = !g_pat.stages[i].slide;
    seq_page_refresh_col(i);
}

static void toggle_skip(intptr_t i) {
    g_pat.stages[i].skip = !g_pat.stages[i].skip;
    seq_page_refresh_col(i);
}

static void open_detail(intptr_t i) { detail_popup_show((int)i); }

void seq_page_refresh_col(int i) {
    col_t *c = &s_cols[i];
    const stage_t *st = &g_pat.stages[i];
    lv_slider_set_value(c->slider, st->pitch, LV_ANIM_OFF);
    refresh_note(i);
    char buf[8];
    snprintf(buf, sizeof buf, "x%d", st->pulses);
    ui_btn_set_text(c->pulse_btn, buf);
    ui_btn_set_text(c->gate_btn, GATE_NAMES[st->gate]);
    ui_btn_set_color(c->gate_btn, C_GATE[st->gate]);
    ui_btn_set_color(c->slide_btn, st->slide ? C_SLIDE : C_BTN_DK);
    ui_btn_set_color(c->skip_btn, st->skip ? C_SKIP : C_BTN_DK);
}

void seq_page_refresh(void) {
    for (int i = 0; i < NUM_STAGES; i++) seq_page_refresh_col(i);
}

void seq_page_refresh_notes(void) {
    for (int i = 0; i < NUM_STAGES; i++) refresh_note(i);
}

void seq_page_light(int stage, int track, bool on) {
    ui_led_set(s_cols[stage].leds[track], on ? C_TRK[track] : C_LED_OFF);
}

void seq_page_show(bool on) {
    if (on)
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void seq_page_create(lv_obj_t *parent) {
    s_panel = ui_panel(parent, 0, PAGE_Y, 1024, PAGE_H, C_PANEL, 0);
    for (int i = 0; i < NUM_STAGES; i++) {
        col_t *c = &s_cols[i];
        int x = SEQ_X0 + i * COL_W;
        int yv = 4;
        for (int t = 0; t < NUM_TRACKS; t++)
            c->leds[t] = ui_led(s_panel, x + t * (COL_INNER / 2 + 2), yv, COL_INNER / 2 - 4, 8);
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        c->num_btn = ui_btn(s_panel, num, x, COL_INNER, 30, open_detail, i, C_BTN_DK, yv + 14, NULL);
        c->note_lbl = ui_label(s_panel, "--", x + 4, yv + 50);

        c->slider = lv_slider_create(s_panel);
        lv_obj_set_size(c->slider, SLIDER_W, SLIDER_H);
        lv_obj_set_pos(c->slider, x + (COL_INNER - SLIDER_W) / 2, yv + 74);
        lv_slider_set_range(c->slider, 0, PITCH_STEPS);
        ui_style_bar_slider(c->slider, C_KNOB_SEQ);
        lv_obj_add_event_cb(c->slider, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_remove_flag(c->slider, LV_OBJ_FLAG_SCROLLABLE);

        int by = yv + 74 + SLIDER_H + 38;
        c->pulse_btn = ui_btn(s_panel, "x1", x, COL_INNER, 30, cycle_pulse, i, C_BTN, by, NULL);
        c->gate_btn = ui_btn(s_panel, "MULT", x, COL_INNER, 30, cycle_gate, i, C_GATE[0], by + 34,
                             NULL);
        int half = COL_INNER / 2 - 3;
        c->slide_btn = ui_btn(s_panel, "SLD", x, half, 30, toggle_slide, i, C_BTN_DK, by + 68, NULL);
        c->skip_btn = ui_btn(s_panel, "SKP", x + half + 6, half, 30, toggle_skip, i, C_BTN_DK,
                             by + 68, NULL);
    }
    // the presets, under the tracks
    preset_row_create(s_panel, 4 + 74 + SLIDER_H + 38 + 98 + 12);
    seq_page_refresh();
}

// =====================================================================
// STAGE DETAIL POPUP (pulse count / gate type / ratchet / prob / accum)
// =====================================================================

static struct {
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *vals[5];
    int stage;
} D = {.stage = -1};

static void detail_refresh(void) {
    if (D.stage < 0) return;
    const stage_t *st = &g_pat.stages[D.stage];
    lv_label_set_text_fmt(D.vals[0], "x%d", st->pulses);
    lv_label_set_text(D.vals[1], GATE_NAMES[st->gate]);
    lv_label_set_text_fmt(D.vals[2], "x%d", st->ratchet);
    lv_label_set_text_fmt(D.vals[3], "%d%%", st->prob);
    lv_label_set_text_fmt(D.vals[4], "%+d st", st->accum);
    seq_page_refresh_col(D.stage);   // keep the SEQ column in sync
}

// arg: row * 2 + (0 = minus, 1 = plus)
static void detail_step(intptr_t arg) {
    if (D.stage < 0) return;
    stage_t *st = &g_pat.stages[D.stage];
    int d = (arg & 1) ? 1 : -1;
    switch (arg >> 1) {
    case 0: st->pulses = clampi(st->pulses + d, 1, 8); break;
    case 1: st->gate = pmod(st->gate + d, GATE_COUNT); break;
    case 2: st->ratchet = clampi(st->ratchet + d, 1, 8); break;
    case 3: st->prob = clampi(st->prob + 10 * d, 0, 100); break;
    case 4: st->accum = clampi(st->accum + d, -24, 24); break;
    }
    detail_refresh();
}

static void detail_close(intptr_t a) {
    D.stage = -1;
    lv_obj_add_flag(D.panel, LV_OBJ_FLAG_HIDDEN);
}

void detail_popup_show(int stage) {
    D.stage = stage;
    lv_label_set_text_fmt(D.title, "Stage %d detail", stage + 1);
    detail_refresh();
    lv_obj_remove_flag(D.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(D.panel);
}

void detail_popup_create(lv_obj_t *parent) {
    D.panel = ui_panel(parent, 0, 0, 560, 320, C_PANEL2, 10);
    lv_obj_align(D.panel, LV_ALIGN_CENTER, 0, 0);
    D.title = ui_section(D.panel, "Stage", 20, 16);
    ui_btn(D.panel, "Close", 440, 100, 40, detail_close, 0, C_BTN, 12, NULL);
    static const char *rows[5] = {"Pulse count", "Gate type", "Ratchet", "Probability",
                                  "Accumulate"};
    for (int r = 0; r < 5; r++) {
        int y = 64 + r * 48;
        ui_label(D.panel, rows[r], 24, y + 8);
        ui_btn(D.panel, "-", 250, 50, 38, detail_step, r * 2, C_BTN, y, NULL);
        D.vals[r] = ui_label(D.panel, "", 330, y + 8);
        ui_btn(D.panel, "+", 420, 50, 38, detail_step, r * 2 + 1, C_BTN, y, NULL);
    }
    lv_obj_add_flag(D.panel, LV_OBJ_FLAG_HIDDEN);
}
