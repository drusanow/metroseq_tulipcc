// gx_model.c - GinTonix data model (see gx_model.h). Function-for-function
// port of the patch / pattern / kit code in gintonix.py.
#include "gx_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char GX_PAT_NAMES[GX_NPAT + 1] = "ABCDEFGHIJKL";

const gx_knob_info_t GX_KNOBS[GX_NKNOB] = {
    [K_MIX] = {"mix", "OSC/NOISE", "OSC/NOISE", 0.5f, false},
    [K_EQ_FREQ] = {"eq_freq", "EQ FREQ", "EQ FREQ", 0.5f, false},
    [K_EQ_GAIN] = {"eq_gain", "EQ GAIN", "EQ GAIN", 0.5f, true},
    [K_DIST] = {"dist", "DISTORT", "DISTORT", 0.0f, false},
    [K_LEVEL] = {"level", "LEVEL", "LEVEL", GX_LEVEL_0DB, false},
    [K_PAN] = {"pan", "PAN", "PAN", 0.5f, true},
    [K_OSC_FREQ] = {"osc_freq", "OSC FREQ", "FREQ", 0.5f, false},
    [K_MOD_AMT] = {"mod_amt", "MOD AMT", "MOD AMT", 0.5f, true},
    [K_MOD_RATE] = {"mod_rate", "MOD RATE", "MOD RATE", 0.484f, false},
    [K_OSC_ATT] = {"osc_att", "OSC ATK", "ATTACK", 0.0f, false},
    [K_OSC_DEC] = {"osc_dec", "OSC DEC", "DECAY", 0.5f, false},
    [K_N_FREQ] = {"n_freq", "NOISE FREQ", "FREQ", 1.0f, false},
    [K_N_Q] = {"n_q", "NOISE Q", "Q", 0.17f, false},
    [K_N_ATT] = {"n_att", "NOISE ATK", "ATTACK", 0.0f, false},
    [K_N_DEC] = {"n_dec", "NOISE DEC", "DECAY", 0.5f, false},
    [K_VEL_OSC] = {"vel_osc", "VEL OSC", "VEL OSC", 0.0f, false},
    [K_VEL_NOISE] = {"vel_noise", "VEL NOISE", "VEL NOIS", 0.0f, false},
    [K_VEL_MOD] = {"vel_mod", "VEL MOD", "VEL MOD", 0.0f, false},
};

const gx_switch_info_t GX_SWITCHES[GX_NSW] = {
    [S_WAVE] = {"wave", "WAVE", 3, {"SIN", "TRI", "SAW"}, 0},
    [S_MOD_MODE] = {"mod_mode", "MOD", 3, {"DECAY", "SINE", "RAND"}, 0},
    [S_N_MODE] = {"n_mode", "FILTER", 3, {"LP", "BP", "HP"}, 0},
    [S_N_ENV] = {"n_env", "NOISE EG", 3, {"EXP", "LIN", "CLAP"}, 0},
    [S_CHOKE] = {"choke", "CHOKE", 2, {"OFF", "ON"}, 0},
};

const char *const GX_RATE_NAMES[GX_NRATES] = {"1/8", "1/8T", "1/16", "1/16T", "1/32"};
const uint8_t GX_RATE_TICKS[GX_NRATES] = {24, 16, 12, 8, 6};
const uint8_t GX_FILL_RATES[GX_NFILLS] = {2, 3, 4, 5, 6, 7, 8};

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static float clamp01(float x) { return clampf(x, 0.0f, 1.0f); }
static float bip(float k) { return (k - 0.5f) * 2.0f; }
static float expmap(float x, float lo, float hi) { return lo * powf(hi / lo, x); }

void gx_patch_default(gx_patch_t *p) {
    for (int i = 0; i < GX_NKNOB; i++) p->k[i] = GX_KNOBS[i].def;
    for (int i = 0; i < GX_NSW; i++) p->s[i] = GX_SWITCHES[i].def;
}

