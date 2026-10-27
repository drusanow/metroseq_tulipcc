// ui_set.c - SET page (quantiser and RANGE, CV output, setup, projects,
// timing) and the SAVE-PROJECT popup with the on-screen keyboard.
//
// The page is a grid: three columns of rows, each row a label on the left
// and its control in the same place - a "-" value "+" stepper, or one
// button the same width.
#include <stdio.h>
#include <string.h>

#include "platform/preview_audio.h"
#include "platform/storage.h"
#include "sdkconfig.h"
#include "tulip/touchcal_ui.h"
#include "ui_common.h"

#define COL_X(c)   (24 + (c) * 336)   // three columns
#define ROW_Y(r)   (44 + (r) * 52)
#define CTRL_DX    122                // the control, after the label
#define CTRL_W     186                // "-" 44 + value 98 + "+" 44
#define STEP_W     44
#define ROW_H      40

static struct {
    lv_obj_t *panel;
    lv_obj_t *root_lbl, *scale_lbl, *boct_lbl, *range_lbl, *notes_lbl;
    ui_btn_t *cv_btn, *merge_btn, *clock_btn, *audio_btn;
    lv_obj_t *cv_status;
    lv_obj_t *info, *proj_lbl;
    lv_obj_t *timing_lbl;
    int project_idx;         // selected named project, -1 = none
    uint32_t poll_ms;
} S = {.project_idx = -1};

static void info(const char *t) { lv_label_set_text(S.info, t); }

// --- the grid ------------------------------------------------------------------------

static void row_label(lv_obj_t *p, int c, int r, const char *t) {
    ui_label(p, t, COL_X(c), ROW_Y(r) + 12);
}

// "-" value "+"; arg*2 and arg*2+1 go to cb
static lv_obj_t *stepper(lv_obj_t *p, int c, int r, const char *name, ui_cb_t cb, int arg) {
    row_label(p, c, r, name);
    int x = COL_X(c) + CTRL_DX, y = ROW_Y(r);
    ui_btn(p, "-", x, STEP_W, ROW_H, cb, arg * 2, C_BTN, y, NULL);
    lv_obj_t *v = ui_label(p, "", x + STEP_W, y + 12);
    lv_obj_set_width(v, CTRL_W - 2 * STEP_W);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
    ui_btn(p, "+", x + CTRL_W - STEP_W, STEP_W, ROW_H, cb, arg * 2 + 1, C_BTN, y, NULL);
    return v;
}

static ui_btn_t *button(lv_obj_t *p, int c, int r, const char *name, const char *txt, ui_cb_t cb) {
    row_label(p, c, r, name);
    return ui_btn(p, txt, COL_X(c) + CTRL_DX, CTRL_W, ROW_H, cb, 0, C_BTN_DK, ROW_Y(r), NULL);
}

