// d9_model.c - Drumster9's tables and state (see d9_model.h).
#include "d9_model.h"

#include <string.h>

const char *const D9_ROLE[D9_LANES] = {"Kick", "Snare", "Clap", "CHat", "OHat", "Cymbal", "Tom"};
const char *const D9_SHORT[D9_LANES] = {"K", "SN", "CL", "CH", "OH", "CY", "TM"};
const uint8_t D9_GM_NOTE[D9_LANES] = {36, 38, 39, 42, 46, 49, 45};
const float D9_DEF_VOL[D9_LANES] = {0.90f, 0.75f, 0.55f, 0.45f, 0.40f, 0.45f, 0.50f};
const char D9_BANK_LETTERS[D9_BANKS + 1] = "ABCDEFGHIJ";
const char *const D9_FILTER_NAME[3] = {"OFF", "LPF", "HPF"};

#define NO D9_NO_SAMPLE
// The sketch's SAMPLE_KITS: Kick, Snare, Clap, CHat, OHat, Cymbal, Tom
const d9_kit_t D9_KIT[D9_KITS] = {
    {"TR-808", "tr808", {0, 12, 3, 9, 10, 18, 16}},
    {"TR-909", "tr909", {256, 268, 258, 260, 263, 259, 271}},
    {"Linn 9000", "linn9000", {273, 280, NO, 277, 278, 276, 282}},
    {"MR-12", "mr12", {285, 286, NO, 283, 284, NO, NO}},
    // a creative pick, not a documented mapping (the sketch's note)
    {"Tokyo Syn", "synthetics", {297, 296, 300, 309, 289, 299, 303}},
    // this bank's root notes are GM numbers: exact matches
    {"80s Power", "power", {312, 314, 315, 318, 322, 325, 321}},
    {"Percussion", "percussion", {350, 351, 375, 377, 376, 372, 368}},
};

// steps -> mask
#define B(s) (1u << (s))
#define B4(a, b, c, d) (B(a) | B(b) | B(c) | B(d))
#define QUARTERS B4(0, 8, 16, 24)
#define EIGHTHS (QUARTERS | B4(4, 12, 20, 28))

const d9_preset_t D9_PRESET[D9_NPRESETS] = {
    {"4 on Floor", {QUARTERS, B(8) | B(24), B(8) | B(24), EIGHTHS, B4(6, 14, 22, 30), B(0), 0}},
    {"Floor+Antic", {QUARTERS | B(28), B(8) | B(24), B(8) | B(24), EIGHTHS, B4(2, 10, 18, 26),
                     B(0) | B(16), B(30)}},
    {"Half Time", {B(0) | B(16), B(16), 0, EIGHTHS, B(12) | B(28), B(0), B(24)}},
    {"Syncopa", {B(0) | B(12) | B(24), B(8) | B(24), B(24),
                 B(0) | B(6) | B(12) | B(18) | B(24) | B(30), B(3) | B(9) | B(15) | B(21) | B(27),
                 B(0), B(20)}},
    {"Breakbeat", {B4(0, 10, 16, 22), B(8) | B(24) | B(26), B(8), B4(2, 6, 10, 14) | B4(18, 22, 26, 30),
                   B(4) | B(20), B(0), B(14) | B(30)}},
    {"Offbeat", {B4(4, 8, 20, 24) | B(28), B(8) | B(24), B(8) | B(24), EIGHTHS, B4(2, 10, 18, 26), 0,
                 B(30)}},
    {"Double", {EIGHTHS, B(8) | B(24), B(8) | B(24), 0x55555555u, 0, B(0), 0}},
    {"Sparse", {B(0) | B(20), B(16), 0, QUARTERS, B(28), 0, B(10)}},
};

void d9_bus_default(d9_bus_t *b) {
    *b = (d9_bus_t){.rev_level = 0, .rev_live = 0.75f, .rev_damp = 0.15f, .echo_level = 0,
                    .echo_ms = 250, .echo_fb = 0.3f, .echo_tone = 0, .filter = FT_OFF,
                    .freq = 4000, .reso = 1.0f, .drive = 1.0f};
}

static float cl(float v, float lo, float hi) { return v != v ? lo : v < lo ? lo : (v > hi ? hi : v); }

void d9_bus_clamp(d9_bus_t *b) {
    b->rev_level = cl(b->rev_level, 0, 1);
    b->rev_live = cl(b->rev_live, 0, 1);
    b->rev_damp = cl(b->rev_damp, 0, 1);
    b->echo_level = cl(b->echo_level, 0, 1);
    b->echo_ms = cl(b->echo_ms, 20, 500);
    b->echo_fb = cl(b->echo_fb, 0, 0.95f);
    b->echo_tone = cl(b->echo_tone, -1, 1);
    b->eq_l = cl(b->eq_l, -15, 15);
    b->eq_m = cl(b->eq_m, -15, 15);
    b->eq_h = cl(b->eq_h, -15, 15);
    if (b->filter < FT_OFF || b->filter > FT_HPF) b->filter = FT_OFF;
    b->freq = cl(b->freq, 100, 12000);
    b->reso = cl(b->reso, 0.5f, 8);
    b->drive = cl(b->drive, 0.2f, 4);
}

