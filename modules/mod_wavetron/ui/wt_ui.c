// wt_ui.c - Wavetron's header, page switching, parameter bindings, popups
// and the on-screen keyboard.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_ui_int.h"

lv_obj_t *wt_scr;

static struct {
    lv_obj_t *pages[PG_COUNT], *tabs[PG_COUNT];
    lv_obj_t *name, *status;
    int page;
    uint32_t toast_until;
    lv_obj_t *popup;
    uint32_t sig_shown;
} U = {.page = -1};

static const char *const TABS[PG_COUNT] = {"OSC", "FILTER", "MOD", "MATRIX", "VOICE", "FX", "PATCH"};

// --- bindings ------------------------------------------------------------------------------------

#define MAXS 96
static sw_slider_t *s_sliders[MAXS];
static int s_ns;

typedef struct {
    int param, n;
    lv_obj_t *b[8];
} choice_t;

typedef struct {
    choice_t *c;
    int i;
} choice_ud_t;

#define MAXC 40
static choice_t *s_choices[MAXC];
static int s_nc;

static float p_get(void *ud) {
    int i = (int)(intptr_t)ud;
    return wt_param_to01(i, g_wt.s.p[i]);
}

static void p_set(void *ud, float x) {
    int i = (int)(intptr_t)ud;
    float v = wt_param_from01(i, x);
    if (v == g_wt.s.p[i]) return;
    g_wt.s.p[i] = v;
    wt_changed();
}

static void p_text(void *ud, char *b, int n) {
    int i = (int)(intptr_t)ud;
    wt_param_text(i, g_wt.s.p[i], b, n);
}

static const sw_slider_ops_t P_OPS = {p_get, p_set, p_text};

static sw_slider_t *keep(sw_slider_t *s) {
    if (s_ns < MAXS) s_sliders[s_ns++] = s;
    return s;
}

sw_slider_t *wb_slider(lv_obj_t *par, int i, int x, int y, int w, uint32_t col) {
    return keep(sw_slider(par, WT_PARAMS[i].label, x, y, w, 44, col, &P_OPS, (void *)(intptr_t)i));
}

// MOD RATE: whichever of RATE / RATIO the MODE row says (the sketch's eff_key)
static int rate_param(void *ud) { return wt_rate_param(&g_wt.s, (int)(intptr_t)ud); }
static float r_get(void *ud) { return p_get((void *)(intptr_t)rate_param(ud)); }
static void r_set(void *ud, float x) { p_set((void *)(intptr_t)rate_param(ud), x); }
static void r_text(void *ud, char *b, int n) {
    int i = rate_param(ud);
    float v = g_wt.s.p[i];
    int div = (int)g_wt.s.p[(int)(intptr_t)ud ? WP_M2_DIV : WP_M1_DIV];
    if (i == WP_M1_RATIO || i == WP_M2_RATIO)
        snprintf(b, n, "x%.2f note", (double)v);
    else if (div > 0)
        snprintf(b, n, "%s beats", WT_SYNC_DIVS[div]);
    else
        snprintf(b, n, "%.2f Hz", (double)v);
}
static const sw_slider_ops_t R_OPS = {r_get, r_set, r_text};

sw_slider_t *wb_rate(lv_obj_t *par, int mod, int x, int y, int w, uint32_t col) {
    return keep(sw_slider(par, "RATE / RATIO", x, y, w, 44, col, &R_OPS, (void *)(intptr_t)mod));
}

// a matrix amount: ud = mod * 16 + dest
static float a_get(void *ud) {
    int v = (int)(intptr_t)ud;
    return (wt_mx_get(&g_wt.s, v >> 4, v & 15) + WT_MX_MAX) / (2 * WT_MX_MAX);
}
static void a_set(void *ud, float x) {
    int v = (int)(intptr_t)ud;
    float amt = roundf((x * 2 - 1) * WT_MX_MAX * 20) / 20;   // 0.05 steps, as the sketch
    if (amt == wt_mx_get(&g_wt.s, v >> 4, v & 15)) return;
    wt_mx_set(&g_wt.s, v >> 4, v & 15, amt);
    wt_changed();
}
static void a_text(void *ud, char *b, int n) {
    int v = (int)(intptr_t)ud;
    float a = wt_mx_get(&g_wt.s, v >> 4, v & 15);
    if (a == 0)
        snprintf(b, n, "off");
    else
        snprintf(b, n, "%+.2f", (double)a);
}
static const sw_slider_ops_t A_OPS = {a_get, a_set, a_text};

sw_slider_t *wb_amount(lv_obj_t *par, int mod, int dest, const char *label, int x, int y, int w,
                       uint32_t col) {
    return keep(sw_slider(par, label, x, y, w, 44, col, &A_OPS, (void *)(intptr_t)(mod * 16 + dest)));
}

static void choice_paint(choice_t *c) {
    int v = (int)(g_wt.s.p[c->param] + 0.5f);
    if (c->n == 1)
        sw_btn_set(c->b[0], v != 0);
    else
        for (int k = 0; k < c->n; k++) sw_btn_set(c->b[k], k == v);
}

