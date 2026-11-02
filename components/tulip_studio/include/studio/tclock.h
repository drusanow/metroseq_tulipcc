// tclock.h - the one master clock every Studio app plays to.
//
// One clock, one transport, for every app that is running: a sequencer
// (metroseq), a drum machine (GinTonix), a synth. It runs in the real-time
// task on core 1 and drives the apps two ways:
//
//  * SINKS (real-time apps, e.g. metroseq's engine): called at the exact
//    time of every master 1/16 pulse, and on start / stop / continue /
//    locate - the same calls metroseq's engine already took from its own
//    MIDI clock follower.
//
//  * The TIMELINE (audio apps, e.g. the drum machine): a small published
//    record - "song position P was at time T, a 1/16 lasts D us" - from
//    which an audio task works out exactly when any step, swung step,
//    triplet or flam falls, and renders it into the right sample ahead of
//    time. Nothing is pushed to it, so a busy audio block can never make a
//    pulse late, and a late block can never shift the grid.
//
// Internal clock: pulses are computed from an anchor (anchor + n * 1/16),
// never summed, so they cannot drift; tempo changes keep the song position
// continuous. PLAY starts the downbeat TC_START_LEAD_US ahead, so audio
// apps (which render a few ms ahead) can place it exactly.
//
// MIDI clock (24 PPQN): hard sync, soft tempo, as metroseq's follower - a
// pulse at every 6th clock's own arrival time, the 1/16 length between
// pulses from an average over the last beat. Start / Continue / Stop /
// Song Position and MMC Play / Stop / Locate are followed.
//
// As the master it can also send MIDI clock, Start / Stop and MMC.
//
// Pure C, no ESP-IDF: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Song position unit: 1/16 = 1680 units, divisible by 1..8 (fills) and by 3
// (triplet rates), so every step and flam lands on a whole unit.
#define TC_U16              1680
#define TC_START_LEAD_US    20000      // PLAY: the downbeat this far ahead
#define TC_MAX_CATCHUP      2          // pulses; more late than that = resync
#define TC_LOOKAHEAD_US     50         // pulses this close are fired together
#define TC_MIN_BPM_X100     2000
#define TC_MAX_BPM_X100     30000
#define TC_MAX_SINKS        4

#define TC_PPQN             24
#define TC_CLOCKS_PER_16TH  6
#define TC_TEMPO_WINDOW     24         // average over one beat of clocks
#define TC_CLOCK_TIMEOUT_US 500000     // no clock this long = clock lost

typedef struct {
    void *ctx;
    // downbeat (song position 0) at t
    void (*start)(void *ctx, int64_t t);
    // resume after a locate: the next pulse() is the located position
    void (*cont)(void *ctx, int64_t t);
    void (*stop)(void *ctx, int64_t t);
    // song position set while stopped: the next continue + pulse is pos16
    void (*locate)(void *ctx, int64_t t, uint32_t pos16);
    // every master 1/16 after start (pos16 >= 1) or continue
    void (*pulse)(void *ctx, int64_t t, uint32_t pos16);
    // the 1/16 length to time everything between pulses with; bpm_x100 is
    // the exact set tempo on the internal clock (0 when measured from MIDI)
    void (*tempo)(void *ctx, int64_t p16_us, int32_t bpm_x100);
} tclock_sink_t;

// The published timeline. Song position (units) at time t:
//   pos(t) = ref_pos + (t - ref_t) * TC_U16 / p16_us, at most max_pos.
typedef struct {
    uint32_t epoch;      // bumps on every start / continue / stop / locate
    uint8_t running;     // transport rolling (or its downbeat is pending)
    uint8_t armed;       // MIDI clock: waiting for the master's first clock
    uint8_t ext;         // following MIDI clock
    int64_t ref_pos;
    int64_t ref_t;
    int64_t p16_us;
    int64_t max_pos;     // MIDI clock: never run ahead of the master
    int32_t bpm_x100;    // set tempo, or the measured one on MIDI clock
} tclock_tl_t;