void d9_state_init(d9_state_t *s) {
    memset(s, 0, sizeof *s);
    for (int l = 0; l < D9_LANES; l++) {
        s->vol[l] = D9_DEF_VOL[l];
        s->kit_ovr[l] = -1;
    }
    for (int k = 0; k < D9_KITS; k++) memcpy(s->kit_map[k], D9_KIT[k].s, sizeof s->kit_map[k]);
    for (int b = 0; b < D9_BUSES; b++) d9_bus_default(&s->fx[b]);
    s->master = D9_MASTER_DEF;
    s->preset = -1;
}

void d9_state_clamp(d9_state_t *s) {
    for (int l = 0; l < D9_LANES; l++) {
        s->vol[l] = cl(s->vol[l], 0, 1);
        s->muted[l] = s->muted[l] ? 1 : 0;
        s->solo[l] = s->solo[l] ? 1 : 0;
        if (s->kit_ovr[l] < -1 || s->kit_ovr[l] >= D9_KITS) s->kit_ovr[l] = -1;
        if (s->bus[l] >= D9_BUSES) s->bus[l] = 0;
    }
    if (s->kit < 0 || s->kit >= D9_KITS) s->kit = 0;
    for (int b = 0; b < D9_BUSES; b++) d9_bus_clamp(&s->fx[b]);
    s->master = cl(s->master, 0, D9_MASTER_MAX);
    for (int i = 0; i < D9_BANKS; i++) {
        s->bank[i].used = s->bank[i].used ? 1 : 0;
        if (s->bank[i].kit < 0 || s->bank[i].kit >= D9_KITS) s->bank[i].kit = s->kit;
    }
    if (s->bank_active < 0 || s->bank_active >= D9_BANKS) s->bank_active = 0;
    if (s->preset < -1 || s->preset >= D9_NPRESETS) s->preset = -1;
}

bool d9_audible(const d9_state_t *s, int l) {
    if (s->muted[l]) return false;
    bool any = false;
    for (int i = 0; i < D9_LANES; i++) any |= s->solo[i];
    return !any || s->solo[l];
}

void d9_slot_store(d9_state_t *s) {
    d9_slot_t *k = &s->bank[s->bank_active];
    k->used = 1;
    k->kit = s->kit;
    memcpy(k->m, s->mask, sizeof k->m);
}

void d9_apply_preset(d9_state_t *s, int i) {
    i = ((i % D9_NPRESETS) + D9_NPRESETS) % D9_NPRESETS;
    memcpy(s->mask, D9_PRESET[i].m, sizeof s->mask);
    s->preset = (int8_t)i;
    d9_slot_store(s);
}

void d9_clear(d9_state_t *s) {
    memset(s->mask, 0, sizeof s->mask);
    s->preset = -1;
    d9_slot_store(s);
}

void d9_slot_switch(d9_state_t *s, int i) {
    if (i < 0 || i >= D9_BANKS) return;
    bool any = false;
    for (int l = 0; l < D9_LANES; l++) any |= s->mask[l] != 0;
    if (s->bank[s->bank_active].used || any) d9_slot_store(s);
    d9_slot_t *k = &s->bank[i];
    if (k->used) {
        memcpy(s->mask, k->m, sizeof s->mask);
        s->kit = k->kit;
    } else {
        memset(s->mask, 0, sizeof s->mask);
    }
    s->bank_active = (int8_t)i;
    s->preset = -1;
    d9_slot_store(s);
}

uint32_t d9_euclid(int n, int k) {
    if (k <= 0) return 0;
    if (k >= n) return n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
    uint32_t m = 0;
    int bucket = 0;
    for (int i = 0; i < n; i++) {
        bucket += k;
        if (bucket >= n) {
            bucket -= n;
            m |= 1u << i;
        }
    }
    return m;
}

static uint32_t rnd(uint32_t *r) {
    uint32_t x = *r ? *r : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *r = x;
}
static int rnd_int(uint32_t *r, int lo, int hi) { return lo + (int)(rnd(r) % (uint32_t)(hi - lo + 1)); }

static uint32_t rot(uint32_t m, int k) {
    k &= 31;
    return k ? (m << k) | (m >> (32 - k)) : m;
}

// every hit to the nearest multiple of grid
static uint32_t snap(uint32_t m, int grid) {
    uint32_t o = 0;
    for (int s = 0; s < D9_STEPS; s++)
        if (m & (1u << s)) o |= 1u << ((((s + grid / 2) / grid) * grid) % D9_STEPS);
    return o;
}

uint32_t d9_random_steps(int lane, uint32_t *r) {
    static const uint8_t RANGE[D9_LANES][2] = {{3, 6}, {1, 3}, {0, 3}, {4, 10}, {0, 4}, {0, 2}, {0, 4}};
    int hits = rnd_int(r, RANGE[lane][0], RANGE[lane][1]);
    if (hits <= 0) return 0;
    uint32_t m = rot(d9_euclid(D9_STEPS, hits), rnd_int(r, 0, D9_STEPS - 1));
    switch (lane) {
    case L_KICK:
        // the bar starts on a kick, all on the beat grid
        if (!(m & 1)) m = rot(m, D9_STEPS - __builtin_ctz(m));
        return snap(m, 4);
    case L_SNARE:
    case L_CLAP:
        m = snap(m, 4);
        if (!(m & (B(8) | B(24))) && rnd(r) % 10 < 6) m |= (rnd(r) & 1) ? B(8) : B(24);
        return m;
    case L_CHAT:
    case L_OHAT: return snap(m, 2);
    case L_CYMBAL: return snap(m, 8);
    default: {   // toms: the back half, if any land there
        m = snap(m, 2);
        uint32_t back = m & 0xFFFF0000u;
        return back ? back : m;
    }
    }
}
