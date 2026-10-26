// wt_params.c - see wt_params.h. Ranges, steps and defaults are the
// sketch's (PAGES and P in wavetron_v1_2.py).
#include "wt_params.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const WT_VOICE_MODES[3] = {"MONO", "UNISON", "POLY"};
const char *const WT_FILTERS[7] = {"OFF", "LP12", "BP", "HP", "LP24", "NOTCH", "PHASR"};
const char *const WT_LFO_SHAPES[6] = {"SINE", "TRI", "SAW", "RAMP", "SQR", "S&H"};
const char *const WT_MOD_MODES[2] = {"LFO", "AUDIO"};
const char *const WT_POLARITY[2] = {"BIPOL", "UNIPOL"};
const char *const WT_EG_CURVES[4] = {"RC", "LIN", "DX7", "EXP"};
const char *const WT_SYNC_DIVS[8] = {"OFF", "4", "2", "1", "1/2", "1/4", "1/8", "1/16"};
const float WT_SYNC_BEATS[8] = {0, 4, 2, 1, 0.5f, 0.25f, 0.125f, 0.0625f};
const char *const WT_ONOFF[2] = {"OFF", "ON"};

const char *const WT_DEST_IDS[WT_NDEST] = {"a_pos", "b_pos", "a_pit", "b_pit", "a_lvl", "b_lvl",
                                           "a_drv", "b_drv", "cut", "f_drv", "pan"};
const char *const WT_DEST_LABELS[WT_NDEST] = {"A.POS", "B.POS", "A.PIT", "B.PIT", "A.LVL", "B.LVL",
                                              "A.DRV", "B.DRV", "CUT", "F.DRV", "PAN"};
const char *const WT_MOD_NAMES[WT_NMOD] = {"MOD1", "MOD2", "MOD3", "MOD4"};

#define E(k, l, lo, hi, d, st, names) {k, l, lo, hi, d, st, WK_E, names}
#define P_(k, l, lo, hi, d, st, kind) {k, l, lo, hi, d, st, kind, NULL}

