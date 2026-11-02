// d9_dsp.c - Drumster9's lanes and FX buses (see d9_dsp.h).
#include "d9_dsp.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define BLK 64                       // processed in chunks of at most this
#define ECHO_SCALE 16384.0f          // the delay lines are int16
#define REV_SCALE 8192.0f
#define IDLE_LEVEL 1e-5f
#define IDLE_FRAMES (D9_SR / 4)

static const int REV_LEN[4] = {1153, 1381, 1607, 1873};   // 26 - 42 ms

bool d9_engine_init(d9_engine_t *e, void *(*alloc)(unsigned bytes)) {
    memset(e, 0, sizeof *e);
    bool ok = true;
    for (int b = 0; b < D9_BUSES; b++) {
        d9_bus_rt_t *u = &e->bus[b];
        u->key_freq = -1;
        for (int k = 0; k < 3; k++) u->key_eq[k] = 999;
        if (!alloc) continue;
        u->echo = alloc(D9_ECHO_MAX * sizeof(int16_t));
        if (u->echo) memset(u->echo, 0, D9_ECHO_MAX * sizeof(int16_t));
        for (int i = 0; i < 4; i++) {
            u->rlen[i] = REV_LEN[i];
            u->rv[i] = alloc(REV_LEN[i] * sizeof(int16_t));
            if (u->rv[i])
                memset(u->rv[i], 0, REV_LEN[i] * sizeof(int16_t));
            else
                ok = false;
        }
        if (!u->echo) ok = false;
    }
    return ok;
}

void d9_engine_silence(d9_engine_t *e) {
    memset(e->v, 0, sizeof e->v);
    memset(e->g, 0, sizeof e->g);
    for (int b = 0; b < D9_BUSES; b++) {
        d9_bus_rt_t *u = &e->bus[b];
        u->ic1 = u->ic2 = u->e_lp = u->e_prev = 0;
        memset(u->eq_x, 0, sizeof u->eq_x);
        memset(u->eq_y, 0, sizeof u->eq_y);
        if (u->echo) memset(u->echo, 0, D9_ECHO_MAX * sizeof(int16_t));
        for (int i = 0; i < 4; i++) {
            if (u->rv[i]) memset(u->rv[i], 0, u->rlen[i] * sizeof(int16_t));
            u->r_lp[i] = 0;
        }
        u->active = false;
        u->quiet = IDLE_FRAMES;
    }
}

// --- coefficients (only when a setting changes) -------------------------------------------

static void filter_coefs(d9_bus_rt_t *u, float freq, float reso) {
    if (freq == u->key_freq && reso == u->key_reso) return;
    u->key_freq = freq;
    u->key_reso = reso;
    float f = freq > D9_SR * 0.45f ? D9_SR * 0.45f : freq;
    u->f_g = tanf(PI_F * f / D9_SR);
    u->f_k = 1.0f / (reso < 0.5f ? 0.5f : reso);
    u->f_a1 = 1.0f / (1.0f + u->f_g * (u->f_g + u->f_k));
    u->f_a2 = u->f_g * u->f_a1;
    u->f_a3 = u->f_g * u->f_a2;
}

// RBJ: 0 low shelf, 1 peak (Q 1), 2 high shelf
static void eq_band(d9_bus_rt_t *u, int k, float db) {
    if (db == u->key_eq[k]) return;
    u->key_eq[k] = db;
    u->eq_on[k] = fabsf(db) > 0.05f;
    if (!u->eq_on[k]) return;
    static const float F0[3] = {800.0f, 2500.0f, 7000.0f};
    float A = powf(10.0f, db / 40.0f), w = 2.0f * PI_F * F0[k] / D9_SR;
    float c = cosf(w), s = sinf(w), sa = sqrtf(A);
    float b0, b1, b2, a0, a1, a2;
    if (k == 1) {
        float al = s / 2.0f;   // Q 1
        b0 = 1 + al * A, b1 = -2 * c, b2 = 1 - al * A;
        a0 = 1 + al / A, a1 = -2 * c, a2 = 1 - al / A;
    } else {
        float al = s / 2.0f * 1.41421356f;   // shelf slope 1
        if (k == 0) {
            b0 = A * ((A + 1) - (A - 1) * c + 2 * sa * al);
            b1 = 2 * A * ((A - 1) - (A + 1) * c);
            b2 = A * ((A + 1) - (A - 1) * c - 2 * sa * al);
            a0 = (A + 1) + (A - 1) * c + 2 * sa * al;
            a1 = -2 * ((A - 1) + (A + 1) * c);
            a2 = (A + 1) + (A - 1) * c - 2 * sa * al;
        } else {
            b0 = A * ((A + 1) + (A - 1) * c + 2 * sa * al);
            b1 = -2 * A * ((A - 1) + (A + 1) * c);
            b2 = A * ((A + 1) + (A - 1) * c - 2 * sa * al);
            a0 = (A + 1) - (A - 1) * c + 2 * sa * al;
            a1 = 2 * ((A - 1) - (A + 1) * c);
            a2 = (A + 1) - (A - 1) * c - 2 * sa * al;
        }
    }
    u->eq_b[k][0] = b0 / a0;
    u->eq_b[k][1] = b1 / a0;
    u->eq_b[k][2] = b2 / a0;
    u->eq_a[k][0] = a1 / a0;
    u->eq_a[k][1] = a2 / a0;
}