static void choice_cb(void *ud) {
    choice_ud_t *u = ud;
    choice_t *c = u->c;
    float v = c->n == 1 ? (g_wt.s.p[c->param] > 0.5f ? 0 : 1) : (float)u->i;
    g_wt.s.p[c->param] = v;
    wt_changed();
    // a MODE change swaps what RATE edits; everything else may show it
    wb_refresh();
}

static choice_t *new_choice(int i) {
    choice_t *c = calloc(1, sizeof *c);
    c->param = i;
    if (s_nc < MAXC) s_choices[s_nc++] = c;
    return c;
}

void wb_choice(lv_obj_t *par, int i, int x, int y, int bw, int h, int gap, uint32_t col) {
    choice_t *c = new_choice(i);
    c->n = (int)WT_PARAMS[i].hi + 1;
    for (int k = 0; k < c->n && k < 8; k++) {
        choice_ud_t *u = calloc(1, sizeof *u);
        u->c = c;
        u->i = k;
        c->b[k] = sw_btn(par, WT_PARAMS[i].names[k], x + k * (bw + gap), y, bw, h, col, choice_cb, u, NULL);
    }
    choice_paint(c);
}

lv_obj_t *wb_toggle(lv_obj_t *par, int i, const char *text, int x, int y, int w, int h, uint32_t col) {
    choice_t *c = new_choice(i);
    c->n = 1;
    choice_ud_t *u = calloc(1, sizeof *u);
    u->c = c;
    c->b[0] = sw_btn(par, text, x, y, w, h, col, choice_cb, u, NULL);
    choice_paint(c);
    return c->b[0];
}

void wb_refresh(void) {
    for (int k = 0; k < s_ns; k++) sw_slider_refresh(s_sliders[k]);
    for (int k = 0; k < s_nc; k++) choice_paint(s_choices[k]);
}

lv_obj_t *wt_section(lv_obj_t *par, const char *text, int x, int y, uint32_t col) {
    return sw_label(par, text, SW_F_LABEL, col, x, y, 0, false);
}

lv_obj_t *wt_frame(lv_obj_t *par, int x, int y, int w, int h) {
    lv_obj_t *f = sw_box(par, x, y, w, h, C_BG, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, sw_c(C_LINE), 0);
    return f;
}

// --- header ---------------------------------------------------------------------------------------

void wt_ui_toast(const char *msg) {
    lv_label_set_text(U.status, msg);
    lv_obj_set_style_text_color(U.status, sw_c(C_TEXT), 0);
    U.toast_until = (uint32_t)(studio_now_us() / 1000) + 2500;
}

void wt_header_refresh(void) {
    char t[40];
    snprintf(t, sizeof t, "%s", g_patch_name[0] ? g_patch_name : "(unsaved)");
    lv_label_set_text(U.name, t);
}

static void show_page(int i) {
    if (i == U.page) return;
    U.page = i;
    for (int k = 0; k < PG_COUNT; k++) {
        sw_btn_set(U.tabs[k], k == i);
        if (k == i)
            lv_obj_remove_flag(U.pages[k], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(U.pages[k], LV_OBJ_FLAG_HIDDEN);
    }
}

static void tab_cb(void *ud) { show_page((int)(intptr_t)ud); }

static void header(lv_obj_t *s) {
    const int y = STUDIO_HDR_Y, h = STUDIO_HDR_BTN_H;
    sw_label(s, "WAVETRON", SW_F_MID, C_TEXT, 8, 16, 0, false);
    for (int k = 0; k < PG_COUNT; k++)
        U.tabs[k] = sw_btn(s, TABS[k], 140 + k * 70, y, 66, h, C_WT, tab_cb, (void *)(intptr_t)k, NULL);
    U.name = sw_label(s, "", SW_F_LABEL, C_WT, 636, 10, STUDIO_SQ_X - 642, false);
    lv_label_set_long_mode(U.name, LV_LABEL_LONG_CLIP);
    U.status = sw_label(s, "", SW_F_SMALL, C_DIM, 636, 30, STUDIO_SQ_X - 642, false);
    lv_label_set_long_mode(U.status, LV_LABEL_LONG_CLIP);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);
}

// --- popups ------------------------------------------------------------------------------------------

static void popup_cancel(void *ud) { wt_popup_close(); }

lv_obj_t *wt_popup(const char *title, uint32_t col) {
    wt_popup_close();
    lv_obj_t *p = U.popup = sw_box(wt_scr, 0, PAGE_Y, 1024, PAGE_H, C_BG, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);   // nothing under it takes a tap
    sw_label(p, title, SW_F_MID, col, 16, 10, 0, false);
    sw_btn(p, "CANCEL", 1024 - 150, 6, 136, 40, C_EDGE, popup_cancel, NULL, NULL);
    sw_line(p, 0, 52, 1024, 1);
    return p;
}

void wt_popup_close(void) {
    if (U.popup) lv_obj_delete(U.popup);
    U.popup = NULL;
}