int gx_knob_index(const char *key) {
    for (int i = 0; i < GX_NKNOB; i++)
        if (strcmp(GX_KNOBS[i].key, key) == 0) return i;
    return -1;
}

int gx_switch_index(const char *key) {
    for (int i = 0; i < GX_NSW; i++)
        if (strcmp(GX_SWITCHES[i].key, key) == 0) return i;
    return -1;
}

// --- mappings ---------------------------------------------------------------------------

float gx_freq_hz(float k) { return expmap(clamp01(k), 20.0f, 20000.0f); }
float gx_decay_ms(float k) { return expmap(clamp01(k), 10.0f, 10000.0f); }
float gx_attack_ms(float k) { return 10000.0f * k * k * k; }
float gx_q_val(float k) { return 0.1f * powf(100000.0f, clamp01(k)); }
float gx_eq_db(float k) { return 80.0f * (k - 0.5f); }
float gx_level_gain(float k) {
    float x = k / GX_LEVEL_0DB;
    return x * x;
}

float gx_mod_semis(const gx_patch_t *p) {
    float rng = p->s[S_MOD_MODE] == 0 ? 96.0f : 48.0f;
    return bip(p->k[K_MOD_AMT]) * rng;
}

float gx_mod_rate(const gx_patch_t *p) {
    float k = p->k[K_MOD_RATE];
    int m = p->s[S_MOD_MODE];
    if (m == 0) {
        if (k < 0.005f) return -1.0f;
        return 10.0f * powf(1248.0f, 1.0f - k);   // inf .. 10 ms
    }
    if (k < 0.005f) return 0.0f;
    if (m == 1) return 0.1f * powf(20000.0f, k);  // 0 .. 2 kHz
    return powf(20000.0f, k);                     // 0 .. 20 kHz
}

static void ms_txt(float ms, char *o, int n) {
    if (ms < 0)
        snprintf(o, n, "INF");
    else if (ms < 1000)
        snprintf(o, n, "%dms", (int)ms);
    else
        snprintf(o, n, "%.2fs", ms / 1000.0f);
}

static void hz_txt(float f, char *o, int n) {
    if (f < 1000)
        snprintf(o, n, "%dHz", (int)f);
    else
        snprintf(o, n, "%.1fk", f / 1000.0f);
}

void gx_fmt(int key, const gx_patch_t *p, char *o, int n) {
    float v = p->k[key];
    switch (key) {
    case K_EQ_FREQ:
    case K_OSC_FREQ:
    case K_N_FREQ:
        hz_txt(gx_freq_hz(v), o, n);
        return;
    case K_EQ_GAIN:
        snprintf(o, n, "%+.1fdB", gx_eq_db(v));
        return;
    case K_MIX: {
        int a = (int)((1.0f - v) * 100 + 0.5f);
        snprintf(o, n, "%d/%d", a, 100 - a);
        return;
    }
    case K_LEVEL:
        if (v < 0.003f)
            snprintf(o, n, "-inf");
        else
            snprintf(o, n, "%+.1fdB", 20.0f * log10f(gx_level_gain(v)));
        return;
    case K_PAN: {
        int b = (int)(bip(v) * 100 + (v >= 0.5f ? 0.5f : -0.5f));
        if (b == 0)
            snprintf(o, n, "C");
        else if (b < 0)
            snprintf(o, n, "L%d", -b);
        else
            snprintf(o, n, "R%d", b);
        return;
    }
    case K_MOD_AMT:
        snprintf(o, n, "%+.1fst", gx_mod_semis(p));
        return;
    case K_MOD_RATE: {
        float r = gx_mod_rate(p);
        if (p->s[S_MOD_MODE] == 0)
            ms_txt(r, o, n);
        else if (r >= 1)
            hz_txt(r, o, n);
        else
            snprintf(o, n, "%.2fHz", r);
        return;
    }
    case K_OSC_ATT:
    case K_N_ATT:
        ms_txt(gx_attack_ms(v), o, n);
        return;
    case K_OSC_DEC:
        ms_txt(gx_decay_ms(v), o, n);
        return;
    case K_N_DEC: {
        float d = gx_decay_ms(v);
        if (p->s[S_N_ENV] == 1) d = d * 2.0f / 3.0f;
        ms_txt(d, o, n);
        return;
    }
    case K_N_Q: {
        float q = gx_q_val(v);
        if (q < 10)
            snprintf(o, n, "%.2f", q);
        else
            snprintf(o, n, "%d", (int)q);
        return;
    }
    case K_VEL_OSC:
    case K_VEL_NOISE:
    case K_VEL_MOD:
        snprintf(o, n, "%d%%", (int)(v * 200 + 0.5f));
        return;
    default:
        snprintf(o, n, "%d", (int)(v * 100 + 0.5f));
        return;
    }
}

