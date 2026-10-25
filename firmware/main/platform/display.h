// display.h - Tulip's 1024x600 RGB panel + LVGL, on core 0.
#pragma once

// Bring up the panel and LVGL, build the UI (ui_create) and run the LVGL
// loop in its own task on core 0.
void display_start(void);
