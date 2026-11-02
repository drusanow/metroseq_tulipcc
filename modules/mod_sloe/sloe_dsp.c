// sloe_dsp.c - Sloe synth voices (see sloe_dsp.h).
#include "sloe_dsp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SL_HOT IRAM_ATTR
#else
#define SL_HOT
#endif

#define PI_F 3.14159265f

const char *const SL_WAVES[4] = {"SAW", "SQUARE", "TRI", "SINE"};

const sl_param_info_t SL_PARAMS[SL_NPARAM] = {
    [P_WAVE1] = {"wave1", "OSC 1", 0.0f, 4},      [P_WAVE2] = {"wave2", "OSC 2", 0.0f, 4},
    [P_SEMI2] = {"semi2", "OSC 2 SEMI", 0.5f, 0}, [P_DETUNE] = {"detune", "DETUNE", 0.15f, 0},
    [P_MIX] = {"mix", "OSC 1/2", 0.5f, 0},        [P_NOISE] = {"noise", "NOISE", 0.0f, 0},
    [P_CUTOFF] = {"cutoff", "CUTOFF", 0.55f, 0},  [P_RESO] = {"reso", "RESONANCE", 0.2f, 0},
    [P_ENVAMT] = {"envamt", "ENV AMT", 0.7f, 0},  [P_KEYTRK] = {"keytrk", "KEY TRACK", 0.5f, 0},
    [P_FA] = {"fa", "F ATTACK", 0.0f, 0},         [P_FD] = {"fd", "F DECAY", 0.35f, 0},
    [P_FS] = {"fs", "F SUSTAIN", 0.2f, 0},        [P_FR] = {"fr", "F RELEASE", 0.3f, 0},
    [P_AA] = {"aa", "ATTACK", 0.0f, 0},           [P_AD] = {"ad", "DECAY", 0.4f, 0},
    [P_AS] = {"as", "SUSTAIN", 0.7f, 0},          [P_AR] = {"ar", "RELEASE", 0.3f, 0},
    [P_GLIDE] = {"glide", "GLIDE", 0.2f, 0},      [P_VEL] = {"vel", "VELOCITY", 0.5f, 0},
    [P_LEVEL] = {"level", "LEVEL", 0.7f, 0},      [P_MONO] = {"mono", "VOICES", 0.0f, 2},
};

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

void sl_patch_default(sl_patch_t *p) {
    for (int i = 0; i < SL_NPARAM; i++) p->v[i] = SL_PARAMS[i].def;
}

int sl_param_step(const sl_patch_t *p, int i) {
    int n = SL_PARAMS[i].steps;
    int s = (int)(p->v[i] * (n - 1) + 0.5f);
    return s < 0 ? 0 : (s >= n ? n - 1 : s);
}

// --- mappings -------------------------------------------------------------------------------

static float env_s(float k) { return 0.001f + 8.0f * k * k * k; }        // 1 ms .. 8 s
static float cutoff_hz(float k) { return 20.0f * powf(900.0f, k); }       // 20 Hz .. 18 kHz
static float semi2(float k) { return roundf((k - 0.5f) * 48.0f); }        // -24 .. +24
static float detune_c(float k) { return k * 50.0f; }                       // 0 .. 50 cents
static float glide_s(float k) { return k < 0.01f ? 0.0f : 0.002f + 2.0f * k * k * k; }

