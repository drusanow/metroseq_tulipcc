// theme.h - the launcher's look: black, thin grey outlines, orange for
// "active" and section titles, cyan as the second accent, red/blue square
// buttons top right. Every colour is exact on the panel's RGB332 palette.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define RGB332(r, g, b) ((uint8_t)((((r) >> 5) << 5) | (((g) >> 5) << 2) | ((b) >> 6)))

#define TH_BG      RGB332(0, 0, 0)
#define TH_FG      RGB332(182, 182, 170)   // body text
#define TH_DIM     RGB332(109, 109, 85)    // secondary text
#define TH_LINE    RGB332(72, 72, 85)      // outlines, separators
#define TH_ORANGE  RGB332(255, 145, 0)
#define TH_CYAN    RGB332(36, 182, 255)
#define TH_RED     RGB332(145, 0, 0)
#define TH_BLUE    RGB332(0, 109, 255)
#define TH_GREEN   RGB332(0, 182, 85)
#define TH_WHITE   RGB332(255, 255, 255)
#define TH_SEL_BG  RGB332(36, 36, 0)       // selected list row

#define SCR_W 1024
#define SCR_H 600
#define TOP_H 56          // top bar
#define PAGE_Y TOP_H
#define PAGE_H (SCR_H - TOP_H)

lv_color_t th_c(uint8_t c332);

// Outlined button with an upper-case label. Acts on PRESS (instant).
lv_obj_t *th_btn(lv_obj_t *parent, const char *txt, int x, int y, int w, int h, lv_event_cb_t cb,
                 void *ud);
void th_btn_set_text(lv_obj_t *btn, const char *txt);
void th_btn_set_active(lv_obj_t *btn, bool on);   // orange fill (like the active tab)
void th_btn_set_enabled(lv_obj_t *btn, bool on);

// Filled square icon button (top bar).
lv_obj_t *th_square(lv_obj_t *parent, const char *symbol, int x, int y, int w, int h,
                    uint8_t color, lv_event_cb_t cb, void *ud);

lv_obj_t *th_title(lv_obj_t *parent, const char *txt, int x, int y, uint8_t color);
lv_obj_t *th_text(lv_obj_t *parent, const char *txt, int x, int y, uint8_t color,
                  const lv_font_t *font);
lv_obj_t *th_box(lv_obj_t *parent, int x, int y, int w, int h);   // plain container
lv_obj_t *th_hline(lv_obj_t *parent, int x, int y, int w);

// Black keys with thin outlines, like the buttons (the default keyboard's
// greys have no exact RGB332 match and come out olive).
void th_style_keyboard(lv_obj_t *kb);

void th_init(void);   // screen background + LVGL default theme colours