// --- a lane ------------------------------------------------------------------------------------

static inline float voice_next(d9_voice_t *v) {
    uint32_t i = v->pos >> 1;
    float x = v->pcm[i];
    if ((v->pos & 1) && i + 1 < v->len) x = 0.5f * (x + v->pcm[i + 1]);
    v->pos++;
    if ((v->pos >> 1) >= v->len) v->pcm = NULL;   // done
    return x * (1.0f / 32768.0f);
}

// frames [a, b) of one lane into out, at gain g0 + dg * frame
static void lane_run(d9_voice_t *pair, float *out, int a, int b, float g0, float dg) {
    for (int j = 0; j < 2; j++) {
        d9_voice_t *v = &pair[j];
        for (int i = a; i < b && v->pcm; i++) {
            float x = voice_next(v) * v->gain;
            if (v->fade) {
                x *= (float)v->fade / D9_FADE_FRAMES;
                if (--v->fade == 0) v->pcm = NULL;
            }
            out[i] += x * (g0 + dg * i);
        }
    }
}

static void lane_hit(d9_voice_t *pair, const d9_ev_t *ev) {
    // the hit still sounding fades out under the new one
    if (pair[0].pcm) {
        pair[1] = pair[0];
        pair[1].fade = D9_FADE_FRAMES;
    }
    pair[0] = (d9_voice_t){.pcm = ev->len ? ev->pcm : NULL, .len = ev->len, .gain = ev->vel};
}

// --- a bus ---------------------------------------------------------------------------------------

static void bus_run(d9_bus_rt_t *u, const d9_bus_t *p, const float *in, float *l, float *r, int n,
                    float *peak) {
    // filter
    float x[BLK];
    memcpy(x, in, n * sizeof(float));
    if (p->filter != FT_OFF) {
        filter_coefs(u, p->freq, p->reso);
        float ic1 = u->ic1, ic2 = u->ic2, a1 = u->f_a1, a2 = u->f_a2, a3 = u->f_a3, k = u->f_k;
        bool hp = p->filter == FT_HPF;
        for (int i = 0; i < n; i++) {
            float v3 = x[i] - ic2;
            float v1 = a1 * ic1 + a2 * v3;
            float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            x[i] = hp ? x[i] - k * v1 - v2 : v2;
        }
        u->ic1 = ic1;
        u->ic2 = ic2;
    }
    // EQ
    const float db[3] = {p->eq_l, p->eq_m, p->eq_h};
    for (int k = 0; k < 3; k++) {
        eq_band(u, k, db[k]);
        if (!u->eq_on[k]) continue;
        float b0 = u->eq_b[k][0], b1 = u->eq_b[k][1], b2 = u->eq_b[k][2];
        float a1 = u->eq_a[k][0], a2 = u->eq_a[k][1];
        float x1 = u->eq_x[k][0], x2 = u->eq_x[k][1], y1 = u->eq_y[k][0], y2 = u->eq_y[k][1];
        for (int i = 0; i < n; i++) {
            float y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = x[i], y2 = y1, y1 = y;
            x[i] = y;
        }
        u->eq_x[k][0] = x1, u->eq_x[k][1] = x2, u->eq_y[k][0] = y1, u->eq_y[k][1] = y2;
    }
    float pk = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(x[i]);
        if (a > pk) pk = a;
    }
    // echo (AMY's: tone > 0 low-passes into the line, < 0 high-passes out)
    float eo[BLK];
    bool echo = u->echo && p->echo_level > 0.0f;
    if (echo) {
        int d = (int)(p->echo_ms * (D9_SR / 1000.0f));
        if (d < 1) d = 1;
        if (d >= D9_ECHO_MAX) d = D9_ECHO_MAX - 1;
        float fb = p->echo_fb, tone = p->echo_tone, lvl = p->echo_level;
        int w = u->ew;
        for (int i = 0; i < n; i++) {
            int rd = w - d;
            if (rd < 0) rd += D9_ECHO_MAX;
            float out = u->echo[rd] * (1.0f / ECHO_SCALE);
            float nin;
            if (tone < 0) {
                float o2 = out + tone * u->e_prev;
                u->e_prev = out;
                out = o2;
                nin = x[i] + fb * out;
            } else {
                nin = x[i] + fb * out;
                if (tone > 0) nin = u->e_lp = nin + tone * (u->e_lp - nin);
            }
            float q = nin * ECHO_SCALE;
            u->echo[w] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
            if (++w >= D9_ECHO_MAX) w = 0;
            eo[i] = out * lvl;
        }
        u->ew = w;
    }
    // reverb: 4 delay lines through a Hadamard matrix, damped in the loop
    bool rev = u->rv[3] && p->rev_level > 0.0f;
    // liveness: how much goes round again; damping: how dark each pass gets
    float g = 0.5f * (0.70f + 0.28f * p->rev_live), damp = 1.0f - 0.85f * p->rev_damp;
    float lvl = p->rev_level;
    for (int i = 0; i < n; i++) {
        float L = x[i], R = x[i];
        if (echo) L += eo[i], R += eo[i];
        if (rev) {
            // each line's oldest sample is where it's written next
            float d[4];
            for (int k = 0; k < 4; k++) {
                float v = u->rv[k][u->ri[k]] * (1.0f / REV_SCALE);
                u->r_lp[k] += damp * (v - u->r_lp[k]);
                d[k] = u->r_lp[k];
            }
            float s01 = d[0] + d[1], d01 = d[0] - d[1], s23 = d[2] + d[3], d23 = d[2] - d[3];
            float hin[4] = {s01 + s23, d01 + d23, s01 - s23, d01 - d23}, inp = x[i] * 0.3f;
            for (int k = 0; k < 4; k++) {
                float q = (inp + g * hin[k]) * REV_SCALE;
                u->rv[k][u->ri[k]] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
                if (++u->ri[k] >= u->rlen[k]) u->ri[k] = 0;
            }
            L += (d[0] + d[2]) * lvl;
            R += (d[1] + d[3]) * lvl;
        }
        l[i] += L;
        r[i] += R;
        float a = fabsf(L) > fabsf(R) ? fabsf(L) : fabsf(R);
        if (a > pk) pk = a;
    }
    *peak = pk;
}

