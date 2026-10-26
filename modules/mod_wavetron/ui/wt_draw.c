// wt_draw.c - Wavetron's displays: the sketch's visual panes, full size.
//
// Each display is a framed box that draws itself (LVGL draw events) from
// the current sound, so it can't disagree with what plays. wd_tick() asks
// each visible one for a signature of what it shows and redraws only when
// that changes - or every tick for the live ones (scope, voices, the
// waterfall's moving position).
//
// The waterfall (the sketch's topographic wavetable view): every frame of
// the table drawn as a waveform line, receding with a parallax skew, nearer
// frames hiding the ones behind. That terrain is rendered once per table
// into a canvas; the POSITION frame (white) and where the playing voice
// really is after modulation (in the oscillator's colour) are drawn over
// it on every change.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_ui_int.h"

#define PI 3.14159265f

enum { D_WATERFALL, D_FILTER, D_ENV, D_LFO, D_DRIVE, D_UNISON, D_SCOPE, D_VOICES };

typedef struct {
    int type, arg;
    lv_obj_t *obj, *canvas, *label;
    int w, h;
    uint16_t *buf;
    uint32_t sig;
    int table;                 // waterfall: which catalogue entry the canvas shows
    void *table_data;
} disp_t;

#define MAXD 24
static disp_t *s_d[MAXD];
static int s_nd;

static uint32_t mix(uint32_t h, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    return (h ^ u) * 16777619u;
}

// --- drawing helpers ------------------------------------------------------------------------

typedef struct {
    float x, y;
} pt_t;

static void polyline(lv_layer_t *layer, const pt_t *p, int n, uint32_t col, int width) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = sw_c(col);
    d.width = width;
    d.round_start = d.round_end = 1;
    for (int i = 1; i < n; i++) {
        d.p1.x = (int32_t)p[i - 1].x;
        d.p1.y = (int32_t)p[i - 1].y;
        d.p2.x = (int32_t)p[i].x;
        d.p2.y = (int32_t)p[i].y;
        lv_draw_line(layer, &d);
    }
}

static void hline(lv_layer_t *layer, float x0, float x1, float y, uint32_t col) {
    pt_t p[2] = {{x0, y}, {x1, y}};
    polyline(layer, p, 2, col, 1);
}

static disp_t *new_disp(lv_obj_t *par, int type, int arg, int x, int y, int w, int h) {
    disp_t *d = calloc(1, sizeof *d);
    d->type = type;
    d->arg = arg;
    d->w = w;
    d->h = h;
    d->table = -1;
    d->obj = wt_frame(par, x, y, w, h);
    lv_obj_set_user_data(d->obj, d);
    if (s_nd < MAXD) s_d[s_nd++] = d;
    return d;
}

// --- the waterfall ---------------------------------------------------------------------------------

typedef struct {
    float top, spacing, amp, skew, xspan;
} wf_geom_t;

static wf_geom_t wf_geom(int w, int h) {
    wf_geom_t g;
    g.amp = h * 0.28f;                                    // a frame's peak-to-peak height
    g.top = 6 + g.amp * 0.5f;
    g.spacing = (h - 12 - g.amp) / (WT_PREV_LINES - 1);   // frame to frame
    g.skew = w * 0.16f;                                   // the parallax
    g.xspan = w - g.skew - 12;
    return g;
}

static void wf_frame(const wf_geom_t *g, float li, float *base, float *xo) {
    *base = g->top + li * g->spacing;
    *xo = 6 + (WT_PREV_LINES - 1 - li) / (WT_PREV_LINES - 1) * g->skew;   // back frames to the right
}

static inline uint16_t c565(uint32_t hex) { return lv_color_to_u16(sw_c(hex)); }

// colour by depth: far frames dim, near ones brighter
static uint32_t depth_col(int li) {
    static const uint32_t ramp[4] = {0x242449, 0x24496d, 0x496d92, 0x6d92b6};
    return ramp[li * 4 / WT_PREV_LINES];
}