const wt_param_info_t WT_PARAMS[WP_COUNT] = {
    [WP_A_POS] = P_("a_pos", "POSITION", 0, 1, 0.35f, 0, WK_F),
    [WP_A_COARSE] = P_("a_coarse", "COARSE", -24, 24, 0, 1, WK_I),
    [WP_A_FINE] = P_("a_fine", "FINE", -50, 50, 0, 1, WK_CT),
    [WP_A_LVL] = P_("a_lvl", "LEVEL", 0, 1, 0.8f, 0, WK_F),
    [WP_A_DRV] = P_("a_drv", "DRIVE", 1, 16, 1, 0, WK_F),
    [WP_A_FOLD] = E("a_fold", "FOLD", 0, 1, 0, 1, WT_ONOFF),
    [WP_A_PHASE] = P_("a_phase", "PHASE", 0, 1, 0, 0, WK_F),
    [WP_B_POS] = P_("b_pos", "POSITION", 0, 1, 0.5f, 0, WK_F),
    [WP_B_COARSE] = P_("b_coarse", "COARSE", -24, 24, 0, 1, WK_I),
    [WP_B_FINE] = P_("b_fine", "FINE", -50, 50, 7, 1, WK_CT),
    [WP_B_LVL] = P_("b_lvl", "LEVEL", 0, 1, 0.8f, 0, WK_F),
    [WP_B_DRV] = P_("b_drv", "DRIVE", 1, 16, 1, 0, WK_F),
    [WP_B_FOLD] = E("b_fold", "FOLD", 0, 1, 0, 1, WT_ONOFF),
    [WP_B_PHASE] = P_("b_phase", "PHASE", 0, 1, 0.25f, 0, WK_F),

    [WP_VMODE] = E("vmode", "VOICE", 0, 2, VM_UNISON, 1, WT_VOICE_MODES),
    [WP_GLIDE] = P_("glide", "GLIDE", 0, 2000, 0, 0, WK_MS),
    [WP_VSENS] = P_("vsens", "VEL > AMP", 0, 1, 0.4f, 0, WK_F),
    [WP_PHSYNC] = E("phsync", "PHASE SYNC", 0, 1, 1, 1, WT_ONOFF),
    [WP_BEND] = P_("bend", "BEND", 0, 12, 2, 1, WK_I),
    [WP_MCH] = P_("mch", "MIDI CH", 1, 16, 1, 1, WK_I),
    [WP_VOL] = P_("vol", "VOLUME", 0, 8, 4, 0, WK_F),

    [WP_FTYPE] = E("ftype", "TYPE", 0, 6, FT_LP24, 1, WT_FILTERS),
    [WP_CUTOFF] = P_("cutoff", "CUTOFF", 30, 12000, 2500, 0, WK_HZ),
    [WP_RESO] = P_("reso", "RESONANCE", 0.5f, 16, 1.2f, 0, WK_F),
    [WP_FENV] = P_("fenv", "ENV AMT", -4, 4, 1.5f, 0, WK_F),
    [WP_FKBD] = P_("fkbd", "KBD TRACK", 0, 1, 0.35f, 0, WK_F),
    [WP_FVEL] = P_("fvel", "VELOCITY", 0, 1, 0.25f, 0, WK_F),
    [WP_FDRV] = P_("fdrv", "DRIVE", 1, 16, 1, 0, WK_F),
    [WP_FMIX] = P_("fmix", "DRIVE MIX", 0, 1, 1, 0, WK_F),

    [WP_AA] = P_("aa", "ATTACK", 0, 4000, 8, 0, WK_MS),
    [WP_AD] = P_("ad", "DECAY", 1, 8000, 600, 0, WK_MS),
    [WP_AS] = P_("as", "SUSTAIN", 0, 1, 0.7f, 0, WK_F),
    [WP_AR] = P_("ar", "RELEASE", 1, 8000, 350, 0, WK_MS),
    [WP_ACURVE] = E("acurve", "CURVE", 0, 3, EC_RC, 1, WT_EG_CURVES),

    [WP_M1_SHAPE] = E("m1_shape", "SHAPE", 0, 5, LS_SINE, 1, WT_LFO_SHAPES),
    [WP_M1_MODE] = E("m1_mode", "MODE", 0, 1, 0, 1, WT_MOD_MODES),
    [WP_M1_RATE] = P_("m1_rate", "RATE", 0.01f, 20, 0.35f, 0, WK_F),
    [WP_M1_RATIO] = P_("m1_ratio", "RATIO", 0.25f, 16, 1, 0.25f, WK_F),
    [WP_M1_DEPTH] = P_("m1_depth", "DEPTH", 0, 1, 1, 0, WK_F),
    [WP_M1_PHASE] = P_("m1_phase", "PHASE", 0, 1, 0, 0, WK_F),
    [WP_M1_DIV] = E("m1_div", "SYNC", 0, 7, 0, 1, WT_SYNC_DIVS),
    [WP_M1_POL] = E("m1_pol", "POLARITY", 0, 1, 0, 1, WT_POLARITY),
    [WP_M1_TRIG] = E("m1_trig", "TRIG", 0, 1, 1, 1, WT_ONOFF),
    [WP_M2_SHAPE] = E("m2_shape", "SHAPE", 0, 5, LS_TRI, 1, WT_LFO_SHAPES),
    [WP_M2_MODE] = E("m2_mode", "MODE", 0, 1, 0, 1, WT_MOD_MODES),
    [WP_M2_RATE] = P_("m2_rate", "RATE", 0.01f, 20, 0.12f, 0, WK_F),
    [WP_M2_RATIO] = P_("m2_ratio", "RATIO", 0.25f, 16, 2, 0.25f, WK_F),
    [WP_M2_DEPTH] = P_("m2_depth", "DEPTH", 0, 1, 1, 0, WK_F),
    [WP_M2_PHASE] = P_("m2_phase", "PHASE", 0, 1, 0.25f, 0, WK_F),
    [WP_M2_DIV] = E("m2_div", "SYNC", 0, 7, 0, 1, WT_SYNC_DIVS),
    [WP_M2_POL] = E("m2_pol", "POLARITY", 0, 1, 0, 1, WT_POLARITY),
    [WP_M2_TRIG] = E("m2_trig", "TRIG", 0, 1, 0, 1, WT_ONOFF),

    [WP_M3_A] = P_("m3_a", "ATTACK", 0, 4000, 4, 0, WK_MS),
    [WP_M3_D] = P_("m3_d", "DECAY", 1, 8000, 900, 0, WK_MS),
    [WP_M3_S] = P_("m3_s", "SUSTAIN", 0, 1, 0, 0, WK_F),
    [WP_M3_R] = P_("m3_r", "RELEASE", 1, 8000, 400, 0, WK_MS),
    [WP_M3_CURVE] = E("m3_curve", "CURVE", 0, 3, EC_RC, 1, WT_EG_CURVES),
    [WP_M3_VEL] = P_("m3_vel", "VELOCITY", 0, 1, 0, 0, WK_F),
    [WP_M3_POL] = E("m3_pol", "POLARITY", 0, 1, 1, 1, WT_POLARITY),
    [WP_M4_A] = P_("m4_a", "ATTACK", 0, 4000, 5, 0, WK_MS),
    [WP_M4_D] = P_("m4_d", "DECAY", 1, 8000, 700, 0, WK_MS),
    [WP_M4_S] = P_("m4_s", "SUSTAIN", 0, 1, 0.35f, 0, WK_F),
    [WP_M4_R] = P_("m4_r", "RELEASE", 1, 8000, 400, 0, WK_MS),
    [WP_M4_CURVE] = E("m4_curve", "CURVE", 0, 3, EC_RC, 1, WT_EG_CURVES),
    [WP_M4_VEL] = P_("m4_vel", "VELOCITY", 0, 1, 0.25f, 0, WK_F),
    [WP_M4_POL] = E("m4_pol", "POLARITY", 0, 1, 1, 1, WT_POLARITY),

    [WP_UNI_DET] = P_("uni_det", "DETUNE", 0, 50, 15, 1, WK_CT),
    [WP_UNI_WIDTH] = P_("uni_width", "WIDTH", 0, 1, 0.6f, 0, WK_F),
    [WP_UNI_POS] = P_("uni_pos", "POS SPREAD", 0, 0.5f, 0.12f, 0, WK_F),
    [WP_UNI_PHASE] = P_("uni_phase", "PHASE SPREAD", 0, 1, 0.5f, 0, WK_F),
    [WP_UNI_DRIFT] = P_("uni_drift", "DRIFT", 0, 1, 0.2f, 0, WK_F),

    [WP_D_DRIVE] = P_("d_drive", "DRIVE", 1, 16, 1, 0, WK_F),
    [WP_D_CLIP] = E("d_clip", "CLIP", 0, 1, 0, 1, WT_ONOFF),
    [WP_D_FOLD] = E("d_fold", "FOLD", 0, 1, 0, 1, WT_ONOFF),
    [WP_D_BITS] = P_("d_bits", "BITS", 1, 24, 24, 1, WK_I),
    [WP_D_RATE] = P_("d_rate", "RATE DIV", 1, 32, 1, 1, WK_I),
    [WP_D_MIX] = P_("d_mix", "MIX", 0, 1, 1, 0, WK_F),
    [WP_CH_LVL] = P_("ch_lvl", "CHORUS", 0, 1, 0, 0, WK_F),
    [WP_CH_DLY] = P_("ch_dly", "DELAY", 1, 320, 320, 1, WK_I),
    [WP_CH_RATE] = P_("ch_rate", "RATE", 0.05f, 8, 0.5f, 0, WK_F),
    [WP_CH_DEP] = P_("ch_dep", "DEPTH", 0, 1, 0.5f, 0, WK_F),
    [WP_EC_LVL] = P_("ec_lvl", "ECHO", 0, 1, 0, 0, WK_F),
    [WP_EC_MS] = P_("ec_ms", "TIME", 20, 1000, 300, 0, WK_MS),
    [WP_EC_FB] = P_("ec_fb", "FEEDBACK", 0, 0.9f, 0.3f, 0, WK_F),
    [WP_EC_TONE] = P_("ec_tone", "TONE", -1, 1, 0, 0, WK_F),
    [WP_RV_LVL] = P_("rv_lvl", "REVERB", 0, 1, 0.12f, 0, WK_F),
    [WP_RV_LIVE] = P_("rv_live", "LIVENESS", 0, 1, 0.85f, 0, WK_F),
    [WP_RV_DAMP] = P_("rv_damp", "DAMPING", 0, 1, 0.5f, 0, WK_F),
    [WP_RV_XOVER] = P_("rv_xover", "XOVER", 200, 12000, 3000, 0, WK_HZ),
    [WP_EQ_L] = P_("eq_l", "EQ LOW", -15, 15, 0, 1, WK_DB),
    [WP_EQ_M] = P_("eq_m", "EQ MID", -15, 15, 0, 1, WK_DB),
    [WP_EQ_H] = P_("eq_h", "EQ HIGH", -15, 15, 0, 1, WK_DB),
};

