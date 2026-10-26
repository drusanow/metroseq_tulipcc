// wt_pages.c - Wavetron's OSC, FILTER and MOD pages, the wavetable picker
// and the file browser (the sketch's OSC A / OSC B / WT LOAD, FILTER / ENV
// and MOD 1-4 pages).
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "wt_ui_int.h"

// =====================================================================================
// OSC
// =====================================================================================

static struct {
    lv_obj_t *name[2];
    int shown[2];
} O = {.shown = {-1, -1}};

static const uint32_t OSC_COL[2] = {C_OSCA, C_OSCB};

static void browse_open(int osc);

static void step_table(void *ud) {
    int v = (int)(intptr_t)ud, osc = v & 1, d = (v >> 1) ? 1 : -1;
    int cur = osc ? g_ib : g_ia;
    wt_select_table(osc, (cur + d + g_cat.n) % g_cat.n);
}

static void pick_cb(void *ud) {
    int v = (int)(intptr_t)ud;
    wt_select_table(v & 1, v >> 1);
    wt_popup_close();
}

static void pick_files(void *ud) { browse_open((int)(intptr_t)ud); }

// every table in RAM, plus the way to the files
static void pick_open(void *ud) {
    int osc = (int)(intptr_t)ud;
    char t[48];
    snprintf(t, sizeof t, "OSC %c: WAVETABLE", osc ? 'B' : 'A');
    lv_obj_t *p = wt_popup(t, OSC_COL[osc]);
    int cur = osc ? g_ib : g_ia;
    for (int i = 0; i < g_cat.n; i++) {
        int x = 16 + (i % 4) * 250, y = 70 + (i / 4) * 64;
        lv_obj_t *b = sw_btn(p, g_cat.t[i].name, x, y, 240, 54, OSC_COL[osc], pick_cb,
                             (void *)(intptr_t)(i << 1 | osc), SW_F_MID);
        sw_btn_set(b, i == cur);
    }
    sw_label(p, "Built-in: SAW SQUARE HARM VOX FOLD BELL (the sketch's). Files you load stay here "
                "while there's room (4 at a time).",
             SW_F_SMALL, C_DIM, 16, 300, 990, false);
    sw_btn(p, "LOAD A WAV FROM /files ...", 16, 340, 360, 54, OSC_COL[osc], pick_files,
           (void *)(intptr_t)osc, NULL);
}

static void osc_column(lv_obj_t *p, int osc) {
    int x0 = 8 + osc * 512;
    uint32_t col = OSC_COL[osc];
    sw_label(p, osc ? "OSC B" : "OSC A", SW_F_MID, col, x0, 12, 0, false);
    sw_btn(p, "<", x0 + 96, 4, 44, 40, col, step_table, (void *)(intptr_t)(osc), SW_F_MID);
    O.name[osc] = sw_btn(p, "", x0 + 144, 4, 196, 40, col, pick_open, (void *)(intptr_t)osc, SW_F_LABEL);
    sw_btn(p, ">", x0 + 344, 4, 44, 40, col, step_table, (void *)(intptr_t)(2 | osc), SW_F_MID);
    sw_btn(p, "LOAD WAV", x0 + 392, 4, 108, 40, col, pick_files, (void *)(intptr_t)osc, NULL);
    wd_waterfall(p, osc, x0, 52, 500, 236);
    int b = osc ? WP_B_POS : WP_A_POS;   // POS COARSE FINE LVL DRV FOLD PHASE
    wb_slider(p, b + 0, x0, 298, 244, col);
    wb_slider(p, b + 3, x0 + 256, 298, 244, col);
    wb_slider(p, b + 1, x0, 348, 244, col);
    wb_slider(p, b + 2, x0 + 256, 348, 244, col);
    wb_slider(p, b + 4, x0, 398, 244, col);
    wb_slider(p, b + 6, x0 + 256, 398, 244, col);
    wb_toggle(p, b + 5, "FOLD", x0, 452, 120, 38, col);
    sw_label(p, "white: POSITION   colour: the playing voice", SW_F_SMALL, C_DIM, x0 + 132, 463,
             370, false);
}

