// ui.c - header bar, page switching and the status poll that draws the
// playheads (Python Header, show_page, _update_playheads, transport).
#include "ui.h"

#include <stdio.h>

#include "ui_common.h"

static struct {
    ui_btn_t *play_btn;
    ui_btn_t *page_btns[4];
    ui_btn_t *cv_btn;
    lv_obj_t *bpm_label;
    lv_obj_t *info;
    int page;
    bool shown_playing;
    int8_t last_disp[NUM_TRACKS];
    int8_t last_mod_disp;
    uint32_t info_seq;
    int cv_on_shown;
} H = {.page = -1, .last_disp = {-1, -1}, .last_mod_disp = -1, .cv_on_shown = -1};

// --- transport / tempo -------------------------------------------------------

static void toggle_play(intptr_t a) { seq_cmd(CMD_TOGGLE_PLAY); }

static void reset_sequence(intptr_t a) {
    seq_cmd(CMD_RESET);
    ui_header_info("reset");
}

void ui_set_bpm(int bpm) {
    g_pat.bpm = clampi(bpm, MIN_BPM, MAX_BPM);
    seq_cmd(CMD_TEMPO);
    lv_label_set_text_fmt(H.bpm_label, "%d BPM", g_pat.bpm);
}

static void bpm_up(intptr_t a) { ui_set_bpm(g_pat.bpm + BPM_STEP); }
static void bpm_down(intptr_t a) { ui_set_bpm(g_pat.bpm - BPM_STEP); }
static void bpm_up_fast(intptr_t a) { ui_set_bpm(g_pat.bpm + BPM_STEP_FAST); }
static void bpm_down_fast(intptr_t a) { ui_set_bpm(g_pat.bpm - BPM_STEP_FAST); }

static void toggle_cv(intptr_t a) { seq_cmd(CMD_CV_TOGGLE); }

static void page_cb(intptr_t i) { show_page((int)i); }

void ui_header_info(const char *t) {
    lv_label_set_text(H.info, t);
}

static void header_create(lv_obj_t *scr) {
    lv_obj_t *p = ui_panel(scr, 0, SCREEN_TOP, 1024, HDR_H, C_PANEL2, 0);
    int y = (HDR_H - HDR_BTN_H) / 2;
    H.play_btn = ui_btn(p, "PLAY", 8, 92, HDR_BTN_H, toggle_play, 0, C_PLAY, y, NULL);
    ui_btn(p, "RST", 108, 56, HDR_BTN_H, reset_sequence, 0, C_BTN_DK, y, NULL);
    ui_btn(p, "-", 180, 40, HDR_BTN_H, bpm_down, 0, C_BTN, y, bpm_down_fast);
    H.bpm_label = ui_label(p, "120 BPM", 226, 18);
    ui_btn(p, "+", 300, 40, HDR_BTN_H, bpm_up, 0, C_BTN, y, bpm_up_fast);
    static const char *names[4] = {"SEQ", "TRK", "MOD", "SET"};
    for (int i = 0; i < 4; i++)
        H.page_btns[i] = ui_btn(p, names[i], 356 + i * 66, 60, HDR_BTN_H, page_cb, i, C_BTN_DK, y,
                                NULL);
    H.cv_btn = ui_btn(p, "CV", 624, 54, HDR_BTN_H, toggle_cv, 0, C_BTN_DK, y, NULL);
    H.info = ui_label(p, "", 688, 18);
}

// --- pages & playheads -------------------------------------------------------

void ui_playheads_invalidate(void) {
    for (int t = 0; t < NUM_TRACKS; t++) H.last_disp[t] = -1;
    H.last_mod_disp = -1;
}

void show_page(int i) {
    H.page = i;
    seq_page_show(i == 0);
    trk_page_show(i == 1);
    mod_page_show(i == 2);
    set_page_show(i == 3);
    for (int k = 0; k < 4; k++) ui_btn_set_color(H.page_btns[k], k == i ? C_ACCENT : C_BTN_DK);
    // force a repaint of whichever playhead the newly shown page owns
    ui_playheads_invalidate();
}

static void update_playheads(bool playing) {
    if (H.page == 0) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            int8_t cur = playing ? g_status.disp_stage[t] : -1;
            int8_t prev = H.last_disp[t];
            if (cur == prev) continue;
            if (prev >= 0 && prev < NUM_STAGES) seq_page_light(prev, t, false);
            if (cur >= 0 && cur < NUM_STAGES) seq_page_light(cur, t, true);
            H.last_disp[t] = cur;
        }
    } else if (H.page == 2) {
        int8_t cur = playing ? g_status.lane_disp[mod_page_selected()] : -1;
        int8_t prev = H.last_mod_disp;
        if (cur != prev) {
            if (prev >= 0 && prev < NUM_STAGES) mod_page_light(prev, false);
            if (cur >= 0 && cur < NUM_STAGES) mod_page_light(cur, true);
            H.last_mod_disp = cur;
        }
    }
}

void ui_refresh_all(void) {
    lv_label_set_text_fmt(H.bpm_label, "%d BPM", g_pat.bpm);
    seq_page_refresh();
    trk_page_refresh();
    mod_page_refresh();
    set_page_refresh();
}

void ui_refresh_notes(void) { seq_page_refresh_notes(); }

void ui_poll(void) {
    bool playing = g_status.playing;
    if (playing != H.shown_playing) {
        H.shown_playing = playing;
        ui_btn_set_text(H.play_btn, playing ? "STOP" : "PLAY");
        ui_btn_set_color(H.play_btn, playing ? C_STOP : C_PLAY);
    }
    update_playheads(playing);

    char msg[40];
    uint32_t seq;
    status_info_get(msg, sizeof msg, &seq);
    if (seq != H.info_seq) {
        H.info_seq = seq;
        ui_header_info(msg);
    }

    int cv_on = g_status.cv_enabled && (g_status.dac_present[0] || g_status.dac_present[1]);
    if (cv_on != H.cv_on_shown) {
        H.cv_on_shown = cv_on;
        ui_btn_set_color(H.cv_btn, cv_on ? C_PLAY : C_BTN_DK);
    }
    if (H.page == 3) set_page_poll();
}

void ui_create(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, c332(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    header_create(scr);
    seq_page_create(scr);
    trk_page_create(scr);
    mod_page_create(scr);
    set_page_create(scr);
    detail_popup_create(scr);
    save_popup_create(scr);
    lv_label_set_text_fmt(H.bpm_label, "%d BPM", g_pat.bpm);
    show_page(0);
}
