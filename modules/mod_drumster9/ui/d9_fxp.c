// d9_fxp.c - the FX page: the 4 buses.
//
// Pick a bus; the lane buttons show where every lane plays (in its bus's
// colour): tap one to bring it onto this bus, tap it again to send it back
// to bus 0. Then the bus's reverb, echo, filter, drive and EQ - as the
// Python's FX page, with sliders.
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "d9_ui_int.h"

typedef struct {
    const char *label;
    size_t off;
    float lo, hi;
    uint8_t log;       // a log scale (frequency, ratios)
    const char *fmt;
} param_t;

enum { P_RLVL, P_RLIVE, P_RDAMP, P_ELVL, P_EMS, P_EFB, P_ETONE, P_FREQ, P_RESO, P_DRIVE, P_EQL, P_EQM, P_EQH, NP };

static const param_t PARAM[NP] = {
    {"LEVEL", offsetof(d9_bus_t, rev_level), 0, 1, 0, "%.0f%%"},
    {"LIVENESS", offsetof(d9_bus_t, rev_live), 0, 1, 0, "%.0f%%"},
    {"DAMPING", offsetof(d9_bus_t, rev_damp), 0, 1, 0, "%.0f%%"},
    {"LEVEL", offsetof(d9_bus_t, echo_level), 0, 1, 0, "%.0f%%"},
    {"TIME", offsetof(d9_bus_t, echo_ms), 20, 500, 0, "%.0f ms"},
    {"FEEDBACK", offsetof(d9_bus_t, echo_fb), 0, 0.95f, 0, "%.0f%%"},
    {"TONE", offsetof(d9_bus_t, echo_tone), -1, 1, 0, "%+.1f"},
    {"CUTOFF", offsetof(d9_bus_t, freq), 100, 12000, 1, "%.0f Hz"},
    {"RESONANCE", offsetof(d9_bus_t, reso), 0.5f, 8, 1, "%.2f"},
    {"DRIVE", offsetof(d9_bus_t, drive), 0.2f, 4, 1, "x%.2f"},
    {"LOW  800 Hz", offsetof(d9_bus_t, eq_l), -15, 15, 0, "%+.0f dB"},
    {"MID  2.5 kHz", offsetof(d9_bus_t, eq_m), -15, 15, 0, "%+.0f dB"},
    {"HIGH  7 kHz", offsetof(d9_bus_t, eq_h), -15, 15, 0, "%+.0f dB"},
};

static struct {
    int bus;
    lv_obj_t *tab[D9_BUSES], *lane[D9_LANES], *ftype[3];
    sw_slider_t *sl[NP];
} F;

static float *val(int i) { return (float *)((char *)&g_d9.fx[F.bus] + PARAM[i].off); }

static float get_p(void *ud) {
    const param_t *p = &PARAM[(intptr_t)ud];
    float v = *val((int)(intptr_t)ud);
    if (p->log) return logf(v / p->lo) / logf(p->hi / p->lo);
    return (v - p->lo) / (p->hi - p->lo);
}

static void set_p(void *ud, float x) {
    const param_t *p = &PARAM[(intptr_t)ud];
    float v = p->log ? p->lo * powf(p->hi / p->lo, x) : p->lo + x * (p->hi - p->lo);
    // the EQ and the echo time land on whole steps, the rest is smooth
    if (p->fmt[1] == '+' && p->fmt[3] == '0') v = roundf(v);
    if ((intptr_t)ud == P_EMS) v = roundf(v / 5) * 5;
    *val((int)(intptr_t)ud) = v < p->lo ? p->lo : (v > p->hi ? p->hi : v);
    d9_edit();
}

static void text_p(void *ud, char *b, int n) {
    const param_t *p = &PARAM[(intptr_t)ud];
    float v = *val((int)(intptr_t)ud);
    if (strstr(p->fmt, "%%")) v *= 100;
    snprintf(b, n, p->fmt, (double)v);
}

static const sw_slider_ops_t OPS = {get_p, set_p, text_p};

