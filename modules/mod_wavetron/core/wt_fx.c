// wt_fx.c - see wt_fx.h.
#include "wt_fx.h"

#include <math.h>
#include <string.h>

#include "wt_params.h"
#include "wt_tables.h"

#define SR 44100.0f
#define PI 3.14159265f

// soft clip: a rational tanh, flat beyond +-3
static inline float sclip(float x) {
    if (x > 3) return 1;
    if (x < -3) return -1;
    float x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}

static inline float fold(float x) {
    for (int g = 0; g < 4; g++) {
        if (x > 1)
            x = 2 - x;
        else if (x < -1)
            x = -2 - x;
    }
    return x;
}

// --- RBJ biquads ----------------------------------------------------------------------------

static void bq_shelf(wt_biquad_t *q, float f0, float db, bool high) {
    float A = powf(10, db / 40), w = 2 * PI * f0 / SR, c = cosf(w), s = sinf(w);
    float al = s / 2 * sqrtf(2), sa = 2 * sqrtf(A) * al;
    float b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * c + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - sa;
    } else {
        b0 = A * ((A + 1) + (A - 1) * c + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - sa;
    }
    q->b0 = b0 / a0, q->b1 = b1 / a0, q->b2 = b2 / a0, q->a1 = a1 / a0, q->a2 = a2 / a0;
}

static void bq_peak(wt_biquad_t *q, float f0, float db, float Q) {
    float A = powf(10, db / 40), w = 2 * PI * f0 / SR, c = cosf(w), al = sinf(w) / (2 * Q);
    float a0 = 1 + al / A;
    q->b0 = (1 + al * A) / a0, q->b1 = -2 * c / a0, q->b2 = (1 - al * A) / a0;
    q->a1 = -2 * c / a0, q->a2 = (1 - al / A) / a0;
}

static inline float bq_run(wt_biquad_t *q, int ch, float x) {
    float y = q->b0 * x + q->z1[ch];
    q->z1[ch] = q->b1 * x - q->a1 * y + q->z2[ch];
    q->z2[ch] = q->b2 * x - q->a2 * y;
    return y;
}

// --- set-up ----------------------------------------------------------------------------------

// Freeverb's tunings (at 44.1 kHz); the right side is spread by 23 samples
static const int COMB_LEN[WT_FX_NCOMB] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int AP_LEN[WT_FX_NAP] = {556, 441, 341, 225};
#define SPREAD 23

bool wt_fx_init(wt_fx_t *f) {
    memset(f, 0, sizeof *f);
    for (int c = 0; c < 2; c++) {
        f->ch_buf[c] = wt_alloc(WT_FX_CH_MAX * sizeof(float));
        f->ec_buf[c] = wt_alloc(WT_FX_ECHO_MAX * sizeof(int16_t));
        if (!f->ch_buf[c] || !f->ec_buf[c]) return false;
        for (int k = 0; k < WT_FX_NCOMB; k++) {
            wt_comb_t *cb = &f->comb[c][k];
            cb->len = COMB_LEN[k] + c * SPREAD;
            cb->buf = wt_alloc(cb->len * sizeof(float));
            if (!cb->buf) return false;
        }
        for (int k = 0; k < WT_FX_NAP; k++) {
            wt_ap_t *a = &f->ap[c][k];
            a->len = AP_LEN[k] + c * SPREAD;
            a->buf = wt_alloc(a->len * sizeof(float));
            if (!a->buf) return false;
        }
    }
    wt_fx_clear(f);
    return true;
}

void wt_fx_clear(wt_fx_t *f) {
    for (int c = 0; c < 2; c++) {
        memset(f->ch_buf[c], 0, WT_FX_CH_MAX * sizeof(float));
        memset(f->ec_buf[c], 0, WT_FX_ECHO_MAX * sizeof(int16_t));
        for (int k = 0; k < WT_FX_NCOMB; k++) {
            memset(f->comb[c][k].buf, 0, f->comb[c][k].len * sizeof(float));
            f->comb[c][k].lp = 0;
        }
        for (int k = 0; k < WT_FX_NAP; k++) memset(f->ap[c][k].buf, 0, f->ap[c][k].len * sizeof(float));
        f->ec_lp[c] = f->ec_hp[c] = f->rv_in_lp[c] = 0;
        for (int b = 0; b < 3; b++) f->eq[b].z1[c] = f->eq[b].z2[c] = 0;
    }
    f->rv_tail = 0;
}