static void wf_render(disp_t *d, const wt_table_t *t) {
    int w = d->w - 2, h = d->h - 2;
    uint16_t *b = d->buf;
    uint16_t bg = c565(C_BG);
    for (int i = 0; i < w * h; i++) b[i] = bg;
    wf_geom_t g = wf_geom(w, h);
    // back to front; each frame blanks the band below its line first, so
    // nearer frames hide the farther ones (the terrain look)
    for (int li = 0; li < WT_PREV_LINES; li++) {
        float base, xo;
        wf_frame(&g, (float)li, &base, &xo);
        uint16_t col = c565(depth_col(li));
        int bottom = (int)(base + g.amp * 0.5f) + 2;
        if (bottom >= h) bottom = h - 1;
        int prev = -1;
        for (int x = (int)xo; x < (int)(xo + g.xspan) && x < w; x++) {
            float fi = (x - xo) / g.xspan * (WT_PREV_PTS - 1);
            int i0 = (int)fi;
            if (i0 > WT_PREV_PTS - 2) i0 = WT_PREV_PTS - 2;
            float f = fi - i0;
            float v = (t->prev[li][i0] + f * (t->prev[li][i0 + 1] - t->prev[li][i0])) / 127.0f;
            int y = (int)(base - v * g.amp * 0.5f);
            if (y < 0) y = 0;
            if (y >= h) y = h - 1;
            for (int yy = y + 1; yy <= bottom; yy++) b[yy * w + x] = bg;
            int y0 = prev < 0 ? y : prev;
            int lo = y0 < y ? y0 : y, hi = y0 < y ? y : y0;
            if (hi - lo > 1) lo++;   // join steep steps
            for (int yy = lo; yy <= hi; yy++) b[yy * w + x] = col;
            prev = y;
        }
    }
    lv_obj_invalidate(d->canvas);
}

// the line of the table at position pos (0..1), between preview frames
static int wf_line(const disp_t *d, const wt_table_t *t, float pos, pt_t *p) {
    int w = d->w - 2, h = d->h - 2;
    wf_geom_t g = wf_geom(w, h);
    float lf = pos * (WT_PREV_LINES - 1);
    int l0 = (int)lf;
    if (l0 > WT_PREV_LINES - 2) l0 = WT_PREV_LINES - 2;
    float lfr = lf - l0;
    float base, xo;
    wf_frame(&g, lf, &base, &xo);
    lv_area_t a;
    lv_obj_get_coords(d->obj, &a);
    int n = 0;
    for (int i = 0; i < WT_PREV_PTS; i += 1) {
        float v = (t->prev[l0][i] + lfr * (t->prev[l0 + 1][i] - t->prev[l0][i])) / 127.0f;
        p[n].x = a.x1 + 1 + xo + (float)i / (WT_PREV_PTS - 1) * g.xspan;
        p[n].y = a.y1 + 1 + base - v * g.amp * 0.5f;
        n++;
    }
    return n;
}

static void wf_post(lv_event_t *e) {
    disp_t *d = lv_event_get_user_data(e);
    int idx = d->arg ? g_ib : g_ia;
    const wt_table_t *t = &g_cat.t[idx];
    lv_layer_t *layer = lv_event_get_layer(e);
    pt_t p[WT_PREV_PTS];
    wt_live_t lv;
    wt_live(&lv);
    float live = d->arg ? lv.pos_b : lv.pos_a;
    float pos = g_wt.s.p[d->arg ? WP_B_POS : WP_A_POS];
    if (live >= 0 && fabsf(live - pos) > 0.004f) {
        int n = wf_line(d, t, live, p);
        polyline(layer, p, n, d->arg ? C_OSCB : C_OSCA, 2);
    }
    int n = wf_line(d, t, pos, p);
    polyline(layer, p, n, C_TEXT, 2);
}