// --- patterns ------------------------------------------------------------------------------------

void gx_pat_clear(gx_pat_t *p) {
    memset(p, 0, sizeof *p);
    p->len = GX_NSTEP;
}

void gx_pat_from_strings(gx_pat_t *p, const char *const *rows, int nrows) {
    gx_pat_clear(p);
    int n = 0;
    for (int c = 0; c < nrows && c < GX_NCH; c++) {
        const char *r = rows[c];
        int len = (int)strlen(r);
        if (len > n) n = len;
        for (int i = 0; i < len && i < GX_NSTEP; i++) {
            uint16_t b = (uint16_t)(1u << i);
            char ch = r[i];
            if (strchr("xXfF", ch) && ch) p->trig[c] |= b;
            if (ch == 'X' || ch == 'F') p->acc[c] |= b;
            if (ch == 'f' || ch == 'F') p->fill[c] |= b;
        }
    }
    p->len = (uint8_t)(n < 1 ? GX_NSTEP : (n > GX_NSTEP ? GX_NSTEP : n));
}

bool gx_pat_empty(const gx_pat_t *p) {
    for (int c = 0; c < GX_NCH; c++)
        if (p->trig[c]) return false;
    return true;
}

void gx_chain_of(const gx_pat_t *pats, int i, int *first, int *count) {
    int a = i, b = i;
    while (a > 0 && pats[a - 1].chain) a--;
    while (b < GX_NPAT - 1 && pats[b].chain) b++;
    *first = a;
    *count = b - a + 1;
}

int gx_chain_next(const gx_pat_t *pats, int i) {
    int a, n;
    gx_chain_of(pats, i, &a, &n);
    if (n <= 1) return -1;
    return a + ((i - a) + 1) % n;
}

int gx_step_state(const gx_pat_t *p, int c, int i) {
    uint16_t b = (uint16_t)(1u << i);
    if (!(p->trig[c] & b)) return 0;
    return 1 | ((p->acc[c] & b) ? 2 : 0) | ((p->fill[c] & b) ? 4 : 0);
}

void gx_edit_step(gx_pat_t *p, int c, int i, char mode) {
    uint16_t b = (uint16_t)(1u << i);
    if (mode == 't') {
        if (p->trig[c] & b) {
            p->trig[c] &= (uint16_t)~b;
            p->acc[c] &= (uint16_t)~b;
            p->fill[c] &= (uint16_t)~b;
        } else {
            p->trig[c] |= b;
        }
        return;
    }
    uint16_t *m = mode == 'a' ? &p->acc[c] : &p->fill[c];
    if ((*m & b) && (p->trig[c] & b)) {
        *m &= (uint16_t)~b;
    } else {
        *m |= b;
        p->trig[c] |= b;
    }
}

static uint16_t rot(uint16_t mask, int n, int d) {
    uint16_t full = (uint16_t)((1u << n) - 1);
    uint16_t low = mask & full;
    if (d > 0)
        low = (uint16_t)(((low << 1) | (low >> (n - 1))) & full);
    else
        low = (uint16_t)(((low >> 1) | ((low & 1) << (n - 1))) & full);
    return (uint16_t)((mask & ~full) | low);
}

