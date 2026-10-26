// seq_clock.h - drive the sequencer engine from the Studio master clock.
//
// The engine runs in "pulses delivered" mode all the time: the master
// clock (internal or MIDI) calls in at the exact time of every 1/16, and
// the engine times everything in between (gate lengths, swing, ratchets,
// glides) from the 1/16 length the clock reports - the same arrangement
// metroseq used for MIDI clock, now used for its own tempo too, so it
// shares one grid with every other Studio app.
//
// Pure C: the host tests drive the same code.
#pragma once

#include <stdbool.h>

#include "seq_engine.h"
#include "studio/tclock.h"

typedef struct {
    seq_engine_t *e;
    seq_actions_t *acts;
    void (*lock)(void);                        // pattern lock (optional)
    void (*unlock)(void);
    void (*flush)(const seq_actions_t *acts);  // execute; NULL = leave in acts
    bool restart_pending;   // start the tracks over on the next pulse
} seq_clock_t;

// Fill in a sink whose ctx is c.
void seq_clock_sink(seq_clock_t *c, tclock_sink_t *out);

// Start the tracks over, in step with the grid: on the next pulse.
static inline void seq_clock_restart(seq_clock_t *c) { c->restart_pending = true; }