lv_obj_t *wd_waterfall(lv_obj_t *par, int osc, int x, int y, int w, int h) {
    disp_t *d = new_disp(par, D_WATERFALL, osc, x, y, w, h);
    d->buf = lv_malloc((w - 2) * (h - 2) * 2);
    d->canvas = lv_canvas_create(d->obj);
    lv_canvas_set_buffer(d->canvas, d->buf, w - 2, h - 2, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(d->canvas, 0, 0);
    lv_obj_remove_flag(d->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->obj, wf_post, LV_EVENT_DRAW_POST, d);
    d->label = sw_label(d->obj, "", SW_F_SMALL, C_DIM, 6, 4, 0, false);
    return d->obj;
}

// --- curves ------------------------------------------------------------------------------------------

static void area_of(disp_t *d, float *x0, float *y0, float *w, float *h) {
    lv_area_t a;
    lv_obj_get_coords(d->obj, &a);
    *x0 = a.x1 + 4;
    *y0 = a.y1 + 4;
    *w = d->w - 8;
    *h = d->h - 8;
}

// the filter's response: a shape that follows the knobs (the sketch's
// draw_vis_filter), log frequency 20 Hz .. 20 kHz across
static void filter_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    const float *p = g_wt.s.p;
    int ft = (int)p[WP_FTYPE];
    float lo = logf(20), span = logf(20000) - lo;
    float opp = span / logf(2) / W;   // octaves per pixel
    float xc = (logf(wt_clampf(p[WP_CUTOFF], 20, 20000)) - lo) / span * W;
    float rdb = wt_clampf((p[WP_RESO] - 0.5f) / 15.5f, 0, 1) * 22;
    const float top = 24, bot = -48;
    pt_t pts[130];
    int n = 0;
    for (float x = 0; x <= W && n < 130; x += W / 128) {
        float dd = (x - xc) * opp, a = fabsf(dd), gdb;
        switch (ft) {
        case FT_OFF: gdb = 0; break;
        case FT_LP12: gdb = dd < 0 ? 0 : -12 * dd; break;
        case FT_LP24: gdb = dd < 0 ? 0 : -24 * dd; break;
        case FT_HP: gdb = dd > 0 ? 0 : -12 * a; break;
        case FT_BP: gdb = -9 * a; break;
        case FT_NOTCH: gdb = -34 / (1 + (a * 3.2f) * (a * 3.2f)); break;
        default: gdb = -7 / (1 + (a * 2.2f) * (a * 2.2f)) + 5 * sinf(dd * 2.4f); break;
        }
        if (ft != FT_OFF && ft != FT_NOTCH) gdb += rdb / (1 + (dd * 3) * (dd * 3));
        gdb = wt_clampf(gdb, bot, top);
        pts[n].x = X + x;
        pts[n].y = Y + (top - gdb) / (top - bot) * H;
        n++;
    }
    hline(layer, X, X + W, Y + top / (top - bot) * H, C_LINE);   // 0 dB
    polyline(layer, pts, n, C_FLT, 2);
}

// an ADSR (the sketch's draw_adsr), with the curve's shape
static void env_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    const float *p = g_wt.s.p;
    int base = d->arg == 0 ? WP_AA : d->arg == 3 ? WP_M3_A : WP_M4_A;
    int curve = (int)p[d->arg == 0 ? WP_ACURVE : d->arg == 3 ? WP_M3_CURVE : WP_M4_CURVE];
    float a = p[base], dc = p[base + 1], s = p[base + 2], r = p[base + 3];
    // widths: square roots, so short and long times both read
    float wa = sqrtf(a + 1), wd = sqrtf(dc + 1), wr = sqrtf(r + 1), ws = (wa + wd + wr) * 0.35f;
    float k = W / (wa + wd + ws + wr);
    wa *= k, wd *= k, ws *= k, wr *= k;
    pt_t pts[100];
    int n = 0;
    float bottom = Y + H, top = Y + 2;