static uint16_t rev(uint16_t mask, int n) {
    uint16_t out = mask & (uint16_t)~((1u << n) - 1);
    for (int i = 0; i < n; i++)
        if (mask & (1u << i)) out |= (uint16_t)(1u << (n - 1 - i));
    return out;
}

void gx_pat_shift(gx_pat_t *p, int d, uint8_t chans) {
    for (int c = 0; c < GX_NCH; c++) {
        if (!(chans & (1u << c))) continue;
        p->trig[c] = rot(p->trig[c], p->len, d);
        p->acc[c] = rot(p->acc[c], p->len, d);
        p->fill[c] = rot(p->fill[c], p->len, d);
    }
}

void gx_pat_reverse(gx_pat_t *p, uint8_t chans) {
    for (int c = 0; c < GX_NCH; c++) {
        if (!(chans & (1u << c))) continue;
        p->trig[c] = rev(p->trig[c], p->len);
        p->acc[c] = rev(p->acc[c], p->len);
        p->fill[c] = rev(p->fill[c], p->len);
    }
}

// --- state / morph ------------------------------------------------------------------------------

void gx_state_init(gx_state_t *s) {
    memset(s, 0, sizeof *s);
    for (int c = 0; c < GX_NCH; c++) {
        gx_patch_default(&s->a[c]);
        gx_patch_default(&s->b[c]);
        snprintf(s->names[c], sizeof s->names[c], "CH %d", c + 1);
    }
    s->master = 1.0f;
    for (int i = 0; i < GX_NPAT; i++) gx_pat_clear(&s->pats[i]);
    s->rate = 2;
    s->tempo = 120.0f;
}

void gx_eff(const gx_state_t *s, int c, gx_patch_t *out) {
    float m = s->morph;
    const gx_patch_t *a = &s->a[c], *b = &s->b[c];
    if (m <= 0.0f) {
        *out = *a;
        return;
    }
    if (m >= 1.0f) {
        *out = *b;
        return;
    }
    for (int i = 0; i < GX_NKNOB; i++) out->k[i] = a->k[i] + (b->k[i] - a->k[i]) * m;
    for (int i = 0; i < GX_NSW; i++) out->s[i] = m < 0.5f ? a->s[i] : b->s[i];
}

void gx_set_knob(gx_state_t *s, int c, int key, float v) {
    v = clamp01(v);
    float m = s->morph;
    float *a = &s->a[c].k[key], *b = &s->b[c].k[key];
    if (m <= 0.0f) {
        *a = v;
    } else if (m >= 1.0f) {
        *b = v;
    } else {
        float cur = *a + (*b - *a) * m;
        float d = v - cur;
        float n = (1 - m) * (1 - m) + m * m;
        *a = clamp01(*a + d * (1 - m) / n);
        *b = clamp01(*b + d * m / n);
    }
}

void gx_set_switch(gx_state_t *s, int c, int sw, int v) {
    if (s->morph < 1.0f) s->a[c].s[sw] = (uint8_t)v;
    if (s->morph > 0.0f) s->b[c].s[sw] = (uint8_t)v;
}

void gx_morph_copy(gx_state_t *s, bool a_to_b) {
    for (int c = 0; c < GX_NCH; c++) {
        if (a_to_b)
            s->b[c] = s->a[c];
        else
            s->a[c] = s->b[c];
    }
}

// --- randomizer ----------------------------------------------------------------------------------