void wt_fx_set(wt_fx_t *f, const float *p) {
    f->d_drive = p[WP_D_DRIVE];
    f->d_clip = p[WP_D_CLIP] > 0.5f;
    f->d_fold = p[WP_D_FOLD] > 0.5f;
    f->d_bits = (int)lroundf(p[WP_D_BITS]);
    f->d_rate = (int)lroundf(p[WP_D_RATE]);
    f->d_mix = p[WP_D_MIX];
    f->d_on = f->d_clip || f->d_fold || f->d_bits < 24 || f->d_rate > 1 || f->d_drive > 1.001f;

    float el = p[WP_EQ_L], em = p[WP_EQ_M], eh = p[WP_EQ_H];
    f->eq_on = fabsf(el) > 0.01f || fabsf(em) > 0.01f || fabsf(eh) > 0.01f;
    bq_shelf(&f->eq[0], 250, el, false);
    bq_peak(&f->eq[1], 1000, em, 0.7f);
    bq_shelf(&f->eq[2], 5000, eh, true);

    f->ch_lvl = p[WP_CH_LVL];
    f->ch_dly = wt_clampf(p[WP_CH_DLY], 1, WT_FX_CH_MAX - 4);
    f->ch_rate = p[WP_CH_RATE];
    f->ch_dep = p[WP_CH_DEP];

    f->ec_lvl = p[WP_EC_LVL];
    f->ec_len = (int)wt_clampf(p[WP_EC_MS] * SR / 1000, 1, WT_FX_ECHO_MAX - 1);
    f->ec_fb = wt_clampf(p[WP_EC_FB], 0, 0.95f);
    f->ec_tone = p[WP_EC_TONE];

    f->rv_lvl = p[WP_RV_LVL];
    f->rv_fb = 0.7f + 0.24f * wt_clampf(p[WP_RV_LIVE], 0, 1);   // 0.94 at most: never runs away
    f->rv_damp = wt_clampf(p[WP_RV_DAMP], 0, 1);
    f->rv_xk = 1 - expf(-2 * PI * p[WP_RV_XOVER] / SR);
}

// --- the chain ------------------------------------------------------------------------------------

static void distort(wt_fx_t *f, float *l, float *r, int n) {
    float steps = (float)(1 << (f->d_bits - 1 > 0 ? f->d_bits - 1 : 0));
    bool crush = f->d_bits < 24;
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]}, w[2];
        for (int c = 0; c < 2; c++) {
            float y = x[c] * f->d_drive;
            if (f->d_fold) y = fold(y);
            if (f->d_clip || !f->d_fold) y = sclip(y);
            if (crush) y = roundf(y * steps) / steps;
            w[c] = y;
        }
        // rate divide: hold every d_rate-th sample
        if (f->d_rate > 1) {
            if (f->crush_n == 0) f->crush_l = w[0], f->crush_r = w[1];
            w[0] = f->crush_l, w[1] = f->crush_r;
            if (++f->crush_n >= f->d_rate) f->crush_n = 0;
        }
        l[i] = x[0] + f->d_mix * (w[0] - x[0]);
        r[i] = x[1] + f->d_mix * (w[1] - x[1]);
    }
}

