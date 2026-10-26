// wt_app.h - shared between Wavetron's module glue (wt_mod.c) and its UI.
//
// The UI task owns the sound: it edits g_wt and calls wt_changed(), which
// hands a copy to the audio task (picked up at the next block) and saves
// it a moment later. MIDI CCs reach the sound through the UI task too
// (wt_midi_next), so only one task ever writes it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/wt_dsp.h"
#include "core/wt_patch.h"
#include "core/wt_tables.h"
#include "studio/studio.h"

extern wt_patch_t g_wt;          // the sound + which tables (paths / built-ins)
extern wt_catalog_t g_cat;       // every table in RAM
extern int g_ia, g_ib;           // the catalogue entries OSC A / OSC B play
extern char g_patch_name[WT_PATCH_NAME_MAX + 1];   // "" = not saved
extern cJSON *g_lib;             // the patch library

void wt_changed(void);           // after editing g_wt.s
void wt_replaced(void);          // after loading a whole sound (cuts notes)
bool wt_select_table(int osc, int idx);   // osc 0 = A, 1 = B
// load a WAV from /files into OSC A / B; false + reason
bool wt_load_file(int osc, const char *path, char *err, int errlen);
void wt_panic_now(void);

// patches (the library file is written at once)
bool wt_patch_save(const char *name);
bool wt_patch_load(int i);       // false + toast-able reason in wt_last_msg
bool wt_patch_delete(int i);
bool wt_patch_rename(int i, const char *name);
void wt_patch_init(void);        // the default sound
extern char wt_last_msg[48];

// where the files live
const char *wt_dir(void);        // "/files/wavetron" (NULL without /files)
const char *wt_tables_dir(void); // ".../wavetron/wavetables"

// live state for the displays (read without locking: display only)
typedef struct {
    bool active[WT_NV], gate[WT_NV];
    float note[WT_NV];
    float pos_a, pos_b;          // where the most recent voice is in each table
    bool sounding;
} wt_live_t;
void wt_live(wt_live_t *out);
int wt_scope_read(float *out, int n);   // the last n output samples
int wt_load_pct(void);

// MIDI CCs, handed to the UI task
typedef struct {
    uint8_t cc, val;
} wt_cc_t;
bool wt_midi_next(wt_cc_t *m);

// UI (ui/wt_ui.c)
void wt_ui_create(lv_obj_t *scr);
void wt_ui_poll(bool visible);
void wt_ui_show(bool shown);
void wt_ui_toast(const char *msg);
