// wt_pages2.c - Wavetron's MATRIX, VOICE, FX and PATCH pages (the sketch's
// MATRIX, MIX + UNISON, DRIVE / FX / REVERB and patch pages).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_ui_int.h"

// =====================================================================================
// MATRIX: all 4 x 11 routings at once; tap a cell, set its amount
// =====================================================================================

#define CELL_W 84
#define CELL_H 58
#define GRID_X 92
#define GRID_Y 30

static struct {
    lv_obj_t *cell[WT_NMOD][WT_NDEST], *bar[WT_NMOD][WT_NDEST], *txt[WT_NMOD][WT_NDEST];
    float shown[WT_NMOD][WT_NDEST];
    int sm, sd, sel_shown;
    lv_obj_t *what;
    sw_slider_t *amt;
    lv_obj_t *clr_all;
    uint32_t clr_armed;
} X = {.sel_shown = -1};

static const uint32_t MOD_COL[WT_NMOD] = {C_MOD, C_MOD, C_ENV, C_ENV};

static void cell_paint(int m, int d) {
    float a = wt_mx_get(&g_wt.s, m, d);
    X.shown[m][d] = a;
    int half = CELL_W / 2 - 4;
    int w = (int)(fabsf(a) / WT_MX_MAX * half + 0.5f);
    if (a != 0 && w < 2) w = 2;
    lv_obj_set_width(X.bar[m][d], w > 0 ? w : 1);
    lv_obj_set_x(X.bar[m][d], a >= 0 ? CELL_W / 2 : CELL_W / 2 - w);
    lv_obj_set_style_bg_color(X.bar[m][d], sw_c(a >= 0 ? MOD_COL[m] : SW_RED), 0);
    if (a == 0)
        lv_obj_add_flag(X.bar[m][d], LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(X.bar[m][d], LV_OBJ_FLAG_HIDDEN);
    char t[12] = "";
    if (a != 0) snprintf(t, sizeof t, "%+.2f", (double)a);
    lv_label_set_text(X.txt[m][d], t);
}

// the AMOUNT slider edits whichever cell is selected
static float sel_get(void *ud) { return (wt_mx_get(&g_wt.s, X.sm, X.sd) + WT_MX_MAX) / (2 * WT_MX_MAX); }
static void sel_set(void *ud, float x) {
    float a = roundf((x * 2 - 1) * WT_MX_MAX * 20) / 20;
    if (a == wt_mx_get(&g_wt.s, X.sm, X.sd)) return;
    wt_mx_set(&g_wt.s, X.sm, X.sd, a);
    wt_changed();
}
static void sel_text(void *ud, char *b, int n) {
    float a = wt_mx_get(&g_wt.s, X.sm, X.sd);
    snprintf(b, n, a == 0 ? "%s > %s: off" : "%s > %s: %+.2f", WT_MOD_NAMES[X.sm],
             WT_DEST_LABELS[X.sd], (double)a);
}
static const sw_slider_ops_t SEL_OPS = {sel_get, sel_set, sel_text};

static void cell_cb(lv_event_t *e) {
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    X.sm = v >> 4;
    X.sd = v & 15;
    sw_slider_refresh(X.amt);
}

static void nudge(void *ud) {
    float a = wt_mx_get(&g_wt.s, X.sm, X.sd) + 0.05f * (float)(intptr_t)ud;
    wt_mx_set(&g_wt.s, X.sm, X.sd, roundf(a * 20) / 20);
    wt_changed();
    sw_slider_refresh(X.amt);
}

static void clear_one(void *ud) {
    wt_mx_set(&g_wt.s, X.sm, X.sd, 0);
    wt_changed();
    sw_slider_refresh(X.amt);
}

static void clear_all(void *ud) {
    uint32_t now = (uint32_t)(studio_now_us() / 1000);
    if (!(X.clr_armed && now - X.clr_armed < 3000)) {
        X.clr_armed = now ? now : 1;
        sw_btn_text(X.clr_all, "SURE?");
        sw_btn_set(X.clr_all, true);
        return;
    }
    X.clr_armed = 0;
    sw_btn_text(X.clr_all, "CLEAR ALL");
    sw_btn_set(X.clr_all, false);
    wt_mx_clear(&g_wt.s);
    wt_changed();
    wb_refresh();
    wt_ui_toast("matrix cleared");
}

void pg_mx(lv_obj_t *p) {
    for (int d = 0; d < WT_NDEST; d++)
        sw_label(p, WT_DEST_LABELS[d], SW_F_SMALL, C_DIM, GRID_X + d * CELL_W, 10, CELL_W, true);
    for (int m = 0; m < WT_NMOD; m++) {
        int y = GRID_Y + m * (CELL_H + 4);
        sw_label(p, WT_MOD_NAMES[m], SW_F_MID, MOD_COL[m], 8, y + 17, 0, false);
        for (int d = 0; d < WT_NDEST; d++) {
            lv_obj_t *c = X.cell[m][d] = sw_box(p, GRID_X + d * CELL_W, y, CELL_W - 4, CELL_H, C_BG, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_set_style_border_color(c, sw_c(C_LINE), 0);
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(c, cell_cb, LV_EVENT_PRESSED, (void *)(intptr_t)(m << 4 | d));
            sw_box(c, CELL_W / 2 - 1, 6, 1, CELL_H - 30, C_LINE, 0);   // zero
            X.bar[m][d] = sw_box(c, CELL_W / 2, 10, 1, CELL_H - 38, MOD_COL[m], 0);
            X.txt[m][d] = sw_label(c, "", SW_F_SMALL, C_TEXT, 0, CELL_H - 20, CELL_W - 6, true);
            X.shown[m][d] = 1e9f;
        }
    }
    X.what = sw_label(p, "", SW_F_SMALL, C_DIM, 8, 290, 1000, false);
    lv_label_set_text(X.what, "Tap a cell: a modulator (row) into a destination (column). Bars right "
                              "add, red bars left subtract. POS and PAN add directly; PIT, CUT and "
                              "DRV in octaves; LVL scales the level.");
    X.amt = sw_slider(p, "AMOUNT", 8, 330, 520, 44, C_MOD, &SEL_OPS, NULL);
    sw_btn(p, "-", 540, 336, 56, 40, C_MOD, nudge, (void *)-1, SW_F_MID);
    sw_btn(p, "+", 600, 336, 56, 40, C_MOD, nudge, (void *)1, SW_F_MID);
    sw_btn(p, "CLEAR", 668, 336, 110, 40, C_MOD, clear_one, NULL, NULL);
    X.clr_all = sw_btn(p, "CLEAR ALL", 786, 336, 130, 40, SW_RED, clear_all, NULL, NULL);
    sw_label(p, "Polarity (MOD page): an LFO swings both ways (BIPOL) or only up (UNIPOL); an "
                "envelope rises 0..amount (UNIPOL) or swings -amount..+amount (BIPOL).\n"
                "MOD 1 / MOD 2 in AUDIO mode run at the note's pitch: into PIT that's FM, into "
                "LVL it's AM / ring-mod - at audio rate.",
             SW_F_SMALL, C_DIM, 8, 400, 1000, false);
}

void pg_mx_tick(void) {
    for (int m = 0; m < WT_NMOD; m++)
        for (int d = 0; d < WT_NDEST; d++)
            if (wt_mx_get(&g_wt.s, m, d) != X.shown[m][d]) cell_paint(m, d);
    int sel = X.sm << 4 | X.sd;
    if (sel != X.sel_shown) {
        if (X.sel_shown >= 0) {
            lv_obj_t *o = X.cell[X.sel_shown >> 4][X.sel_shown & 15];
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, sw_c(C_LINE), 0);
        }
        lv_obj_t *o = X.cell[X.sm][X.sd];
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_set_style_border_color(o, sw_c(C_TEXT), 0);
        X.sel_shown = sel;
    }
    if (X.clr_armed && (uint32_t)(studio_now_us() / 1000) - X.clr_armed > 3000) {
        X.clr_armed = 0;
        sw_btn_text(X.clr_all, "CLEAR ALL");
        sw_btn_set(X.clr_all, false);
    }
    sw_slider_refresh(X.amt);
}

// =====================================================================================
// VOICE: mode, glide, bend, volume, channel, panic; unison; what's playing
// =====================================================================================

static struct {
    lv_obj_t *ch;
    int ch_shown;
} V = {.ch_shown = -1};

static void ch_cb(void *ud) {
    float c = g_wt.s.p[WP_MCH] + (float)(intptr_t)ud;
    g_wt.s.p[WP_MCH] = c < 1 ? 16 : (c > 16 ? 1 : c);
    wt_changed();
}

static void panic_cb(void *ud) {
    wt_panic_now();
    wt_ui_toast("PANIC: all notes off");
}

void pg_voice(lv_obj_t *p) {
    wt_section(p, "VOICE MODE", 8, 8, C_VOICE);
    wb_choice(p, WP_VMODE, 8, 30, 160, 44, 10, C_VOICE);
    sw_label(p, "MONO: one voice, legato + glide.  UNISON: every note on all 4 voices, spread.  "
                "POLY: 4 notes.",
             SW_F_SMALL, C_DIM, 8, 80, 500, false);
    wb_slider(p, WP_GLIDE, 8, 110, 244, C_VOICE);
    wb_slider(p, WP_BEND, 264, 110, 244, C_VOICE);
    wb_slider(p, WP_VOL, 8, 160, 244, C_VOICE);
    wb_toggle(p, WP_PHSYNC, "PHASE SYNC", 264, 166, 244, 38, C_VOICE);
    sw_label(p, "MIDI IN", SW_F_SMALL, C_DIM, 8, 230, 0, false);
    sw_btn(p, "-", 70, 218, 44, 40, C_VOICE, ch_cb, (void *)-1, SW_F_MID);
    V.ch = sw_label(p, "", SW_F_MID, C_TEXT, 116, 226, 90, true);
    sw_btn(p, "+", 208, 218, 44, 40, C_VOICE, ch_cb, (void *)1, SW_F_MID);
    sw_btn(p, "PANIC", 264, 218, 244, 40, SW_RED, panic_cb, NULL, SW_F_MID);

    sw_line(p, 516, 8, 1, 270);
    wt_section(p, "UNISON", 528, 8, C_VOICE);
    wd_unison(p, 528, 30, 488, 120);
    wb_slider(p, WP_UNI_DET, 528, 160, 238, C_VOICE);
    wb_slider(p, WP_UNI_WIDTH, 778, 160, 238, C_VOICE);
    wb_slider(p, WP_UNI_POS, 528, 210, 238, C_VOICE);
    wb_slider(p, WP_UNI_PHASE, 778, 210, 238, C_VOICE);
    wb_slider(p, WP_UNI_DRIFT, 528, 260, 238, C_VOICE);
    sw_label(p, "PHASE SPREAD needs PHASE SYNC on.", SW_F_SMALL, C_DIM, 778, 272, 238, false);

    sw_line(p, 0, 318, 1024, 1);
    wt_section(p, "VOICES", 8, 328, C_VOICE);
    wd_voices(p, 8, 350, 500, 180);
    wt_section(p, "OUTPUT", 528, 328, C_VOICE);
    wd_scope(p, 528, 350, 488, 180);
}

void pg_voice_tick(void) {
    int c = (int)g_wt.s.p[WP_MCH];
    if (c != V.ch_shown) {
        V.ch_shown = c;
        char t[8];
        snprintf(t, sizeof t, "CH %d", c);
        lv_label_set_text(V.ch, t);
    }
}

// =====================================================================================
// FX
// =====================================================================================

void pg_fx(lv_obj_t *p) {
    wt_section(p, "DISTORTION", 8, 6, C_FX);
    wd_drive(p, 8, 28, 328, 110);
    wb_toggle(p, WP_D_CLIP, "CLIP", 8, 146, 160, 38, C_FX);
    wb_toggle(p, WP_D_FOLD, "FOLD", 176, 146, 160, 38, C_FX);
    wb_slider(p, WP_D_DRIVE, 8, 192, 328, C_FX);
    wb_slider(p, WP_D_BITS, 8, 240, 328, C_FX);
    wb_slider(p, WP_D_RATE, 8, 288, 328, C_FX);
    wb_slider(p, WP_D_MIX, 8, 336, 328, C_FX);

    sw_line(p, 344, 6, 1, 440);
    wt_section(p, "CHORUS", 352, 6, C_FX);
    wb_slider(p, WP_CH_LVL, 352, 28, 328, C_FX);
    wb_slider(p, WP_CH_DLY, 352, 76, 328, C_FX);
    wb_slider(p, WP_CH_RATE, 352, 124, 328, C_FX);
    wb_slider(p, WP_CH_DEP, 352, 172, 328, C_FX);
    wt_section(p, "ECHO", 352, 228, C_FX);
    wb_slider(p, WP_EC_LVL, 352, 250, 328, C_FX);
    wb_slider(p, WP_EC_MS, 352, 298, 328, C_FX);
    wb_slider(p, WP_EC_FB, 352, 346, 328, C_FX);
    wb_slider(p, WP_EC_TONE, 352, 394, 328, C_FX);

    sw_line(p, 688, 6, 1, 440);
    wt_section(p, "REVERB", 696, 6, C_FX);
    wb_slider(p, WP_RV_LVL, 696, 28, 320, C_FX);
    wb_slider(p, WP_RV_LIVE, 696, 76, 320, C_FX);
    wb_slider(p, WP_RV_DAMP, 696, 124, 320, C_FX);
    wb_slider(p, WP_RV_XOVER, 696, 172, 320, C_FX);
    wt_section(p, "EQ", 696, 228, C_FX);
    wb_slider(p, WP_EQ_L, 696, 250, 320, C_FX);
    wb_slider(p, WP_EQ_M, 696, 298, 320, C_FX);
    wb_slider(p, WP_EQ_H, 696, 346, 320, C_FX);

    sw_label(p, "In AMY's order: distortion > EQ > chorus > echo > reverb, on Wavetron's own "
                "output. BITS < 24 crushes; RATE DIV holds samples. ECHO TONE: left darker, right "
                "brighter repeats. XOVER: damping works above it.",
             SW_F_SMALL, C_DIM, 8, 470, 1008, false);
}

// =====================================================================================
// PATCH: 24 slots in the sketch's wt_patches.json
// =====================================================================================

static struct {
    lv_obj_t *slot[WT_MAX_PATCHES], *del, *msg;
    int sel;
    uint32_t sig, del_armed;
} P = {.sel = -1, .sig = 0xFFFFFFFFu};

static void slot_cb(void *ud) {
    int i = (int)(intptr_t)ud;
    if (i >= wt_lib_count(g_lib)) i = -1;
    P.sel = i;
    P.sig = 0;
}

static void say(const char *m) { lv_label_set_text(P.msg, m); }

static void load_cb(void *ud) {
    if (P.sel < 0) return say("tap a patch first");
    wt_patch_load(P.sel);
    wb_refresh();
    wt_header_refresh();
    say(wt_last_msg);
    P.sig = 0;
}

static void saved(const char *name) {
    if (wt_patch_save(name)) {
        P.sel = wt_lib_find(g_lib, g_patch_name);
        wt_header_refresh();
    }
    say(wt_last_msg);
    P.sig = 0;
}

static void save_new_cb(void *ud) {
    char def[16];
    snprintf(def, sizeof def, "PATCH %d", (wt_lib_count(g_lib) + 1) % 100);
    wt_keyboard("SAVE AS", g_patch_name[0] ? g_patch_name : def, saved);
}

static void save_over_cb(void *ud) {
    if (P.sel < 0) return say("tap the patch to save over");
    char nm[WT_PATCH_NAME_MAX + 1];
    snprintf(nm, sizeof nm, "%s", wt_lib_name(g_lib, P.sel));
    saved(nm);
}

static void renamed(const char *name) {
    if (P.sel >= 0 && wt_patch_rename(P.sel, name)) {
        wt_header_refresh();
        say("renamed");
    } else {
        say(wt_last_msg);
    }
    P.sig = 0;
}

static void rename_cb(void *ud) {
    if (P.sel < 0) return say("tap the patch to rename");
    wt_keyboard("RENAME", wt_lib_name(g_lib, P.sel), renamed);
}

static void del_cb(void *ud) {
    if (P.sel < 0) return say("tap the patch to delete");
    uint32_t now = (uint32_t)(studio_now_us() / 1000);
    if (!(P.del_armed && now - P.del_armed < 3000)) {
        P.del_armed = now ? now : 1;
        sw_btn_text(P.del, "SURE?");
        sw_btn_set(P.del, true);
        return;
    }
    P.del_armed = 0;
    sw_btn_text(P.del, "DELETE");
    sw_btn_set(P.del, false);
    char t[40];
    snprintf(t, sizeof t, "deleted %s", wt_lib_name(g_lib, P.sel));
    if (wt_patch_delete(P.sel)) say(t);
    P.sel = -1;
    wt_header_refresh();
    P.sig = 0;
}

static void init_cb(void *ud) {
    wt_patch_init();
    wb_refresh();
    wt_header_refresh();
    say("the default sound");
    P.sig = 0;
}

void pg_patch(lv_obj_t *p) {
    for (int i = 0; i < WT_MAX_PATCHES; i++) {
        P.slot[i] = sw_btn(p, "", 8 + (i % 4) * 252, 8 + (i / 4) * 52, 244, 46, C_WT, slot_cb,
                           (void *)(intptr_t)i, SW_F_LABEL);
        lv_obj_set_style_text_align(sw_btn_label(P.slot[i]), LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(sw_btn_label(P.slot[i]), LV_ALIGN_LEFT_MID, 10, 0);
    }
    int y = 330;
    sw_btn(p, "LOAD", 8, y, 150, 50, C_WT, load_cb, NULL, SW_F_MID);
    sw_btn(p, "SAVE AS ...", 166, y, 170, 50, C_WT, save_new_cb, NULL, NULL);
    sw_btn(p, "SAVE OVER", 344, y, 150, 50, C_WT, save_over_cb, NULL, NULL);
    sw_btn(p, "RENAME ...", 502, y, 150, 50, C_WT, rename_cb, NULL, NULL);
    P.del = sw_btn(p, "DELETE", 660, y, 150, 50, SW_RED, del_cb, NULL, NULL);
    sw_btn(p, "INIT SOUND", 818, y, 198, 50, C_WT, init_cb, NULL, NULL);
    P.msg = sw_label(p, "", SW_F_LABEL, C_TEXT, 8, 396, 1000, false);
    sw_label(p, "Patches are in /files/wavetron/wt_patches.json - the AMYboard sketch's own file "
                "format, so you can copy your wt_patches.json there. A patch names its wavetables by "
                "file: they're looked for where it says, then in /files/wavetron/wavetables.",
             SW_F_SMALL, C_DIM, 8, 430, 1000, false);
}

void pg_patch_tick(void) {
    uint32_t sig = (uint32_t)wt_lib_count(g_lib) * 131u + (uint32_t)(P.sel + 1) * 7u + (uint32_t)strlen(g_patch_name);
    for (const char *c = g_patch_name; *c; c++) sig = sig * 31u + (uint8_t)*c;
    if (P.del_armed && (uint32_t)(studio_now_us() / 1000) - P.del_armed > 3000) {
        P.del_armed = 0;
        sw_btn_text(P.del, "DELETE");
        sw_btn_set(P.del, false);
    }
    if (sig == P.sig) return;
    P.sig = sig;
    int n = wt_lib_count(g_lib);
    for (int i = 0; i < WT_MAX_PATCHES; i++) {
        char t[32];
        if (i < n)
            snprintf(t, sizeof t, "%02d  %s", i + 1, wt_lib_name(g_lib, i));
        else
            snprintf(t, sizeof t, "%02d  --", i + 1);
        sw_btn_text(P.slot[i], t);
        sw_btn_set(P.slot[i], i == P.sel);
        bool cur = i < n && !strcmp(wt_lib_name(g_lib, i), g_patch_name);
        if (i != P.sel)
            lv_obj_set_style_text_color(sw_btn_label(P.slot[i]), sw_c(cur ? C_WT : (i < n ? C_TEXT : C_LINE)), 0);
    }
}
