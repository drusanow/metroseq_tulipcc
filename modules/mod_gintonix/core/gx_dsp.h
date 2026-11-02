// gx_dsp.h - one GinTonix drum voice, rendered natively (no AMY).
//
//   TONE  sine / triangle / saw (band-limited), pitch modulation:
//         DECAY  an exponential sweep of +-96 semitones,
//         SINE   +-48 semitones at 0..2 kHz (into FM territory),
//         RAND   sample & hold at 0..20 kHz (per hit below 0.4 Hz);
//         linear attack, exponential decay (-60 dB at DECAY).
//   NOISE white noise -> 12 dB state-variable LP / BP / HP, then its own
//         envelope: EXP, LIN (straight fall) or CLAP (2-3 fast bursts
//         during the attack time, then the decay).
//   HEAD  equal-power OSC/NOISE mix -> distortion -> peaking EQ ->
//         LEVEL x MASTER -> pan.
//
// The UI turns a patch into a gx_vp_t (all the coefficients) once per
// change; the audio task only runs gx_voice_render(). Pure C: rendered and
// checked on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gx_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GX_SR 44100.0f

typedef struct {
    // tone
    float f0;               // Hz
    uint8_t wave;           // 0 sine 1 triangle 2 saw
    uint8_t mod_mode;       // 0 decay 1 sine 2 random
    float mod_oct;          // modulation depth, octaves
    float mod_k;            // decay: per-sample multiplier (1 = never moves)
    float mod_inc;          // sine: phase increment / random: holds per sample
    bool mod_per_hit;       // random below 0.4 Hz: one new value per hit
    float osc_att_n;        // attack, samples
    float osc_dk;           // decay multiplier per sample
    float g_osc;            // mix gain
    // noise
    float g_noise;
    uint8_t n_mode, n_env;
    bool n_bypass;          // filter wide open: skip it
    float a1, a2, a3, k;    // SVF coefficients
    float n_att_n;          // attack, samples
    float n_dk;             // EXP / CLAP decay per sample
    float n_lin_step;       // LIN: fall per sample
    int clap_bursts;        // CLAP: bursts before the main hit
    int clap_gap;           // samples between bursts
    float clap_dk;          // a burst falls to 8 % over the gap
    // head
    bool dist_on;
    float drive, dist_norm;
    bool eq_on;
    float b0, b1, b2, ea1, ea2;
    float gain_l, gain_r;   // level x master x pan x trim
    float vs_osc, vs_noise, vs_mod;   // velocity sensitivity 0..2
    bool choke;
} gx_vp_t;

typedef struct {
    bool active;
    float vel;
    // tone
    float phase;
    float env_o;
    uint32_t t;             // samples since the hit
    float pm;               // decay modulation 1 -> 0
    float lfo;              // sine mod phase
    float rnd, rnd_cnt;     // random mod value / hold counter
    float oct;              // velocity-scaled depth
    float g_o, g_n;         // velocity-scaled part gains
    // noise
    float env_n;
    uint32_t seed;
    float ic1, ic2;
    // eq
    float z1, z2;
    // choke
    float fade, fade_k;
} gx_voice_t;

void gx_vp_from_patch(gx_vp_t *vp, const gx_patch_t *p, float master);

void gx_voice_trigger(gx_voice_t *v, const gx_vp_t *vp, float vel);
void gx_voice_choke(gx_voice_t *v);    // fade out within ~3 ms
// Adds n frames into l / r. Returns false once the voice has died away.
bool gx_voice_render(gx_voice_t *v, const gx_vp_t *vp, float *l, float *r, int n);

#ifdef __cplusplus
}
#endif