void gx_random_patch(gx_patch_t *p, gx_rnd_fn rnd, void *x) {
    gx_patch_default(p);
    float kind = rnd(x);
    p->s[S_WAVE] = rnd(x) < 0.6f ? 0 : (rnd(x) < 0.6f ? 1 : 2);
    p->s[S_MOD_MODE] = rnd(x) < 0.65f ? 0 : (rnd(x) < 0.6f ? 1 : 2);
    if (kind < 0.3f) {   // low drum
        p->k[K_OSC_FREQ] = 0.05f + rnd(x) * 0.2f;
        p->k[K_MIX] = rnd(x) * 0.3f;
        p->k[K_MOD_AMT] = 0.55f + rnd(x) * 0.2f;
        p->k[K_MOD_RATE] = 0.5f + rnd(x) * 0.3f;
        p->k[K_OSC_DEC] = 0.35f + rnd(x) * 0.3f;
    } else if (kind < 0.6f) {   // snare / clap / tom
        p->k[K_OSC_FREQ] = 0.2f + rnd(x) * 0.3f;
        p->k[K_MIX] = 0.4f + rnd(x) * 0.5f;
        p->s[S_N_MODE] = rnd(x) < 0.5f ? 1 : (rnd(x) < 0.5f ? 0 : 2);
        p->k[K_N_FREQ] = 0.4f + rnd(x) * 0.45f;
        p->s[S_N_ENV] = rnd(x) < 0.25f ? 2 : (rnd(x) < 0.4f ? 1 : 0);
        p->k[K_N_ATT] = p->s[S_N_ENV] == 2 ? rnd(x) * 0.15f : 0.0f;
        p->k[K_MOD_AMT] = 0.5f + rnd(x) * 0.15f;
        p->k[K_OSC_DEC] = 0.2f + rnd(x) * 0.3f;
        p->k[K_N_DEC] = 0.2f + rnd(x) * 0.35f;
    } else {   // metal / hats / perc
        p->k[K_OSC_FREQ] = 0.4f + rnd(x) * 0.5f;
        p->k[K_MIX] = 0.3f + rnd(x) * 0.7f;
        p->s[S_N_MODE] = rnd(x) < 0.6f ? 2 : 1;
        p->k[K_N_FREQ] = 0.6f + rnd(x) * 0.35f;
        p->k[K_N_Q] = 0.1f + rnd(x) * 0.35f;
        p->k[K_MOD_AMT] = rnd(x);
        p->k[K_MOD_RATE] = 0.3f + rnd(x) * 0.6f;
        p->k[K_OSC_DEC] = 0.05f + rnd(x) * 0.35f;
        p->k[K_N_DEC] = 0.05f + rnd(x) * 0.4f;
    }
    p->k[K_EQ_FREQ] = rnd(x);
    p->k[K_EQ_GAIN] = 0.5f + (rnd(x) - 0.5f) * 0.4f;
    p->k[K_DIST] = rnd(x) < 0.4f ? rnd(x) * 0.4f : 0.0f;
    p->k[K_N_Q] = clamp01(p->k[K_N_Q] + (rnd(x) - 0.5f) * 0.2f);
    p->k[K_PAN] = 0.5f + (rnd(x) - 0.5f) * 0.4f;
}

void gx_alter_patch(gx_patch_t *p, float amount, gx_rnd_fn rnd, void *x) {
    for (int i = 0; i < GX_NKNOB; i++) {
        if (i == K_LEVEL) continue;
        p->k[i] = clamp01(p->k[i] + (rnd(x) * 2.0f - 1.0f) * amount);
    }
}

void gx_random_pattern(gx_pat_t *p, int length, gx_rnd_fn rnd, void *x) {
    static const float dens[GX_NCH] = {0.35f, 0.18f, 0.55f, 0.15f, 0.12f, 0.12f, 0.08f, 0.08f};
    gx_pat_clear(p);
    p->len = (uint8_t)length;
    for (int c = 0; c < GX_NCH; c++) {
        uint16_t m = 0, a = 0, f = 0;
        for (int i = 0; i < length; i++) {
            float pr = dens[c];
            if (c == 0 && i % 4 == 0) pr = 0.8f;
            if (c == 1 && i % 8 == 4) pr = 0.85f;
            if (c == 2 && i % 2 == 0) pr = 0.7f;
            if (rnd(x) < pr) {
                m |= (uint16_t)(1u << i);
                if (rnd(x) < 0.25f) a |= (uint16_t)(1u << i);
                if (rnd(x) < 0.05f) f |= (uint16_t)(1u << i);
            }
        }
        p->trig[c] = m;
        p->acc[c] = a & m;
        p->fill[c] = f & m;
    }
}