void sl_param_text(const sl_patch_t *p, int i, char *o, int n) {
    float k = p->v[i];
    switch (i) {
    case P_WAVE1:
    case P_WAVE2: snprintf(o, n, "%s", SL_WAVES[sl_param_step(p, i)]); return;
    case P_MONO: snprintf(o, n, "%s", sl_param_step(p, i) ? "MONO" : "POLY 6"); return;
    case P_SEMI2: snprintf(o, n, "%+d st", (int)semi2(k)); return;
    case P_DETUNE: snprintf(o, n, "%d ct", (int)(detune_c(k) + 0.5f)); return;
    case P_CUTOFF: {
        float f = cutoff_hz(k);
        if (f < 1000)
            snprintf(o, n, "%d Hz", (int)f);
        else
            snprintf(o, n, "%.1f kHz", f / 1000);
        return;
    }
    case P_ENVAMT: snprintf(o, n, "%+.1f oct", (k - 0.5f) * 2 * 6); return;
    case P_FA: case P_FD: case P_FR: case P_AA: case P_AD: case P_AR: {
        float s = env_s(k);
        if (s < 1)
            snprintf(o, n, "%d ms", (int)(s * 1000));
        else
            snprintf(o, n, "%.2f s", s);
        return;
    }
    case P_GLIDE: {
        float s = glide_s(k);
        if (s <= 0)
            snprintf(o, n, "off");
        else if (s < 1)
            snprintf(o, n, "%d ms", (int)(s * 1000));
        else
            snprintf(o, n, "%.2f s", s);
        return;
    }
    default: snprintf(o, n, "%d%%", (int)(k * 100 + 0.5f)); return;
    }
}

// --- presets ------------------------------------------------------------------------------------

#define PATCH(...) {{__VA_ARGS__}}
//                   w1  w2  semi det  mix  nse  cut  res  env  key  fa   fd   fs   fr   aa   ad   as   ar   gli  vel  lvl mono
const sl_preset_t SL_PRESETS[] = {
    {"Init",    PATCH(0, 0, .5f, .15f, .5f, 0, .55f, .2f, .70f, .5f, 0, .35f, .2f, .3f, 0, .4f, .7f, .3f, .2f, .5f, .7f, 0)},
    {"Sloe Bass", PATCH(0, 1.f/3, .25f, .1f, .35f, 0, .32f, .35f, .78f, .3f, 0, .28f, .1f, .2f, 0, .35f, .6f, .15f, .25f, .6f, .8f, 1)},
    {"Acid",    PATCH(1.f/3, 0, .5f, 0, 0, 0, .28f, .82f, .85f, .2f, 0, .3f, 0, .2f, 0, .3f, .8f, .12f, .3f, .7f, .7f, 1)},
    {"Pluck",   PATCH(0, 1.f/3, .5f, .25f, .5f, .02f, .35f, .25f, .82f, .6f, 0, .25f, 0, .25f, 0, .3f, 0, .3f, 0, .6f, .7f, 0)},
    {"Pad",     PATCH(0, 0, .5f, .45f, .5f, .05f, .45f, .15f, .6f, .5f, .55f, .6f, .6f, .6f, .5f, .6f, .9f, .6f, 0, .2f, .55f, 0)},
    {"Lead",    PATCH(1.f/3, 0, .75f, .2f, .45f, 0, .6f, .3f, .65f, .6f, 0, .4f, .5f, .3f, .05f, .4f, .8f, .25f, .35f, .4f, .65f, 1)},
    {"Brass",   PATCH(0, 0, .5f, .3f, .5f, 0, .38f, .2f, .75f, .5f, .3f, .45f, .4f, .3f, .2f, .4f, .8f, .3f, 0, .5f, .65f, 0)},
    {"Bell",    PATCH(1, 2.f/3, .75f, .05f, .4f, 0, .8f, .1f, .5f, .7f, 0, .5f, .3f, .55f, 0, .62f, 0, .6f, 0, .6f, .6f, 0)},
};
const int SL_NPRESETS = sizeof SL_PRESETS / sizeof SL_PRESETS[0];

// --- engine ----------------------------------------------------------------------------------------

void sl_init(sl_engine_t *e) {
    memset(e, 0, sizeof *e);
    sl_patch_default(&e->p);
    for (int i = 0; i < SL_VOICES; i++) e->v[i].seed = 0x5EED1u * (uint32_t)(i + 3);
    e->last_pitch = 60;
}

static float glide_k(const sl_engine_t *e, int32_t glide_us) {
    float s = glide_us > 0 ? glide_us * 1e-6f : glide_s(e->p.v[P_GLIDE]);
    if (s <= 0) return 0.0f;
    // exponential approach, ~95 % of the way after s seconds
    return expf(-3.0f / (s * SL_SR));
}