int64_t tclock_tl_pos(const tclock_tl_t *tl, int64_t t);    // units
int64_t tclock_tl_time(const tclock_tl_t *tl, int64_t pos);  // us

typedef void (*tclock_out_fn)(void *ctx, const uint8_t *bytes, int n);

typedef struct {
    // settings
    bool ext;              // follow MIDI clock (else internal)
    bool clock_out;        // internal: send MIDI clock + Start/Stop
    bool mmc_out;          // internal: send MMC Play / Stop + Locate 0
    int32_t bpm_x100;

    // transport
    bool running;
    bool start_pending;
    int64_t start_t;
    uint32_t pos16;        // song position of the last pulse
    int64_t pulse_t;       // its time
    // internal grid
    int64_t anchor_t;
    uint32_t anchor_n;
    int64_t next_pulse_t;
    // clock out
    uint32_t out_n;        // next MIDI clock to send (24 PPQN since start)
    int64_t next_out_t;

    // MIDI clock follower
    int64_t last_clock_t;
    int32_t iv[TC_TEMPO_WINDOW];
    int n_iv, idx_iv, outliers;
    int64_t sum_iv;
    bool armed;            // next clock is the downbeat
    bool at_start;
    uint32_t clocks;       // clocks since the downbeat
    uint32_t located;      // SPP / MMC position (1/16s)

    tclock_sink_t sinks[TC_MAX_SINKS];
    int nsinks;
    tclock_out_fn out;
    void *out_ctx;

    tclock_tl_t tl;
    uint32_t tl_ver;       // bumps whenever tl changes
    uint32_t resyncs;
} tclock_t;

void tclock_init(tclock_t *c, int32_t bpm_x100);
void tclock_add_sink(tclock_t *c, const tclock_sink_t *s);
void tclock_set_out(tclock_t *c, tclock_out_fn fn, void *ctx);

// Settings (any time; switching source stops the transport).
void tclock_set_ext(tclock_t *c, bool ext, int64_t now);
void tclock_set_clock_out(tclock_t *c, bool on, int64_t now);
void tclock_set_bpm(tclock_t *c, int32_t bpm_x100, int64_t now);

// Transport buttons. On MIDI clock, PLAY arms (start on the next clock) or
// disarms, STOP stops.
void tclock_play(tclock_t *c, int64_t now);
void tclock_stop(tclock_t *c, int64_t now);
void tclock_toggle(tclock_t *c, int64_t now);
// Start again from the top (internal), or re-arm on the next clock.
void tclock_restart(tclock_t *c, int64_t now);

// MIDI IN events (mp_type_t values from midi_parse.h). arg = SPP position.
// mmc = MMC command bytes (d[0] = command, LOCATE: d[1..4] = hh mm ss ff).
void tclock_midi(tclock_t *c, int type, uint16_t arg, const uint8_t *mmc, int64_t t);

// Fire everything due at or before now (internal pulses, clock out).
void tclock_advance(tclock_t *c, int64_t now);
// When tclock_advance() next has something to do (INT64_MAX if nothing).
int64_t tclock_next_due(const tclock_t *c);

// MIDI clock being received? Forgets the tempo after a timeout.
bool tclock_ext_alive(tclock_t *c, int64_t now);
int tclock_ext_bpm_x10(const tclock_t *c);   // 0 = unknown
// The 1/16 length in use now.
int64_t tclock_p16_us(const tclock_t *c);
// Transport rolling, or (MIDI clock) waiting for the master's clock.
static inline bool tclock_playing(const tclock_t *c) { return c->running; }
static inline bool tclock_armed(const tclock_t *c) { return c->ext && c->armed; }

static inline int64_t tclock_p16_from_bpm(int32_t bpm_x100) {
    return 1500000000LL / bpm_x100;
}

#ifdef __cplusplus
}
#endif
