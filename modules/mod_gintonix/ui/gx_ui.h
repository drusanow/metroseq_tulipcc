// gx_ui.h - the GinTonix touch UI (LVGL task).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

void gx_ui_create(lv_obj_t *scr);
void gx_ui_poll(bool visible);
void gx_ui_show(bool shown);
// Boot: load the last used kit slot into g_gx (no UI needed).
void gx_boot_load(void);