#define SEG(x0, x1, v0, v1, shape)                                                  \
    for (int i = 0; i <= 16; i++) {                                                 \
        float t = i / 16.0f, f;                                                     \
        if (shape == 0)                                                             \
            f = t;                                                                  \
        else                                                                        \
            f = 1 - expf(-4 * t) * (1 - t * expf(-4));                              \
        float v = v0 + (v1 - v0) * f;                                               \
        pts[n].x = x0 + (x1 - x0) * t;                                              \
        pts[n].y = bottom - v * (bottom - top);                                     \
        n++;                                                                        \
    }
    int sh = curve == EC_LIN ? 0 : 1;
    SEG(X, X + wa, 0.0f, 1.0f, (curve == EC_RC || curve == EC_DX7) ? 1 : 0)
    SEG(X + wa, X + wa + wd, 1.0f, s, sh)
    SEG(X + wa + wd, X + wa + wd + ws, s, s, 0)
    SEG(X + wa + wd + ws, X + W, s, 0.0f, sh)
#undef SEG
    polyline(layer, pts, n, d->arg == 0 ? C_FLT : C_ENV, 2);
}

// a modulator's own waveform (the sketch's draw_vis_lfo)
static void lfo_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    const float *p = g_wt.s.p;
    int b = d->arg ? WP_M2_SHAPE : WP_M1_SHAPE;
    int shape = (int)p[b];
    bool audio = p[b + 1] > 0.5f;
    float depth = p[b + 4], ph0 = p[b + 5];
    int div = (int)p[b + 6];
    float hz = div > 0 ? (studio_bpm_x100() / 6000.0f) / WT_SYNC_BEATS[div] : p[b + 2];
    float cycles = audio ? 6 : wt_clampf(hz * 1.5f, 0.5f, 6);
    static const float JIT[8] = {0.31f, -0.72f, 0.55f, -0.18f, 0.88f, -0.45f, 0.12f, -0.63f};
    pt_t pts[130];
    int n = 0;
    float mid = Y + H / 2, amp = (H / 2 - 2) * depth;
    for (int i = 0; i <= 128; i++) {
        float t = (float)i / 128 * cycles + ph0, f = t - floorf(t), v;
        switch (shape) {
        case LS_SINE: v = sinf(2 * PI * t); break;
        case LS_TRI: v = 4 * fabsf(f - 0.5f) - 1; break;
        case LS_SAW: v = 1 - 2 * f; break;
        case LS_RAMP: v = 2 * f - 1; break;
        case LS_SQR: v = f < 0.5f ? 1 : -1; break;
        default: v = JIT[(int)t & 7]; break;
        }
        pts[n].x = X + W * i / 128;
        pts[n].y = mid - v * amp;
        n++;
    }
    hline(layer, X, X + W, mid, C_LINE);
    polyline(layer, pts, n, C_MOD, 2);
}

// the distortion transfer curve (the sketch's draw_vis_drive)
static void drive_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    const float *p = g_wt.s.p;
    float drive = p[WP_D_DRIVE], mixv = p[WP_D_MIX];
    bool clip = p[WP_D_CLIP] > 0.5f, fld = p[WP_D_FOLD] > 0.5f;
    float steps = (float)(1 << ((int)p[WP_D_BITS] - 1));
    pt_t pts[130];
    int n = 0;
    for (int i = 0; i <= 128; i++) {
        float v = (float)i / 128 * 2 - 1, y = v * drive;
        bool on = clip || fld || p[WP_D_BITS] < 24 || p[WP_D_RATE] > 1 || drive > 1.001f;
        if (fld)
            for (int g = 0; g < 4; g++) y = y > 1 ? 2 - y : (y < -1 ? -2 - y : y);
        if (clip || !fld) {   // the engine's soft clip (a rational tanh)
            float c = wt_clampf(y, -3, 3);
            y = c * (27 + c * c) / (27 + 9 * c * c);
        }
        if (p[WP_D_BITS] < 24) y = roundf(y * steps) / steps;
        y = on ? v + mixv * (y - v) : v;   // all off: bypassed, as the engine does
        pts[n].x = X + W * i / 128;
        pts[n].y = Y + H / 2 - wt_clampf(y, -1, 1) * (H / 2 - 1);
        n++;
    }
    hline(layer, X, X + W, Y + H / 2, C_LINE);
    polyline(layer, pts, n, C_FX, 2);
}