static sl_voice_t *pick_voice(sl_engine_t *e, uint8_t src, uint8_t key) {
    // the same source + key again (a retrigger), else a free voice, else
    // the oldest releasing one, else the oldest
    sl_voice_t *best = NULL;
    for (int i = 0; i < SL_VOICES; i++)
        if (e->v[i].active && e->v[i].src == src && e->v[i].key == key) return &e->v[i];
    for (int i = 0; i < SL_VOICES; i++)
        if (!e->v[i].active) return &e->v[i];
    for (int i = 0; i < SL_VOICES; i++)
        if (!e->v[i].on && (!best || e->v[i].age < best->age)) best = &e->v[i];
    if (best) return best;
    best = &e->v[0];
    for (int i = 1; i < SL_VOICES; i++)
        if (e->v[i].age < best->age) best = &e->v[i];
    return best;
}

void sl_note_on(sl_engine_t *e, uint8_t src, uint8_t key, float note, float vel, int32_t glide_us) {
    bool mono = sl_param_step(&e->p, P_MONO) == 1;
    sl_voice_t *v;
    bool legato = false;
    if (mono) {
        v = &e->v[0];
        legato = v->on;   // a held note: slide into the new one, no retrigger
    } else {
        v = pick_voice(e, src, key);
        // a sequencer track is one voice: its slides start from its last note
        legato = false;
    }
    float from = v->active ? v->pitch : (mono ? e->last_pitch : note);
    if (!mono && src < 0x80 && glide_us > 0) {
        // routed track: glide from that track's previous note
        for (int i = 0; i < SL_VOICES; i++)
            if (e->v[i].active && e->v[i].src == src) from = e->v[i].pitch;
    }
    bool glide = (glide_us > 0) || (mono && legato && glide_s(e->p.v[P_GLIDE]) > 0);
    v->target = note;
    v->pitch = glide ? from : note;
    v->glide_k = glide ? glide_k(e, glide_us) : 0.0f;
    v->src = src;
    v->key = key;
    v->vel = vel;
    v->on = true;
    v->age = ++e->clock;
    if (!legato || !v->active) {
        v->ast = v->fst = 0;
        if (!v->active) {
            v->aenv = v->fenv = 0;
            v->ic1 = v->ic2 = 0;
            v->ph1 = 0;
            v->ph2 = 0.25f;
        }
    }
    v->active = true;
    e->last_pitch = note;
}

void sl_note_off(sl_engine_t *e, uint8_t src, uint8_t key) {
    for (int i = 0; i < SL_VOICES; i++) {
        sl_voice_t *v = &e->v[i];
        if (v->on && v->src == src && (v->key == key || src < 0x80)) {
            v->on = false;
            v->ast = v->fst = 2;
        }
    }
}

void sl_all_off(sl_engine_t *e, int src) {
    for (int i = 0; i < SL_VOICES; i++)
        if (src < 0 || e->v[i].src == src) {
            e->v[i].on = false;
            e->v[i].ast = e->v[i].fst = 2;
        }
}

static inline float blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float osc(int wave, float ph, float dt) {
    switch (wave) {
    case 0: return (2.0f * ph - 1.0f) - blep(ph, dt);   // saw
    case 1: {                                              // square
        float s = ph < 0.5f ? 1.0f : -1.0f;
        float p2 = ph + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        return s + blep(ph, dt) - blep(p2, dt);
    }
    case 2: return 4.0f * fabsf(ph - 0.5f) - 1.0f;      // triangle
    default: return sinf(2.0f * PI_F * ph);
    }
}

// one ADSR step: stage 0 attack (linear), 1 decay to sustain (exp), 2 release
static inline float adsr(float x, uint8_t *st, float a_inc, float d_k, float sus, float r_k) {
    if (*st == 0) {
        x += a_inc;
        if (x >= 1.0f) {
            x = 1.0f;
            *st = 1;
        }
    } else if (*st == 1) {
        x = sus + (x - sus) * d_k;
    } else {
        x *= r_k;
    }
    return x;
}