void d9_engine_render(d9_engine_t *e, const d9_mix_t *m, const d9_ev_t *ev, int nev, float *l,
                      float *r, int n) {
    if (m->stop)
        for (int L = 0; L < D9_LANES; L++)
            for (int j = 0; j < 2; j++)
                if (e->v[L][j].pcm && !e->v[L][j].fade) e->v[L][j].fade = D9_FADE_FRAMES;
    for (int done = 0; done < n;) {
        int c = n - done < BLK ? n - done : BLK;
        float in[D9_BUSES][BLK];
        bool live[D9_BUSES] = {0};
        memset(in, 0, sizeof in);
        for (int L = 0; L < D9_NVOICE; L++) {
            float tgt = m->lane_gain[L];
            float g0 = e->g[L], dg = (tgt - g0) / c;
            e->g[L] = tgt;
            d9_voice_t *pair = e->v[L];
            int b = m->lane_bus[L] < D9_BUSES ? m->lane_bus[L] : 0;
            // this chunk's hits for the lane, in frame order
            int at = 0;
            for (int k = 0; k < nev; k++) {
                if (ev[k].lane != L) continue;
                int f = ev[k].frame - done;
                if (f < 0 || f >= c) {
                    if (!(f < 0 && done == 0)) continue;
                    f = 0;
                }
                if (f > at) lane_run(pair, in[b], at, f, g0, dg);
                if (f > at) at = f;
                lane_hit(pair, &ev[k]);
            }
            if (pair[0].pcm || pair[1].pcm) {
                lane_run(pair, in[b], at, c, g0, dg);
                live[b] = true;
            }
        }
        for (int b = 0; b < D9_BUSES; b++) {
            d9_bus_rt_t *u = &e->bus[b];
            if (live[b]) {
                u->active = true;
                u->quiet = 0;
            }
            if (!u->active) {
                e->bus_peak[b] = 0;
                continue;
            }
            float pk;
            bus_run(u, &m->bus[b], in[b], l + done, r + done, c, &pk);
            if (pk > e->peak) e->peak = pk;
            if (pk > e->bus_peak[b]) e->bus_peak[b] = pk;
            // nothing in and the tails gone: stop working on this bus
            if (!live[b] && pk < IDLE_LEVEL && (u->quiet += c) >= IDLE_FRAMES) u->active = false;
            else if (pk >= IDLE_LEVEL) u->quiet = 0;
        }
        done += c;
    }
}
