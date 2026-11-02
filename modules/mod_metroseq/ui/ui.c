// ui.c - header bar, page switching and the status poll that draws the
// playheads (Python Header, show_page, _update_playheads, transport).
#include "ui.h"

#include <stdio.h>

#include "platform/storage.h"
#include "tulip/touchcal_ui.h"
#include "studio/widgets.h"
#include "ui_common.h"

static struct {
    ui_btn_t *play_btn;
    ui_btn_t *page_btns[4];
    ui_btn_t *cv_btn;
    lv_obj_t *bpm_label;
    lv_obj_t *info;
    int page;
    int8_t last_disp[NUM_TRACKS];
    int8_t last_mod_disp;
    uint32_t info_seq;
    int cv_on_shown;
    int play_shown;      // 0 stopped, 1 playing, 2 armed (waiting for MIDI clock)
    int tempo_shown;     // last value drawn in the BPM label
} H = {.page = -1, .last_disp = {-1, -1}, .last_mod_disp = -1, .cv_on_shown = -1,
       .play_shown = -1, .tempo_shown = -1};

// --- transport / tempo -------------------------------------------------------

static void toggle_play(intptr_t a) { seq_cmd(CMD_TOGGLE_PLAY); }

static void reset_sequence(intptr_t a) {
    seq_cmd(CMD_RESET);
    ui_header_info("reset");
}

// BPM label: the master tempo on the internal clock (shared by every
// Studio app, so another app may change it), or the measured tempo when
// following MIDI clock (encoded negative so it never collides).
static void draw_tempo(void) {
    studio_clock_status_t st;
    studio_clock_status(&st);
    if (!st.ext) {
        // keep the pattern's tempo in step, so a saved preset has it
        int bpm = clampi((studio_bpm_x100() + 50) / 100, MIN_BPM, MAX_BPM);
        if (bpm != g_pat.bpm) g_pat.bpm = (uint16_t)bpm;
    }
    int v = st.ext ? -1 - st.ext_bpm_x10 : g_pat.bpm;
    if (v == H.tempo_shown) return;
    H.tempo_shown = v;
    if (v >= 0)
        lv_label_set_text_fmt(H.bpm_label, "%d BPM", v);
    else if (st.ext_bpm_x10)
        lv_label_set_text_fmt(H.bpm_label, "MIDI %d", (st.ext_bpm_x10 + 5) / 10);
    else
        lv_label_set_text(H.bpm_label, "MIDI --");
}

void ui_set_bpm(int bpm) {
    g_pat.bpm = clampi(bpm, MIN_BPM, MAX_BPM);
    seq_cmd(CMD_TEMPO);
    draw_tempo();
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

// The Studio header shape (studio.h): buttons STUDIO_HDR_BTN_H tall at
// STUDIO_HDR_Y, the app's name on the left, the squares on the right.
static void header_create(lv_obj_t *scr) {
    const int y = STUDIO_HDR_Y, h = STUDIO_HDR_BTN_H;
    lv_obj_t *p = ui_panel(scr, 0, 0, 1024, STUDIO_HDR_LINE_Y, C_BG, 0);
    lv_obj_t *t = ui_label(p, "METROSEQ", 6, 15);
    lv_obj_set_style_text_font(t, SW_F_MID, 0);
    H.play_btn = ui_btn(p, "PLAY", 134, 84, h, toggle_play, 0, C_PLAY, y, NULL);
    ui_btn(p, "RST", 224, 50, h, reset_sequence, 0, C_BTN_DK, y, NULL);
    ui_btn(p, "-", 284, 40, h, bpm_down, 0, C_BTN, y, bpm_down_fast);
    H.bpm_label = ui_label(p, "120 BPM", 326, 19);
    lv_obj_set_width(H.bpm_label, 76);
    lv_obj_set_style_text_align(H.bpm_label, LV_TEXT_ALIGN_CENTER, 0);
    ui_btn(p, "+", 404, 40, h, bpm_up, 0, C_BTN, y, bpm_up_fast);
    static const char *names[4] = {"SEQ", "TRK", "MOD", "SET"};
    for (int i = 0; i < 4; i++)
        H.page_btns[i] = ui_btn(p, names[i], 456 + i * 62, 58, h, page_cb, i, C_BTN_DK, y, NULL);
    H.cv_btn = ui_btn(p, "CV", 708, 50, h, toggle_cv, 0, C_BTN_DK, y, NULL);
    H.info = ui_label(p, "", 766, 19);
    lv_obj_set_style_text_color(H.info, c332(C_DIM), 0);
    lv_obj_set_width(H.info, STUDIO_SQ_X - 766 - 6);
    lv_label_set_long_mode(H.info, LV_LABEL_LONG_CLIP);
    // the Studio squares: blue = next app, red = Studio home / launcher
    studio_chrome(p);
    ui_panel(scr, 0, STUDIO_HDR_LINE_Y, 1024, 1, C_PANEL2, 0);
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
    draw_tempo();
    seq_page_refresh();
    trk_page_refresh();
    mod_page_refresh();
    set_page_refresh();
}

void ui_refresh_notes(void) { seq_page_refresh_notes(); }

void ui_poll(bool visible) {
    preset_poll();          // the current preset keeps every change
    if (!visible) return;   // still playing; nothing to draw
    bool playing = g_status.playing;
    studio_clock_status_t st;
    studio_clock_status(&st);
    int ps = playing ? 1 : (st.armed ? 2 : 0);
    if (ps != H.play_shown) {
        H.play_shown = ps;
        static const char *const txt[3] = {"PLAY", "STOP", "ARMED"};
        static const uint8_t col[3] = {C_PLAY, C_STOP, C_SEL};
        ui_btn_set_text(H.play_btn, txt[ps]);
        ui_btn_set_color(H.play_btn, col[ps]);
    }
    update_playheads(playing);
    draw_tempo();

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

static bool s_built;
bool ui_built(void) { return s_built; }

void ui_create(lv_obj_t *scr) {
    s_built = true;
    lv_obj_set_style_bg_color(scr, c332(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    header_create(scr);
    seq_page_create(scr);
    trk_page_create(scr);
    mod_page_create(scr);
    set_page_create(scr);
    detail_popup_create(scr);
    save_popup_create(scr);
    draw_tempo();
    show_page(0);
    tulip_touchcal_set_notify(ui_header_info);
}
