// ui_common.h - colours, Button and slider helpers shared by the pages
// (the Python Button / strut / style_bar_slider helpers).
#pragma once

#include <stdint.h>

#include "app.h"
#include "lvgl.h"

// --- layout (1024 x 600), same numbers as the Python -----------------------
#define LCD_W       1024
#define LCD_H       600
#define SCREEN_TOP  8
#define HDR_H       52
#define HDR_BTN_H   42
#define PAGE_Y      (SCREEN_TOP + HDR_H + 6)
#define PAGE_H      (600 - PAGE_Y - 6)
#define SEQ_X0      34
#define COL_W       122
#define COL_INNER   108
#define SLIDER_H    250
#define SLIDER_W    20

// --- palette: the Python's RGB332 values -----------------------------------
#define RGB332(r, g, b) ((uint8_t)((((r) >> 5) << 5) | (((g) >> 5) << 2) | ((b) >> 6)))

#define C_BG            0
#define C_PANEL         RGB332(38, 38, 50)
#define C_PANEL2        RGB332(28, 28, 38)
#define C_BTN           RGB332(60, 110, 200)
#define C_BTN_DK        RGB332(50, 55, 75)
#define C_PLAY          RGB332(40, 170, 90)
#define C_STOP          RGB332(200, 60, 50)
#define C_ACCENT        RGB332(150, 90, 200)
#define C_FLASH         RGB332(255, 140, 0)
#define C_LED_OFF       RGB332(40, 45, 55)
#define C_ON            RGB332(0, 210, 120)
#define C_TRK0          RGB332(70, 170, 255)
#define C_TRK1          RGB332(255, 150, 40)
#define C_SKIP          RGB332(120, 60, 60)
#define C_SLIDE         RGB332(120, 90, 200)
#define C_SEL           RGB332(240, 190, 40)
#define C_PRESET_CUR    RGB332(255, 150, 40)
#define C_SLIDER_TRACK  RGB332(205, 205, 205)
#define C_KNOB_FRAME    0
#define C_KNOB_SEQ      RGB332(225, 40, 40)
#define C_KNOB_MOD      RGB332(70, 150, 255)
#define C_MOD_ZERO      RGB332(60, 120, 240)

extern const uint8_t C_TRK[NUM_TRACKS];
extern const uint8_t C_GATE[GATE_COUNT];

lv_color_t c332(uint8_t c);

// --- Button --------------------------------------------------------------------

typedef void (*ui_cb_t)(intptr_t arg);

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *label;
    uint8_t color;
    ui_cb_t cb;
    ui_cb_t repeat_cb;   // optional hold-to-repeat
    intptr_t arg;
} ui_btn_t;

ui_btn_t *ui_btn(lv_obj_t *parent, const char *text, int x, int w, int h, ui_cb_t cb,
                 intptr_t arg, uint8_t color, int y, ui_cb_t repeat_cb);
void ui_btn_set_color(ui_btn_t *b, uint8_t c);
void ui_btn_set_text(ui_btn_t *b, const char *t);
void ui_btn_flash(ui_btn_t *b);

// --- misc ------------------------------------------------------------------------

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h, uint8_t color, int radius);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, int x, int y);
lv_obj_t *ui_led(lv_obj_t *parent, int x, int y, int w, int h);
void ui_led_set(lv_obj_t *led, uint8_t color);
void ui_style_bar_slider(lv_obj_t *sl, uint8_t center);
void ui_depad(lv_obj_t *o);

// --- cross-page hooks (ui.c) ---------------------------------------------------

void show_page(int i);
void ui_playheads_invalidate(void);
void ui_refresh_all(void);       // after a whole-pattern change
void ui_refresh_notes(void);     // SEQ note labels (root/scale/octave changed)
void ui_header_info(const char *t);
void ui_set_bpm(int bpm);

// pages
void seq_page_create(lv_obj_t *parent);
void seq_page_refresh(void);
void seq_page_refresh_col(int i);
void seq_page_refresh_notes(void);
void seq_page_light(int stage, int track, bool on);
void seq_page_show(bool on);

void detail_popup_create(lv_obj_t *parent);
void detail_popup_show(int stage);

void trk_page_create(lv_obj_t *parent);
void trk_page_refresh(void);
void trk_page_show(bool on);

void mod_page_create(lv_obj_t *parent);
void mod_page_refresh(void);
void mod_page_light(int stage, bool on);
void mod_page_show(bool on);
int mod_page_selected(void);

void set_page_create(lv_obj_t *parent);
void set_page_refresh(void);
void set_page_show(bool on);
void set_page_poll(void);

void save_popup_create(lv_obj_t *parent);
void save_popup_show(void);

