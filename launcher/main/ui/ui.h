// ui.h - the launcher UI (LVGL task only).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

void ui_create(void);
void ui_poll(void);

// shared by the pages
enum { PG_HOME = 0, PG_FILES, PG_WIFI, PG_SETTINGS, PG_CONSOLE, PG_COUNT };
void ui_show_page(int page);
void ui_toast(const char *msg);   // 3 s message in the top bar
void ui_launch(const char *path);   // confirm-free launch with progress overlay

// pages: create once, show/hide, poll while visible
void home_create(lv_obj_t *page);
void home_show(void);
void home_poll(void);
void files_create(lv_obj_t *page);
void files_show(void);
void files_poll(void);
void files_open_dir(const char *abs);
void wifi_page_create(lv_obj_t *page);
void wifi_page_show(void);
void wifi_page_poll(void);
void settings_create(lv_obj_t *page);
void settings_show(void);
void settings_poll(void);
void console_page_create(lv_obj_t *page);
void console_page_show(void);
void console_page_poll(void);
