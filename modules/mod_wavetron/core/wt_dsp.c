// wt_dsp.c - see wt_dsp.h.
//
// Each voice runs at two rates. Every WT_CR frames (control rate) it steps
// its envelopes and LFOs, evaluates the modulation matrix and works out its
// oscillator increments, positions, levels, drives and filter coefficients.
// Per sample it reads the two wavetables, shapes and mixes them, ramps the
// amp envelope, filters and pans - plus, for a MOD in AUDIO mode routed to
// a pitch or level, the operator itself, so that FM / AM happen at audio
// rate.
#include "wt_dsp.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265f
#define TAU 6.2831853f
#define DT ((float)WT_CR / WT_SR)

// the sketch's unison spreads, per voice
static const float UNI_DETUNE[WT_NV] = {-1.0f, -1.0f / 3, 1.0f / 3, 1.0f};
static const float UNI_PAN[WT_NV] = {-1.0f, 0.45f, -0.45f, 1.0f};
static const float UNI_PHASE[WT_NV] = {0.00f, 0.37f, 0.61f, 0.19f};
static const float UNI_POS[WT_NV] = {-1.0f, 0.4f, -0.4f, 1.0f};
static const float DRIFT_RATES[WT_NV] = {0.021f, 0.034f, 0.017f, 0.028f};
static const float DRIFT_PH0[WT_NV] = {0.00f, 0.30f, 0.60f, 0.10f};

// --- small fast helpers ---------------------------------------------------------------------

// 2^n for an integer n in -126..127, straight into the exponent bits
static inline float pow2i(int n) {
    union {
        uint32_t u;
        float f;
    } v = {.u = (uint32_t)(n + 127) << 23};
    return v.f;
}

// 2^x, good to ~0.1 % over the range FM needs
static inline float exp2_fast(float x) {
    if (x < -30) return 0;
    if (x > 30) x = 30;
    float fl = floorf(x), f = x - fl;
    float p = 1.0f + f * (0.6951786f + f * (0.2261868f + f * 0.0782080f));
    return p * pow2i((int)fl);
}

// sin(2 pi p) for p in [0, 1): a refined parabola (error < 0.1 %)
static inline float sin01(float p) {
    float x = p * 2 - 1;                  // sin(2 pi p) = -sin(pi x)
    float y = 4 * x * (1 - fabsf(x));
    y += 0.225f * (y * fabsf(y) - y);
    return -y;
}

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

