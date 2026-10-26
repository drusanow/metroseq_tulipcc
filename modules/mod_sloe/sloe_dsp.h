// sloe_dsp.h - Sloe, a small subtractive synth for the Tulip Studio.
//
// Two oscillators (saw / square / triangle / sine, band-limited) + noise ->
// 12 dB low-pass (state-variable, trapezoidal) with its own ADSR -> amp
// ADSR. 6 voices, or one (MONO, legato with glide). Notes come from MIDI
// IN or from a sequencer track routed to it on the Studio home page; a
// metroseq SLIDE becomes a glide. Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SL_VOICES 6
#define SL_SR 44100.0f

enum {
    P_WAVE1 = 0, P_WAVE2, P_SEMI2, P_DETUNE, P_MIX, P_NOISE,
    P_CUTOFF, P_RESO, P_ENVAMT, P_KEYTRK,
    P_FA, P_FD, P_FS, P_FR,
    P_AA, P_AD, P_AS, P_AR,
    P_GLIDE, P_VEL, P_LEVEL, P_MONO,
    SL_NPARAM
};

typedef struct {
    float v[SL_NPARAM];   // 0..1 each (waves / mono: steps)
} sl_patch_t;

typedef struct {
    const char *key, *label;
    float def;
    uint8_t steps;        // > 0: a switch with this many positions
} sl_param_info_t;

extern const sl_param_info_t SL_PARAMS[SL_NPARAM];
extern const char *const SL_WAVES[4];

void sl_patch_default(sl_patch_t *p);
void sl_param_text(const sl_patch_t *p, int i, char *out, int len);
int sl_param_step(const sl_patch_t *p, int i);   // switch position

typedef struct {
    const char *name;
    sl_patch_t p;
} sl_preset_t;
extern const sl_preset_t SL_PRESETS[];
extern const int SL_NPRESETS;

// --- engine (audio task) ---------------------------------------------------------------

typedef struct {
    bool on;          // key held
    bool active;      // sounding (release included)
    uint8_t src, key;
    uint32_t age;
    float vel;
    float pitch, target, glide_k;   // semitones (MIDI note numbers)
    float ph1, ph2;
    float fenv, aenv;
    uint8_t fst, ast;   // 0 attack 1 decay/sustain 2 release
    float ic1, ic2;
    uint32_t seed;
} sl_voice_t;

typedef struct {
    sl_patch_t p;
    sl_voice_t v[SL_VOICES];
    uint32_t clock;
    float last_pitch;   // for mono / glide
} sl_engine_t;

void sl_init(sl_engine_t *e);
// note on: src / key identify it for its note-off; glide_us > 0 slides
// from the source's previous pitch over that long
void sl_note_on(sl_engine_t *e, uint8_t src, uint8_t key, float note, float vel, int32_t glide_us);
void sl_note_off(sl_engine_t *e, uint8_t src, uint8_t key);
void sl_all_off(sl_engine_t *e, int src);   // src < 0: everyone
void sl_render(sl_engine_t *e, float *l, float *r, int n);