SL_HOT void sl_render(sl_engine_t *e, float *l, float *r, int n) {
    const sl_patch_t *p = &e->p;
    int w1 = sl_param_step(p, P_WAVE1), w2 = sl_param_step(p, P_WAVE2);
    float semi = semi2(p->v[P_SEMI2]) + detune_c(p->v[P_DETUNE]) / 100.0f;
    float mix = p->v[P_MIX], g1 = cosf(mix * PI_F * 0.5f), g2 = sinf(mix * PI_F * 0.5f);
    float gn = p->v[P_NOISE] * 0.7f;
    float cut0 = cutoff_hz(p->v[P_CUTOFF]);
    float k = 2.0f - 1.95f * p->v[P_RESO];   // 1/Q: 2 (flat) .. 0.05 (ringing)
    float env_oct = (p->v[P_ENVAMT] - 0.5f) * 2.0f * 6.0f;
    float keytrk = p->v[P_KEYTRK];
    float fa = 1.0f / (env_s(p->v[P_FA]) * SL_SR), aa = 1.0f / (env_s(p->v[P_AA]) * SL_SR);
    float fd = expf(-5.0f / (env_s(p->v[P_FD]) * SL_SR)), ad = expf(-5.0f / (env_s(p->v[P_AD]) * SL_SR));
    float fr = expf(-5.0f / (env_s(p->v[P_FR]) * SL_SR)), ar = expf(-5.0f / (env_s(p->v[P_AR]) * SL_SR));
    float fs = p->v[P_FS], as = p->v[P_AS];
    float level = p->v[P_LEVEL] * p->v[P_LEVEL] * 0.5f;
    float vsens = p->v[P_VEL];
    for (int vi = 0; vi < SL_VOICES; vi++) {
        sl_voice_t *v = &e->v[vi];
        if (!v->active) continue;
        float vg = level * (1.0f - vsens + vsens * v->vel);
        for (int i = 0; i < n; i += 8) {
            // pitch and filter move every 8 samples (0.18 ms)
            if (v->glide_k > 0) v->pitch = v->target + (v->pitch - v->target) * powf(v->glide_k, 8);
            else v->pitch = v->target;
            float f1 = 440.0f * exp2f((v->pitch - 69.0f) / 12.0f);
            float dt1 = clampf(f1 / SL_SR, 0.0f, 0.45f);
            float dt2 = clampf(f1 * exp2f(semi / 12.0f) / SL_SR, 0.0f, 0.45f);
            float fc = cut0 * exp2f(v->fenv * env_oct + keytrk * (v->pitch - 60.0f) / 12.0f);
            float g = tanf(PI_F * clampf(fc, 20.0f, 0.45f * SL_SR) / SL_SR);
            float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            int m = n - i < 8 ? n - i : 8;
            for (int j = 0; j < m; j++) {
                float x = g1 * osc(w1, v->ph1, dt1) + g2 * osc(w2, v->ph2, dt2);
                if (gn > 0) {
                    uint32_t s = v->seed;
                    s ^= s << 13;
                    s ^= s >> 17;
                    s ^= s << 5;
                    v->seed = s;
                    x += gn * ((float)(int32_t)s * (1.0f / 2147483648.0f));
                }
                v->ph1 += dt1;
                if (v->ph1 >= 1.0f) v->ph1 -= 1.0f;
                v->ph2 += dt2;
                if (v->ph2 >= 1.0f) v->ph2 -= 1.0f;
                float v3 = x - v->ic2;
                float v1 = a1 * v->ic1 + a2 * v3;
                float v2 = v->ic2 + a2 * v->ic1 + a3 * v3;
                v->ic1 = 2.0f * v1 - v->ic1;
                v->ic2 = 2.0f * v2 - v->ic2;
                v->fenv = adsr(v->fenv, &v->fst, fa, fd, fs, fr);
                v->aenv = adsr(v->aenv, &v->ast, aa, ad, as, ar);
                float y = v2 * v->aenv * vg;
                l[i + j] += y;
                r[i + j] += y;
            }
        }
        if (v->ast == 2 && v->aenv < 1e-4f) v->active = false;
        if (v->ast == 1 && as < 1e-4f && v->aenv < 1e-4f) v->active = false;   // plucked out
    }
}