// four voices spread by DETUNE across and WIDTH down (the sketch's
// draw_vis_unison), drifting live
static void unison_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    static const float DET[4] = {-1.0f, -1.0f / 3, 1.0f / 3, 1.0f};
    static const float PAN[4] = {-1.0f, 0.45f, -0.45f, 1.0f};
    const float *p = g_wt.s.p;
    pt_t c[2] = {{X + W / 2, Y}, {X + W / 2, Y + H}};
    polyline(layer, c, 2, C_LINE, 1);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = sw_c(C_VOICE);
    r.radius = 0;
    for (int v = 0; v < 4; v++) {
        float cents = DET[v] * p[WP_UNI_DET];
        float x = X + W / 2 + cents / 50 * (W / 2 - 10);
        float pan = wt_clampf(0.5f + PAN[v] * p[WP_UNI_WIDTH] * 0.5f, 0, 1);
        float y = Y + 4 + (H - 20) * pan;
        lv_area_t a = {(int32_t)x - 7, (int32_t)y, (int32_t)x + 7, (int32_t)y + 14};
        lv_draw_rect(layer, &r, &a);
    }
}

static void scope_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    static float s[256];
    int n = wt_scope_read(s, 256);
    pt_t pts[256];
    float mid = Y + H / 2;
    for (int i = 0; i < n; i++) {
        pts[i].x = X + W * i / (n - 1);
        pts[i].y = mid - wt_clampf(s[i] * 1.5f, -1, 1) * (H / 2 - 1);
    }
    hline(layer, X, X + W, mid, C_LINE);
    polyline(layer, pts, n, C_VOICE, 1);
}

static void voices_draw(lv_layer_t *layer, disp_t *d) {
    float X, Y, W, H;
    area_of(d, &X, &Y, &W, &H);
    wt_live_t lv;
    wt_live(&lv);
    float bw = W / WT_NV;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = 0;
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.font = SW_F_LABEL;
    t.align = LV_TEXT_ALIGN_CENTER;
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int v = 0; v < WT_NV; v++) {
        lv_area_t a = {(int32_t)(X + v * bw + 4), (int32_t)Y, (int32_t)(X + (v + 1) * bw - 4), (int32_t)(Y + H)};
        r.bg_color = sw_c(lv.gate[v] ? C_VOICE : (lv.active[v] ? 0x24496d : 0x242424));
        lv_draw_rect(layer, &r, &a);
        if (lv.active[v]) {
            static char txt[WT_NV][16];
            int nn = (int)lroundf(lv.note[v]);
            if (nn < 0) nn = 0;
            snprintf(txt[v], sizeof txt[v], "%s%d", NN[nn % 12], nn / 12 - 1);
            t.text = txt[v];
            t.color = sw_c(lv.gate[v] ? C_BG : C_TEXT);
            lv_area_t ta = {a.x1, a.y1 + (int32_t)H / 2 - 9, a.x2, a.y2};
            lv_draw_label(layer, &t, &ta);
        }
    }
}

static void curve_main(lv_event_t *e) {
    disp_t *d = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    switch (d->type) {
    case D_FILTER: filter_draw(layer, d); break;
    case D_ENV: env_draw(layer, d); break;
    case D_LFO: lfo_draw(layer, d); break;
    case D_DRIVE: drive_draw(layer, d); break;
    case D_UNISON: unison_draw(layer, d); break;
    case D_SCOPE: scope_draw(layer, d); break;
    case D_VOICES: voices_draw(layer, d); break;
    }
}

static lv_obj_t *curve(lv_obj_t *par, int type, int arg, int x, int y, int w, int h) {
    disp_t *d = new_disp(par, type, arg, x, y, w, h);
    lv_obj_add_event_cb(d->obj, curve_main, LV_EVENT_DRAW_MAIN_END, d);
    d->label = sw_label(d->obj, "", SW_F_SMALL, C_DIM, 6, 4, 0, false);
    return d->obj;
}