// a value with no control, where the control would be
static lv_obj_t *readout(lv_obj_t *p, int c, int r, const char *name) {
    row_label(p, c, r, name);
    lv_obj_t *v = ui_label(p, "", COL_X(c) + CTRL_DX, ROW_Y(r) + 12);
    lv_obj_set_width(v, CTRL_W);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(v, c332(C_DIM), 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
    return v;
}

// --- quantiser ---------------------------------------------------------------------------

static void refresh_quant(void) {
    lv_label_set_text(S.root_lbl, ROOT_NAMES[g_pat.root]);
    lv_label_set_text(S.scale_lbl, SCALES[g_pat.scale_idx].name);
    lv_label_set_text_fmt(S.boct_lbl, "%+d", g_pat.base_octave);
    lv_label_set_text_fmt(S.range_lbl, "%d OCT", g_pat.range_oct);
    char lo[8], hi[8];
    note_name(eff_base_midi(&g_pat), lo, sizeof lo);
    note_name(eff_base_midi(&g_pat) + 12 * g_pat.range_oct, hi, sizeof hi);
    lv_label_set_text_fmt(S.notes_lbl, "%s .. %s", lo, hi);
}

// arg: 0/1 root -/+, 2/3 scale -/+, 4/5 base octave -/+, 6/7 range -/+
static void quant_step(intptr_t arg) {
    int d = (arg & 1) ? 1 : -1;
    switch (arg >> 1) {
    case 0: g_pat.root = pmod(g_pat.root + d, 12); break;
    case 1: g_pat.scale_idx = pmod(g_pat.scale_idx + d, NUM_SCALES); break;
    case 2:
        g_pat.base_octave = clampi(g_pat.base_octave + d, MIN_BASE_OCTAVE, MAX_BASE_OCTAVE);
        break;
    case 3: g_pat.range_oct = clampi(g_pat.range_oct + d, MIN_RANGE_OCT, MAX_RANGE_OCT); break;
    }
    refresh_quant();   // light touch: no whole-page redraw while playing
    ui_refresh_notes();
}

// --- CV / setup --------------------------------------------------------------------------

static void cv_toggle(intptr_t a) { seq_cmd(CMD_CV_TOGGLE); }
static void cv_rescan(intptr_t a) { seq_cmd(CMD_CV_RESCAN); }

static void toggle_merge(intptr_t a) {
    g_pat.merge_cv = !g_pat.merge_cv;
    seq_cmd(CMD_OUTPUTS_CHANGED);   // drop everything low, clear ownership
    set_page_refresh();
    ui_header_info(g_pat.merge_cv ? "TRK2->TRK1 CV" : "TRK1/TRK2 split");
}

static void toggle_clock(intptr_t a) {
    // the Studio master clock: every app follows it
    studio_set_clock_src(studio_clock_src() == CLOCK_SRC_MIDI ? CLOCK_SRC_INT : CLOCK_SRC_MIDI);
    set_page_refresh();
}

static void touch_cal(intptr_t a) { tulip_touchcal_start(NULL); }

static void toggle_audio(intptr_t a) {
    preview_set_enabled(!preview_enabled());
    set_page_refresh();
}

// --- projects --------------------------------------------------------------------------

static void refresh_projects(void) {
    int n = storage_project_count();
    if (n == 0 || S.project_idx < 0)
        lv_label_set_text(S.proj_lbl, n ? "-- pick a project --" : "-- no projects yet --");
    else
        lv_label_set_text_fmt(S.proj_lbl, "%d/%d  %s", S.project_idx + 1, n,
                              storage_project_name(S.project_idx));
}

static void project_step(intptr_t up) {
    int n = storage_project_count();
    if (n == 0) return;
    S.project_idx = pmod(S.project_idx + (up ? 1 : -1), n);
    refresh_projects();
}

static void project_load(intptr_t a) {
    static pattern_t p;
    if (S.project_idx < 0 || !storage_project_get(S.project_idx, &p)) {
        info("no project selected");
        return;
    }
    // into the current preset, on the top of the sequence; its tempo too
    ui_set_bpm(p.bpm);
    pat_swap(&p);
    char msg[48];
    snprintf(msg, sizeof msg, "%s '%s'", g_status.playing ? "on the top:" : "loaded",
             storage_project_name(S.project_idx));
    info(msg);
}

static void open_save_project(intptr_t a) { save_popup_show(); }

static bool do_save_project(const char *name_in) {
    char name[MAX_PROJECT_NAME_LEN + 1];
    // strip leading/trailing spaces
    while (*name_in == ' ') name_in++;
    snprintf(name, sizeof name, "%s", name_in);
    for (int i = (int)strlen(name) - 1; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    if (!name[0]) snprintf(name, sizeof name, "Project %d", storage_project_count() + 1);
    static pattern_t p;
    pat_load(&p);
    S.project_idx = storage_project_save(name, &p);
    refresh_projects();
    char msg[48];
    snprintf(msg, sizeof msg, "saved '%s'", name);
    info(msg);
    return true;
}

// --- status: CV and timing ------------------------------------------------------------

static void stats_reset(intptr_t a) { seq_cmd(CMD_STATS_RESET); }

void set_page_poll(void) {
    uint32_t now = lv_tick_get();
    if (now - S.poll_ms < 250) return;
    S.poll_ms = now;
    int n = (g_status.dac_present[0] ? 1 : 0) + (g_status.dac_present[1] ? 1 : 0);
    bool on = g_status.cv_enabled && n > 0;
    lv_label_set_text_fmt(S.cv_status, "%d DAC%s%s", n, n == 1 ? "" : "s",
                          !on ? ", off" : !g_status.cv_ready ? ", starting" : ", ready");
    ui_btn_set_text(S.cv_btn, on ? "ON" : "OFF");
    ui_btn_set_color(S.cv_btn, on ? C_PLAY : C_BTN_DK);
    studio_clock_status_t st;
    studio_clock_status(&st);
    uint32_t cnt = g_status.late_count;
    uint32_t avg = cnt ? (uint32_t)(g_status.late_sum_us / cnt) : 0;
    uint32_t over1ms = g_status.late_hist[5] + g_status.late_hist[6];
    lv_label_set_text_fmt(S.timing_lbl,
                          "gates %lu   late avg %lu us, max %lu us   >1 ms %lu   resync %lu/%lu   "
                          "i2c err %lu   audio xrun %lu",
                          (unsigned long)cnt, (unsigned long)avg,
                          (unsigned long)g_status.late_max_us, (unsigned long)over1ms,
                          (unsigned long)st.resyncs,
                          (unsigned long)g_status.late_gates_dropped,
                          (unsigned long)g_status.i2c_errors,
                          (unsigned long)studio_audio_underruns());
}

// --- page ------------------------------------------------------------------------------

void set_page_refresh(void) {
    refresh_quant();
    ui_btn_set_text(S.merge_btn, g_pat.merge_cv ? "TRK2 > TRK1" : "SPLIT");
    ui_btn_set_color(S.merge_btn, g_pat.merge_cv ? C_PLAY : C_BTN_DK);
    bool midi = studio_clock_src() == CLOCK_SRC_MIDI;
    ui_btn_set_text(S.clock_btn, midi ? "MIDI" : "INTERNAL");
    ui_btn_set_color(S.clock_btn, midi ? C_PLAY : C_BTN_DK);
    ui_btn_set_text(S.audio_btn, preview_enabled() ? "ON" : "OFF");
    ui_btn_set_color(S.audio_btn, preview_enabled() ? C_PLAY : C_BTN_DK);
    refresh_projects();
    S.poll_ms = 0;
    set_page_poll();
}

void set_page_show(bool on) {
    if (on) {
        set_page_refresh();
        lv_obj_remove_flag(S.panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(S.panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *hint(lv_obj_t *p, const char *t, int x, int y) {
    lv_obj_t *l = ui_label(p, t, x, y);
    lv_obj_set_style_text_color(l, c332(C_DIM), 0);
    return l;
}

void set_page_create(lv_obj_t *parent) {
    lv_obj_t *p = S.panel = ui_panel(parent, 0, PAGE_Y, 1024, PAGE_H, C_PANEL, 0);

    // column 1: what the sliders play
    ui_section(p, "QUANTISER", COL_X(0), 12);
    S.root_lbl = stepper(p, 0, 0, "Root", quant_step, 0);
    S.scale_lbl = stepper(p, 0, 1, "Scale", quant_step, 1);
    S.boct_lbl = stepper(p, 0, 2, "Base octave", quant_step, 2);
    S.range_lbl = stepper(p, 0, 3, "Range", quant_step, 3);
    S.notes_lbl = readout(p, 0, 4, "Sliders span");

    // column 2: the Mabee DACs
    ui_section(p, "CV OUTPUT", COL_X(1), 12);
    S.cv_btn = button(p, 1, 0, "CV out", "OFF", cv_toggle);
    button(p, 1, 1, "DACs", "RESCAN", cv_rescan);
    S.merge_btn = button(p, 1, 2, "CV routing", "SPLIT", toggle_merge);
    S.cv_status = readout(p, 1, 3, "Status");
    hint(p, "TRK 1 0x59, TRK 2 0x58\nVOUT0 gate, VOUT1 pitch 1 V/oct", COL_X(1), ROW_Y(4) + 4);

    // column 3: the rest of the setup
    ui_section(p, "SETUP", COL_X(2), 12);
    S.clock_btn = button(p, 2, 0, "Clock", "INTERNAL", toggle_clock);
    S.audio_btn = button(p, 2, 1, "Preview", "ON", toggle_audio);
    button(p, 2, 2, "Touch", "CALIBRATE", touch_cal);
    hint(p, "Clock is the Studio's: every\napp follows it. MIDI OUT per\ntrack is on the TRK page.",
         COL_X(2), ROW_Y(3) + 4);

    ui_panel(p, COL_X(0), 316, 1024 - 2 * COL_X(0), 1, C_PANEL2, 0);

    // projects: named copies of the whole pattern
    ui_section(p, "PROJECTS", COL_X(0), 330);
    hint(p, "a named copy of the whole pattern; LOAD puts it in the current preset", 120, 330);
    int y = 358;
    ui_btn(p, "SAVE AS...", COL_X(0), 150, ROW_H, open_save_project, 0, C_PLAY, y, NULL);
    ui_btn(p, "<", COL_X(0) + 166, STEP_W, ROW_H, project_step, 0, C_BTN, y, NULL);
    S.proj_lbl = ui_label(p, "", COL_X(0) + 166 + STEP_W, y + 12);
    lv_obj_set_width(S.proj_lbl, 300);
    lv_obj_set_style_text_align(S.proj_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(S.proj_lbl, LV_LABEL_LONG_CLIP);
    ui_btn(p, ">", COL_X(0) + 166 + STEP_W + 300, STEP_W, ROW_H, project_step, 1, C_BTN, y, NULL);
    ui_btn(p, "LOAD", COL_X(0) + 166 + 2 * STEP_W + 300 + 16, 120, ROW_H, project_load, 0, C_BTN, y,
           NULL);
    S.info = ui_label(p, "", COL_X(2) + CTRL_DX - 40, y + 12);
    lv_obj_set_width(S.info, 1000 - (COL_X(2) + CTRL_DX - 40));
    lv_label_set_long_mode(S.info, LV_LABEL_LONG_CLIP);

    ui_panel(p, COL_X(0), 418, 1024 - 2 * COL_X(0), 1, C_PANEL2, 0);

    // timing: live numbers from the sequencer
    ui_section(p, "TIMING", COL_X(0), 432);
    S.timing_lbl = ui_label(p, "", COL_X(0), 462);
    lv_obj_set_width(S.timing_lbl, 1000 - COL_X(0) - 90);
    lv_label_set_long_mode(S.timing_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(S.timing_lbl, c332(C_DIM), 0);
    ui_btn(p, "CLEAR", 1000 - 80, 80, ROW_H, stats_reset, 0, C_BTN_DK, 456, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

// =====================================================================
// SAVE-PROJECT POPUP (name entry via the on-screen keyboard)
// =====================================================================

static struct {
    lv_obj_t *panel, *ta, *kb;
} P;

static void popup_hide(intptr_t a) {
    lv_obj_add_flag(P.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.kb, LV_OBJ_FLAG_HIDDEN);
}

static void popup_save(intptr_t a) {
    if (do_save_project(lv_textarea_get_text(P.ta))) popup_hide(0);
}

static void ta_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY)
        popup_save(0);
    else if (c == LV_EVENT_CANCEL)
        popup_hide(0);
}

void save_popup_show(void) {
    char def[24];
    snprintf(def, sizeof def, "Project %d", storage_project_count() + 1);
    lv_textarea_set_text(P.ta, def);
    lv_obj_remove_flag(P.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(P.panel);
    lv_obj_remove_flag(P.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(P.kb);
}

void save_popup_create(lv_obj_t *parent) {
    P.panel = ui_panel(parent, 0, 0, 560, 150, C_PANEL2, 8);
    lv_obj_align(P.panel, LV_ALIGN_CENTER, 0, -180);
    ui_section(P.panel, "SAVE PROJECT AS", 16, 16);
    P.ta = lv_textarea_create(P.panel);
    lv_obj_set_size(P.ta, 528, 48);
    lv_obj_set_pos(P.ta, 16, 46);
    lv_textarea_set_one_line(P.ta, true);
    lv_textarea_set_max_length(P.ta, MAX_PROJECT_NAME_LEN);
    lv_textarea_set_placeholder_text(P.ta, "Project name");
    lv_obj_add_event_cb(P.ta, ta_event, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(P.ta, c332(C_BG), 0);
    lv_obj_set_style_border_color(P.ta, c332(C_EDGE), 0);
    lv_obj_set_style_border_width(P.ta, 1, 0);
    lv_obj_set_style_radius(P.ta, 0, 0);
    lv_obj_set_style_text_color(P.ta, lv_color_white(), 0);
    ui_btn(P.panel, "Save", 316, 110, 40, popup_save, 0, C_PLAY, 102, NULL);
    ui_btn(P.panel, "Cancel", 434, 110, 40, popup_hide, 0, C_BTN_DK, 102, NULL);
    lv_obj_add_flag(P.panel, LV_OBJ_FLAG_HIDDEN);

    P.kb = lv_keyboard_create(parent);
    lv_obj_set_size(P.kb, 1024, 260);
    lv_obj_align(P.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(P.kb, P.ta);
    // the Studio look: black keys with an outline
    lv_obj_set_style_bg_color(P.kb, c332(C_BG), 0);
    lv_obj_set_style_border_width(P.kb, 0, 0);
    lv_obj_set_style_radius(P.kb, 0, 0);
    lv_obj_set_style_bg_color(P.kb, c332(C_BG), LV_PART_ITEMS);
    lv_obj_set_style_border_color(P.kb, c332(C_EDGE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(P.kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(P.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(P.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(P.kb, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(P.kb, c332(C_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(P.kb, c332(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(P.kb, lv_color_black(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_flag(P.kb, LV_OBJ_FLAG_HIDDEN);
}
