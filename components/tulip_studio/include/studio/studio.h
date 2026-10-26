// studio.h - Tulip Studio: several apps running at once on one clock.
//
// A Studio image is built from one or more MODULES (apps compiled in
// together). They all run at the same time and play to one master clock
// (tclock.h). You switch between their screens with the blue square in the
// top-right corner - like the original Tulip - and they keep playing while
// they're not on screen. The red square goes to the Studio home page (in a
// single-app image, back to the launcher).
//
// Every module carries a ROLE tag that tells the runtime what to give it:
//
//   SEQUENCER  plays to the clock (follows MIDI clock / Start / Stop) and
//              produces notes: CV out, and tracks you can route to a synth
//              on the Tulip or to MIDI OUT. (metroseq)
//   DRUMS      a drum machine / sampler: plays in time with the clock and
//              transport, renders audio, no CV. (GinTonix)
//   SYNTH      plays notes - from MIDI IN, or routed from a sequencer
//              track. (Sloe)
//
// Threads, and which module hooks run where:
//
//   core 1  rt     prio 23  clock, sequencer engines (rt_*), CV + touch on
//                           the I2C bus, MIDI OUT        <- real time
//   core 1  midi   prio 22  MIDI IN bytes, timestamped -> clock / midi()
//   core 1  audio  prio 21  render(): every module's audio, mixed -> I2S
//   core 0  lvgl   prio 4   ui_*(): screens, touch
//
// A module never blocks in rt_*, render() or midi(); they hand work to each
// other through the lock-free mechanisms here (commands, the published
// timeline, note events) - never by waiting.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "studio/tclock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STUDIO_MAX_MODULES  6
#define STUDIO_SR           44100
#define STUDIO_BLOCK        48        // frames per render() call (1.09 ms)

// --- roles / tags -----------------------------------------------------------------

typedef enum {
    TMOD_SEQUENCER = 0,
    TMOD_DRUMS,
    TMOD_SYNTH,
} tmod_role_t;

#define TMOD_F_CV        (1u << 0)   // drives the CV DACs (one module only)
#define TMOD_F_AUDIO     (1u << 1)   // renders audio
#define TMOD_F_CLOCKED   (1u << 2)   // plays to the master clock / transport
#define TMOD_F_NOTES_IN  (1u << 3)   // takes routed notes (synths)

const char *studio_role_name(tmod_role_t r);

// --- notes (routing) ------------------------------------------------------------------

enum { TN_ON = 1, TN_OFF, TN_ALL_OFF };

typedef struct {
    uint8_t type;      // TN_*
    uint8_t src;       // who sent it (route source id, or 0x80 | MIDI channel)
    uint8_t key;       // voice key: the track number, or the MIDI note
    uint8_t vel;       // 1..127
    int32_t cents;     // pitch, MIDI note * 100 (+ fraction)
    int32_t glide_us;  // > 0: slide from the previous pitch of this src
    int64_t t;         // when it was due (clock us)
} tnote_t;

// --- audio ------------------------------------------------------------------------------

typedef struct {
    int n;             // frames (STUDIO_BLOCK)
    int64_t t0;        // when frame 0 reaches the DAC (clock us)
    uint64_t frame;    // running frame count of frame 0
} studio_block_t;

// Frame within the block at which time t (clock us) sounds; may be < 0
// (late) or >= n (a later block).
static inline int studio_frame_at(const studio_block_t *b, int64_t t) {
    int64_t d = t - b->t0;
    return (int)((d >= 0 ? d * STUDIO_SR + 500000 : d * STUDIO_SR - 500000) / 1000000);
}
static inline int64_t studio_time_at(const studio_block_t *b, int frame) {
    return b->t0 + ((int64_t)frame * 1000000) / STUDIO_SR;
}

// --- the module ---------------------------------------------------------------------------

typedef struct tmod {
    const char *name;        // short id: files, settings ("gintonix")
    const char *title;       // what the tiles show ("GINTONIX")
    const char *blurb;       // one line for the Studio home page
    uint8_t role;            // tmod_role_t
    uint8_t flags;           // TMOD_F_*
    uint8_t n_note_outs;     // sequencer: routable tracks
    const char *const *note_out_names;   // "TRK 1", ...

    // boot, before any task starts (load settings, allocate)
    void (*init)(void);

    // --- real-time task (core 1) ---
    void (*rt_init)(void);                       // I2C bus is up
    tclock_sink_t sink;                          // clock calls (all optional)
    int64_t (*rt_next_due)(void);                // INT64_MAX = nothing
    void (*rt_advance)(int64_t now);             // do what's due
    void (*rt_cmd)(uint8_t cmd, int32_t arg, int64_t now);   // studio_cmd()
    void (*rt_poll)(int64_t now);                // every pass (publish status)
    void (*rt_enable)(bool on, int64_t now);     // opened / closed in Studio

    // --- audio task (core 1) ---
    // ADD this block into l / r (n = STUDIO_BLOCK frames).
    void (*render)(float *l, float *r, const studio_block_t *b);
    // A routed note (called from the rt or midi task: queue it, don't play).
    void (*note)(const tnote_t *n);

    // --- MIDI task (core 1) ---
    // Channel messages from MIDI IN (status with channel, data). Queue it.
    void (*midi)(const uint8_t *m, int len, int64_t t);

    // --- UI task (core 0) ---
    // what it takes from MIDI IN, for the home page ("notes on ch 1")
    void (*midi_desc)(char *buf, int len);
    void (*ui_create)(lv_obj_t *scr);            // build on this screen
    void (*ui_poll)(bool visible);               // every UI pass
    void (*ui_show)(bool shown);                 // screen shown / hidden
    // Before leaving to the launcher: true while it still has work to
    // finish (a flash write); the runtime waits for it.
    bool (*busy)(void);
} tmod_t;