static inline uint32_t rnd(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// 2^x to ~1e-7 (a 5th-order polynomial on the fraction): exact enough for
// pitch, and far cheaper than exp2f
static inline float exp2_acc(float x) {
    if (x < -60) return 0;
    if (x > 60) x = 60;
    float fl = floorf(x), f = x - fl;
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    return p * pow2i((int)fl);
}

static inline float note_hz(float n) { return 440.0f * exp2_acc((n - 69.0f) * (1.0f / 12)); }

// tan(x) for 0 < x < 1.42 (cutoffs up to 0.45 x the sample rate): a Pade
// approximant, within 0.1 % below 1 and 6 % at the very top
static inline float tan_fast(float x) {
    float x2 = x * x;
    return x * (15 - x2) / (15 - 6 * x2);
}

// --- envelopes ---------------------------------------------------------------------------------
// ADSR with the sketch's four curve shapes (AMY's eg types):
//   RC   one-pole approach (aiming past 1 so the attack ends in time)
//   LIN  straight lines
//   DX7  a fast-rising attack; decay and release straight in dB
//   EXP  attack straight in dB from -60 dB; decay and release in dB

enum { EG_IDLE, EG_A, EG_D, EG_S, EG_R };

#define EG_FLOOR 1e-4f   // -80 dB: the end of a release

static void eg_coefs(wt_egc_t *c, float a_ms, float d_ms, float s, float r_ms, int curve) {
    float a = a_ms * 0.001f, d = d_ms * 0.001f, r = r_ms * 0.001f;
    if (d < DT) d = DT;
    if (r < DT) r = DT;
    c->curve = (uint8_t)curve;
    c->s = s;
    c->a_now = a <= DT * 0.5f;
    c->a_inc = c->a_now ? 1 : DT / a;
    // RC aims at 1.25 (reaches 1 at t = a); DX7 at 2 (a sharper start)
    c->a_tgt = curve == EC_DX7 ? 2.0f : 1.25f;
    c->a_k = c->a_now ? 1 : 1 - expf(-DT / (a / logf(c->a_tgt / (c->a_tgt - 1))));
    c->a_mul = c->a_now ? 1 : powf(1000.0f, DT / a);   // EXP: -60 dB to 0 in a
    c->d_inc = (1 - s) * DT / d;
    c->d_k = 1 - expf(-DT / (d / 4.6f));
    float end = s > EG_FLOOR ? s : EG_FLOOR;
    c->d_mul = powf(end, DT / d);                       // straight in dB
    c->r_s = r;
    c->r_k = expf(-DT / (r / 4.6f));
}

static void eg_gate(wt_env_t *g, bool on, const wt_egc_t *c) {
    if (on) {
        g->st = EG_A;
    } else if (g->st != EG_IDLE) {
        g->st = EG_R;
        float r0 = g->v > EG_FLOOR ? g->v : EG_FLOOR;
        if (c->curve == EC_LIN)
            g->rk = r0 * DT / c->r_s;
        else if (c->curve == EC_RC)
            g->rk = c->r_k;
        else
            g->rk = powf(EG_FLOOR / r0, DT / c->r_s);   // straight in dB to -80
    }
}

static float eg_step(wt_env_t *g, const wt_egc_t *c) {
    switch (g->st) {
    case EG_A:
        if (c->a_now)
            g->v = 1;
        else if (c->curve == EC_LIN)
            g->v += c->a_inc;
        else if (c->curve == EC_EXP)
            g->v = (g->v > 0.001f ? g->v : 0.001f) * c->a_mul;
        else
            g->v += (c->a_tgt - g->v) * c->a_k;
        if (g->v >= 1) {
            g->v = 1;
            g->st = EG_D;
        }
        break;
    case EG_D:
        if (c->curve == EC_LIN) {
            g->v -= c->d_inc;
            if (g->v <= c->s) g->v = c->s, g->st = EG_S;
        } else if (c->curve == EC_RC) {
            g->v += (c->s - g->v) * c->d_k;
            if (g->v - c->s < 1e-4f) g->v = c->s, g->st = EG_S;
        } else {
            g->v *= c->d_mul;
            if (g->v <= c->s || g->v <= EG_FLOOR) g->v = c->s, g->st = EG_S;
        }
        break;
    case EG_S: g->v = c->s; break;
    case EG_R:
        if (c->curve == EC_LIN)
            g->v -= g->rk;
        else
            g->v *= g->rk;
        if (g->v <= EG_FLOOR) g->v = 0, g->st = EG_IDLE;
        break;
    default: g->v = 0; break;
    }
    return g->v;
}

static void all_coefs(wt_engine_t *e) {
    const float *p = e->s.p;
    e->nroute = 0;
    for (int k = 0; k < WT_NMOD; k++)
        for (int d = 0; d < WT_NDEST; d++) {
            float amt = wt_mx_get(&e->s, k, d);
            if (amt == 0) continue;
            e->route[e->nroute].mod = (uint8_t)k;
            e->route[e->nroute].dest = (uint8_t)d;
            e->route[e->nroute].amt = amt;
            e->nroute++;
        }
    eg_coefs(&e->egc[0], p[WP_AA], p[WP_AD], p[WP_AS], p[WP_AR], (int)p[WP_ACURVE]);
    eg_coefs(&e->egc[1], p[WP_M3_A], p[WP_M3_D], p[WP_M3_S], p[WP_M3_R], (int)p[WP_M3_CURVE]);
    eg_coefs(&e->egc[2], p[WP_M4_A], p[WP_M4_D], p[WP_M4_S], p[WP_M4_R], (int)p[WP_M4_CURVE]);
}

// --- set-up -----------------------------------------------------------------------------------------

bool wt_init(wt_engine_t *e) {
    memset(e, 0, sizeof *e);
    wt_sound_default(&e->s);
    e->bpm = 120;
    e->seed = 0x2545F491u;
    for (int v = 0; v < WT_NV; v++) e->v[v].pitch = e->v[v].target = 60;
    e->vmode = VM_UNISON;
    all_coefs(e);
    if (!wt_fx_init(&e->fx)) return false;
    wt_fx_set(&e->fx, e->s.p);
    return true;
}

void wt_set_sound(wt_engine_t *e, const wt_sound_t *s) {
    int old = (int)lroundf(e->s.p[WP_VMODE]);
    e->s = *s;
    // a new voice mode: nothing is reallocated, but notes are cut (as the
    // sketch's apply_mode)
    if ((int)lroundf(s->p[WP_VMODE]) != old) wt_all_off(e);
    all_coefs(e);
    wt_fx_set(&e->fx, s->p);
}

void wt_set_tables(wt_engine_t *e, const wt_wave_t *a, const wt_wave_t *b) {
    e->wa = *a;
    e->wb = *b;
}

// --- voices ---------------------------------------------------------------------------------------------

static int vmode(const wt_engine_t *e) { return (int)lroundf(e->s.p[WP_VMODE]); }

static float voice_phase(const wt_engine_t *e, int v, float base) {
    if (vmode(e) != VM_UNISON) return base;
    float p = base + UNI_PHASE[v] * e->s.p[WP_UNI_PHASE];
    return p - floorf(p);
}

static void voice_on(wt_engine_t *e, int v, uint8_t src, uint8_t key, float note, float vel,
                     bool retrigger, int32_t glide_us) {
    wt_voice_t *vo = &e->v[v];
    const float *p = e->s.p;
    vo->src = src;
    vo->key = key;
    vo->target = note;
    float gl = glide_us > 0 ? glide_us * 1e-6f : p[WP_GLIDE] * 0.001f;
    if (!vo->active || gl <= 0) {
        vo->pitch = note;   // nothing to glide from
        vo->glide_k = 1;
    } else {
        vo->glide_k = 1 - expf(-DT / (gl / 3));   // ~95 % of the way in gl
    }
    if (!retrigger) return;
    vo->vel = vel;
    vo->gate = true;
    vo->active = true;
    eg_gate(&vo->amp, true, &e->egc[0]);
    eg_gate(&vo->m3, true, &e->egc[1]);
    eg_gate(&vo->m4, true, &e->egc[2]);
    e->quiet = 0;
    if (p[WP_PHSYNC] > 0.5f) {
        vo->pha = voice_phase(e, v, p[WP_A_PHASE]);
        vo->phb = voice_phase(e, v, p[WP_B_PHASE]);
    }
    if (p[WP_M1_TRIG] > 0.5f) vo->lph[0] = p[WP_M1_PHASE];
    if (p[WP_M2_TRIG] > 0.5f) vo->lph[1] = p[WP_M2_PHASE];
}

static void voice_off(wt_engine_t *e, int v) {
    wt_voice_t *vo = &e->v[v];
    if (!vo->gate) return;
    vo->gate = false;
    eg_gate(&vo->amp, false, &e->egc[0]);
    eg_gate(&vo->m3, false, &e->egc[1]);
    eg_gate(&vo->m4, false, &e->egc[2]);
}

// POLY: the voice already holding this note, a free one, else the oldest
static int claim(wt_engine_t *e, uint8_t src, uint8_t key) {
    e->clock++;
    for (int v = 0; v < WT_NV; v++)
        if (e->v[v].gate && e->v[v].src == src && e->v[v].key == key) {
            e->v[v].age = e->clock;
            return v;
        }
    int best = -1;
    for (int v = 0; v < WT_NV; v++)
        if (!e->v[v].gate && (best < 0 || (e->v[best].active && !e->v[v].active) ||
                              (e->v[best].active == e->v[v].active && e->v[v].age < e->v[best].age)))
            best = v;
    if (best < 0) {
        best = 0;
        for (int v = 1; v < WT_NV; v++)
            if (e->v[v].age < e->v[best].age) best = v;
        voice_off(e, best);
    }
    e->v[best].age = e->clock;
    return best;
}

static void held_remove(wt_engine_t *e, uint8_t src, uint8_t key) {
    for (int i = 0; i < e->nheld; i++)
        if (e->held[i].src == src && e->held[i].key == key) {
            memmove(&e->held[i], &e->held[i + 1], (e->nheld - i - 1) * sizeof e->held[0]);
            e->nheld--;
            return;
        }
}

void wt_note_on(wt_engine_t *e, uint8_t src, uint8_t key, float note, float vel, int32_t glide_us) {
    held_remove(e, src, key);
    if (e->nheld == WT_HELD) {
        memmove(&e->held[0], &e->held[1], (WT_HELD - 1) * sizeof e->held[0]);
        e->nheld--;
    }
    e->held[e->nheld++] = (wt_held_t){src, key, note};
    e->cur_vel = vel;
    int m = vmode(e);
    e->vmode = m;
    bool legato = e->nheld > 1;
    if (m == VM_MONO) {
        voice_on(e, 0, src, key, note, vel, !legato, glide_us);
    } else if (m == VM_UNISON) {
        for (int v = 0; v < WT_NV; v++) voice_on(e, v, src, key, note, vel, !legato, glide_us);
    } else {
        int v = claim(e, src, key);
        voice_on(e, v, src, key, note, vel, true, glide_us);
    }
}

void wt_note_off(wt_engine_t *e, uint8_t src, uint8_t key) {
    held_remove(e, src, key);
    int m = vmode(e);
    if (m == VM_POLY) {
        for (int v = 0; v < WT_NV; v++)
            if (e->v[v].gate && e->v[v].src == src && e->v[v].key == key) voice_off(e, v);
        return;
    }
    int nv = m == VM_MONO ? 1 : WT_NV;
    if (e->nheld > 0) {
        // fall back to the note still held underneath, without retriggering
        const wt_held_t *h = &e->held[e->nheld - 1];
        for (int v = 0; v < nv; v++) voice_on(e, v, h->src, h->key, h->note, e->cur_vel, false, 0);
    } else {
        for (int v = 0; v < nv; v++) voice_off(e, v);
    }
}

void wt_all_off(wt_engine_t *e) {
    e->nheld = 0;
    for (int v = 0; v < WT_NV; v++) voice_off(e, v);
}

void wt_panic(wt_engine_t *e) {
    wt_all_off(e);
    for (int v = 0; v < WT_NV; v++) {
        wt_voice_t *vo = &e->v[v];
        vo->active = false;
        vo->amp.st = vo->m3.st = vo->m4.st = EG_IDLE;
        vo->amp.v = vo->m3.v = vo->m4.v = vo->amp_out = 0;
        vo->s1a = vo->s1b = vo->s2a = vo->s2b = vo->apfb = 0;
        memset(vo->ap, 0, sizeof vo->ap);
    }
    wt_fx_clear(&e->fx);
    e->oleft = 0;   // drop what was left of the last period
}

// --- one voice --------------------------------------------------------------------------------------------

// a MOD1 / MOD2 shape at phase p (0..1)
static inline float lfo_shape(int shape, float p, float sh) {
    switch (shape) {
    case LS_SINE: return sin01(p);
    case LS_TRI: return 4 * fabsf(p - 0.5f) - 1;
    case LS_SAW: return 1 - 2 * p;
    case LS_RAMP: return 2 * p - 1;
    case LS_SQR: return p < 0.5f ? 1.0f : -1.0f;
    default: return sh;
    }
}

typedef struct {
    float inc_a, inc_b;            // phase increments
    float pos_a, pos_b;
    float lvl_a, lvl_b;
    float drv_a, drv_b;
    bool shp_a, shp_b, fld_a, fld_b;
    float fdrv, fmix;
    bool fon;
    float k, apfbk;                // filter resonance (the coefficients live in the voice)
    float amp0, amp1;
    // audio-rate MODs: per-sample increments and their pitch / level amounts
    bool audio[2];
    float ainc[2];
    float pm_a[2], pm_b[2], am_a[2], am_b[2];
} vctl_t;

// a modulator's value through its polarity (the sketch's mod_terms)
static inline float polar(const float *p, int mod, float m, float depth) {
    static const int POL[WT_NMOD] = {WP_M1_POL, WP_M2_POL, WP_M3_POL, WP_M4_POL};
    bool uni = p[POL[mod]] > 0.5f;
    if (mod < 2) return uni ? (m + depth) * 0.5f : m;   // LFO swings +-depth
    return uni ? m : 2 * m - 1;                         // envelope 0..1
}

static void control(wt_engine_t *e, int v, vctl_t *c) {
    wt_voice_t *vo = &e->v[v];
    const float *p = e->s.p;
    bool uni = vmode(e) == VM_UNISON;

    // glide
    vo->pitch += (vo->target - vo->pitch) * vo->glide_k;
    float bend = e->bend * p[WP_BEND];
    float pitch = vo->pitch + bend;

    // envelopes
    c->amp0 = vo->amp_out;
    float amp = eg_step(&vo->amp, &e->egc[0]);
    vo->amp_out = amp;
    c->amp1 = amp;
    float e3 = eg_step(&vo->m3, &e->egc[1]);
    float e4 = eg_step(&vo->m4, &e->egc[2]);
    e3 *= 1 - p[WP_M3_VEL] + p[WP_M3_VEL] * vo->vel;
    e4 *= 1 - p[WP_M4_VEL] + p[WP_M4_VEL] * vo->vel;
    if (amp <= 0 && vo->amp.st == EG_IDLE && !vo->gate) vo->active = false;

    // MOD1 / MOD2
    float mval[WT_NMOD];
    for (int k = 0; k < 2; k++) {
        int base = k ? WP_M2_SHAPE : WP_M1_SHAPE;   // SHAPE MODE RATE RATIO DEPTH PHASE DIV
        int shape = (int)p[base];
        bool audio = p[base + 1] > 0.5f;
        float depth = p[base + 4];
        float hz;
        if (audio) {
            hz = note_hz(pitch) * p[base + 3];
        } else {
            int div = (int)p[base + 6];
            hz = div > 0 ? (e->bpm / 60.0f) / WT_SYNC_BEATS[div] : p[base + 2];
        }
        float inc = hz / WT_SR;
        c->audio[k] = audio;
        c->ainc[k] = inc;
        mval[k] = lfo_shape(shape, vo->lph[k], vo->lsh[k]) * depth;
        if (!audio) {
            float ph = vo->lph[k] + inc * WT_CR;
            if (ph >= 1) {
                ph -= floorf(ph);
                vo->lsh[k] = (float)(int32_t)rnd(&e->seed) * (1.0f / 2147483648.0f);
            }
            vo->lph[k] = ph;
        }
    }
    mval[2] = e3;
    mval[3] = e4;

    // the matrix (control rate), over its live routings only. An
    // AUDIO-rate MOD's pitch / level routings are left to the per-sample loop.
    float sum[WT_NDEST] = {0}, m[WT_NMOD];
    for (int k = 0; k < WT_NMOD; k++) m[k] = polar(p, k, mval[k], k < 2 ? p[k ? WP_M2_DEPTH : WP_M1_DEPTH] : 1);
    for (int k = 0; k < 2; k++) c->pm_a[k] = c->pm_b[k] = c->am_a[k] = c->am_b[k] = 0;
    for (int i = 0; i < e->nroute; i++) {
        int k = e->route[i].mod, d = e->route[i].dest;
        float amt = e->route[i].amt;
        if (k < 2 && c->audio[k]) {
            if (d == D_A_PIT) { c->pm_a[k] = amt; continue; }
            if (d == D_B_PIT) { c->pm_b[k] = amt; continue; }
            if (d == D_A_LVL) { c->am_a[k] = amt; continue; }
            if (d == D_B_LVL) { c->am_b[k] = amt; continue; }
        }
        sum[d] += amt * m[k];
    }

    // oscillators
    float cents = uni ? UNI_DETUNE[v] * p[WP_UNI_DET] + e->drift[v] : 0;
    float na = pitch + p[WP_A_COARSE] + (p[WP_A_FINE] + cents) * 0.01f;
    float nb = pitch + p[WP_B_COARSE] + (p[WP_B_FINE] + cents) * 0.01f;
    c->inc_a = note_hz(na) * exp2_acc(sum[D_A_PIT]) / WT_SR;
    c->inc_b = note_hz(nb) * exp2_acc(sum[D_B_PIT]) / WT_SR;
    if (c->inc_a > 0.45f) c->inc_a = 0.45f;
    if (c->inc_b > 0.45f) c->inc_b = 0.45f;
    float poff = uni ? UNI_POS[v] * p[WP_UNI_POS] : 0;
    c->pos_a = wt_clampf(p[WP_A_POS] + poff + sum[D_A_POS], 0, 1);
    c->pos_b = wt_clampf(p[WP_B_POS] + poff + sum[D_B_POS], 0, 1);
    vo->pos_a = c->pos_a;
    vo->pos_b = c->pos_b;
    float vg = 1 - p[WP_VSENS] + p[WP_VSENS] * vo->vel;
    c->lvl_a = p[WP_A_LVL] * fmaxf(0, 1 + sum[D_A_LVL]) * vg;
    c->lvl_b = p[WP_B_LVL] * fmaxf(0, 1 + sum[D_B_LVL]) * vg;
    // drive / fold stages: on only when asked for (an always-on clipper
    // colours the sound even at unity - the sketch's _stage_needed)
    c->fld_a = p[WP_A_FOLD] > 0.5f;
    c->fld_b = p[WP_B_FOLD] > 0.5f;
    float da = p[WP_A_DRV], db = p[WP_B_DRV];
    if (c->fld_a && da < 2) da = 2;   // folding only bites past +-1
    if (c->fld_b && db < 2) db = 2;
    c->drv_a = wt_clampf(da * exp2_acc(sum[D_A_DRV]), 0.0625f, 16);
    c->drv_b = wt_clampf(db * exp2_acc(sum[D_B_DRV]), 0.0625f, 16);
    c->shp_a = c->fld_a || da > 1.001f || sum[D_A_DRV] != 0;
    c->shp_b = c->fld_b || db > 1.001f || sum[D_B_DRV] != 0;

    // filter drive + filter
    c->fdrv = wt_clampf(p[WP_FDRV] * exp2_acc(sum[D_F_DRV]), 0.0625f, 16);
    c->fon = p[WP_FDRV] > 1.001f || sum[D_F_DRV] != 0;
    c->fmix = p[WP_FMIX];
    float oct = p[WP_FKBD] * (pitch - 69) / 12 + p[WP_FVEL] * vo->vel + sum[D_CUT];
    float fc = wt_clampf(p[WP_CUTOFF] * exp2_acc(oct), 20, WT_SR * 0.45f);
    float k = 1.0f / wt_clampf(p[WP_RESO], 0.5f, 16);
    c->k = k;
    c->apfbk = wt_clampf((p[WP_RESO] - 0.5f) / 15.5f * 0.9f, 0, 0.9f);
    // new coefficients only when the cutoff moved (0.2 %) or the resonance did
    if (fabsf(fc - vo->fc_c) > vo->fc_c * 0.002f || k != vo->k_c) {
        vo->fc_c = fc;
        vo->k_c = k;
        float g = tan_fast(PI * fc / WT_SR);
        vo->g = g;
        vo->a1 = 1 / (1 + g * (g + k));
        vo->a2 = g * vo->a1;
        vo->a3 = g * vo->a2;
        const float k2 = 1.41421356f;   // LP24's second stage: Butterworth
        vo->b1 = 1 / (1 + g * (g + k2));
        vo->b2 = g * vo->b1;
        vo->b3 = g * vo->b2;
        vo->apc = (g - 1) / (g + 1);
    }

    // pan (equal power), recomputed only when it moves
    float pan = uni ? 0.5f + UNI_PAN[v] * p[WP_UNI_WIDTH] * 0.5f : 0.5f;
    pan = wt_clampf(pan + sum[D_PAN], 0, 1);
    if (pan != vo->pan_c || vo->pl + vo->pr == 0) {
        vo->pan_c = pan;
        vo->pl = cosf(pan * PI * 0.5f);
        vo->pr = sinf(pan * PI * 0.5f);
    }
}

static inline float svf(float x, float *s1, float *s2, float a1, float a2, float a3, float k,
                        float *band, float *high) {
    float v3 = x - *s2;
    float v1 = a1 * *s1 + a2 * v3;
    float v2 = *s2 + a2 * *s1 + a3 * v3;
    *s1 = 2 * v1 - *s1;
    *s2 = 2 * v2 - *s2;
    *band = v1;
    *high = x - k * v1 - v2;
    return v2;
}

static void voice_run(wt_engine_t *e, int v, const vctl_t *c, float *l, float *r, int n) {
    wt_voice_t *vo = &e->v[v];
    const wt_wave_t *wa = &e->wa, *wb = &e->wb;
    int ft = (int)e->s.p[WP_FTYPE];
    int sh[2] = {(int)e->s.p[WP_M1_SHAPE], (int)e->s.p[WP_M2_SHAPE]};
    float dep[2] = {e->s.p[WP_M1_DEPTH], e->s.p[WP_M2_DEPTH]};
    bool fm = c->audio[0] || c->audio[1];
    float pha = vo->pha, phb = vo->phb;
    float da = (c->amp1 - c->amp0) / n, amp = c->amp0;
    for (int i = 0; i < n; i++) {
        float ia = c->inc_a, ib = c->inc_b, la = c->lvl_a, lb = c->lvl_b;
        if (fm) {
            for (int k = 0; k < 2; k++) {
                if (!c->audio[k]) continue;
                float m = polar(e->s.p, k, lfo_shape(sh[k], vo->lph[k], vo->lsh[k]) * dep[k], dep[k]);
                if (c->pm_a[k] != 0) ia *= exp2_fast(c->pm_a[k] * m);
                if (c->pm_b[k] != 0) ib *= exp2_fast(c->pm_b[k] * m);
                if (c->am_a[k] != 0) la *= fmaxf(0, 1 + c->am_a[k] * m);
                if (c->am_b[k] != 0) lb *= fmaxf(0, 1 + c->am_b[k] * m);
                float ph = vo->lph[k] + c->ainc[k];
                if (ph >= 1) {
                    ph -= floorf(ph);
                    vo->lsh[k] = (float)(int32_t)rnd(&e->seed) * (1.0f / 2147483648.0f);
                }
                vo->lph[k] = ph;
            }
        }
        float a = wt_read(wa, pha, c->pos_a);
        float b = wt_read(wb, phb, c->pos_b);
        pha += ia;
        if (pha >= 1) pha -= 1;
        phb += ib;
        if (phb >= 1) phb -= 1;
        if (c->shp_a) a = sclip(c->fld_a ? fold(a * c->drv_a) : a * c->drv_a);
        if (c->shp_b) b = sclip(c->fld_b ? fold(b * c->drv_b) : b * c->drv_b);
        float x = (a * la + b * lb) * amp;
        amp += da;
        if (c->fon) x += c->fmix * (sclip(x * c->fdrv) - x);
        float band, high, y;
        switch (ft) {
        case FT_LP12: y = svf(x, &vo->s1a, &vo->s1b, vo->a1, vo->a2, vo->a3, c->k, &band, &high); break;
        case FT_BP:
            svf(x, &vo->s1a, &vo->s1b, vo->a1, vo->a2, vo->a3, c->k, &band, &high);
            y = band * c->k;   // unity at the peak
            break;
        case FT_HP:
            svf(x, &vo->s1a, &vo->s1b, vo->a1, vo->a2, vo->a3, c->k, &band, &high);
            y = high;
            break;
        case FT_LP24: {
            float y1 = svf(x, &vo->s1a, &vo->s1b, vo->a1, vo->a2, vo->a3, c->k, &band, &high);
            y = svf(y1, &vo->s2a, &vo->s2b, vo->b1, vo->b2, vo->b3, 1.41421356f, &band, &high);
            break;
        }
        case FT_NOTCH: {
            float lo = svf(x, &vo->s1a, &vo->s1b, vo->a1, vo->a2, vo->a3, c->k, &band, &high);
            y = lo + high;
            break;
        }
        case FT_PHASER: {
            float in = x + c->apfbk * vo->apfb, z = in;
            for (int s = 0; s < 4; s++) {
                float o = vo->apc * z + vo->ap[s];
                vo->ap[s] = z - vo->apc * o;
                z = o;
            }
            vo->apfb = z;
            y = 0.5f * (x + z);
            break;
        }
        default: y = x; break;
        }
        l[i] += y * vo->pl;
        r[i] += y * vo->pr;
    }
    vo->pha = pha;
    vo->phb = phb;
}

// --- the block ---------------------------------------------------------------------------------------------

#define OUT_GAIN 0.045f  // volume 4 (the sketch's default): a unison note peaks ~0.55

// Silent this long with no voice sounding (every echo and reverb tail
// below -100 dB): Wavetron stops rendering until the next note, so an idle
// Wavetron in Studio costs nothing.
#define IDLE_FRAMES (WT_SR / 4)
#define IDLE_LEVEL 1e-5f

// One control period (WT_CR frames) of every voice, through the effects.
// Envelopes, glide and the amp ramps step by exactly WT_CR frames, so the
// period is never split, whatever block size the caller uses.
static void period(wt_engine_t *e, float g) {
    float *bl = e->ol, *br = e->or_;
    memset(bl, 0, sizeof e->ol);
    memset(br, 0, sizeof e->or_);
    for (int v = 0; v < WT_NV; v++) {
        if (!e->v[v].active) continue;
        vctl_t c;
        control(e, v, &c);
        voice_run(e, v, &c, bl, br, WT_CR);
    }
    for (int i = 0; i < WT_CR; i++) bl[i] *= g, br[i] *= g;
    wt_fx_process(&e->fx, bl, br, WT_CR);
}

void wt_render(wt_engine_t *e, float *l, float *r, int n) {
    if (!e->wa.data || !e->wb.data) return;
    bool any = false;
    for (int v = 0; v < WT_NV; v++) any |= e->v[v].active;
    if (!any && e->quiet >= IDLE_FRAMES) return;
    // analogue drift: a slow sine per voice, unison only (the sketch's
    // service_drift)
    float da = e->s.p[WP_UNI_DRIFT];
    e->drift_t += (float)n / WT_SR;
    for (int v = 0; v < WT_NV; v++) {
        float ph = DRIFT_PH0[v] + e->drift_t * DRIFT_RATES[v];
        e->drift[v] = da > 0 ? sinf(TAU * ph) * da * 4 : 0;
    }
    float g = e->s.p[WP_VOL] * OUT_GAIN;
    int done = 0;
    while (done < n) {
        if (e->oleft == 0) {
            period(e, g);
            e->oleft = WT_CR;
        }
        int m = n - done < e->oleft ? n - done : e->oleft;
        const float *bl = e->ol + WT_CR - e->oleft, *br = e->or_ + WT_CR - e->oleft;
        float pk = 0;
        for (int i = 0; i < m; i++) {
            l[done + i] += bl[i];
            r[done + i] += br[i];
            float x = 0.5f * (bl[i] + br[i]);
            e->scope[e->scope_w++ % WT_SCOPE] = x;
            pk = fmaxf(pk, fmaxf(fabsf(bl[i]), fabsf(br[i])));
        }
        e->oleft -= m;
        done += m;
        if (any || pk > IDLE_LEVEL) {
            e->quiet = 0;
        } else if ((e->quiet += m) >= IDLE_FRAMES) {
            // gone quiet (what's left of the tails is below -100 dB): stop
            memset(e->scope, 0, sizeof e->scope);
            e->oleft = 0;
            return;
        }
    }
}
