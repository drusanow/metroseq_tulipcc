// gx_seq.h - the GinTonix pattern player, on the Studio master clock.
//
// The Python kept one pass of the pattern in AMY's sequencer as repeating
// events and patched them as you edited. Here nothing is scheduled ahead
// at all: every audio block asks "which hits fall between the song
// position where the last block ended and the one where this block ends?"
// and works them out from the pattern as it is right now, from the
// master clock's timeline (studio/tclock.h). So:
//
//  * edits, mutes, swing, rate and length changes are heard on the very
//    next step, with nothing to rewrite or get out of step;
//  * the drums can't drift from the clock, or from metroseq: they read the
//    same grid, and each hit lands on the exact sample of its position;
//  * passes are anchored to song position 0 (MIDI Start / Song Position),
//    like the Python; chains and pattern changes happen at the end of a
//    pass.
//
// Positions are in master-clock units (TC_U16 = 1680 per 1/16), fine
// enough that swing, triplet rates and 2..8-hit fills all land on whole
// units - the Python rounded them to 1/48 of a beat (10 ms at 120 BPM).
//
// Pure C: tested on the host against the Python's layout().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gx_model.h"
#include "studio/tclock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GX_UNITS_PER_TICK (TC_U16 / 12)   // 48 PPQ tick = 140 units

typedef struct {
    int64_t pos;      // song position (units)
    uint8_t ch;
    uint8_t step;     // step of the pattern pass
    float vel;        // 1.0 accent, 0.5 normal, fills fade
} gx_hit_t;

// Live controls (written by the UI / MIDI, read by the player).
typedef struct {
    volatile uint8_t mute;        // channel bit mask
    volatile uint8_t hold_mute;   // MIDI keys C2..G2 held
    volatile uint8_t choke;       // channels with CHOKE on
    volatile int8_t next;         // switch to this pattern at the end of the pass, -1 none
} gx_live_t;

typedef struct {
    uint32_t epoch;     // the clock's epoch we are following
    bool active;
    int64_t cursor;     // everything up to here has been played
    int64_t pass_start;
    uint32_t pass_no;   // swing parity for odd lengths
    int pat;            // pattern playing
} gx_seq_t;

void gx_seq_init(gx_seq_t *q);

// The hits with song position in (cursor, pos_end], in time order (max
// `max`); advances the cursor. Restarts itself when the clock's epoch
// changes; returns 0 while the transport is stopped.
int gx_seq_run(gx_seq_t *q, const gx_state_t *st, gx_live_t *live, const tclock_tl_t *tl,
               int64_t pos_end, gx_hit_t *out, int max);

// Step of the playing pass that is sounding at song position pos (-1: none).
int gx_seq_step_at(const gx_seq_t *q, const gx_state_t *st, int64_t pos);

// Step length (units) of a rate.
static inline int64_t gx_step_units(int rate) {
    return (int64_t)GX_RATE_TICKS[rate] * GX_UNITS_PER_TICK;
}

#ifdef __cplusplus
}
#endif