static void chorus(wt_fx_t *f, float *l, float *r, int n) {
    float inc = f->ch_rate / SR, mid = f->ch_dly * 0.5f, sw = f->ch_dly * 0.45f * f->ch_dep;
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) f->ch_buf[c][f->ch_pos] = x[c];
        for (int c = 0; c < 2; c++) {
            float ph = f->ch_ph + c * 0.25f;   // a quarter cycle apart: width
            float s = sinf(2 * PI * ph);
            float d = mid + sw * s + 1;
            float rp = (float)f->ch_pos - d;
            while (rp < 0) rp += WT_FX_CH_MAX;
            int i0 = (int)rp;
            float fr = rp - (float)i0;
            int i1 = (i0 + 1) % WT_FX_CH_MAX;
            float tap = f->ch_buf[c][i0] + fr * (f->ch_buf[c][i1] - f->ch_buf[c][i0]);
            (c ? r : l)[i] = x[c] + f->ch_lvl * tap;
        }
        f->ch_pos = (f->ch_pos + 1) % WT_FX_CH_MAX;
        f->ch_ph += inc;
        if (f->ch_ph >= 1) f->ch_ph -= 1;
    }
}

#define EC_SCALE 8192.0f   // int16 line: +-4 full scale

static void echo(wt_fx_t *f, float *l, float *r, int n) {
    // tone: < 0 a low-pass in the loop (darker repeats), > 0 a high-pass
    float t = f->ec_tone;
    float klp = t < 0 ? 1 - 0.95f * -t : 1;
    float khp = t > 0 ? 0.02f + 0.3f * t * t : 0;
    for (int i = 0; i < n; i++) {
        int rp = f->ec_pos - f->ec_len;
        if (rp < 0) rp += WT_FX_ECHO_MAX;
        for (int c = 0; c < 2; c++) {
            float *x = c ? &r[i] : &l[i];
            float y = f->ec_buf[c][rp] * (1.0f / EC_SCALE);
            f->ec_lp[c] += klp * (y - f->ec_lp[c]);
            float yt = f->ec_lp[c];
            f->ec_hp[c] += khp * (yt - f->ec_hp[c]);
            yt -= f->ec_hp[c];
            float w = (*x + f->ec_fb * yt) * EC_SCALE;
            f->ec_buf[c][f->ec_pos] = (int16_t)(w > 32767 ? 32767 : (w < -32768 ? -32768 : w));
            *x += f->ec_lvl * y;
        }
        if (++f->ec_pos >= WT_FX_ECHO_MAX) f->ec_pos = 0;
    }
}

static void reverb(wt_fx_t *f, float *l, float *r, int n) {
    const float gain = 0.015f, damp = f->rv_damp, fb = f->rv_fb, xk = f->rv_xk;
    float tail = f->rv_tail;
    for (int i = 0; i < n; i++) {
        float in = (l[i] + r[i]) * gain;
        for (int c = 0; c < 2; c++) {
            float acc = 0;
            for (int k = 0; k < WT_FX_NCOMB; k++) {
                wt_comb_t *cb = &f->comb[c][k];
                float o = cb->buf[cb->pos];
                // damping above the crossover: highs decay faster
                cb->lp += xk * (o - cb->lp);
                float fo = o + damp * (cb->lp - o);
                cb->buf[cb->pos] = in + fo * fb;
                if (++cb->pos >= cb->len) cb->pos = 0;
                acc += o;
            }
            for (int k = 0; k < WT_FX_NAP; k++) {
                wt_ap_t *a = &f->ap[c][k];
                float b = a->buf[a->pos];
                a->buf[a->pos] = acc + b * 0.5f;
                acc = b - acc;
                if (++a->pos >= a->len) a->pos = 0;
            }
            (c ? r : l)[i] += f->rv_lvl * 0.5f * acc;
            float m = fabsf(acc);
            tail = m > tail ? m : tail * 0.9999f;
        }
    }
    f->rv_tail = tail;
}

void wt_fx_process(wt_fx_t *f, float *l, float *r, int n) {
    if (f->d_on) distort(f, l, r, n);
    if (f->eq_on)
        for (int i = 0; i < n; i++)
            for (int b = 0; b < 3; b++) {
                l[i] = bq_run(&f->eq[b], 0, l[i]);
                r[i] = bq_run(&f->eq[b], 1, r[i]);
            }
    if (f->ch_lvl > 0.001f) chorus(f, l, r, n);
    // echo and reverb run while their level is up; their lines keep the
    // tail so that turning one down lets it ring out
    if (f->ec_lvl > 0.001f) echo(f, l, r, n);
    if (f->rv_lvl > 0.001f) reverb(f, l, r, n);
}