float wt_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void wt_sound_default(wt_sound_t *s) {
    for (int i = 0; i < WP_COUNT; i++) s->p[i] = WT_PARAMS[i].def;
    memset(s->mx, 0, sizeof s->mx);
    // the sketch's starting matrix: the tables move on their own
    s->mx[0][D_A_POS] = 0.35f;    // MOD1 (LFO) -> OSC A position
    s->mx[0][D_B_POS] = -0.25f;   // ...and OSC B the other way
    s->mx[2][D_A_POS] = 0.4f;     // MOD3 (env) -> OSC A position sweep
}

// MOD4 -> CUTOFF is the filter envelope amount: one value, two places to
// edit it (FILTER's ENV AMT and the matrix), as in the sketch.
float wt_mx_get(const wt_sound_t *s, int mod, int dest) {
    if (mod == 3 && dest == D_CUT) return s->p[WP_FENV];
    return s->mx[mod][dest];
}

void wt_mx_set(wt_sound_t *s, int mod, int dest, float amt) {
    amt = wt_clampf(amt, -WT_MX_MAX, WT_MX_MAX);
    if (mod == 3 && dest == D_CUT)
        s->p[WP_FENV] = amt;
    else
        s->mx[mod][dest] = amt;
}

void wt_mx_clear(wt_sound_t *s) {
    memset(s->mx, 0, sizeof s->mx);
    s->p[WP_FENV] = 0;
}