// Start everything: the module list, in switcher order. Never returns
// control to the caller's flow (app_main may return).
void studio_run(const tmod_t *const *mods, int n);

int studio_module_count(void);
const tmod_t *studio_module(int i);
int studio_module_index(const tmod_t *m);
bool studio_module_enabled(int i);
bool studio_multi(void);          // more than one module: Studio home exists

// --- transport / clock (any task; non-blocking) --------------------------------------

void studio_play(void);
void studio_stop(void);
void studio_toggle_play(void);
void studio_restart(void);        // from the top (internal) / re-arm (MIDI)

enum { CLOCK_SRC_INT = 0, CLOCK_SRC_MIDI = 1 };
void studio_set_clock_src(int src);
int studio_clock_src(void);
void studio_set_clock_out(bool on);
bool studio_clock_out(void);
void studio_set_mmc(bool on);     // MMC: in when following MIDI, out on internal
bool studio_mmc(void);

void studio_set_bpm_x100(int32_t bpm_x100);
int32_t studio_bpm_x100(void);    // the set tempo

typedef struct {
    bool playing;     // transport rolling
    bool armed;       // MIDI clock: waiting for the master
    bool ext;         // following MIDI clock
    int ext_bpm_x10;  // measured, 0 = no clock
    uint32_t resyncs;
} studio_clock_status_t;
void studio_clock_status(studio_clock_status_t *st);

// Copy of the published timeline (any task, lock-free).
void studio_timeline(tclock_tl_t *out);

int64_t studio_now_us(void);      // the clock's time base

// A command for a module's rt_cmd() (runs in the rt task). False if full.
bool studio_cmd(const tmod_t *m, uint8_t cmd, int32_t arg);

// --- notes / routing ----------------------------------------------------------------------

// Route destinations for a sequencer track:
enum { ROUTE_NONE = -1, ROUTE_MIDI_OUT = 100 };   // or a module index 0..
// MIDI OUT channel used when routed to MIDI OUT: track k -> channel base + k.

// A sequencer track's note (from its rt task). Returns true if the track is
// routed somewhere (the sequencer then skips its own preview sound).
bool studio_note_out(const tmod_t *m, int track, const tnote_t *n);
bool studio_track_routed(const tmod_t *m, int track);
int studio_route_get(int mod, int track);
void studio_route_set(int mod, int track, int dest);
int studio_route_midi_ch(int mod, int track);       // 1..16
void studio_route_set_midi_ch(int mod, int track, int ch);

// --- MIDI OUT (any task) ---------------------------------------------------------------------

bool studio_midi_out(const uint8_t *b, int n);              // as soon as possible
bool studio_midi_out_at(int64_t t, const uint8_t *b, int n);  // at clock time t
// Last MIDI IN message, as text (for monitors). Returns a counter that
// bumps on every new message.
uint32_t studio_midi_monitor(char *buf, int len);

// --- audio --------------------------------------------------------------------------------

void studio_set_volume(int v);    // 0..10
int studio_volume(void);
int studio_audio_load_pct(void);  // render time / real time
uint32_t studio_audio_underruns(void);
const char *studio_audio_problem(void);   // NULL, or why there's no sound

// The mixer: one channel per audio app. level 0..100 % (STUDIO_MIX_UNITY =
// 0 dB, 100 = +2 dB); peak is the channel's level since the last call, 0..1.
#define STUDIO_MIX_UNITY 80
void studio_mix_set_level(int mod, int pct);
int studio_mix_level(int mod);
void studio_mix_set_mute(int mod, bool on);
bool studio_mix_muted(int mod);
float studio_mix_peak(int mod);

// --- UI -------------------------------------------------------------------------------------

// Every app's header has the same shape, so the Tulip squares and the
// header buttons are the same size and place on every page: buttons
// STUDIO_HDR_BTN_H tall at y = STUDIO_HDR_Y, a rule at STUDIO_HDR_LINE_Y.
#define STUDIO_HDR_Y        8
#define STUDIO_HDR_BTN_H    40
#define STUDIO_HDR_LINE_Y   56
#define STUDIO_SQ           40                                   // the squares
#define STUDIO_SQ_X         (1024 - 8 - 2 * STUDIO_SQ - 6)       // left square
// The two Tulip squares (blue: next app, red: Studio home / launcher), at
// the top right of parent's screen - the same on every app.
lv_obj_t *studio_chrome(lv_obj_t *parent);
void studio_show_home(void);
void studio_next_app(void);
void studio_show_module(int i);
// Leave to the launcher (stops, waits for modules' busy(), reboots).
void studio_exit_to_launcher(void);

// Files for module data live in /files/<name>/ (mounted by the runtime).
const char *studio_files_root(void);   // "/files", or NULL if no storage

#ifdef __cplusplus
}
#endif
