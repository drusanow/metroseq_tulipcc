// wt_ui_int.h - Wavetron's UI internals: layout, colours, the widgets that
// bind to a parameter, the displays, and the pages.
//
// The sketch's fifteen 128 x 128 OLED pages become seven touch pages; every
// parameter it had is here, each as a slider or a row of buttons, and its
// little visual panes become full-size displays (the 3-D wavetable
// waterfall first of all).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "studio/widgets.h"
#include "wt_app.h"

#define PAGE_Y   (STUDIO_HDR_LINE_Y + 4)
#define PAGE_H   (600 - PAGE_Y)

// colours (all survive the panel's RGB332 palette)
#define C_BG     SW_BG
#define C_TEXT   SW_TEXT
#define C_DIM    SW_DIM
#define C_LINE   SW_LINE
#define C_EDGE   SW_EDGE
#define C_WT     0xb66dff   // Wavetron (violet)
#define C_OSCA   SW_CYAN
#define C_OSCB   SW_ORANGE
#define C_FLT    SW_GREEN
#define C_MOD    0xb66dff
#define C_ENV    SW_YELLOW
#define C_FX     0xff6dff
#define C_VOICE  SW_CYAN

enum { PG_OSC, PG_FLT, PG_MOD, PG_MX, PG_VOICE, PG_FX, PG_PATCH, PG_COUNT };

// --- bindings: widgets that show and edit a parameter -------------------------------------

// a slider for parameter i (its label is the parameter's)
sw_slider_t *wb_slider(lv_obj_t *par, int i, int x, int y, int w, uint32_t col);
// MOD1 / MOD2 RATE: Hz as an LFO, the note ratio in AUDIO mode
sw_slider_t *wb_rate(lv_obj_t *par, int mod, int x, int y, int w, uint32_t col);
// a matrix amount (-4..4)
sw_slider_t *wb_amount(lv_obj_t *par, int mod, int dest, const char *label, int x, int y, int w,
                       uint32_t col);
// one button per choice of an enum parameter
void wb_choice(lv_obj_t *par, int i, int x, int y, int bw, int h, int gap, uint32_t col);
// an on / off parameter
lv_obj_t *wb_toggle(lv_obj_t *par, int i, const char *text, int x, int y, int w, int h, uint32_t col);
void wb_refresh(void);   // after the sound changed elsewhere (a patch, MIDI)

// a section title
lv_obj_t *wt_section(lv_obj_t *par, const char *text, int x, int y, uint32_t col);
lv_obj_t *wt_frame(lv_obj_t *par, int x, int y, int w, int h);

// --- displays (wt_draw.c) -------------------------------------------------------------------

lv_obj_t *wd_waterfall(lv_obj_t *par, int osc, int x, int y, int w, int h);
lv_obj_t *wd_filter(lv_obj_t *par, int x, int y, int w, int h);
lv_obj_t *wd_env(lv_obj_t *par, int which, int x, int y, int w, int h);   // 0 amp, 3 / 4 MOD3 / MOD4
lv_obj_t *wd_lfo(lv_obj_t *par, int mod, int x, int y, int w, int h);
lv_obj_t *wd_drive(lv_obj_t *par, int x, int y, int w, int h);
lv_obj_t *wd_unison(lv_obj_t *par, int x, int y, int w, int h);
lv_obj_t *wd_scope(lv_obj_t *par, int x, int y, int w, int h);
lv_obj_t *wd_voices(lv_obj_t *par, int x, int y, int w, int h);
// redraw what changed (sig) and what moves (live) - visible page only
void wd_tick(int page);

// --- pages --------------------------------------------------------------------------------

void pg_osc(lv_obj_t *p);
void pg_flt(lv_obj_t *p);
void pg_mod(lv_obj_t *p);
void pg_mx(lv_obj_t *p);
void pg_voice(lv_obj_t *p);
void pg_fx(lv_obj_t *p);
void pg_patch(lv_obj_t *p);
void pg_osc_tick(void);
void pg_mx_tick(void);
void pg_voice_tick(void);
void pg_patch_tick(void);

// --- popups ----------------------------------------------------------------------------------

// a full-screen layer over the page (hidden with wt_popup_close)
lv_obj_t *wt_popup(const char *title, uint32_t col);
void wt_popup_close(void);
// the on-screen keyboard: done(text) on OK
void wt_keyboard(const char *title, const char *initial, void (*done)(const char *text));
void wt_header_refresh(void);
extern lv_obj_t *wt_scr;
