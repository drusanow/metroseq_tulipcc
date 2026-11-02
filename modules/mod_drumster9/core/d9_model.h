// d9_model.h - Drumster9's data: the 7 drum lanes, the kits, the pattern
// presets and bank, the 4 FX buses. A port of the tables and state in the
// Python sketch (reference/drumster9/drumster9v6.py).
//
// A pattern is 32 steps of 1/32 notes - one bar - per lane, kept as a
// 32-bit mask (bit k = step k). Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "d9_samples.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D9_LANES     7
#define D9_STEPS     32
#define D9_BUSES     4
#define D9_BANKS     10     // pattern slots A..J
#define D9_KITS      7
#define D9_NPRESETS  8

enum { L_KICK, L_SNARE, L_CLAP, L_CHAT, L_OHAT, L_CYMBAL, L_TOM };

extern const char *const D9_ROLE[D9_LANES];    // "Kick" ... (the keys in files)
extern const char *const D9_SHORT[D9_LANES];   // "K", "SN" ... (the lane labels)
extern const uint8_t D9_GM_NOTE[D9_LANES];     // 36, 38, 39, 42, 46, 49, 45
extern const float D9_DEF_VOL[D9_LANES];
extern const char D9_BANK_LETTERS[D9_BANKS + 1];

typedef struct {
    const char *name;
    const char *bank;            // its own sample bank (the KITS page's pool)
    int16_t s[D9_LANES];         // preset per role, D9_NO_SAMPLE = none
} d9_kit_t;
extern const d9_kit_t D9_KIT[D9_KITS];

typedef struct {
    const char *name;
    uint32_t m[D9_LANES];
} d9_preset_t;
extern const d9_preset_t D9_PRESET[D9_NPRESETS];

// --- an FX bus (the Python's BUS_FX_DEFAULTS) --------------------------------------

enum { FT_OFF, FT_LPF, FT_HPF };
extern const char *const D9_FILTER_NAME[3];

typedef struct {
    float rev_level, rev_live, rev_damp;          // 0..1
    float echo_level, echo_ms, echo_fb, echo_tone;  // 0..1, 20..500 ms, 0..0.95, -1..1
    float eq_l, eq_m, eq_h;                       // dB, -15..15
    int filter;                                   // FT_*
    float freq, reso;                             // Hz 100..12000, 0.5..8
    float drive;                                  // gain, 0.2..4
} d9_bus_t;

void d9_bus_default(d9_bus_t *b);
void d9_bus_clamp(d9_bus_t *b);

// --- the whole state ------------------------------------------------------------------

// A pattern-bank slot: the grid and the kit (what the Python's
// snapshot_pattern() kept, less the tempo, which is the Studio's).
typedef struct {
    uint8_t used;
    int8_t kit;
    uint32_t m[D9_LANES];
} d9_slot_t;

typedef struct {
    uint32_t mask[D9_LANES];          // the live grid
    float vol[D9_LANES];              // 0..1
    uint8_t muted[D9_LANES], solo[D9_LANES];
    int8_t kit_ovr[D9_LANES];         // a lane's own kit, -1 = the global one
    uint8_t bus[D9_LANES];
    int8_t kit;                       // the global kit
    // the sample each kit plays for each role: the defaults, and what the
    // KITS page changed (the Python's kit_overrides)
    int16_t kit_map[D9_KITS][D9_LANES];
    d9_bus_t fx[D9_BUSES];
    float master;                     // the Python's units: 15 = the default level
    d9_slot_t bank[D9_BANKS];
    int8_t bank_active;
    int8_t preset;                    // last preset picked (shown), -1 none
} d9_state_t;

#define D9_MASTER_DEF 15.0f
#define D9_MASTER_MAX 30.0f

void d9_state_init(d9_state_t *s);
// After loading: everything back in range.
void d9_state_clamp(d9_state_t *s);

static inline int d9_lane_kit(const d9_state_t *s, int l) {
    return s->kit_ovr[l] >= 0 ? s->kit_ovr[l] : s->kit;
}
static inline int d9_lane_preset(const d9_state_t *s, int l) {
    return s->kit_map[d9_lane_kit(s, l)][l];
}
// Not muted, and not soloed out.
bool d9_audible(const d9_state_t *s, int l);

void d9_apply_preset(d9_state_t *s, int i);
void d9_clear(d9_state_t *s);

// The active slot follows the grid (the Python's _auto_save_slot()).
void d9_slot_store(d9_state_t *s);
// Switch to slot i: the current grid is kept in its slot first. An empty
// slot starts empty (and takes the current kit).
void d9_slot_switch(d9_state_t *s, int i);

// Euclidean k of n, as a mask.
uint32_t d9_euclid(int n, int k);
// The Python's random_steps(): a role-shaped Euclidean pattern. rng is a
// running seed.
uint32_t d9_random_steps(int lane, uint32_t *rng);

#ifdef __cplusplus
}
#endif
