// gx_model.h - GinTonix: drum patches, patterns, kits (the data model of
// gintonix.py, ported to C).
//
// 8 drum channels, each a Microtonic-style voice: TONE (sine / triangle /
// saw with decaying, sine or random pitch modulation) + NOISE (filtered,
// exponential / linear / clap envelope), mixed, distorted, EQ'd, panned.
// Every knob is a 0..1 position with the Microtonic's mapping; a channel
// has two patches, A and B, and the MORPH fader blends between them.
//
// 12 patterns (A-L) of up to 16 steps; each step of each channel is off,
// a hit, an accented hit, a fill (a flam / roll of 2-8 hits) or both.
//
// Pure C, no ESP-IDF: tested on the host against the Python.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX_NCH      8
#define GX_NSTEP    16
#define GX_NPAT     12
#define GX_NAME_MAX 24       // drum sound names
#define GX_KIT_NAME_MAX 16   // kit slot names (NAME_MAX in the Python)
#define GX_LEVEL_0DB 0.5623f // LEVEL knob position of 0 dB (max = +10 dB)
#define GX_MASTER_MAX 3.1626f   // (1 / LEVEL_0DB)^2: +10 dB

extern const char GX_PAT_NAMES[GX_NPAT + 1];   // "ABCDEFGHIJKL"

// --- the drum patch -------------------------------------------------------------------

enum {
    K_MIX = 0, K_EQ_FREQ, K_EQ_GAIN, K_DIST, K_LEVEL, K_PAN,
    K_OSC_FREQ, K_MOD_AMT, K_MOD_RATE, K_OSC_ATT, K_OSC_DEC,
    K_N_FREQ, K_N_Q, K_N_ATT, K_N_DEC,
    K_VEL_OSC, K_VEL_NOISE, K_VEL_MOD,
    GX_NKNOB
};

enum { S_WAVE = 0, S_MOD_MODE, S_N_MODE, S_N_ENV, S_CHOKE, GX_NSW };

typedef struct {
    float k[GX_NKNOB];
    uint8_t s[GX_NSW];
} gx_patch_t;

typedef struct {
    const char *key;     // JSON key ("osc_freq")
    const char *label;   // "OSC FREQ"
    const char *title;   // short pot title ("FREQ")
    float def;
    bool bipolar;
} gx_knob_info_t;

typedef struct {
    const char *key;
    const char *label;
    uint8_t n;
    const char *opts[3];
    uint8_t def;
} gx_switch_info_t;

extern const gx_knob_info_t GX_KNOBS[GX_NKNOB];
extern const gx_switch_info_t GX_SWITCHES[GX_NSW];

void gx_patch_default(gx_patch_t *p);
int gx_knob_index(const char *key);     // -1 if unknown
int gx_switch_index(const char *key);

// value mappings (Microtonic ranges)
float gx_freq_hz(float k);        // 20 Hz .. 20 kHz
float gx_decay_ms(float k);       // 10 ms .. 10 s
float gx_attack_ms(float k);      // 0 .. 10 s
float gx_q_val(float k);          // 0.1 .. 10000
float gx_eq_db(float k);          // -40 .. +40 dB
float gx_level_gain(float k);     // 0 .. +10 dB (linear)
float gx_mod_semis(const gx_patch_t *p);
// decaying: ms (< 0 = never moves), sine / random: Hz
float gx_mod_rate(const gx_patch_t *p);
// The knob's value as the Microtonic shows it ("632Hz", "+3.0dB", "L30" ...).
void gx_fmt(int knob, const gx_patch_t *p, char *out, int len);

// --- patterns ------------------------------------------------------------------------------

typedef struct {
    uint8_t len;      // 1..16
    uint8_t chain;    // plays into the next pattern
    uint16_t trig[GX_NCH], acc[GX_NCH], fill[GX_NCH];
} gx_pat_t;

