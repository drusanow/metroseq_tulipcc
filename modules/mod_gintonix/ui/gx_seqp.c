// gx_seqp.c - the SEQ page (MatrixPage in gintonix.py): all 8 channels x
// 16 steps; tap = toggle in the chosen mode (TRIGGER / ACCENT / FILL), the
// top bar sets the pattern LENGTH; patterns, RATE, TEMPO / SWING / FILL.
#include <stdio.h>
#include <string.h>

#include "gx_ui_int.h"

#define X0 150
#define SW 54
#define RH 46
#define Y0 22

static struct {
    char mode;
    lv_obj_t *leds[GX_NSTEP];
    lv_obj_t *cells[GX_NCH][GX_NSTEP];
    uint32_t shown[GX_NCH][GX_NSTEP];
    lv_obj_t *sel_b[GX_NCH], *mute_b[GX_NCH];
    lv_obj_t *mode_b[3];
    lv_obj_t *rate_b;
    int rate_shown;
    int led_shown;
    char names_shown[GX_NCH][24];
    gx_pbar_t *pbar;
    gx_knob_t *knobs[3];
    uint32_t knob_t;
} Q = {.mode = 't', .rate_shown = -1, .led_shown = -1};

static uint32_t step_color(int s, bool inside) {
    if (!s) return inside ? STEP_OFF : STEP_OUT;
    if (!inside) return STEP_DIM;   // set, but past the pattern LENGTH
    if (s & 4) return FILL_C;
    if (s & 2) return ACC_C;
    return TRIG_C;
}

static void set_len(lv_event_t *e) {
    g_gx.pats[g_gx.cur].len = (uint8_t)((intptr_t)lv_event_get_user_data(e) + 1);
    pattern_edited();
}

static void tap(lv_event_t *e) {
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    gx_edit_step(&g_gx.pats[g_gx.cur], (int)(v >> 4), (int)(v & 15), Q.mode);
    pattern_edited();
}

static void set_mode(void *ud) {
    static const char modes[3] = {'t', 'a', 'f'};
    int j = (int)(intptr_t)ud;
    Q.mode = modes[j];
    for (int i = 0; i < 3; i++) sw_btn_set(Q.mode_b[i], i == j);
}

static void sel_cb(void *ud) { select_channel((int)(intptr_t)ud, true); }
static void mute_cb(void *ud) { toggle_mute((int)(intptr_t)ud, false); }

static void refresh_rate(void) {
    if (Q.rate_shown == g_gx.rate) return;
    Q.rate_shown = g_gx.rate;
    char t[16];
    snprintf(t, sizeof t, "RATE\n%s", GX_RATE_NAMES[g_gx.rate]);
    sw_btn_text(Q.rate_b, t);
}

static void next_rate(void *ud) {
    g_gx.rate = (uint8_t)((g_gx.rate + 1) % GX_NRATES);
    refresh_rate();
    pattern_edited();
}

static void nudge_cb(void *ud) {
    intptr_t v = (intptr_t)ud;
    nudge(KEY_G((int)(v >> 2)), (v & 1) ? 1 : -1);
}

