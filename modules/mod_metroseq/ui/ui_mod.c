// ui_mod.c - MOD page: 8 modulation lanes with internal routing, and each
// lane's value out as a MIDI CC.
#include <stdio.h>

#include "ui_common.h"

#define MOD_SLIDER_H SLIDER_H

enum { C_ORDER, C_LEN, C_DIV, C_DEPTH, C_DEST, C_CC, C_CCCH, C_COUNT };

static lv_obj_t *s_panel;
static ui_btn_t *s_lane_btns[NUM_MOD_LANES];
static lv_obj_t *s_leds[NUM_STAGES];
static lv_obj_t *s_sliders[NUM_STAGES];
static lv_obj_t *s_val_lbls[NUM_STAGES];
static lv_obj_t *s_ctrl_vals[C_COUNT];
static int s_sel;

int mod_page_selected(void) { return s_sel; }

static mod_lane_cfg_t *lane(void) { return &g_pat.mods[s_sel]; }

void mod_page_refresh(void) {
    const mod_lane_cfg_t *ln = lane();
    for (int i = 0; i < NUM_MOD_LANES; i++) {
        const mod_lane_cfg_t *la = &g_pat.mods[i];
        if (i == s_sel)
            ui_btn_set_color(s_lane_btns[i], C_SEL);
        else if ((la->dest != 0 && la->depth > 0) || g_pat.lane_cc[i])
            ui_btn_set_color(s_lane_btns[i], C_BTN);
        else
            ui_btn_set_color(s_lane_btns[i], C_BTN_DK);
    }
    for (int i = 0; i < NUM_STAGES; i++) {
        lv_slider_set_value(s_sliders[i], ln->values[i], LV_ANIM_OFF);
        lv_label_set_text_fmt(s_val_lbls[i], "%+d", ln->values[i]);
    }
    lv_label_set_text(s_ctrl_vals[C_ORDER], ORDER_NAMES[ln->order]);
    lv_label_set_text_fmt(s_ctrl_vals[C_LEN], "%d", ln->length);
    lv_label_set_text(s_ctrl_vals[C_DIV], DIV_NAMES[ln->div_i]);
    lv_label_set_text_fmt(s_ctrl_vals[C_DEPTH], "%d%%", ln->depth);
    lv_label_set_text(s_ctrl_vals[C_DEST], DEST_NAMES[ln->dest]);
    int cc = g_pat.lane_cc[s_sel];
    if (cc)
        lv_label_set_text_fmt(s_ctrl_vals[C_CC], "CC %d", cc);
    else
        lv_label_set_text(s_ctrl_vals[C_CC], "OFF");
    lv_label_set_text_fmt(s_ctrl_vals[C_CCCH], "CH %d", g_pat.lane_cc_ch[s_sel]);
}

static void select_lane(intptr_t i) {
    s_sel = (int)i;
    mod_page_refresh();
    ui_playheads_invalidate();
}

static void slider_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    lane()->values[i] = clampi(v, -100, 100);   // live: the engine reads it directly
    lv_label_set_text_fmt(s_val_lbls[i], "%+d", v);
}

// arg: control * 2 + (0 = minus, 1 = plus)
static void step(intptr_t arg) {
    mod_lane_cfg_t *ln = lane();
    int d = (arg & 1) ? 1 : -1;
    switch (arg >> 1) {
    case C_ORDER: ln->order = pmod(ln->order + d, ORD_COUNT); break;
    case C_LEN: ln->length = clampi(ln->length + d, 1, NUM_STAGES); break;
    case C_DIV: ln->div_i = clampi(ln->div_i + d, 0, NUM_DIVS - 1); break;
    case C_DEPTH: ln->depth = clampi(ln->depth + 5 * d, 0, 100); break;
    case C_DEST: ln->dest = pmod(ln->dest + d, NUM_DESTS); break;
    case C_CC: g_pat.lane_cc[s_sel] = clampi(g_pat.lane_cc[s_sel] + d, 0, 127); break;
    case C_CCCH: g_pat.lane_cc_ch[s_sel] = pmod(g_pat.lane_cc_ch[s_sel] - 1 + d, 16) + 1; break;
    }
    mod_page_refresh();
}