void gx_pat_clear(gx_pat_t *p);
// rows of '.' off, 'x' hit, 'X' accent, 'f' fill, 'F' accented fill
void gx_pat_from_strings(gx_pat_t *p, const char *const *rows, int nrows);
bool gx_pat_empty(const gx_pat_t *p);
// The patterns of pattern i's chain: first index and count.
void gx_chain_of(const gx_pat_t *pats, int i, int *first, int *count);
int gx_chain_next(const gx_pat_t *pats, int i);   // -1: no chain

// step state: 0 off, else 1 | 2 accent | 4 fill
int gx_step_state(const gx_pat_t *p, int c, int i);
// TRIGGER / ACCENT / FILL edit modes ('t', 'a', 'f')
void gx_edit_step(gx_pat_t *p, int c, int i, char mode);

// pattern tools; chans = channel mask (0xFF = all)
void gx_pat_shift(gx_pat_t *p, int d, uint8_t chans);
void gx_pat_reverse(gx_pat_t *p, uint8_t chans);

// --- rates ------------------------------------------------------------------------------------

#define GX_NRATES 5
#define GX_NFILLS 7
extern const char *const GX_RATE_NAMES[GX_NRATES];   // "1/8" .. "1/32"
extern const uint8_t GX_RATE_TICKS[GX_NRATES];        // 48 PPQ ticks per step
extern const uint8_t GX_FILL_RATES[GX_NFILLS];        // 2..8 hits per fill

// --- the whole state (a kit / bank slot) ----------------------------------------------------

typedef struct {
    gx_patch_t a[GX_NCH], b[GX_NCH];   // morph end points
    char names[GX_NCH][GX_NAME_MAX + 1];
    float morph;                        // 0 = A .. 1 = B
    float master;                       // 0 .. GX_MASTER_MAX (linear)
    gx_pat_t pats[GX_NPAT];
    uint8_t rate;                       // index into GX_RATE_*
    uint8_t fill_rate;                  // index into GX_FILL_RATES
    float swing;                        // 0..1 = 0..50 % of a step
    float tempo;                        // BPM
    uint8_t cur;                        // selected pattern
} gx_state_t;

void gx_state_init(gx_state_t *s);
// The sounding patch of channel c at the current morph position.
void gx_eff(const gx_state_t *s, int c, gx_patch_t *out);
// Turn a knob of channel c at the current morph position: both end points
// move so the sounding value follows the knob.
void gx_set_knob(gx_state_t *s, int c, int knob, float v);
// Switches: at MORPH = A only A, at B only B, between: both.
void gx_set_switch(gx_state_t *s, int c, int sw, int v);
void gx_morph_copy(gx_state_t *s, bool a_to_b);

// --- randomizer (Microtonic Randomize / Alter) ----------------------------------------------

typedef float (*gx_rnd_fn)(void *ctx);   // 0..1
void gx_random_patch(gx_patch_t *p, gx_rnd_fn rnd, void *ctx);
void gx_alter_patch(gx_patch_t *p, float amount, gx_rnd_fn rnd, void *ctx);
void gx_random_pattern(gx_pat_t *p, int length, gx_rnd_fn rnd, void *ctx);
void gx_pat_alter(gx_pat_t *p, uint8_t chans, gx_rnd_fn rnd, void *ctx);
void gx_random_accents_fills(gx_pat_t *p, uint8_t chans, gx_rnd_fn rnd, void *ctx);

// --- factory content (gx_factory.c, generated from gintonic_presets.py) ----------------------

typedef struct {
    const char *name;
    gx_patch_t p;
} gx_drum_t;

typedef struct {
    const char *name;
    const char *drums[GX_NCH];
    const char *const *patterns[GX_NPAT];   // 8 rows each, or NULL
    float tempo;
    uint8_t rate, fill_rate;
    float swing;
    const char *chain;
} gx_kit_t;

extern const gx_drum_t GX_DRUMS[];
extern const int GX_NDRUMS;
extern const gx_kit_t GX_KITS[];
extern const int GX_NKITS;

const gx_patch_t *gx_drum_by_name(const char *name);
void gx_kit_state(const gx_kit_t *k, gx_state_t *out);

#ifdef __cplusplus
}
#endif
