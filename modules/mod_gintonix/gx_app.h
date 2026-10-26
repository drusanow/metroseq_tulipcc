// gx_app.h - GinTonix state shared between the UI (core 0), the audio task
// and the MIDI task (core 1).
//
//  * g_gx (the kit: sounds + patterns) is edited by the UI. The audio task
//    reads the patterns while it works out a block's hits, inside
//    gx_lock() - for microseconds. Whole-kit loads swap it inside the lock.
//  * Sounds reach the audio task as ready-made coefficients
//    (gx_sound_changed()), never as patches it has to convert itself.
//  * Live triggers (pads, MIDI notes) go through a queue; mutes and the
//    queued pattern are single-byte writes (g_live).
//  * The audio task publishes the playing step / pattern / hit lights in
//    g_gxs for the UI to draw. Nobody ever waits for anybody.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/gx_model.h"
#include "core/gx_seq.h"
#include "studio/studio.h"

extern gx_state_t g_gx;
extern gx_live_t g_live;

void gx_lock(void);
void gx_unlock(void);

// A sound changed (patch, morph, master): c = channel, -1 = all.
void gx_sound_changed(int c);
// Play channel c now (a pad, a MIDI note); vel 0..1.
void gx_trigger(int c, float vel);
// Cut every sounding drum (kit change).
void gx_silence(void);

// --- status from the audio task -----------------------------------------------------------

typedef struct {
    volatile bool running;        // the transport is rolling (and we follow it)
    volatile int8_t step;         // step sounding now, -1 none
    volatile int8_t pat;          // pattern sounding now
    volatile uint32_t hit_ms[GX_NCH];   // when each channel last played (lights)
    volatile int load_pct;        // this app's share of the audio time
} gx_status_t;

extern gx_status_t g_gxs;

// --- settings (gintonic_settings.json) -------------------------------------------------------

#define GX_MAX_CC 64

typedef struct {
    int8_t midi_ch;      // 0 omni, 1..16, -1 off
    bool cc_sel;         // learned drum knobs follow the selected channel
    uint8_t midi_out;    // pattern out channel, 0 = off
    int8_t sync_ms;      // drums this much earlier (+) / later (-) than the grid
    uint8_t slot;        // last kit slot 1..128
    int ncc;
    struct {
        uint8_t cc;
        char key[20];    // "k:osc_freq", "c3:level", "g:morph" (the Python's keys)
    } cc[GX_MAX_CC];
} gx_settings_t;

extern gx_settings_t g_gxset;
void gx_settings_load(void);
void gx_settings_save(void);   // deferred: written when stopped (UI task)
void gx_settings_poll(void);

// --- MIDI IN for the UI (CC, program change, pattern keys) -----------------------------------

typedef struct {
    uint8_t status, d1, d2;
} gx_midi_msg_t;
bool gx_midi_next(gx_midi_msg_t *m);   // UI task

// --- files ----------------------------------------------------------------------------------------

#define GX_BANK_SLOTS 128
const char *gx_dir(void);   // "/files/gintonix" (NULL without storage)
void gx_path(char *out, int len, const char *name);

