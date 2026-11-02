// wt_fx.h - Wavetron's effects, written for it in C (the sketch used AMY's
// bus effects), in AMY's order: distortion -> EQ -> chorus -> echo ->
// reverb, on the synth's stereo output.
//
//   distortion  drive (1..16) into a fold and / or a soft clip, a bit
//               crusher (bits 1..24, sample-rate divide 1..32), wet mix
//   EQ          low shelf 250 Hz, mid peak 1 kHz, high shelf 5 kHz, +-15 dB
//   chorus      level, max delay (samples), rate (Hz), depth: two taps,
//               a quarter cycle apart for width
//   echo        level, time (20..1000 ms), feedback, tone (-1 dark .. +1
//               bright, in the loop)
//   reverb      level, liveness (decay), damping above the crossover: a
//               Freeverb-style network (4 combs + 2 all-passes per side)
//
// Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WT_FX_ECHO_MAX   44100     // 1 s
#define WT_FX_CH_MAX     512
#define WT_FX_NCOMB      4
#define WT_FX_NAP        2

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1[2], z2[2];
} wt_biquad_t;

// the reverb's lines: 16-bit, small enough for internal RAM (they're read
// at 12 places every sample - from PSRAM that traffic starved the panel)
typedef struct {
    int16_t *buf;
    int len, pos;
    float lp;
} wt_comb_t;

typedef struct {
    int16_t *buf;
    int len, pos;
} wt_ap_t;

typedef struct {
    // settings, from the sound (wt_fx_set)
    float d_drive, d_mix;
    bool d_clip, d_fold, d_on;
    int d_bits, d_rate;
    bool eq_on;
    wt_biquad_t eq[3];
    float ch_lvl, ch_dly, ch_rate, ch_dep;
    float ec_lvl, ec_fb, ec_tone;
    int ec_len;
    float rv_lvl, rv_fb, rv_damp, rv_xk;
    // state
    int crush_n;
    float crush_l, crush_r;
    float *ch_buf[2];
    int ch_pos;
    float ch_ph;
    int16_t *ec_buf[2];
    int ec_pos;
    float ec_lp[2], ec_hp[2];
    wt_comb_t comb[2][WT_FX_NCOMB];
    wt_ap_t ap[2][WT_FX_NAP];
} wt_fx_t;

bool wt_fx_init(wt_fx_t *f);
void wt_fx_set(wt_fx_t *f, const float *p);   // p = wt_sound_t.p
void wt_fx_clear(wt_fx_t *f);                 // silence every tail
void wt_fx_process(wt_fx_t *f, float *l, float *r, int n);