void d9_fx_page_refresh(void) {
    for (int b = 0; b < D9_BUSES; b++) sw_btn_set(F.tab[b], b == F.bus);
    for (int l = 0; l < D9_LANES; l++) {
        sw_btn_accent(F.lane[l], D9_BUS_COLOR[g_d9.bus[l]]);
        sw_btn_set(F.lane[l], true);
        // the ones on this bus stand out
        lv_obj_set_style_border_color(F.lane[l], sw_c(g_d9.bus[l] == F.bus ? SW_TEXT : D9_BUS_COLOR[g_d9.bus[l]]), 0);
        lv_obj_set_style_border_width(F.lane[l], g_d9.bus[l] == F.bus ? 3 : 1, 0);
    }
    for (int t = 0; t < 3; t++) sw_btn_set(F.ftype[t], g_d9.fx[F.bus].filter == t);
    for (int i = 0; i < NP; i++) sw_slider_refresh(F.sl[i]);
}

static void bus_cb(void *ud) {
    F.bus = (int)(intptr_t)ud;
    d9_fx_page_refresh();
}

static void lane_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_d9.bus[l] = (uint8_t)(g_d9.bus[l] == F.bus && F.bus ? 0 : F.bus);
    d9_edit();
    d9_fx_page_refresh();
}

static void ftype_cb(void *ud) {
    g_d9.fx[F.bus].filter = (int)(intptr_t)ud;
    d9_edit();
    d9_fx_page_refresh();
}

static void reset_cb(void *ud) {
    d9_bus_default(&g_d9.fx[F.bus]);
    d9_edit();
    d9_fx_page_refresh();
    d9_say("bus reset");
}

static void slider(lv_obj_t *p, int i, int x, int y, uint32_t col) {
    F.sl[i] = sw_slider(p, PARAM[i].label, x, y, 488, 44, col, &OPS, (void *)(intptr_t)i);
}

void d9_fx_page_create(lv_obj_t *p) {
    for (int b = 0; b < D9_BUSES; b++) {
        char t[8];
        snprintf(t, sizeof t, "BUS %d", b);
        F.tab[b] = sw_btn(p, t, 12 + b * 104, 8, 98, 40, D9_BUS_COLOR[b], bus_cb, (void *)(intptr_t)b, SW_F_LABEL);
    }
    sw_btn(p, "RESET BUS", 436, 8, 120, 40, SW_RED, reset_cb, NULL, NULL);
    sw_label(p, "tap a lane to put it on this bus; tap it again to send it back to BUS 0", SW_F_SMALL,
             C_DIM, 572, 22, 0, false);
    sw_label(p, "LANES", SW_F_SMALL, C_DIM, 12, 72, 0, false);
    for (int l = 0; l < D9_LANES; l++)
        F.lane[l] = sw_btn(p, D9_ROLE[l], 64 + l * 136, 60, 130, 40, SW_CYAN, lane_cb, (void *)(intptr_t)l, NULL);
    sw_line(p, 0, 110, 1024, 1);

    const int x0 = 12, x1 = 524;
    sw_label(p, "REVERB", SW_F_LABEL, SW_ORANGE, x0, 120, 0, false);
    slider(p, P_RLVL, x0, 140, SW_ORANGE);
    slider(p, P_RLIVE, x0, 190, SW_ORANGE);
    slider(p, P_RDAMP, x0, 240, SW_ORANGE);
    sw_label(p, "ECHO", SW_F_LABEL, SW_CYAN, x0, 296, 0, false);
    slider(p, P_ELVL, x0, 316, SW_CYAN);
    slider(p, P_EMS, x0, 366, SW_CYAN);
    slider(p, P_EFB, x0, 416, SW_CYAN);
    slider(p, P_ETONE, x0, 466, SW_CYAN);

    sw_label(p, "FILTER", SW_F_LABEL, ACC, x1, 120, 0, false);
    for (int t = 0; t < 3; t++)
        F.ftype[t] = sw_btn(p, D9_FILTER_NAME[t], x1 + 100 + t * 130, 112, 124, 34, ACC, ftype_cb,
                            (void *)(intptr_t)t, NULL);
    slider(p, P_FREQ, x1, 160, ACC);
    slider(p, P_RESO, x1, 210, ACC);
    slider(p, P_DRIVE, x1, 260, ACC);
    sw_label(p, "EQ", SW_F_LABEL, SW_GREEN, x1, 316, 0, false);
    slider(p, P_EQL, x1, 336, SW_GREEN);
    slider(p, P_EQM, x1, 386, SW_GREEN);
    slider(p, P_EQH, x1, 436, SW_GREEN);
    d9_fx_page_refresh();
}
