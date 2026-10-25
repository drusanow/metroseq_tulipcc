// ui_set.c - SET page (quantiser, CV output, presets, projects, timing)
// and the SAVE-PROJECT popup with the on-screen keyboard.
#include <stdio.h>
#include <string.h>

#include "platform/preview_audio.h"
#include "platform/storage.h"
#include "sdkconfig.h"
#include "ui_common.h"

static struct {
    lv_obj_t *panel;
    lv_obj_t *root_lbl, *scale_lbl, *boct_lbl, *range_lbl;
    ui_btn_t *merge_btn;
    lv_obj_t *cv_status, *audio_lbl;
    ui_btn_t *slot_btns[NUM_PRESETS];
    lv_obj_t *save_lbl, *info, *proj_lbl;
    ui_btn_t *copy_btn, *paste_btn;
    lv_obj_t *timing_lbl;
    int save_slot;
    int cur_preset;          // last loaded/saved slot, -1 = none
    int project_idx;         // selected named project, -1 = none
    bool have_clip;
    pattern_t clip;          // COPY/PASTE snapshot
    uint32_t poll_ms;
} S = {.cur_preset = -1, .project_idx = -1};

static void info(const char *t) { lv_label_set_text(S.info, t); }

// Replace the live pattern (preset load, paste, project load).
static void apply_snapshot(const pattern_t *p) {
    pattern_t tmp = *p;
    pattern_sanitise(&tmp);
    pat_store(&tmp);
    seq_cmd(CMD_OUTPUTS_CHANGED);      // nothing left high across the swap
    seq_cmd(CMD_RESTART_IF_PLAYING);   // Python: start_playback() if playing
    ui_refresh_all();
}

// --- quantiser -------------------------------------------------------------------

static void refresh_quant(void) {
    lv_label_set_text(S.root_lbl, ROOT_NAMES[g_pat.root]);
    lv_label_set_text(S.scale_lbl, SCALES[g_pat.scale_idx].name);
    lv_label_set_text_fmt(S.boct_lbl, "%+d", g_pat.base_octave);
    char lo[8], hi[8];
    note_name(eff_base_midi(&g_pat), lo, sizeof lo);
    note_name(eff_base_midi(&g_pat) + PITCH_RANGE, hi, sizeof hi);
    lv_label_set_text_fmt(S.range_lbl, "%s .. %s", lo, hi);
}

// arg: 0/1 root -/+, 2/3 scale -/+, 4/5 base octave -/+
static void quant_step(intptr_t arg) {
    int d = (arg & 1) ? 1 : -1;
    switch (arg >> 1) {
    case 0: g_pat.root = pmod(g_pat.root + d, 12); break;
    case 1: g_pat.scale_idx = pmod(g_pat.scale_idx + d, NUM_SCALES); break;
    case 2: g_pat.base_octave = clampi(g_pat.base_octave + d, -3, 5); break;
    }
    refresh_quant();   // light touch: no whole-page redraw while playing
    ui_refresh_notes();
}

// --- CV / audio ------------------------------------------------------------------

static void cv_toggle(intptr_t a) { seq_cmd(CMD_CV_TOGGLE); }
static void cv_rescan(intptr_t a) { seq_cmd(CMD_CV_RESCAN); }

static void toggle_merge(intptr_t a) {
    g_pat.merge_cv = !g_pat.merge_cv;
    seq_cmd(CMD_OUTPUTS_CHANGED);   // drop everything low, clear ownership
    set_page_refresh();
    ui_header_info(g_pat.merge_cv ? "TRK2->TRK1 CV" : "TRK1/TRK2 split");
}

static void toggle_audio(intptr_t a) {
    preview_set_enabled(!preview_enabled());
    set_page_refresh();
}

// --- presets ---------------------------------------------------------------------

static void refresh_slots(void) {
    for (int i = 0; i < NUM_PRESETS; i++) {
        if (i == S.cur_preset)
            ui_btn_set_color(S.slot_btns[i], C_PRESET_CUR);
        else if (storage_preset_used(i))
            ui_btn_set_color(S.slot_btns[i], C_BTN);
        else
            ui_btn_set_color(S.slot_btns[i], C_BTN_DK);
    }
}

static void load_preset(intptr_t i) {
    static pattern_t p;
    char msg[32];
    if (!storage_preset_get((int)i, &p)) {
        snprintf(msg, sizeof msg, "slot %d empty", (int)i + 1);
        info(msg);
        return;
    }
    apply_snapshot(&p);
    S.cur_preset = (int)i;
    refresh_slots();
    snprintf(msg, sizeof msg, "loaded slot %d", (int)i + 1);
    info(msg);
}

