// d9_seq.h - Drumster9's pattern player, on the Studio master clock.
//
// The Python handed one bar of events to AMY's sequencer and patched them
// (with workarounds for its tag limit and a PCM retrigger bug). Here, as in
// GinTonix, nothing is scheduled ahead: each audio block asks which 1/32
// steps fall between where the last block ended and where this one ends,
// on the master clock's timeline, and reads the grid as it is right now.
// Every hit lands on the exact sample of its step, and an edit is heard on
// the very next step.
//
// Bars are anchored to song position 0, so the pattern lines up with
// metroseq and GinTonix. A queued pattern-bank slot switches on the
// downbeat, before that step's hits are worked out.
//
// Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "d9_model.h"
#include "studio/tclock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D9_STEP_UNITS (TC_U16 / 2)                 // a 1/32 step
#define D9_BAR_UNITS  (D9_STEP_UNITS * D9_STEPS)   // one bar

typedef struct {
    int64_t pos;     // song position (units)
    uint8_t lane;
    uint8_t step;
} d9_hit_t;

typedef struct {
    uint32_t epoch;
    bool active;
    int64_t cursor;     // everything up to here has been played
} d9_seq_t;

void d9_seq_init(d9_seq_t *q);

// The hits in (cursor, pos_end], in time order (max `max`); advances the
// cursor. On a downbeat, a queued slot (*bank_next >= 0) is switched in
// first (d9_slot_switch) and *bank_next set to -1; *switched counts those.
// Muted / soloed-out lanes, and lanes whose kit has no sample, don't hit.
// Restarts itself when the clock's epoch changes; 0 while stopped.
int d9_seq_run(d9_seq_t *q, d9_state_t *st, const tclock_tl_t *tl, int64_t pos_end,
               volatile int8_t *bank_next, uint32_t *switched, d9_hit_t *out, int max);

// The step sounding at song position pos (-1: stopped).
int d9_seq_step_at(const d9_seq_t *q, int64_t pos);

#ifdef __cplusplus
}
#endif
