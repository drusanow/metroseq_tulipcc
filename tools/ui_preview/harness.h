// harness.h - host LVGL harness shared by the UI preview programs: a
// 1024x600 display flushed through the device's RGB565 -> RGB332
// conversion, simulated touch (through the calibration hook like the
// device), and PNG-able screenshots.
#pragma once

#include <stdbool.h>

void hx_init(void);
void hx_pump(int ms);                    // run LVGL for ms of simulated time
void hx_tap(int x, int y);               // press + release at screen x,y
void hx_touch(int x, int y, int pressed);
void hx_save(const char *name);          // build/<name>.ppm (Makefile -> .png)