float wt_param_fix(int i, float v) {
    const wt_param_info_t *pi = &WT_PARAMS[i];
    if (!(v == v)) v = pi->def;   // NaN from a bad file
    v = wt_clampf(v, pi->lo, pi->hi);
    if (pi->step > 0) v = pi->lo + roundf((v - pi->lo) / pi->step) * pi->step;
    return wt_clampf(v, pi->lo, pi->hi);
}

void wt_param_text(int i, float v, char *out, int len) {
    const wt_param_info_t *pi = &WT_PARAMS[i];
    switch (pi->kind) {
    case WK_E: {
        int k = (int)(v + 0.5f);
        int n = (int)(pi->hi + 0.5f);
        snprintf(out, len, "%s", pi->names[k < 0 ? 0 : (k > n ? n : k)]);
        return;
    }
    case WK_I: snprintf(out, len, "%d", (int)lroundf(v)); return;
    case WK_MS:
        if (v >= 1000)
            snprintf(out, len, "%.1fs", (double)(v / 1000));
        else
            snprintf(out, len, "%dms", (int)v);
        return;
    case WK_HZ:
        if (v >= 1000)
            snprintf(out, len, "%.1fk", (double)(v / 1000));
        else
            snprintf(out, len, "%dHz", (int)v);
        return;
    case WK_CT: snprintf(out, len, "%+dc", (int)lroundf(v)); return;
    case WK_DB: snprintf(out, len, "%+ddB", (int)lroundf(v)); return;
    default: snprintf(out, len, "%.2f", (double)v); return;
    }
}

// Times: a cube, so the short end (where attacks live) gets most of the
// travel and 0 stays reachable. Hz: logarithmic.
float wt_param_to01(int i, float v) {
    const wt_param_info_t *pi = &WT_PARAMS[i];
    v = wt_clampf(v, pi->lo, pi->hi);
    if (pi->kind == WK_HZ) return logf(v / pi->lo) / logf(pi->hi / pi->lo);
    if (pi->kind == WK_MS) return cbrtf(v / pi->hi);
    return (v - pi->lo) / (pi->hi - pi->lo);
}

float wt_param_from01(int i, float x) {
    const wt_param_info_t *pi = &WT_PARAMS[i];
    x = wt_clampf(x, 0, 1);
    float v;
    if (pi->kind == WK_HZ)
        v = pi->lo * powf(pi->hi / pi->lo, x);
    else if (pi->kind == WK_MS)
        v = pi->hi * x * x * x;
    else
        v = pi->lo + x * (pi->hi - pi->lo);
    return wt_param_fix(i, v);
}

int wt_rate_param(const wt_sound_t *s, int mod) {
    if (mod == 0) return s->p[WP_M1_MODE] > 0.5f ? WP_M1_RATIO : WP_M1_RATE;
    return s->p[WP_M2_MODE] > 0.5f ? WP_M2_RATIO : WP_M2_RATE;
}
