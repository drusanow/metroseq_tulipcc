// wt_params.h - Wavetron's parameters: the sketch's P dict as a table.
//
// Every sound parameter of wavetron_v1_2.py keeps its key, range, step and
// default, so the sketch's patch files load as they are (wt_patch.c). Values
// are stored in their own units (Hz, ms, cents, 0..1 ...), as the sketch did.
// The modulation matrix is a 4 x 11 grid of amounts beside them.
//
// Pure C, no ESP-IDF: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    // oscillator A / B (the table itself is chosen by catalogue index, kept
    // outside this array: see wt_state_t)
    WP_A_POS, WP_A_COARSE, WP_A_FINE, WP_A_LVL, WP_A_DRV, WP_A_FOLD, WP_A_PHASE,
    WP_B_POS, WP_B_COARSE, WP_B_FINE, WP_B_LVL, WP_B_DRV, WP_B_FOLD, WP_B_PHASE,
    // voice / global
    WP_VMODE, WP_GLIDE, WP_VSENS, WP_PHSYNC, WP_BEND, WP_MCH, WP_VOL,
    // filter
    WP_FTYPE, WP_CUTOFF, WP_RESO, WP_FENV, WP_FKBD, WP_FVEL, WP_FDRV, WP_FMIX,
    // amp envelope
    WP_AA, WP_AD, WP_AS, WP_AR, WP_ACURVE,
    // MOD 1 / MOD 2: oscillators (LFO or audio-rate operator)
    WP_M1_SHAPE, WP_M1_MODE, WP_M1_RATE, WP_M1_RATIO, WP_M1_DEPTH, WP_M1_PHASE, WP_M1_DIV,
    WP_M1_POL, WP_M1_TRIG,
    WP_M2_SHAPE, WP_M2_MODE, WP_M2_RATE, WP_M2_RATIO, WP_M2_DEPTH, WP_M2_PHASE, WP_M2_DIV,
    WP_M2_POL, WP_M2_TRIG,
    // MOD 3 / MOD 4: envelopes (MOD 4 is the filter envelope)
    WP_M3_A, WP_M3_D, WP_M3_S, WP_M3_R, WP_M3_CURVE, WP_M3_VEL, WP_M3_POL,
    WP_M4_A, WP_M4_D, WP_M4_S, WP_M4_R, WP_M4_CURVE, WP_M4_VEL, WP_M4_POL,
    // unison
    WP_UNI_DET, WP_UNI_WIDTH, WP_UNI_POS, WP_UNI_PHASE, WP_UNI_DRIFT,
    // effects: distortion, chorus, echo, reverb, EQ
    WP_D_DRIVE, WP_D_CLIP, WP_D_FOLD, WP_D_BITS, WP_D_RATE, WP_D_MIX,
    WP_CH_LVL, WP_CH_DLY, WP_CH_RATE, WP_CH_DEP,
    WP_EC_LVL, WP_EC_MS, WP_EC_FB, WP_EC_TONE,
    WP_RV_LVL, WP_RV_LIVE, WP_RV_DAMP, WP_RV_XOVER,
    WP_EQ_L, WP_EQ_M, WP_EQ_H,
    WP_COUNT
};

// how a value is shown and how a slider moves through it
enum { WK_F, WK_I, WK_E, WK_MS, WK_HZ, WK_CT, WK_DB };

typedef struct {
    const char *key;          // the sketch's P key ("a_pos")
    const char *label;        // what the UI calls it ("POSITION")
    float lo, hi, def;
    float step;               // 0 = continuous
    uint8_t kind;             // WK_*
    const char *const *names; // WK_E: the choices
} wt_param_info_t;

extern const wt_param_info_t WT_PARAMS[WP_COUNT];

// enum choices (the sketch's lists)
extern const char *const WT_VOICE_MODES[3];   // MONO UNISON POLY
extern const char *const WT_FILTERS[7];       // OFF LP12 BP HP LP24 NOTCH PHASR
extern const char *const WT_LFO_SHAPES[6];    // SINE TRI SAW RAMP SQR S&H
extern const char *const WT_MOD_MODES[2];     // LFO AUDIO
extern const char *const WT_POLARITY[2];      // BIPOL UNIPOL
extern const char *const WT_EG_CURVES[4];     // RC LIN DX7 EXP
extern const char *const WT_SYNC_DIVS[8];     // OFF 4 2 1 1/2 1/4 1/8 1/16
extern const float WT_SYNC_BEATS[8];
extern const char *const WT_ONOFF[2];

enum { VM_MONO, VM_UNISON, VM_POLY };
enum { FT_OFF, FT_LP12, FT_BP, FT_HP, FT_LP24, FT_NOTCH, FT_PHASER };
enum { LS_SINE, LS_TRI, LS_SAW, LS_RAMP, LS_SQR, LS_SH };
enum { EC_RC, EC_LIN, EC_DX7, EC_EXP };

// --- the modulation matrix ---------------------------------------------------------------

#define WT_NMOD  4
#define WT_NDEST 11
enum { D_A_POS, D_B_POS, D_A_PIT, D_B_PIT, D_A_LVL, D_B_LVL, D_A_DRV, D_B_DRV, D_CUT, D_F_DRV,
       D_PAN };
extern const char *const WT_DEST_IDS[WT_NDEST];      // "a_pos" ... (patch files)
extern const char *const WT_DEST_LABELS[WT_NDEST];   // "A.POS" ...
extern const char *const WT_MOD_NAMES[WT_NMOD];      // "MOD1" ...
#define WT_MX_MAX 4.0f

// --- the whole sound -----------------------------------------------------------------------

typedef struct {
    float p[WP_COUNT];
    float mx[WT_NMOD][WT_NDEST];   // MOD4 -> CUT is p[WP_FENV], see wt_mx_get()
} wt_sound_t;

void wt_sound_default(wt_sound_t *s);
float wt_mx_get(const wt_sound_t *s, int mod, int dest);
void wt_mx_set(wt_sound_t *s, int mod, int dest, float amt);
void wt_mx_clear(wt_sound_t *s);   // every routing, MOD4 -> CUT included

float wt_clampf(float v, float lo, float hi);
// clamp to the range, snap to the step for integer / enum kinds
float wt_param_fix(int i, float v);
// value as the sketch showed it ("600ms", "2.5k", "+7c", "UNISON")
void wt_param_text(int i, float v, char *out, int len);
// a slider's 0..1 <-> value (log for Hz, a cube for times, linear else)
float wt_param_to01(int i, float v);
float wt_param_from01(int i, float x);
// MOD1 / MOD2 RATE shows the ratio in AUDIO mode (the sketch's eff_key)
int wt_rate_param(const wt_sound_t *s, int mod);   // mod 0 / 1
