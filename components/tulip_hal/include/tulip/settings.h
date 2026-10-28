// settings.h - device-wide settings shared by the launcher and every app
// (NVS namespace "tulip"): touch calibration and backlight brightness.
// Calibrate once in the launcher and every app gets the same touch map.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t cal_valid;     // cal[] holds a user calibration
    float cal[6];          // raw -> screen affine map (tulip/touch_cal.h)
    uint8_t brightness;    // 1..9 (0 = not set: full)
    uint8_t cal_prompt;    // launcher: offer calibration at power-on
    uint8_t cal_grid;      // cal_dx / cal_dy hold the edge correction (9 targets)
    float cal_dx[9], cal_dy[9];
} tulip_settings_t;

extern tulip_settings_t g_tulip;

// Initialises NVS (if needed) and loads g_tulip. Call once at boot.
void tulip_settings_load(void);

// Writes g_tulip to NVS (small, synchronous).
void tulip_settings_save(void);
