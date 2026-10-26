// app.h - state shared between the UI (core 0) and the sequencer (core 1).
//
// Ownership rules - these are what keep the sequence from ever stalling:
//
//  * g_pat (the pattern) is written by the UI and read by the engine.
//    Single-field edits are plain byte writes (atomic on Xtensa). Anything
//    that changes many fields at once (preset load, paste) goes through
//    pat_lock()/pat_unlock(), a spinlock the engine also holds while it
//    computes - for microseconds, never across I/O.
//
//  * The engine's runtime state, the I2C bus, the DACs and the touch
//    controller belong to the sequencer task alone. The UI talks to it only
//    through seq_cmd() (a non-blocking queue) and reads back g_status.
//
//  * The UI never waits on the sequencer and the sequencer never waits on
//    the UI. A slow redraw can delay a playhead LED, never a gate.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "engine/seq_model.h"

#ifdef __cplusplus
extern "C" {
#endif

extern pattern_t g_pat;

// --- device settings (not part of a pattern; saved in NVS) -----------------

enum { CLOCK_INT = 0, CLOCK_MIDI = 1 };

// (Touch calibration and brightness are device-wide: tulip/settings.h.)
typedef struct {
    uint8_t clock_src;        // CLOCK_INT or CLOCK_MIDI (follow MIDI IN clock)
} settings_t;

extern settings_t g_settings;

void pat_lock(void);
void pat_unlock(void);

// Copy a whole pattern in/out atomically with respect to the engine.
void pat_store(const pattern_t *src);
void pat_load(pattern_t *dst);

// --- sequencer status, published by the sequencer task -------------------

#define LATE_BUCKETS 7   // <50us <100 <250 <500 <1ms <2ms >=2ms

typedef struct {
    volatile bool playing;
    volatile int8_t disp_stage[NUM_TRACKS];
    volatile int8_t lane_disp[NUM_MOD_LANES];

    volatile bool cv_enabled;
    volatile bool dac_present[NUM_TRACKS];
    volatile bool cv_ready;          // soft start finished
    volatile bool touch_ready;       // GT911 found and polling

    // MIDI clock (when g_settings.clock_src == CLOCK_MIDI)
    volatile bool ext_armed;         // waiting for the next clock to start
    volatile int ext_bpm_x10;        // measured tempo, 0 = no clock

    // timing instrumentation: how late each output batch started vs the
    // time the engine scheduled it for
    volatile uint32_t late_hist[LATE_BUCKETS];
    volatile uint32_t late_max_us;
    volatile uint32_t late_count;
    volatile uint64_t late_sum_us;
    volatile uint32_t resyncs;
    volatile uint32_t late_gates_dropped;   // hits skipped after a stall
    volatile uint32_t i2c_errors;
    volatile uint32_t touch_polls;
    volatile uint32_t touch_deferred;   // polls postponed to keep a gate on time
    volatile uint32_t touch_forced;     // polls forced after starvation

    volatile uint32_t info_seq;         // bumps when info changes
    char info[40];
} seq_status_t;

extern seq_status_t g_status;

// Post a short message to the header info line (any task).
void status_info(const char *msg);
void status_info_get(char *buf, int len, uint32_t *seq);

// --- commands to the sequencer task -----------------------------------------

typedef enum {
    CMD_TOGGLE_PLAY = 0,
    CMD_RESET,
    CMD_TEMPO,                // g_pat.bpm changed
    CMD_RESTART_IF_PLAYING,   // pattern replaced (preset / paste / project)
    CMD_OUTPUTS_CHANGED,      // merge mode toggled
    CMD_CV_TOGGLE,
    CMD_CV_RESCAN,
    CMD_STATS_RESET,
    CMD_CLOCK_CHANGED,        // g_settings.clock_src changed
} seq_cmd_t;

// Non-blocking; returns false only if the command queue is full.
bool seq_cmd(seq_cmd_t cmd);

#ifdef __cplusplus
}
#endif
