// ui.h - the touch UI (LVGL), a port of the Python sketch's pages.
//
// Everything here runs in the LVGL task on core 0. UI handlers edit g_pat
// directly (single-field writes) and send transport/CV commands to the
// sequencer with seq_cmd(). Playheads are drawn by polling g_status, never
// by a callback from the sequencer.
#pragma once

#include <stdbool.h>

#include "lvgl.h"

void ui_create(lv_obj_t *scr);   // Studio: build on this screen
void ui_poll(bool visible);      // every LVGL loop pass