void pg_osc(lv_obj_t *p) {
    osc_column(p, 0);
    osc_column(p, 1);
    sw_line(p, 508, 8, 1, 480);
    sw_label(p, "PHASE is each table's start phase when PHASE SYNC is on (VOICE page). DRIVE "
                "saturates, FOLD wavefolds. Copy WAVs to /files/wavetron/wavetables with the "
                "launcher's Wi-Fi page.",
             SW_F_SMALL, C_DIM, 8, 504, 1008, false);
}

void pg_osc_tick(void) {
    for (int osc = 0; osc < 2; osc++) {
        int idx = osc ? g_ib : g_ia;
        if (idx != O.shown[osc]) {
            O.shown[osc] = idx;
            sw_btn_text(O.name[osc], g_cat.t[idx].name);
        }
    }
}

// --- the file browser (the sketch's WT LOAD page) ----------------------------------------------------

#define MAXE 160
#define ROWS 8

static struct {
    int osc;
    char cwd[240];
    struct {
        char name[64];
        bool dir;
    } e[MAXE];
    int n, top;
    lv_obj_t *path, *msg, *rows[ROWS];
} B;

static int ent_cmp(const void *a, const void *b) {
    const typeof(B.e[0]) *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool is_table(const char *n) {
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".wav") || !strcasecmp(dot, ".wt"));
}

static void browse_list(void);

