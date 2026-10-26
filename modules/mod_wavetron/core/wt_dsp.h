// wt_dsp.h - Wavetron's sound engine: 4 voices x 2 wavetable oscillators,
// the sketch's voice architecture rendered natively instead of through AMY.
//
//   per voice   OSC A + OSC B (wavetable, position, pitch, level, drive /
//               fold)  -> amp envelope -> filter drive -> filter (OFF / LP12
//               / BP / HP / LP24 / NOTCH / PHASER) -> pan
//               MOD1, MOD2: LFOs, or in AUDIO mode operators that track the
//               note (true audio-rate FM / AM here - AMY could only do it at
//               block rate); MOD3: an envelope; MOD4: the filter envelope.
//               The 4 x 11 matrix routes the four into eleven destinations.
//   voices      MONO (legato, last note), UNISON (one note on all four,
//               spread in detune / pan / position / phase, with drift), POLY
//               (oldest stolen) - the sketch's three allocation policies.
//   then        the effects (wt_fx.h) on the summed stereo signal.
//
// Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wt_fx.h"
#include "wt_params.h"
#include "wt_tables.h"

#define WT_NV 4
#define WT_SR 44100.0f
#define WT_CR 16              // control rate: envelopes, LFOs, matrix every 16 frames
#define WT_HELD 16
#define WT_SCOPE 512

typedef struct {
    uint8_t st;               // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
    float v, rel0;
} wt_env_t;

typedef struct {
    bool gate, active;
    uint8_t src, key;         // who started it (poly note-off)
    uint32_t age;
    float vel, target, pitch, glide_k;
    float pha, phb;
    float lph[2], lsh[2];     // MOD1 / MOD2 phase, sample-and-hold value
    wt_env_t amp, m3, m4;
    float amp_out;            // amp envelope at the end of the last step
    float s1a, s1b, s2a, s2b; // filter (two SVF stages)
    float ap[4], apfb;        // phaser
    float pos_a, pos_b;       // live positions (the display)
} wt_voice_t;

typedef struct {
    uint8_t src, key;
    float note;
} wt_held_t;

typedef struct {
    wt_sound_t s;
    wt_wave_t wa, wb;
    float bpm;
    float bend;               // the wheel, -1..1
    wt_voice_t v[WT_NV];
    wt_held_t held[WT_HELD];
    int nheld;
    uint32_t clock, seed;
    float cur_vel;
    float drift_t, drift[WT_NV];
    int vmode;                // the mode the voices were started in
    wt_fx_t fx;
    float scope[WT_SCOPE];    // the last output (mono), for the display
    uint32_t scope_w;
} wt_engine_t;

// fx memory comes from wt_alloc() (PSRAM on the Tulip)
bool wt_init(wt_engine_t *e);
// a new sound / tables (the UI's copy); notes keep playing
void wt_set_sound(wt_engine_t *e, const wt_sound_t *s);
void wt_set_tables(wt_engine_t *e, const wt_wave_t *a, const wt_wave_t *b);
// notes: src / key identify a note for its note-off; glide_us > 0 slides
// from the previous pitch over that long (a metroseq SLIDE)
void wt_note_on(wt_engine_t *e, uint8_t src, uint8_t key, float note, float vel, int32_t glide_us);
void wt_note_off(wt_engine_t *e, uint8_t src, uint8_t key);
void wt_all_off(wt_engine_t *e);
void wt_panic(wt_engine_t *e);    // all off, voices and effects silenced at once
// ADDS n frames into l / r
void wt_render(wt_engine_t *e, float *l, float *r, int n);