lv_obj_t *wd_filter(lv_obj_t *par, int x, int y, int w, int h) { return curve(par, D_FILTER, 0, x, y, w, h); }
lv_obj_t *wd_env(lv_obj_t *par, int which, int x, int y, int w, int h) { return curve(par, D_ENV, which, x, y, w, h); }
lv_obj_t *wd_lfo(lv_obj_t *par, int mod, int x, int y, int w, int h) { return curve(par, D_LFO, mod, x, y, w, h); }
lv_obj_t *wd_drive(lv_obj_t *par, int x, int y, int w, int h) { return curve(par, D_DRIVE, 0, x, y, w, h); }
lv_obj_t *wd_unison(lv_obj_t *par, int x, int y, int w, int h) { return curve(par, D_UNISON, 0, x, y, w, h); }
lv_obj_t *wd_scope(lv_obj_t *par, int x, int y, int w, int h) { return curve(par, D_SCOPE, 0, x, y, w, h); }
lv_obj_t *wd_voices(lv_obj_t *par, int x, int y, int w, int h) { return curve(par, D_VOICES, 0, x, y, w, h); }

// --- ticking ------------------------------------------------------------------------------------------------

static uint32_t sig_of(disp_t *d) {
    const float *p = g_wt.s.p;
    uint32_t h = 2166136261u;
    switch (d->type) {
    case D_WATERFALL: {
        wt_live_t lv;
        wt_live(&lv);
        h = mix(h, (float)(d->arg ? g_ib : g_ia));
        h = mix(h, p[d->arg ? WP_B_POS : WP_A_POS]);
        h = mix(h, roundf((d->arg ? lv.pos_b : lv.pos_a) * 200));
        break;
    }
    case D_FILTER:
        for (int i = WP_FTYPE; i <= WP_RESO; i++) h = mix(h, p[i]);
        break;
    case D_ENV: {
        int b = d->arg == 0 ? WP_AA : d->arg == 3 ? WP_M3_A : WP_M4_A;
        for (int i = b; i <= b + 4; i++) h = mix(h, p[i]);
        break;
    }
    case D_LFO: {
        int b = d->arg ? WP_M2_SHAPE : WP_M1_SHAPE;
        for (int i = b; i <= b + 6; i++) h = mix(h, p[i]);
        h = mix(h, (float)studio_bpm_x100());
        break;
    }
    case D_DRIVE:
        for (int i = WP_D_DRIVE; i <= WP_D_MIX; i++) h = mix(h, p[i]);
        break;
    case D_UNISON:
        for (int i = WP_UNI_DET; i <= WP_UNI_WIDTH; i++) h = mix(h, p[i]);
        break;
    default: h = 0; break;   // live: always
    }
    return h;
}

void wd_tick(int page) {
    (void)page;
    for (int k = 0; k < s_nd; k++) {
        disp_t *d = s_d[k];
        if (!lv_obj_is_visible(d->obj)) continue;
        if (d->type == D_WATERFALL) {
            int idx = d->arg ? g_ib : g_ia;
            const wt_table_t *t = &g_cat.t[idx];
            if (idx != d->table || t->w.data != d->table_data) {
                d->table = idx;
                d->table_data = t->w.data;
                wf_render(d, t);
                char s[48];
                snprintf(s, sizeof s, "%s  -  %d frames", t->name, t->w.ncyc);
                lv_label_set_text(d->label, s);
            }
        }
        uint32_t s = sig_of(d);
        if (s == 0 || s != d->sig) {
            d->sig = s;
            // labels change here, never while drawing (that would redraw forever)
            if (d->type == D_LFO)
                lv_label_set_text(d->label, g_wt.s.p[d->arg ? WP_M2_MODE : WP_M1_MODE] > 0.5f
                                                ? "AUDIO: an FM / AM operator on the note"
                                                : "");
            lv_obj_invalidate(d->obj);
        }
    }
}
