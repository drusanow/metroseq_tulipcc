// gx_dsp.c - GinTonix drum voice (see gx_dsp.h).
#include "gx_dsp.h"

#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define GX_HOT IRAM_ATTR   // the render loop runs from internal RAM
#else
#define GX_HOT
#endif

#define PI_F 3.14159265f
#define G_TONE  0.55f      // tone level into the head at mix 100/0 (the Python's)
#define G_NOISE 0.6f       // noise level at mix 0/100
#define OUT_TRIM 0.65f     // whole voice: a full kit peaks around -3 dB
#define SILENT 1e-4f       // -80 dB: a part is finished
#define DEC_60DB (-6.9077553f)   // ln(0.001)
#define TABLE_N 1024

static float s_sin[TABLE_N + 1];

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x, ~1e-5 relative error
static inline float fexp2(float x) {
    if (x < -60.0f) return 0.0f;
    if (x > 60.0f) x = 60.0f;
    int i = (int)x;
    if ((float)i > x) i--;   // floor
    float f = x - (float)i;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    union {
        float f;
        int32_t i;
    } u = {p};
    u.i += i * (1 << 23);
    return u.f;
}

static inline float ftanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline float dk_for(float seconds) {
    if (seconds < 1e-4f) seconds = 1e-4f;
    return expf(DEC_60DB / (seconds * GX_SR));
}

// --- patch -> coefficients ------------------------------------------------------------------

void gx_vp_from_patch(gx_vp_t *vp, const gx_patch_t *p, float master) {
    if (s_sin[TABLE_N / 4] == 0.0f)
        for (int i = 0; i <= TABLE_N; i++) s_sin[i] = sinf(2.0f * PI_F * i / TABLE_N);
    memset(vp, 0, sizeof *vp);

    // --- tone
    vp->f0 = gx_freq_hz(p->k[K_OSC_FREQ]);
    vp->wave = p->s[S_WAVE];
    vp->mod_mode = p->s[S_MOD_MODE];
    vp->mod_oct = gx_mod_semis(p) / 12.0f;
    float rate = gx_mod_rate(p);
    vp->mod_k = 1.0f;
    if (vp->mod_mode == 0) {
        // the sweep: an exponential fall that has all but finished at RATE
        if (rate > 0) vp->mod_k = expf(-3.0f / (rate * 0.001f * GX_SR));
    } else if (vp->mod_mode == 1) {
        vp->mod_inc = rate / GX_SR;
    } else {
        vp->mod_per_hit = rate < 0.4f;
        vp->mod_inc = rate / GX_SR;   // holds per sample
    }
    vp->osc_att_n = gx_attack_ms(p->k[K_OSC_ATT]) * 0.001f * GX_SR;
    vp->osc_dk = dk_for(gx_decay_ms(p->k[K_OSC_DEC]) * 0.001f);

    // --- mix (equal power)
    float mix = p->k[K_MIX];
    float g_osc = cosf(mix * PI_F * 0.5f), g_noise = sinf(mix * PI_F * 0.5f);
    if (g_osc < 0.002f) g_osc = 0.0f;
    if (g_noise < 0.002f) g_noise = 0.0f;
    vp->g_osc = g_osc * G_TONE;

    // --- noise
    vp->n_mode = p->s[S_N_MODE];
    vp->n_env = p->s[S_N_ENV];
    float nf = clampf(gx_freq_hz(p->k[K_N_FREQ]), 20.0f, 19000.0f);
    float q = clampf(gx_q_val(p->k[K_N_Q]), 0.1f, 60.0f);
    float ncomp;
    if (vp->n_mode == 1)
        ncomp = 1.0f / sqrtf(q > 1.0f ? q : 1.0f);
    else
        ncomp = q > 0.707f ? powf(0.707f / q, 0.6f) : 1.0f;
    vp->n_bypass = (vp->n_mode == 0 && nf >= 18000.0f && q <= 1.0f) ||
                   (vp->n_mode == 2 && nf <= 25.0f && q <= 1.0f);
    if (vp->n_bypass) ncomp = 1.0f;
    // Zavalishin's trapezoidal SVF: stable right up to the top of the band
    float g = tanf(PI_F * clampf(nf, 20.0f, 0.45f * GX_SR) / GX_SR);
    vp->k = 1.0f / q;
    vp->a1 = 1.0f / (1.0f + g * (g + vp->k));
    vp->a2 = g * vp->a1;
    vp->a3 = g * vp->a2;
    if (vp->n_mode == 1) ncomp *= vp->k * sqrtf(q);   // band-pass: unity-ish peak
    vp->g_noise = g_noise * G_NOISE * ncomp;
    float natt = gx_attack_ms(p->k[K_N_ATT]);
    float ndec = gx_decay_ms(p->k[K_N_DEC]);
    vp->n_att_n = natt * 0.001f * GX_SR;
    vp->n_dk = dk_for(ndec * 0.001f);
    if (vp->n_env == 1) {
        float fall = ndec * 2.0f / 3.0f * 0.001f * GX_SR;
        vp->n_lin_step = 1.0f / (fall < 1.0f ? 1.0f : fall);
    } else if (vp->n_env == 2) {
        // Microtonic's modulated (clap) EG: bursts while the attack runs
        int n = natt < 6.0f ? 1 : (natt < 25.0f ? 2 : 3);
        vp->clap_bursts = n - 1;
        if (n > 1) {
            int gap = (int)(natt / (float)(n - 1) * 0.001f * GX_SR);
            vp->clap_gap = gap < 1 ? 1 : gap;
            vp->clap_dk = expf(logf(0.08f) / (float)vp->clap_gap);
        }
        vp->n_att_n = 0;
    }

    // --- head: distortion -> EQ, then level, pan
    float dist = p->k[K_DIST];
    vp->dist_on = dist > 0.03f;
    if (vp->dist_on) {
        vp->drive = 1.0f * powf(24.0f, dist);
        // about the same peak level as clean, much more body
        vp->dist_norm = 0.55f / ftanh(0.5f * vp->drive);
    }
    float db = gx_eq_db(p->k[K_EQ_GAIN]);
    vp->eq_on = fabsf(db) >= 1.5f;
    if (vp->eq_on) {
        float ef = clampf(gx_freq_hz(p->k[K_EQ_FREQ]), 25.0f, 18000.0f);
        float A = powf(10.0f, db / 40.0f);
        float w0 = 2.0f * PI_F * ef / GX_SR;
        float alpha = sinf(w0) / (2.0f * 0.9f);
        float cw = cosf(w0);
        float a0 = 1.0f + alpha / A;
        vp->b0 = (1.0f + alpha * A) / a0;
        vp->b1 = (-2.0f * cw) / a0;
        vp->b2 = (1.0f - alpha * A) / a0;
        vp->ea1 = (-2.0f * cw) / a0;
        vp->ea2 = (1.0f - alpha / A) / a0;
    }
    float lvl = gx_level_gain(p->k[K_LEVEL]) * master * OUT_TRIM;
    float pan = clampf(p->k[K_PAN], 0.0f, 1.0f);
    // balance law: both sides full in the centre
    vp->gain_l = lvl * clampf(2.0f * (1.0f - pan), 0.0f, 1.0f);
    vp->gain_r = lvl * clampf(2.0f * pan, 0.0f, 1.0f);
    vp->vs_osc = p->k[K_VEL_OSC] * 2.0f;
    vp->vs_noise = p->k[K_VEL_NOISE] * 2.0f;
    vp->vs_mod = p->k[K_VEL_MOD] * 2.0f;
    vp->choke = p->s[S_CHOKE] != 0;
}

