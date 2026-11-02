// display.h - Tulip CC 1024x600 RGB panel + LVGL 9 port.
//
// The LVGL task, the panel's bounce-buffer interrupt and all rendering run
// on the core you choose, so an app can keep its other core free for
// real-time work (metroseq puts the UI on core 0 and its sequencer on 1;
// the launcher puts the UI on core 1 and Wi-Fi on core 0).
//
// Touch arrives through gt911.h's event queue; whoever owns the I2C bus
// polls the controller (see tulip_input_start() for the simple case).
#pragma once

#include <stdbool.h>

#include "tulip/touch.h"

typedef struct {
    int core;                 // core for the LVGL task and panel interrupt
    int priority;
    void (*ui_create)(void);  // build the UI (runs in the LVGL task)
    void (*ui_poll)(void);    // called every loop pass (LVGL task)
} tulip_display_cfg_t;

void tulip_display_start(const tulip_display_cfg_t *cfg);

// Backlight 1 (dim) .. 9 (full), Tulip's scale.
void tulip_display_set_brightness(int level);