// label, then "-" value "+" (vw: the value's width); hold to repeat
static void ctrl_w(int x, int y, const char *name, int which, int vw) {
    ui_label(s_panel, name, x, y + 11);
    ui_btn(s_panel, "-", x + 96, 44, 38, step, which * 2, C_BTN, y, step);
    lv_obj_t *v = s_ctrl_vals[which] = ui_label(s_panel, "", x + 140, y + 11);
    lv_obj_set_width(v, vw);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
    ui_btn(s_panel, "+", x + 140 + vw, 44, 38, step, which * 2 + 1, C_BTN, y, step);
}

static void ctrl(int x, int y, const char *name, int which) { ctrl_w(x, y, name, which, 110); }

void mod_page_light(int stage, bool on) {
    ui_led_set(s_leds[stage], on ? C_ACCENT : C_LED_OFF);
}

void mod_page_show(bool on) {
    if (on) {
        mod_page_refresh();
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void mod_page_create(lv_obj_t *parent) {
    s_panel = ui_panel(parent, 0, PAGE_Y, 1024, PAGE_H, C_PANEL, 0);
    for (int i = 0; i < NUM_MOD_LANES; i++) {
        char nm[4];
        snprintf(nm, sizeof nm, "M%d", i + 1);
        s_lane_btns[i] = ui_btn(s_panel, nm, 24 + i * 122, 112, 34, select_lane, i, C_BTN_DK, 6,
                                NULL);
    }
    int sy = 50;
    for (int i = 0; i < NUM_STAGES; i++) {
        int x = SEQ_X0 + i * COL_W;
        s_leds[i] = ui_led(s_panel, x, sy, COL_INNER, 8);
        int slx = x + (COL_INNER - SLIDER_W) / 2;
        lv_obj_t *sl = lv_slider_create(s_panel);
        lv_obj_set_size(sl, SLIDER_W, MOD_SLIDER_H);
        lv_obj_set_pos(sl, slx, sy + 16);
        lv_slider_set_range(sl, -100, 100);   // bipolar; 0 = centre
        lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);   // fills from the centre
        ui_style_bar_slider(sl, C_KNOB_MOD);
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_remove_flag(sl, LV_OBJ_FLAG_SCROLLABLE);
        s_sliders[i] = sl;
        // blue 0-point marker just left of the track
        lv_obj_t *zero = ui_panel(s_panel, slx - 20, sy + 16 + MOD_SLIDER_H / 2 - 2, 18, 4,
                                  C_MOD_ZERO, 0);
        lv_obj_remove_flag(zero, LV_OBJ_FLAG_CLICKABLE);
        s_val_lbls[i] = ui_label(s_panel, "0", slx - 6, sy + 16 + MOD_SLIDER_H + 4);
    }
    // three columns of the same "-" value "+" rows
    const int cx[3] = {SEQ_X0, SEQ_X0 + 330, SEQ_X0 + 660};
    int cy = sy + 16 + MOD_SLIDER_H + 36;
    ctrl(cx[0], cy, "Order", C_ORDER);
    ctrl(cx[1], cy, "Length", C_LEN);
    ctrl(cx[2], cy, "Clock div", C_DIV);
    int cy2 = cy + 48;
    ctrl(cx[0], cy2, "Depth", C_DEPTH);
    ctrl(cx[1], cy2, "Route to", C_DEST);
    int cy3 = cy2 + 48;
    ctrl(cx[0], cy3, "MIDI CC", C_CC);
    ctrl(cx[1], cy3, "CC channel", C_CCCH);
    lv_obj_t *h = ui_label(s_panel, "the lane's value 0-127,\nsent as it steps", cx[2], cy3 + 2);
    lv_obj_set_style_text_color(h, c332(C_DIM), 0);
    mod_page_refresh();
}
