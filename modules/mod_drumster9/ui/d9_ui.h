// d9_ui.h - Drumster9's touch UI (LVGL task).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

void d9_ui_create(lv_obj_t *scr);
void d9_ui_poll(bool visible);
void d9_ui_show(bool shown);