// --- voice ---------------------------------------------------------------------------------------

static inline uint32_t xs(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static inline float urnd(uint32_t *s) {   // -1 .. 1
    return (float)(int32_t)xs(s) * (1.0f / 2147483648.0f);
}

// Microtonic velocity sensitivity: gain = 1 - s * (1 - vel)
static float vel_gain(float s, float vel) { return clampf(1.0f - s * (1.0f - vel), 0.0f, 1.0f); }

void gx_voice_trigger(gx_voice_t *v, const gx_vp_t *vp, float vel) {
    uint32_t seed = v->seed ? v->seed : 0x2545F491u;
    memset(v, 0, sizeof *v);
    v->seed = seed;
    v->active = true;
    v->vel = vel;
    v->g_o = vp->g_osc * vel_gain(vp->vs_osc, vel);
    v->g_n = vp->g_noise * vel_gain(vp->vs_noise, vel);
    v->oct = vp->mod_oct * vel_gain(vp->vs_mod, vel);
    v->pm = 1.0f;
    v->env_o = vp->osc_att_n < 1.0f ? 1.0f : 0.0f;
    v->env_n = (vp->n_att_n < 1.0f || vp->n_env == 2) ? 1.0f : 0.0f;
    v->rnd = urnd(&v->seed);
    v->fade = 1.0f;
    v->fade_k = 1.0f;
}

void gx_voice_choke(gx_voice_t *v) {
    if (v->active) v->fade_k = expf(DEC_60DB / (0.003f * GX_SR));
}

static inline float wave_at(int wave, float ph, float inc) {
    if (wave == 0) {
        float x = ph * TABLE_N;
        int i = (int)x;
        float f = x - (float)i;
        return s_sin[i] + (s_sin[i + 1] - s_sin[i]) * f;
    }
    if (wave == 1) return 4.0f * fabsf(ph - 0.5f) - 1.0f;
    // saw with a polyBLEP at the wrap, so high hats don't fold back
    float s = 2.0f * ph - 1.0f;
    if (ph < inc) {
        float t = ph / inc;
        s -= t + t - t * t - 1.0f;
    } else if (ph > 1.0f - inc) {
        float t = (ph - 1.0f) / inc;
        s -= t * t + t + t + 1.0f;
    }
    return s;
}

GX_HOT bool gx_voice_render(gx_voice_t *v, const gx_vp_t *vp, float *l, float *r, int n) {
    if (!v->active) return false;
    const bool tone = v->g_o > 0.0f;
    const bool noise = v->g_n > 0.0f;
    for (int i = 0; i < n; i++) {
        float x = 0.0f;
        uint32_t t = v->t++;
        // --- tone
        if (tone && (v->env_o > SILENT * 0.5f || (float)t < vp->osc_att_n)) {
            float m;
            switch (vp->mod_mode) {
            case 0:
                m = v->pm;
                v->pm *= vp->mod_k;
                break;
            case 1:
                m = s_sin[(int)(v->lfo * TABLE_N)];
                v->lfo += vp->mod_inc;
                if (v->lfo >= 1.0f) v->lfo -= 1.0f;
                break;
            default:
                if (!vp->mod_per_hit) {
                    v->rnd_cnt += vp->mod_inc;
                    if (v->rnd_cnt >= 1.0f) {
                        v->rnd_cnt -= (float)(int)v->rnd_cnt;
                        v->rnd = urnd(&v->seed);
                    }
                }
                m = v->rnd;
                break;
            }
            float f = vp->f0 * fexp2(v->oct * m);
            float inc = clampf(f, 1.0f, 20000.0f) * (1.0f / GX_SR);
            x += wave_at(vp->wave, v->phase, inc) * v->env_o * v->g_o;
            v->phase += inc;
            if (v->phase >= 1.0f) v->phase -= 1.0f;
            if ((float)t < vp->osc_att_n)
                v->env_o = ((float)t + 1.0f) / vp->osc_att_n;
            else
                v->env_o *= vp->osc_dk;
        }
        // --- noise
        if (noise && (v->env_n > SILENT * 0.5f || (float)t < vp->n_att_n)) {
            float w = urnd(&v->seed);
            float y;
            if (vp->n_bypass) {
                y = w;
            } else {
                float v3 = w - v->ic2;
                float v1 = vp->a1 * v->ic1 + vp->a2 * v3;
                float v2 = v->ic2 + vp->a2 * v->ic1 + vp->a3 * v3;
                v->ic1 = 2.0f * v1 - v->ic1;
                v->ic2 = 2.0f * v2 - v->ic2;
                y = vp->n_mode == 0 ? v2 : (vp->n_mode == 1 ? v1 : w - vp->k * v1 - v2);
            }
            x += y * v->env_n * v->g_n;
            float ft = (float)t;
            if (vp->n_env == 2) {
                // bursts, each falling to 8 %, then the main hit
                uint32_t nt = t + 1, end = (uint32_t)(vp->clap_gap * vp->clap_bursts);
                if (nt < end)
                    v->env_n = (nt % (uint32_t)vp->clap_gap) == 0 ? 1.0f : v->env_n * vp->clap_dk;
                else if (nt == end)
                    v->env_n = 1.0f;
                else
                    v->env_n *= vp->n_dk;
            } else if (ft < vp->n_att_n) {
                v->env_n = (ft + 1.0f) / vp->n_att_n;
            } else if (vp->n_env == 1) {
                v->env_n -= vp->n_lin_step;
                if (v->env_n < 0.0f) v->env_n = 0.0f;
            } else {
                v->env_n *= vp->n_dk;
            }
        }
        // --- head
        if (vp->dist_on) x = ftanh(x * vp->drive) * vp->dist_norm;
        if (vp->eq_on) {
            float y = vp->b0 * x + v->z1;
            v->z1 = vp->b1 * x - vp->ea1 * y + v->z2;
            v->z2 = vp->b2 * x - vp->ea2 * y;
            x = y;
        }
        x *= v->fade;
        v->fade *= v->fade_k;
        l[i] += x * vp->gain_l;
        r[i] += x * vp->gain_r;
    }
    bool tone_live = tone && (v->env_o > SILENT || (float)v->t < vp->osc_att_n);
    bool noise_live = noise && (v->env_n > SILENT || (float)v->t < vp->n_att_n);
    if ((!tone_live && !noise_live) || v->fade < SILENT) v->active = false;
    return v->active;
}
