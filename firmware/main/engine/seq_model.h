// seq_model.h - the sequencer's data model (the part that gets saved).
//
// A direct port of the Stage / Track / ModLane classes and app-level
// settings from metropolix_tulip.py. Everything is a small fixed-size POD
// so a whole pattern can be memcpy'd (presets, clipboard, flash storage)
// and the engine never allocates.
//
// Float settings from the Python sketch are stored as integer percents
// (prob, swing, gate length, mod depth) or -100..+100 (mod values), which
// is exactly the resolution the UI steps in, and keeps the engine free of
// floating point.
//
// This file has no ESP-IDF dependencies: it builds on the host for tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- configuration (same values as the Python sketch) -------------------

#define NUM_STAGES      8    // the main sequence is 8 stages (Metropolix)
#define NUM_TRACKS      2    // TRK 1 and TRK 2
#define NUM_MOD_LANES   8
#define MASTER_DIV      16   // master clock: 16 pulses per bar (1/16 grid)

#define NUM_PRESETS     16
#define MAX_PROJECTS    16
#define MAX_PROJECT_NAME_LEN 24

#define MIN_BPM         30
#define MAX_BPM         300
#define BPM_STEP        1
#define BPM_STEP_FAST   10

#define PITCH_RANGE     36   // slider span in semitones (3 octaves)
#define CV_BASE_MIDI    36   // note that slider 0 names (C2)
#define CV_V0_MIDI      12   // MIDI note at 0V on the pitch CV (C0 = 0V)

// Gate types for a stage
enum { GATE_MULT = 0, GATE_SING, GATE_HOLD, GATE_REST, GATE_COUNT };

// Playback ORDER
enum { ORD_FWD = 0, ORD_REV, ORD_PEND, ORD_RAND, ORD_BROWN, ORD_COUNT };

#define NUM_DIVS 6
#define NUM_SCALES 11

// Mod routing destinations. Index 0 is OFF; the rest map to (track, param).
enum {
    MP_TRANSPOSE = 0, MP_OCTAVE, MP_PITCH, MP_PULSES, MP_GATE, MP_SWING,
    MP_COUNT
};
#define NUM_DESTS (1 + NUM_TRACKS * MP_COUNT)   // 13, same order as DESTS

extern const char *const GATE_NAMES[GATE_COUNT];
extern const char *const ORDER_NAMES[ORD_COUNT];
extern const uint8_t DIV_VALUES[NUM_DIVS];
extern const char *const DIV_NAMES[NUM_DIVS];
extern const char *const ROOT_NAMES[12];
extern const char *const DEST_NAMES[NUM_DESTS];

typedef struct {
    const char *name;
    uint8_t n;
    uint8_t ivals[12];
} scale_t;
extern const scale_t SCALES[NUM_SCALES];

// --- model ---------------------------------------------------------------

// One of the 8 shared stages. Both tracks read this same data.
typedef struct {
    uint8_t pitch;      // semitone offset, 0..PITCH_RANGE (slider)
    uint8_t pulses;     // PULSE COUNT 1..8
    uint8_t gate;       // GATE_* mode
    uint8_t slide;      // bool: glide the pitch into this stage
    uint8_t skip;       // bool: skip this stage in playback
    uint8_t ratchet;    // 1..8 hits per gated pulse
    uint8_t prob;       // 0..100 % chance the stage plays
    int8_t  accum;      // -24..24 semitones added per pass
} stage_t;

// A playback interpretation of the shared sequence.
typedef struct {
    uint8_t order;      // ORD_*
    uint8_t length;     // 1..NUM_STAGES
    uint8_t div_i;      // index into DIV_VALUES
    uint8_t swing;      // 0..60 %
    uint8_t gate_pct;   // 5..100 % gate length
    int8_t  transpose;  // -24..24
    int8_t  octave;     // -3..3
    uint8_t invert;     // bool
    uint8_t mute_audio; // bool (audio preview only; CV still outputs)
} track_cfg_t;

// One modulation lane.
typedef struct {
    int8_t  values[NUM_STAGES];  // -100..100 per stage (bipolar)
    uint8_t order;
    uint8_t length;
    uint8_t div_i;
    uint8_t depth;               // 0..100 %
    uint8_t dest;                // index into DEST_NAMES (0 = OFF)
} mod_lane_cfg_t;

// The whole saveable state (Python _snapshot()).
typedef struct {
    uint16_t bpm;
    uint8_t  root;          // 0..11
    uint8_t  scale_idx;     // 0..NUM_SCALES-1
    int8_t   base_octave;   // -3..5
    uint8_t  merge_cv;      // bool: TRK2 -> TRK1 shared CV out
    stage_t  stages[NUM_STAGES];
    track_cfg_t tracks[NUM_TRACKS];
    mod_lane_cfg_t mods[NUM_MOD_LANES];
} pattern_t;

#define PATTERN_VERSION 1

// --- helpers -------------------------------------------------------------

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Python-style floor modulo / division (results match Python for b > 0).
static inline int pmod(int a, int b) {
    int r = a % b;
    return r < 0 ? r + b : r;
}
static inline int pfloordiv(int a, int b) {
    return (a - pmod(a, b)) / b;
}

// Round a / b to nearest, halves away from zero (b > 0).
static inline int32_t rdiv(int32_t a, int32_t b) {
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

static inline uint8_t dest_track(uint8_t dest) {
    return dest == 0 ? 0xff : (uint8_t)((dest - 1) / MP_COUNT);
}
static inline uint8_t dest_param(uint8_t dest) {
    return dest == 0 ? 0xff : (uint8_t)((dest - 1) % MP_COUNT);
}

// Snap a raw semitone offset to the nearest note of the scale relative to
// root, preserving octave. Returns a semitone offset (Python quantise()).
int quantise(int semi, const scale_t *scale, int root);

// "C#3" style note name into buf (>= 6 bytes).
void note_name(int midi, char *buf, int len);

// The MIDI note a stage pitch of 0 maps to, incl. the global base octave.
static inline int eff_base_midi(const pattern_t *p) {
    return CV_BASE_MIDI + 12 * p->base_octave;
}

// Clamp every field into range (used after loading from storage, so a
// corrupt or old blob can never drive the engine out of bounds).
void pattern_sanitise(pattern_t *p);

// Power-on pattern (Python _load_default()).
void pattern_default(pattern_t *p);

#ifdef __cplusplus
}
#endif
