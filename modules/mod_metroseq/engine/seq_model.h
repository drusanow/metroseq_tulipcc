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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- configuration (same values as the Python sketch) -------------------

#define NUM_STAGES      8    // the main sequence is 8 stages (Metropolix)
#define NUM_TRACKS      2    // TRK 1 and TRK 2
#define NUM_MOD_LANES   8
#define MASTER_DIV      16   // master clock: 16 pulses per bar (1/16 grid)

#define NUM_PRESETS     10
#define MAX_PROJECTS    16
#define MAX_PROJECT_NAME_LEN 24

#define MIN_BPM         30
#define MAX_BPM         300
#define BPM_STEP        1
#define BPM_STEP_FAST   10

// A stage's pitch slider has PITCH_STEPS positions; the pattern's RANGE
// (1-4 octaves) says how many semitones the whole slider spans, so the
// same melody can be squeezed into one octave or spread over four.
#define PITCH_STEPS     48
#define MIN_RANGE_OCT   1
#define MAX_RANGE_OCT   4
#define DEF_RANGE_OCT   3    // the Python's fixed 36 semitones
#define CV_BASE_MIDI    36   // note that slider 0 names (C2)
#define CV_V0_MIDI      12   // MIDI note at 0V on the pitch CV (C0 = 0V)
// The pitch CV spans the GP8413's full 0-10 V: C0 (0 V) .. C10 (10 V).
#define CV_MIN_MIDI     CV_V0_MIDI
#define CV_MAX_MIDI     (CV_V0_MIDI + 120)
#define MIN_BASE_OCTAVE (-2)   // base C0: the lowest slider note sits on 0 V
#define MAX_BASE_OCTAVE 5      // base C7: the highest slider note sits on 10 V

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
    uint8_t pitch;      // slider position, 0..PITCH_STEPS (see stage_semi())
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
    int8_t   base_octave;   // MIN_BASE_OCTAVE..MAX_BASE_OCTAVE
    uint8_t  merge_cv;      // bool: TRK2 -> TRK1 shared CV out
    stage_t  stages[NUM_STAGES];
    track_cfg_t tracks[NUM_TRACKS];
    mod_lane_cfg_t mods[NUM_MOD_LANES];
    // --- version 2: appended, so a version 1 record is a prefix of this ---
    uint8_t  range_oct;                  // the sliders span 1..4 octaves
    uint8_t  lane_cc[NUM_MOD_LANES];     // MIDI CC out per lane: 0 = off, else the CC number
    uint8_t  lane_cc_ch[NUM_MOD_LANES];  // its MIDI channel, 1..16
} pattern_t;

#define PATTERN_VERSION 2
// Version 1 (before RANGE and the CC lanes): the first fields, padded to
// the struct's 2-byte alignment. Its pitches were semitones over 3 octaves.
#define PATTERN_V1_SIZE ((offsetof(pattern_t, range_oct) + 1) & ~(size_t)1)

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
// Keep a note inside the 0-10 V CV span by whole octaves, so an
// out-of-range note stays in key instead of piling up on 0 V or 10 V.
static inline int fold_to_cv_range(int midi) {
    if (midi < CV_MIN_MIDI) midi += 12 * ((CV_MIN_MIDI - midi + 11) / 12);
    if (midi > CV_MAX_MIDI) midi -= 12 * ((midi - CV_MAX_MIDI + 11) / 12);
    return midi;
}

static inline int eff_base_midi(const pattern_t *p) {
    return CV_BASE_MIDI + 12 * p->base_octave;
}

// A slider position as semitones above the base, over the pattern's RANGE
// (before the scale snaps it).
static inline int stage_semi(const pattern_t *p, int pos) {
    return (int)rdiv(pos * p->range_oct * 12, PITCH_STEPS);
}

// A lane value (-100..100) as a MIDI CC value (0..127).
static inline int lane_cc_value(int v) {
    return (int)rdiv((v + 100) * 127, 200);
}

// A version 1 record (zero-filled past PATTERN_V1_SIZE) -> version 2.
void pattern_upgrade_v1(pattern_t *p);

// Clamp every field into range (used after loading from storage, so a
// corrupt or old blob can never drive the engine out of bounds).
void pattern_sanitise(pattern_t *p);

// Power-on pattern (Python _load_default()).
void pattern_default(pattern_t *p);

#ifdef __cplusplus
}
#endif
