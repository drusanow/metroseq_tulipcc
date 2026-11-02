// studio_priv.h - shared between the Studio runtime's source files.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "studio/midi_parse.h"
#include "studio/studio.h"

#define STUDIO_MAX_OUTS 4   // note outputs per module

typedef struct {
    int32_t bpm_x100;
    uint8_t clock_src;
    uint8_t clock_out;
    uint8_t mmc;
    uint8_t volume;
    uint16_t open_mask;         // unused: Studio starts with every app off
    int8_t shown;               // last module on screen (-1 = home)
    int8_t route[STUDIO_MAX_MODULES][STUDIO_MAX_OUTS];
    uint8_t route_ch[STUDIO_MAX_MODULES][STUDIO_MAX_OUTS];
    // the CV clock (drum machines, when no sequencer has the DACs)
    uint8_t cv_clock;           // on / off
    uint8_t cv_div;             // a pulse every 2 (1/16), 4 (1/8) or 8 (1/4) 1/32 steps
    uint8_t cv_vout1;           // CV_V1_RESET / CV_V1_RUN
} studio_cfg_t;

extern studio_cfg_t g_cfg;

// The mixer: saved beside g_cfg (its own NVS record, so older settings load).
typedef struct {
    uint8_t level[STUDIO_MAX_MODULES];   // 0..100 %, 80 = 0 dB
    uint8_t mute;                        // bit per module
} studio_mix_t;
extern studio_mix_t g_mix;
extern const tmod_t *g_mods[STUDIO_MAX_MODULES];
extern int g_nmods;
extern volatile bool g_enabled[STUDIO_MAX_MODULES];
// started: init() has run (UI task). The rt task keeps its own flag for
// rt_init(); no hook of an app runs before both.
extern volatile bool g_started[STUDIO_MAX_MODULES];
extern char g_single[24];   // "" = the Studio, else the app running on its own

void cfg_load(void);
void cfg_changed(void);      // save soon (UI task does the flash write)
void cfg_poll(void);         // UI task

// rt.c
enum {
    HC_PLAY = 1,
    HC_STOP,
    HC_TOGGLE,
    HC_RESTART,
    HC_BPM,
    HC_CLOCK_SRC,
    HC_CLOCK_OUT,
    HC_MMC,
    HC_ENABLE,       // arg = module << 8 | on
    HC_MODULE,       // module command
    HC_START,        // arg = module: rt_init() it, give it the tempo, mark it started
    HC_CV,           // the CV clock settings changed
};
void rt_start(void);
bool rt_post(uint8_t kind, uint8_t mod, uint8_t cmd, int32_t arg);
void rt_post_midi_in(const mp_ev_t *ev, int64_t t);
bool rt_ready(void);

// midi_io.c
void midi_io_start(void);
void midi_io_write(const uint8_t *b, int n);   // rt task only
void midi_monitor_set(const char *txt);

// audio.c
void audio_start(void);

// shell.c
void shell_ui_create(void);
void shell_ui_poll(void);
void shell_set_open(int i, bool on);   // open / close an app in Studio

// loader.c: apps from /files/studio/apps/*.elf (only: just that one, or NULL
// for all). Returns how many were added to out.
int loader_load_apps(const tmod_t **out, int room, const tmod_t *const *mods, int n, const char *only);
bool loader_is_loaded(const tmod_t *m);
// around a loaded app's code running for the first time (init, its screen):
// a crash in between keeps it out next time
void loader_guard_begin(const tmod_t *m);
void loader_guard_end(const tmod_t *m);

