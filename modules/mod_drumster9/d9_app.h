// d9_app.h - Drumster9's state, shared by the module glue (d9_mod.c) and
// its UI (ui/).
//
// Ownership, as in GinTonix: the UI (core 0) owns g_d9 and edits it with
// single-field writes; anything touching several fields at once (a preset,
// a project, a slot switch) holds d9_lock(), a spinlock the audio task also
// takes for the few microseconds it reads the grid. The audio task never
// waits on the UI, and the UI never waits on the audio.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/d9_json.h"
#include "core/d9_model.h"
#include "core/d9_samples.h"
#include "studio/studio.h"

#ifdef __cplusplus
extern "C" {
#endif

extern d9_state_t g_d9;
// the sample index lives in PSRAM (8 KB of internal RAM saved)
extern d9_index_t *g_d9ix_p;
#define g_d9ix (*g_d9ix_p)
// the A-J slot that comes in on the next downbeat, -1 none (audio takes it)
extern volatile int8_t g_d9_next;

typedef struct {
    volatile int8_t step;             // the step sounding, -1 stopped
    volatile bool playing;
    volatile uint32_t hit_ms[D9_LANES];
    volatile uint32_t swaps;          // bumps when a queued slot has come in
    volatile float peak;              // output peak since the UI last took it
    volatile int load_pct;
    volatile bool samples_ok;         // the samples partition was found
    volatile bool cv_present;         // standalone: a Mabee DAC answered
} d9_status_t;
extern d9_status_t g_d9s;

enum { CV_RESET = 0, CV_RUN };
typedef struct {
    int8_t midi_ch;     // notes in: 0 omni, 1..16, -1 off
    bool cv_on;         // standalone: the clock on the Mabee DAC
    uint8_t cv_div;     // clock: a pulse every 2 (1/16), 4 (1/8) or 8 (1/4) steps
    uint8_t cv_vout1;   // CV_RESET: a pulse each bar; CV_RUN: high while playing
} d9_settings_t;
extern d9_settings_t g_d9set;

void d9_lock(void);
void d9_unlock(void);

// The state changed: kept in /files/drumster9/state.json a moment later.
void d9_changed(void);
void d9_settings_save(void);

// Samples. The kits' samples are read from the samples partition into
// PSRAM when they're first needed; these make sure they are.
void d9_ensure_lane_samples(void);   // every lane's kit, and the queued slot's
bool d9_ensure_sample(int preset);
const char *d9_name(int preset);     // "OFF", or the sample's name

// Play now (from the UI's lane pads or MIDI): a lane, or any sample (the
// KITS page's preview).
void d9_trigger(int lane, float vel);
void d9_audition(int preset);

// The pattern was replaced (a project, a Studio patch): the UI redraws.
extern volatile uint32_t g_d9_reload;

// --- projects: /files/drumster9/tulipdrums_projects.json (the Python's format) ----

int d9_proj_count(void);
const char *d9_proj_name(int i);
int d9_proj_find(const char *name);
bool d9_proj_save(const char *name);   // new, or over the one with that name
bool d9_proj_load(int i);
bool d9_proj_delete(int i);
extern char g_d9_proj_cur[D9_PROJECT_NAME + 1];   // the one loaded / saved last

// --- CV clock (standalone: no other app in the image drives the DACs) --------------

bool d9_cv_available(void);
void d9_cv_apply(void);   // after g_d9set.cv_* changed

#ifdef __cplusplus
}
#endif