void gx_pat_alter(gx_pat_t *p, uint8_t chans, gx_rnd_fn rnd, void *x) {
    int n = p->len;
    for (int c = 0; c < GX_NCH; c++) {
        if (!(chans & (1u << c))) continue;
        int hits[GX_NSTEP], nh = 0;
        for (int i = 0; i < n; i++)
            if (p->trig[c] & (1u << i)) hits[nh++] = i;
        if (!nh) continue;
        for (int k = 0; k < 1 + nh / 4; k++) {
            int i = hits[(int)(rnd(x) * nh) % nh];
            int j = (int)(rnd(x) * n) % n;
            uint16_t *keys[3] = {&p->trig[c], &p->acc[c], &p->fill[c]};
            for (int q = 0; q < 3; q++) {
                uint16_t v = *keys[q];
                int bi = (v >> i) & 1, bj = (v >> j) & 1;
                v &= (uint16_t)~((1u << i) | (1u << j));
                v |= (uint16_t)((bj << i) | (bi << j));
                *keys[q] = v;
            }
        }
    }
}

void gx_random_accents_fills(gx_pat_t *p, uint8_t chans, gx_rnd_fn rnd, void *x) {
    for (int c = 0; c < GX_NCH; c++) {
        if (!(chans & (1u << c))) continue;
        uint16_t a = 0, f = 0;
        for (int i = 0; i < p->len; i++) {
            if (!(p->trig[c] & (1u << i))) continue;
            if (rnd(x) < 0.3f) a |= (uint16_t)(1u << i);
            if (rnd(x) < 0.08f) f |= (uint16_t)(1u << i);
        }
        p->acc[c] = a;
        p->fill[c] = f;
    }
}

// --- factory kits ---------------------------------------------------------------------------------

const gx_patch_t *gx_drum_by_name(const char *name) {
    for (int i = 0; i < GX_NDRUMS; i++)
        if (strcmp(GX_DRUMS[i].name, name) == 0) return &GX_DRUMS[i].p;
    return NULL;
}

void gx_kit_state(const gx_kit_t *k, gx_state_t *s) {
    gx_state_init(s);
    for (int c = 0; c < GX_NCH; c++) {
        const char *dn = k->drums[c];
        const gx_patch_t *d = dn ? gx_drum_by_name(dn) : NULL;
        if (d) {
            s->a[c] = *d;
            snprintf(s->names[c], sizeof s->names[c], "%s", dn);
        } else {
            gx_patch_default(&s->a[c]);
            snprintf(s->names[c], sizeof s->names[c], "Init");
        }
        s->b[c] = s->a[c];
    }
    for (int i = 0; i < GX_NPAT; i++) {
        if (k->patterns[i])
            gx_pat_from_strings(&s->pats[i], k->patterns[i], GX_NCH);
        else
            gx_pat_clear(&s->pats[i]);
    }
    const char *ch = k->chain ? k->chain : "";
    for (int j = 0; ch[j] && ch[j + 1]; j++) {
        const char *p = strchr(GX_PAT_NAMES, ch[j]);
        if (!p) continue;
        int i = (int)(p - GX_PAT_NAMES);
        if (i < GX_NPAT - 1 && ch[j + 1] == GX_PAT_NAMES[i + 1]) s->pats[i].chain = 1;
    }
    s->rate = k->rate;
    s->fill_rate = k->fill_rate;
    s->swing = clamp01(k->swing / 100.0f);
    s->tempo = k->tempo;
    s->cur = 0;
}