static void save_preset(intptr_t a) {
    static pattern_t p;
    pat_load(&p);
    storage_preset_save(S.save_slot, &p);
    S.cur_preset = S.save_slot;
    refresh_slots();
    char msg[40];
    snprintf(msg, sizeof msg, "saved slot %d%s", S.save_slot + 1,
             (CONFIG_METRO_DEFER_FLASH_WHILE_PLAYING && g_status.playing) ? " (flash on STOP)" : "");
    info(msg);
}

static void slot_step(intptr_t up) {
    S.save_slot = pmod(S.save_slot + (up ? 1 : -1), NUM_PRESETS);
    lv_label_set_text_fmt(S.save_lbl, "slot %d", S.save_slot + 1);
}

static void clip_copy(intptr_t a) {
    pat_load(&S.clip);
    S.have_clip = true;
    ui_btn_flash(S.copy_btn);
    info("copied");
}

static void clip_paste(intptr_t a) {
    if (!S.have_clip) {
        info("clipboard empty");
        return;
    }
    apply_snapshot(&S.clip);
    ui_btn_flash(S.paste_btn);
    info("pasted");
}

// --- projects --------------------------------------------------------------------

static void refresh_projects(void) {
    int n = storage_project_count();
    if (n == 0 || S.project_idx < 0)
        lv_label_set_text(S.proj_lbl, "-- no project --");
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
    apply_snapshot(&p);
    char msg[48];
    snprintf(msg, sizeof msg, "loaded '%s'", storage_project_name(S.project_idx));
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

// --- timing panel (new: live jitter numbers from the sequencer) -------------

static void stats_reset(intptr_t a) { seq_cmd(CMD_STATS_RESET); }

void set_page_poll(void) {
    uint32_t now = lv_tick_get();
    if (now - S.poll_ms < 250) return;
    S.poll_ms = now;
    int n = (g_status.dac_present[0] ? 1 : 0) + (g_status.dac_present[1] ? 1 : 0);
    bool on = g_status.cv_enabled && n > 0;
    lv_label_set_text_fmt(S.cv_status, "CV %s - %d DAC(s) found%s", on ? "ON" : "off", n,
                          (on && !g_status.cv_ready) ? " (soft start)" : "");
    uint32_t cnt = g_status.late_count;
    uint32_t avg = cnt ? (uint32_t)(g_status.late_sum_us / cnt) : 0;
    uint32_t over1ms = g_status.late_hist[5] + g_status.late_hist[6];
    lv_label_set_text_fmt(S.timing_lbl,
                          "TIMING  gates %lu | late avg %lu us, max %lu us | >1ms %lu | "
                          "resync %lu | i2c err %lu | touch deferred %lu",
                          (unsigned long)cnt, (unsigned long)avg,
                          (unsigned long)g_status.late_max_us, (unsigned long)over1ms,
                          (unsigned long)g_status.resyncs, (unsigned long)g_status.i2c_errors,
                          (unsigned long)g_status.touch_deferred);
}

// --- page ------------------------------------------------------------------------

void set_page_refresh(void) {
    refresh_quant();
    ui_btn_set_color(S.merge_btn, g_pat.merge_cv ? C_PLAY : C_BTN_DK);
    lv_label_set_text(S.audio_lbl, preview_enabled() ? "ON" : "off");
    refresh_slots();
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

void set_page_create(lv_obj_t *parent) {
    lv_obj_t *p = S.panel = ui_panel(parent, 0, PAGE_Y, 1024, PAGE_H, C_PANEL, 0);

    ui_label(p, "QUANTISER", 30, 14);
    ui_label(p, "Root", 30, 56);
    ui_btn(p, "-", 120, 44, 38, quant_step, 0, C_BTN, 50, NULL);
    S.root_lbl = ui_label(p, "C", 178, 58);
    ui_btn(p, "+", 230, 44, 38, quant_step, 1, C_BTN, 50, NULL);
    ui_label(p, "Scale", 30, 106);
    ui_btn(p, "-", 120, 44, 38, quant_step, 2, C_BTN, 100, NULL);
    S.scale_lbl = ui_label(p, "Chromatic", 178, 108);
    ui_btn(p, "+", 320, 44, 38, quant_step, 3, C_BTN, 100, NULL);
    ui_label(p, "Base oct", 430, 56);
    ui_btn(p, "-", 540, 44, 38, quant_step, 4, C_BTN, 50, NULL);
    S.boct_lbl = ui_label(p, "0", 600, 58);
    ui_btn(p, "+", 650, 44, 38, quant_step, 5, C_BTN, 50, NULL);
    S.range_lbl = ui_label(p, "", 430, 108);

    ui_label(p, "CV OUTPUT  (TRK1 -> 0x59, TRK2 -> 0x58; VOUT0 gate, VOUT1 pitch, 1V/oct)", 30,
             166);
    ui_btn(p, "CV ON/OFF", 30, 130, 42, cv_toggle, 0, C_BTN_DK, 196, NULL);
    ui_btn(p, "RESCAN DAC", 172, 150, 42, cv_rescan, 0, C_BTN_DK, 196, NULL);
    S.merge_btn = ui_btn(p, "MERGE T2>T1", 340, 180, 42, toggle_merge, 0, C_BTN_DK, 196, NULL);
    S.cv_status = ui_label(p, "", 30, 246);
    ui_label(p, "Audio preview", 560, 246);
    ui_btn(p, "TOGGLE", 680, 110, 42, toggle_audio, 0, C_BTN_DK, 240, NULL);
    S.audio_lbl = ui_label(p, "", 800, 246);

    ui_label(p, "PRESETS  (tap to load; current slot lit orange; SAVE writes the chosen slot; "
                "COPY/PASTE the live seq)", 30, 296);
    for (int i = 0; i < NUM_PRESETS; i++) {
        char nm[4];
        snprintf(nm, sizeof nm, "%d", i + 1);
        S.slot_btns[i] = ui_btn(p, nm, 24 + i * 61, 55, 40, load_preset, i, C_BTN_DK, 324, NULL);
    }
    ui_btn(p, "SAVE ->", 24, 104, 40, save_preset, 0, C_PLAY, 372, NULL);
    ui_btn(p, "<", 138, 38, 40, slot_step, 0, C_BTN, 372, NULL);
    S.save_lbl = ui_label(p, "slot 1", 186, 382);
    ui_btn(p, ">", 244, 38, 40, slot_step, 1, C_BTN, 372, NULL);
    S.copy_btn = ui_btn(p, "COPY", 300, 84, 40, clip_copy, 0, C_BTN_DK, 372, NULL);
    S.paste_btn = ui_btn(p, "PASTE", 392, 84, 40, clip_paste, 0, C_BTN_DK, 372, NULL);
    S.info = ui_label(p, "", 496, 382);

    ui_label(p, "PROJECT", 24, 436);
    ui_btn(p, "SAVE AS...", 110, 120, 40, open_save_project, 0, C_PLAY, 428, NULL);
    ui_btn(p, "<", 250, 38, 40, project_step, 0, C_BTN, 428, NULL);
    S.proj_lbl = ui_label(p, "-- no project --", 300, 438);
    ui_btn(p, ">", 470, 38, 40, project_step, 1, C_BTN, 428, NULL);
    ui_btn(p, "LOAD", 520, 100, 40, project_load, 0, C_BTN, 428, NULL);

    S.timing_lbl = ui_label(p, "", 24, 488);
    ui_btn(p, "CLR", 930, 70, 34, stats_reset, 0, C_BTN_DK, 480, NULL);

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
    ui_label(P.panel, "Save project as:", 16, 16);
    P.ta = lv_textarea_create(P.panel);
    lv_obj_set_size(P.ta, 528, 48);
    lv_obj_set_pos(P.ta, 16, 46);
    lv_textarea_set_one_line(P.ta, true);
    lv_textarea_set_max_length(P.ta, MAX_PROJECT_NAME_LEN);
    lv_textarea_set_placeholder_text(P.ta, "Project name");
    lv_obj_add_event_cb(P.ta, ta_event, LV_EVENT_ALL, NULL);
    ui_btn(P.panel, "Save", 316, 110, 40, popup_save, 0, C_PLAY, 102, NULL);
    ui_btn(P.panel, "Cancel", 434, 110, 40, popup_hide, 0, C_BTN_DK, 102, NULL);
    lv_obj_add_flag(P.panel, LV_OBJ_FLAG_HIDDEN);

    P.kb = lv_keyboard_create(parent);
    lv_obj_set_size(P.kb, 1024, 260);
    lv_obj_align(P.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(P.kb, P.ta);
    lv_obj_add_flag(P.kb, LV_OBJ_FLAG_HIDDEN);
}
