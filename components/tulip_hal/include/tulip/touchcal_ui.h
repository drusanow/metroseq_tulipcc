// touchcal_ui.h - the touchscreen calibration screen (LVGL task only).
//
// Five targets, a least-squares fit (tulip/touch_cal.h), a live test, then
// SAVE / RETRY / CANCEL; 20 s without a touch cancels. The prompt and the
// targets read raw coordinates and use half-screen choices, so they work
// even when the current calibration is badly wrong. SAVE stores the map
// device-wide (tulip/settings.h), so every app picks it up.
#pragma once

#include <stdbool.h>

// "Tap left to calibrate, right (or wait 5 s) to skip". `done` runs when
// the screen closes, whichever way.
void tulip_touchcal_boot_prompt(void (*done)(void));

// Straight to the targets (e.g. from a settings page).
void tulip_touchcal_start(void (*done)(void));

// Call every LVGL loop pass.
void tulip_touchcal_poll(void);
bool tulip_touchcal_active(void);

// Optional: where to show "saved" / "unchanged" messages.
void tulip_touchcal_set_notify(void (*msg)(const char *));