static void browse_show(void) {
    char t[256];
    const char *root = studio_files_root();
    snprintf(t, sizeof t, "%s", B.cwd + (root ? strlen(root) : 0));
    lv_label_set_text(B.path, t[0] ? t : "/");
    for (int r = 0; r < ROWS; r++) {
        int i = B.top + r;
        if (i < B.n) {
            char s[80];
            snprintf(s, sizeof s, B.e[i].dir ? "%s /" : "%s", B.e[i].name);
            sw_btn_text(B.rows[r], s);
            lv_obj_remove_flag(B.rows[r], LV_OBJ_FLAG_HIDDEN);
            sw_btn_set(B.rows[r], false);
        } else {
            lv_obj_add_flag(B.rows[r], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!B.n) lv_label_set_text(B.msg, "no folders or .wav / .wt files here");
}

static void browse_list(void) {
    B.n = B.top = 0;
    DIR *d = opendir(B.cwd);
    if (!d) {
        lv_label_set_text(B.msg, "can't open this folder");
        browse_show();
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && B.n < MAXE) {
        if (de->d_name[0] == '.') continue;
        char full[320];
        snprintf(full, sizeof full, "%.239s/%.63s", B.cwd, de->d_name);
        struct stat st;
        bool dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (!dir && !is_table(de->d_name)) continue;
        snprintf(B.e[B.n].name, sizeof B.e[0].name, "%.63s", de->d_name);
        B.e[B.n].dir = dir;
        B.n++;
    }
    closedir(d);
    qsort(B.e, B.n, sizeof B.e[0], ent_cmp);
    lv_label_set_text(B.msg, "");
    browse_show();
}

static void row_cb(void *ud) {
    int i = B.top + (int)(intptr_t)ud;
    if (i >= B.n) return;
    char full[320];
    snprintf(full, sizeof full, "%.239s/%.63s", B.cwd, B.e[i].name);
    if (B.e[i].dir) {
        snprintf(B.cwd, sizeof B.cwd, "%.239s", full);
        browse_list();
        return;
    }
    char err[48];
    lv_label_set_text(B.msg, "loading ...");
    if (wt_load_file(B.osc, full, err, sizeof err)) {
        char t[64];
        snprintf(t, sizeof t, "%.20s > OSC %c", B.e[i].name, B.osc ? 'B' : 'A');
        wt_popup_close();
        wt_ui_toast(t);
    } else {
        char t[96];
        snprintf(t, sizeof t, "%s: %s", B.e[i].name, err);
        lv_label_set_text(B.msg, t);
    }
}

static void up_cb(void *ud) {
    const char *root = studio_files_root();
    char *s = strrchr(B.cwd, '/');
    if (!root || !s || strlen(B.cwd) <= strlen(root)) return;
    *s = 0;
    if (strlen(B.cwd) < strlen(root)) snprintf(B.cwd, sizeof B.cwd, "%.239s", root);
    browse_list();
}

static void page_cb(void *ud) {
    int d = (int)(intptr_t)ud;
    int t = B.top + d * ROWS;
    if (t < 0 || t >= B.n) return;
    B.top = t;
    browse_show();
}

static void browse_open(int osc) {
    const char *root = studio_files_root();
    char t[48];
    snprintf(t, sizeof t, "LOAD A WAVETABLE INTO OSC %c", osc ? 'B' : 'A');
    lv_obj_t *p = wt_popup(t, OSC_COL[osc]);
    if (!root) {
        sw_label(p, "no /files on this Tulip", SW_F_MID, C_TEXT, 16, 80, 0, false);
        return;
    }
    B.osc = osc;
    const char *td = wt_tables_dir();
    snprintf(B.cwd, sizeof B.cwd, "%.239s", td ? td : root);
    sw_btn(p, "UP", 16, 62, 90, 40, OSC_COL[osc], up_cb, NULL, NULL);
    B.path = sw_label(p, "", SW_F_LABEL, C_TEXT, 118, 74, 700, false);
    for (int r = 0; r < ROWS; r++) {
        B.rows[r] = sw_btn(p, "", 16, 110 + r * 46, 880, 42, OSC_COL[osc], row_cb, (void *)(intptr_t)r, NULL);
        lv_obj_set_style_text_align(sw_btn_label(B.rows[r]), LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(sw_btn_label(B.rows[r]), LV_ALIGN_LEFT_MID, 12, 0);
    }
    sw_btn(p, LV_SYMBOL_UP, 908, 110, 100, 180, OSC_COL[osc], page_cb, (void *)-1, SW_F_MID);
    sw_btn(p, LV_SYMBOL_DOWN, 908, 300, 100, 178, OSC_COL[osc], page_cb, (void *)1, SW_F_MID);
    B.msg = sw_label(p, "", SW_F_LABEL, C_TEXT, 16, 488, 990, false);
    sw_label(p, "Any WAV of 2 or more 256-sample cycles: 8/16/24/32-bit or float, mono or stereo "
                "(the sketch's rules). Copy them with the launcher's Wi-Fi page.",
             SW_F_SMALL, C_DIM, 16, 514, 990, false);
    browse_list();
}

// =====================================================================================
// FILTER + AMP
// =====================================================================================

void pg_flt(lv_obj_t *p) {
    wt_section(p, "FILTER", 8, 8, C_FLT);
    wb_choice(p, WP_FTYPE, 8, 30, 66, 38, 4, C_FLT);
    wd_filter(p, 8, 76, 500, 170);
    wb_slider(p, WP_CUTOFF, 8, 256, 244, C_FLT);
    wb_slider(p, WP_RESO, 264, 256, 244, C_FLT);
    wb_slider(p, WP_FENV, 8, 304, 244, C_FLT);
    wb_slider(p, WP_FKBD, 264, 304, 244, C_FLT);
    wb_slider(p, WP_FVEL, 8, 352, 244, C_FLT);
    wb_slider(p, WP_FDRV, 264, 352, 244, C_FLT);
    wb_slider(p, WP_FMIX, 8, 400, 244, C_FLT);
    sw_label(p, "ENV AMT is MOD 4 -> CUTOFF: MOD 4 (MOD page) is the filter envelope. Cutoff "
                "modulation adds in octaves.",
             SW_F_SMALL, C_DIM, 264, 404, 244, false);

    sw_line(p, 516, 8, 1, 470);
    wt_section(p, "AMP ENVELOPE", 528, 8, C_FLT);
    wb_choice(p, WP_ACURVE, 528, 30, 90, 38, 4, C_FLT);
    wd_env(p, 0, 528, 76, 488, 170);
    wb_slider(p, WP_AA, 528, 256, 238, C_FLT);
    wb_slider(p, WP_AD, 778, 256, 238, C_FLT);
    wb_slider(p, WP_AS, 528, 304, 238, C_FLT);
    wb_slider(p, WP_AR, 778, 304, 238, C_FLT);
    wb_slider(p, WP_VSENS, 528, 352, 238, C_FLT);
    sw_label(p, "CURVE: RC, straight lines, DX7-style or exponential segments.", SW_F_SMALL, C_DIM,
             778, 362, 238, false);
}

// =====================================================================================
// MOD 1-4
// =====================================================================================

static struct {
    lv_obj_t *tab[WT_NMOD], *panel[WT_NMOD];
} M;

static void mod_show(void *ud) {
    int k = (int)(intptr_t)ud;
    for (int i = 0; i < WT_NMOD; i++) {
        sw_btn_set(M.tab[i], i == k);
        if (i == k)
            lv_obj_remove_flag(M.panel[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(M.panel[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void lfo_panel(lv_obj_t *q, int k) {
    int b = k ? WP_M2_SHAPE : WP_M1_SHAPE;   // SHAPE MODE RATE RATIO DEPTH PHASE DIV POL TRIG
    wd_lfo(q, k, 8, 0, 500, 190);
    wt_section(q, "SHAPE", 528, 0, C_MOD);
    wb_choice(q, b + 0, 528, 20, 76, 38, 6, C_MOD);
    wt_section(q, "MODE", 528, 66, C_MOD);
    wb_choice(q, b + 1, 528, 86, 100, 38, 6, C_MOD);
    wt_section(q, "POLARITY", 776, 66, C_MOD);
    wb_choice(q, b + 7, 776, 86, 116, 38, 6, C_MOD);
    wt_section(q, "TEMPO SYNC (beats of the Studio tempo)", 528, 132, C_MOD);
    wb_choice(q, b + 6, 528, 152, 56, 38, 6, C_MOD);
    wb_rate(q, k, 8, 200, 244, C_MOD);
    wb_slider(q, b + 4, 264, 200, 244, C_MOD);
    wb_slider(q, b + 5, 528, 200, 238, C_MOD);
    wb_toggle(q, b + 8, "RETRIGGER", 778, 206, 238, 38, C_MOD);
}

static void env_panel(lv_obj_t *q, int k) {
    int b = k == 2 ? WP_M3_A : WP_M4_A;   // A D S R CURVE VEL POL
    wd_env(q, k + 1, 8, 0, 500, 190);
    wt_section(q, "CURVE", 528, 0, C_ENV);
    wb_choice(q, b + 4, 528, 20, 90, 38, 6, C_ENV);
    wt_section(q, "POLARITY", 528, 66, C_ENV);
    wb_choice(q, b + 6, 528, 86, 116, 38, 6, C_ENV);
    wb_slider(q, b + 5, 778, 80, 238, C_ENV);
    wb_slider(q, b + 0, 528, 140, 238, C_ENV);
    wb_slider(q, b + 1, 778, 140, 238, C_ENV);
    wb_slider(q, b + 2, 8, 200, 244, C_ENV);
    wb_slider(q, b + 3, 264, 200, 244, C_ENV);
    sw_label(q, k == 3 ? "MOD 4 is the filter envelope: its CUTOFF amount is FILTER's ENV AMT."
                       : "MOD 3: a free envelope for anything below.",
             SW_F_SMALL, C_DIM, 528, 206, 488, false);
}

void pg_mod(lv_obj_t *p) {
    static const char *const SUB[WT_NMOD] = {"oscillator: LFO, or AUDIO-rate on the note",
                                            "oscillator: LFO, or AUDIO-rate on the note",
                                            "envelope", "envelope (the filter's)"};
    for (int k = 0; k < WT_NMOD; k++)
        M.tab[k] = sw_btn(p, WT_MOD_NAMES[k], 8 + k * 126, 6, 120, 40, k < 2 ? C_MOD : C_ENV, mod_show,
                          (void *)(intptr_t)k, SW_F_MID);
    for (int k = 0; k < WT_NMOD; k++) {
        lv_obj_t *q = M.panel[k] = sw_box(p, 0, 56, 1024, PAGE_H - 56, -1, 0);
        lv_obj_add_flag(q, LV_OBJ_FLAG_CLICKABLE);
        if (k < 2)
            lfo_panel(q, k);
        else
            env_panel(q, k);
        uint32_t col = k < 2 ? C_MOD : C_ENV;
        char t[64];
        snprintf(t, sizeof t, "%s GOES TO  (the matrix row; 0 = off)", WT_MOD_NAMES[k]);
        wt_section(q, t, 8, 260, col);
        for (int d = 0; d < WT_NDEST; d++)
            wb_amount(q, k, d, WT_DEST_LABELS[d], 8 + (d % 4) * 252, 284 + (d / 4) * 50, 240, col);
        sw_label(q, SUB[k], SW_F_SMALL, C_DIM, 8 + 3 * 252, 284 + 2 * 50 + 10, 240, false);
    }
    mod_show((void *)0);
}
