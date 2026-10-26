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
    uint8_t open_mask;          // modules open in Studio
    int8_t shown;               // last module on screen (-1 = home)
    int8_t route[STUDIO_MAX_MODULES][STUDIO_MAX_OUTS];
    uint8_t route_ch[STUDIO_MAX_MODULES][STUDIO_MAX_OUTS];
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
