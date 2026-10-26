// midi_clock.h - follow an external MIDI clock (24 PPQN) and transport.
//
// Hard sync, soft tempo: every 6th MIDI clock (one 1/16) fires a master
// pulse at the clock's own arrival timestamp, so the sequence is
// phase-locked to the master with no added lag or drift. Everything that
// happens BETWEEN pulses (gate length, swing, ratchets, glides) is timed
// from a tempo estimate averaged over the last beat of clocks, so a jittery
// clock source doesn't make gate lengths jitter too.
//
// Pure C, no ESP-IDF: the same code is tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "seq_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_PPQN            24
#define MIDI_CLOCKS_PER_16TH 6
#define MIDI_TEMPO_WINDOW    24        // average over one beat of clocks
#define MIDI_CLOCK_TIMEOUT_US 500000   // no clock this long = clock lost

// --- byte parser (runs in the MIDI RX task) --------------------------------

typedef enum {
    MIDI_EV_NONE = 0,
    MIDI_EV_CLOCK,      // 0xF8
    MIDI_EV_START,      // 0xFA
    MIDI_EV_CONTINUE,   // 0xFB
    MIDI_EV_STOP,       // 0xFC
    MIDI_EV_SPP,        // 0xF2 lsb msb: position in 1/16ths ("MIDI beats")
} midi_ev_type_t;

typedef struct {
    uint8_t status;     // current system-common status awaiting data, or 0
    uint8_t need;       // data bytes still expected
    uint8_t data[2];
    uint8_t have;
    bool in_sysex;
} midi_parser_t;

typedef struct {
    uint8_t type;       // midi_ev_type_t
    uint16_t arg;       // SPP position
} midi_ev_t;

void midi_parser_init(midi_parser_t *p);
// Feed one byte; returns an event (type NONE for everything we ignore).
// Real-time bytes are recognised anywhere, even inside other messages.
midi_ev_t midi_parse_byte(midi_parser_t *p, uint8_t b);

// --- clock follower (runs in the sequencer task) -----------------------------

typedef struct {
    // tempo estimate
    int64_t last_clock_us;
    int32_t iv[MIDI_TEMPO_WINDOW];
    int n, idx;
    int64_t sum;
    int outliers;
    // transport
    bool armed;          // START seen (or PLAY pressed): next clock = downbeat
    bool at_start;       // position is the top of the song
    uint32_t clocks;     // MIDI clocks since the downbeat
} midi_clock_t;

void midi_clock_init(midi_clock_t *m);

// Handle one parsed event that arrived at time t (us, sequencer clock).
void midi_clock_event(midi_clock_t *m, seq_engine_t *e, const midi_ev_t *ev, int64_t t,
                      seq_actions_t *out);

// Local transport buttons while following MIDI clock:
// PLAY arms a start on the next clock, STOP stops, RESET re-arms.
void midi_clock_arm(midi_clock_t *m);
void midi_clock_disarm(midi_clock_t *m);

// Call periodically; forgets the tempo when the clock has gone away.
// Returns true while a clock is being received.
bool midi_clock_alive(midi_clock_t *m, int64_t now);

// Measured tempo in 1/16 us (0 = unknown) and BPM x10 (0 = unknown).
int64_t midi_clock_p16_us(const midi_clock_t *m);
int midi_clock_bpm_x10(const midi_clock_t *m);

#ifdef __cplusplus
}
#endif
