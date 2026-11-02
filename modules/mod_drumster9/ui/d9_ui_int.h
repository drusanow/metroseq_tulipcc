// d9_ui_int.h - shared by Drumster9's UI pages.
//
// The Studio look (studio/widgets.h): black pages, thin outlines, flat
// buttons that fill with their colour when on and act on PRESS. Five pages
// under one header:
//
//   SEQ   the 7 lanes x 32 steps, the playhead, presets, kits, A-J slots
//   KITS  which sample each kit plays for each part, from any bank
//   FX    the 4 buses: reverb, echo, filter, drive, EQ, and who's on them
//   MIX   every lane's level, mute and solo, the master, a meter
//   SET   projects, MIDI, clock, the CV clock
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "d9_app.h"
#include "lvgl.h"
#include "studio/widgets.h"

#define ACC      0xdb6dff   // Drumster9's colour
#define C_DIM    SW_DIM
#define PAGE_Y   (STUDIO_HDR_LINE_Y + 8)
#define PAGE_H   (600 - PAGE_Y)

extern const uint32_t D9_BUS_COLOR[D9_BUSES];

// pages
enum { PG_SEQ, PG_KITS, PG_FX, PG_MIX, PG_SET, PG_COUNT };

void d9_seq_page_create(lv_obj_t *p);
void d9_seq_page_refresh(void);      // the whole grid and strip
void d9_seq_page_tick(uint32_t ms);  // playhead, hit lights, slots
void d9_kit_page_create(lv_obj_t *p);
void d9_kit_page_refresh(void);
void d9_fx_page_create(lv_obj_t *p);
void d9_fx_page_refresh(void);
void d9_mix_page_create(lv_obj_t *p);
void d9_mix_page_refresh(void);
void d9_mix_page_tick(uint32_t ms);
void d9_set_page_create(lv_obj_t *p);
void d9_set_page_refresh(void);

// ui.c
void d9_say(const char *msg);        // the header's info line, for a while
void d9_edit(void);                  // the grid / sound changed: save soon
lv_obj_t *d9_step_btn(lv_obj_t *p, int x, int y, int w, sw_cb_t cb, void *ud);   // - or +
void d9_kit_changed(void);           // a lane's sample may have changed