void seq_page_create(lv_obj_t *g) {
    for (int i = 0; i < GX_NSTEP; i++) {
        lv_obj_t *led = Q.leds[i] = sw_box(g, X0 + i * SW, 4, SW - 8, 10, STEP_OUT, 0);
        lv_obj_add_flag(led, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(led, 6);
        lv_obj_add_event_cb(led, set_len, LV_EVENT_PRESSED, (void *)(intptr_t)i);
    }
    for (int c = 0; c < GX_NCH; c++) {
        int y = Y0 + c * RH;
        char nm[24];
        Q.sel_b[c] = sw_btn(g, ch_name(c, nm, sizeof nm), 2, y, 98, RH - 6, ACC, sel_cb,
                            (void *)(intptr_t)c, F_LABEL);
        Q.mute_b[c] = sw_btn(g, "M", 110, y, 34, RH - 6, MUTE_C, mute_cb, (void *)(intptr_t)c,
                             F_LABEL);
        for (int i = 0; i < GX_NSTEP; i++) {
            lv_obj_t *o = Q.cells[c][i] =
                sw_box(g, X0 + i * SW + (i % 4 == 0 ? 2 : 0), y, SW - 8, RH - 8, STEP_OFF, 0);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, tap, LV_EVENT_PRESSED, (void *)(intptr_t)((c << 4) | i));
            Q.shown[c][i] = 0xFFFFFFFF;
        }
    }
    int yb = Y0 + GX_NCH * RH + 4, H = 120, yo = yb + 6, x1 = 4 + H + 10;
    static const char *const mn[3] = {"TRIGGER", "ACCENT", "FILL"};
    static const uint32_t mc[3] = {TRIG_C, ACC_C, FILL_C};
    for (int j = 0; j < 3; j++)
        Q.mode_b[j] = sw_btn(g, mn[j], x1 + j * 98, yo, 92, 50, mc[j], set_mode, (void *)(intptr_t)j,
                             F_LABEL);
    int cx = x1 + GX_NPAT * 45 + 4;   // CHAIN > column; RATE above it
    Q.pbar = pbar_new(g, x1, yo + 62, GX_NPAT, 42, H - 62, 3, cx, yo + 62, 84, H - 62, 4, yo, H, H);
    int tx = x1 + 3 * 98 + 8;
    sw_label(g, "Tap: TRIGGER / ACCENT / FILL\n(accent, fill set the trigger)\nTop bar = LENGTH",
             F_LABEL, C_DIM, tx, yo, cx - tx - 6, false);
    Q.rate_b = sw_btn(g, "RATE", cx, yo, 84, 50, SEQ_ACC, next_rate, NULL, F_LABEL);
    // TEMPO / SWING / FILL rate in the row of the A-L buttons, - / + above
    // each: one step (1 BPM, 1 %, one fill rate), hold to repeat
    int kx = cx + 84 + 8, kw = (1020 - kx) / 3;
    static const int keys[3] = {G_TEMPO, G_SWING, G_FILL};
    for (int j = 0; j < 3; j++) {
        int bw = (kw - 10) / 2;
        lv_obj_t *b = sw_btn(g, "-", kx + j * kw + 2, yo, bw, 40, SEQ_ACC, nudge_cb,
                             (void *)(intptr_t)(keys[j] << 2), F_BIG);
        sw_btn_repeat(b);
        b = sw_btn(g, "+", kx + j * kw + 8 + bw, yo, bw, 40, SEQ_ACC, nudge_cb,
                   (void *)(intptr_t)((keys[j] << 2) | 1), F_BIG);
        sw_btn_repeat(b);
    }
    int ky = yo + 62 + (H - 62) / 2 - (KNOB_Y + KNOB / 2);
    for (int j = 0; j < 3; j++)
        Q.knobs[j] = knob_new(g, KEY_G(keys[j]), kx + j * kw, ky, SEQ_ACC, kw, 0, 0, true);
    set_mode((void *)0);
}

void seq_page_refresh(void) {
    const gx_pat_t *p = &g_gx.pats[g_gx.cur];
    refresh_rate();
    uint8_t off = g_live.mute | g_live.hold_mute;
    for (int c = 0; c < GX_NCH; c++) {
        sw_btn_set(Q.sel_b[c], c == A.sel);
        sw_btn_set(Q.mute_b[c], off & (1u << c));
        char nm[24];
        ch_name(c, nm, sizeof nm);
        if (strcmp(nm, Q.names_shown[c])) {
            snprintf(Q.names_shown[c], sizeof Q.names_shown[c], "%s", nm);
            sw_btn_text(Q.sel_b[c], nm);
        }
        for (int i = 0; i < GX_NSTEP; i++) {
            uint32_t col = step_color(gx_step_state(p, c, i), i < p->len);
            if (Q.shown[c][i] != col) {
                Q.shown[c][i] = col;
                lv_obj_set_style_bg_color(Q.cells[c][i], sw_c(col), 0);
            }
        }
    }
    Q.led_shown = -1;
}

void seq_page_tick(uint32_t now) {
    if (A.grid_dirty || A.strip_dirty) {
        A.grid_dirty = A.strip_dirty = false;
        seq_page_refresh();
    }
    pbar_tick(Q.pbar);
    int n = g_gx.pats[g_gx.cur].len;
    int led = (audible() && g_gxs.pat == g_gx.cur) ? g_gxs.step : -1;
    int st = n * 32 + led + 1;
    if (st != Q.led_shown) {
        Q.led_shown = st;
        for (int i = 0; i < GX_NSTEP; i++)
            lv_obj_set_style_bg_color(Q.leds[i],
                                      sw_c(i == led ? LED_ON : (i < n ? SEQ_DIM : STEP_OUT)), 0);
    }
    // the tempo can change from another app, or follow MIDI clock
    if (now - Q.knob_t > 200) {
        Q.knob_t = now;
        for (int j = 0; j < 3; j++) knob_refresh(Q.knobs[j]);
    }
}