static struct {
    lv_obj_t *ta;
    void (*done)(const char *);
} K;

static void kb_ok(void *ud) {
    char t[WT_PATCH_NAME_MAX + 1];
    snprintf(t, sizeof t, "%s", lv_textarea_get_text(K.ta));
    void (*done)(const char *) = K.done;
    wt_popup_close();
    if (done) done(t);
}

static void kb_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY)
        kb_ok(NULL);
    else if (c == LV_EVENT_CANCEL)
        wt_popup_close();
}

void wt_keyboard(const char *title, const char *initial, void (*done)(const char *text)) {
    lv_obj_t *p = wt_popup(title, C_WT);
    K.done = done;
    K.ta = lv_textarea_create(p);
    lv_obj_set_pos(K.ta, 16, 70);
    lv_obj_set_size(K.ta, 700, 52);
    lv_textarea_set_one_line(K.ta, true);
    lv_textarea_set_max_length(K.ta, WT_PATCH_NAME_MAX);
    lv_textarea_set_text(K.ta, initial);
    lv_obj_set_style_bg_color(K.ta, sw_c(C_BG), 0);
    lv_obj_set_style_border_color(K.ta, sw_c(C_EDGE), 0);
    lv_obj_set_style_border_width(K.ta, 1, 0);
    lv_obj_set_style_radius(K.ta, 0, 0);
    lv_obj_set_style_text_color(K.ta, sw_c(C_TEXT), 0);
    lv_obj_set_style_text_font(K.ta, SW_F_MID, 0);
    lv_obj_add_event_cb(K.ta, kb_event, LV_EVENT_ALL, NULL);
    sw_btn(p, "OK", 730, 70, 130, 52, C_WT, kb_ok, NULL, SW_F_MID);
    lv_obj_t *kb = lv_keyboard_create(p);
    lv_obj_set_size(kb, 1024, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, K.ta);
    lv_obj_set_style_bg_color(kb, sw_c(C_BG), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, sw_c(C_BG), LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, sw_c(C_EDGE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, sw_c(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, sw_c(C_WT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, sw_c(C_WT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, sw_c(C_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
}

// --- MIDI CCs (the sketch's midi_cb) ---------------------------------------------------------------------

static void apply_cc(const wt_cc_t *m) {
    float v = m->val / 127.0f;
    switch (m->cc) {
    case 74: g_wt.s.p[WP_CUTOFF] = wt_param_fix(WP_CUTOFF, 30 + v * v * 11970); break;   // cutoff
    case 71: g_wt.s.p[WP_RESO] = wt_param_fix(WP_RESO, 0.5f + v * 15.5f); break;         // resonance
    case 1:                                                                                // mod wheel
        g_wt.s.p[WP_A_POS] = v;
        g_wt.s.p[WP_B_POS] = v;
        break;
    case 76: g_wt.s.p[WP_UNI_DET] = wt_param_fix(WP_UNI_DET, v * 50); break;   // the macro: detune
    default: return;
    }
    wt_changed();
}

// --- entry points ---------------------------------------------------------------------------------------

void wt_ui_create(lv_obj_t *scr) {
    wt_scr = scr;
    header(scr);
    void (*make[PG_COUNT])(lv_obj_t *) = {pg_osc, pg_flt, pg_mod, pg_mx, pg_voice, pg_fx, pg_patch};
    for (int k = 0; k < PG_COUNT; k++) {
        U.pages[k] = sw_box(scr, 0, PAGE_Y, 1024, PAGE_H, C_BG, 0);
        lv_obj_add_flag(U.pages[k], LV_OBJ_FLAG_CLICKABLE);
        make[k](U.pages[k]);
    }
    show_page(PG_OSC);
    wt_header_refresh();
}

void wt_ui_show(bool shown) {
    if (shown) wb_refresh();
}

void wt_ui_poll(bool visible) {
    static uint32_t last;
    wt_cc_t m;
    bool cc = false;
    while (wt_midi_next(&m)) {
        apply_cc(&m);
        cc = true;
    }
    if (!visible) return;
    uint32_t now = (uint32_t)(studio_now_us() / 1000);
    if (cc) wb_refresh();
    if (now - last < 50) return;   // displays at 20 Hz
    last = now;
    if (U.toast_until && now > U.toast_until) {
        U.toast_until = 0;
        lv_label_set_text(U.status, "");
    }
    static int load_shown = -1;
    if (!U.toast_until) {
        int ld = wt_load_pct();
        if (ld != load_shown) {   // only when it changes: every redraw costs
            load_shown = ld;
            char t[32];
            snprintf(t, sizeof t, "DSP %d%%", ld);
            lv_label_set_text(U.status, t);
            lv_obj_set_style_text_color(U.status, sw_c(C_DIM), 0);
        }
    } else {
        load_shown = -1;
    }
    wd_tick(U.page);
    if (U.page == PG_OSC) pg_osc_tick();
    if (U.page == PG_MX) pg_mx_tick();
    if (U.page == PG_VOICE) pg_voice_tick();
    if (U.page == PG_PATCH) pg_patch_tick();
}
