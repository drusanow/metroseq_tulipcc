// seq_engine.h - deterministic, allocation-free sequencer engine.
//
// This is the SEQUENCER ENGINE section of metropolix_tulip.py ported to C,
// with one structural change: instead of an AMY clock callback plus a
// pile of tulip.defer() timers, everything that will happen is an event
// with an ABSOLUTE timestamp in microseconds in one priority queue:
//
//   * the master 1/16 pulse         (computed from an anchor, never summed,
//                                    so it cannot drift)
//   * swung / ratcheted gate-ons
//   * gate-offs, audio note-offs     (token-guarded, like the Python)
//   * glide (slide) pitch updates
//
// The caller (the high-priority sequencer task on core 1) asks for the
// time of the next event, sleeps on a hardware timer until then, and calls
// seq_engine_advance(now). The engine appends the resulting output actions
// (DAC gate/pitch writes, audio preview notes) to a list which the caller
// executes AFTER releasing the pattern lock - so no I/O ever happens while
// holding a lock, and the engine itself never blocks.
//
// No ESP-IDF dependencies: the identical code runs in the host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "seq_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- tunables ------------------------------------------------------------

// If the sequencer task was held off (it shouldn't be - see the timing
// design notes) and we come back more than this many master pulses late,
// resync to "now" instead of firing the backlog as a burst. Same policy as
// the Python MAX_CATCHUP.
#define SEQ_MAX_CATCHUP       2

// Events this close to now are processed in the same batch.
#define SEQ_LOOKAHEAD_US      50

// Gate minimums (the Python used 4 ms / 3 ms) and the minimum low time
// before a retrigger, so 100% gates and fast ratchets still produce a
// clean edge every module can see (a deliberate improvement over the
// Python, where a 100% gate retriggered with ~0 ms low time).
#define SEQ_MIN_GATE_US       4000
#define SEQ_MIN_RATCHET_US    3000
#define SEQ_MIN_GATE_GAP_US   1000

// Pitch update interval while a SLIDE is gliding (the Python used 8 fixed
// steps; this is a smooth ramp at 500 Hz, independent of glide length).
#define SEQ_GLIDE_STEP_US     2000

#define SEQ_MAX_EVENTS        128
#define SEQ_MAX_ACTIONS       96

// --- output actions ------------------------------------------------------

typedef enum {
    ACT_GATE = 0,     // out = physical output (DAC index), value = 0/1
    ACT_PITCH,        // out = physical output, value = pitch in cents (midi*100)
    ACT_AUDIO_ON,     // out = track, value = midi note
    ACT_AUDIO_OFF,    // out = track
} seq_act_type_t;

typedef struct {
    int64_t due_us;   // when the engine wanted this to happen (for stats)
    uint8_t type;
    uint8_t out;
    int32_t value;
} seq_action_t;

typedef struct {
    seq_action_t a[SEQ_MAX_ACTIONS];
    int n;
    uint32_t dropped;
} seq_actions_t;

static inline void seq_actions_clear(seq_actions_t *l) { l->n = 0; }

// --- internal event queue ---------------------------------------------------

typedef struct {
    int64_t  t;
    uint32_t seq;       // insertion order, for stable FIFO at equal times
    uint8_t  prio;      // lower runs first at equal t
    uint8_t  kind;
    uint8_t  track;     // logical track
    uint8_t  phys;      // physical output (gate lows)
    uint32_t token;
    int32_t  arg;       // gate length (us) for pulses
} seq_event_t;

// --- runtime state -------------------------------------------------------

typedef struct {
    int8_t   cur_stage;
    uint8_t  pulse_i;
    int8_t   pulse_left;
    int8_t   pdir;          // pendulum direction
    int32_t  accum_run;     // running accumulate transpose
    bool     play_this;
    bool     holding;       // a HOLD gate is open on this track
    int32_t  cur_cents;     // current target pitch (midi * 100)
    uint32_t next16;        // master pulse at which this track fires next
    uint32_t ticks;         // pulses fired since play (swing parity)
    int8_t   disp_stage;    // stage the playhead should show
    // glide
    uint32_t glide_tok;
    int32_t  glide_from, glide_to;
    int64_t  glide_t0;
    int32_t  glide_dur;
} track_rt_t;

typedef struct {
    int8_t   cur_stage;
    int8_t   pdir;
    uint32_t next16;
    int8_t   disp_stage;
} lane_rt_t;

typedef struct {
    // instrumentation (read by the UI's timing panel)
    uint32_t pulses;           // master pulses processed
    uint32_t resyncs;          // stalls that triggered a resync
    uint32_t queue_overflows;  // events dropped because the queue was full
} seq_stats_t;

typedef struct {
    const pattern_t *pat;      // shared, edited live by the UI

    bool     playing;
    uint32_t clk;              // master 1/16 counter since play
    uint16_t bpm;              // tempo the anchor was computed with
    int64_t  anchor_us;        // time of master pulse anchor_n
    uint32_t anchor_n;
    int64_t  next_pulse_us;    // time of master pulse clk+1

    track_rt_t tr[NUM_TRACKS];
    lane_rt_t  ln[NUM_MOD_LANES];

    // CV gate ownership (merge mode) + retrigger tokens, per physical out
    uint32_t gate_tok[NUM_TRACKS];
    uint32_t audio_tok[NUM_TRACKS];
    bool     shared_high;
    int8_t   shared_owner;     // -1 = none

    seq_event_t ev[SEQ_MAX_EVENTS];   // binary min-heap
    int      nev;
    uint32_t ev_seq;

    uint32_t rng;
    seq_stats_t stats;
} seq_engine_t;

// --- API -------------------------------------------------------------------

void seq_engine_init(seq_engine_t *e, const pattern_t *pat, uint32_t seed);

// PLAY: rewind and sound stage 0 on both tracks at `now`.
void seq_engine_start(seq_engine_t *e, int64_t now, seq_actions_t *out);

// STOP: drop all pending events, all gates low, audio off.
void seq_engine_stop(seq_engine_t *e, int64_t now, seq_actions_t *out);

// RST: rewind both tracks and clear accumulate (restarts if playing).
void seq_engine_reset(seq_engine_t *e, int64_t now, seq_actions_t *out);

// Merge mode toggled (or CV routing changed): everything low, ownership
// cleared, so no gate can be left stuck.
void seq_engine_outputs_changed(seq_engine_t *e, int64_t now, seq_actions_t *out);

// Process everything due at or before `now` (+lookahead).
void seq_engine_advance(seq_engine_t *e, int64_t now, seq_actions_t *out);

// Absolute time of the next thing that needs doing (INT64_MAX if idle).
int64_t seq_engine_next_due(const seq_engine_t *e);

// Microseconds per master 1/16 pulse at `bpm` (exact for integer bpm up to
// the int64 range; used for gate lengths etc.).
static inline int64_t seq_sixteenth_us(uint16_t bpm) { return 15000000LL / bpm; }

// Effective (modulated) values, exposed for the UI and the tests.
int seq_mod_offset(const seq_engine_t *e, int t, int param, int32_t scale_num);

#ifdef __cplusplus
}
#endif
