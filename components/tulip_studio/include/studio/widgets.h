// widgets.h - the Studio look (GinTonix's): black, thin olive-grey outlines,
// flat latching buttons that fill with their accent colour when on, and act
// on PRESS so the screen feels instant. Colours are 0xRRGGBB values that
// survive the panel's RGB332 palette.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define SW_BG        0x000000
#define SW_LINE      0x494955
#define SW_EDGE      0x6d6d55
#define SW_TEXT      0xffffff
#define SW_DIM       0x9292aa
#define SW_ORANGE    0xff9200
#define SW_CYAN      0x24b6ff
#define SW_GREEN     0x6dff00
#define SW_RED       0xff2400
#define SW_YELLOW    0xffdb00
#define SW_SQ_RED    0x920000
#define SW_SQ_BLUE   0x006dff

extern const lv_font_t *const SW_F_SMALL;   // 12
extern const lv_font_t *const SW_F_LABEL;   // 14
extern const lv_font_t *const SW_F_MID;     // 20
extern const lv_font_t *const SW_F_BIG;     // 28

typedef void (*sw_cb_t)(void *ud);

lv_color_t sw_c(uint32_t hex);

// Strip LVGL's default styling: no padding / border / radius / scroll.
// bg < 0 = transparent.
lv_obj_t *sw_plain(lv_obj_t *o, int32_t bg, int radius);
lv_obj_t *sw_box(lv_obj_t *parent, int x, int y, int w, int h, int32_t bg, int radius);
lv_obj_t *sw_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                   int x, int y, int w, bool center);
lv_obj_t *sw_line(lv_obj_t *parent, int x, int y, int w, int h);

// Flat latching button: accent fill when on. cb runs on PRESS. font NULL
// = SW_F_LABEL.
lv_obj_t *sw_btn(lv_obj_t *parent, const char *text, int x, int y, int w, int h, uint32_t accent,
                 sw_cb_t cb, void *ud, const lv_font_t *font);
void sw_btn_set(lv_obj_t *b, bool on);
bool sw_btn_is_on(lv_obj_t *b);
void sw_btn_text(lv_obj_t *b, const char *text);
void sw_btn_accent(lv_obj_t *b, uint32_t accent);
lv_obj_t *sw_btn_label(lv_obj_t *b);
// Holding the button repeats cb (for - / + steppers).
void sw_btn_repeat(lv_obj_t *b);
// Filled square (the Tulip app squares).
lv_obj_t *sw_square(lv_obj_t *parent, int x, int y, int s, uint32_t color, const char *sym,
                    sw_cb_t cb, void *ud);

// A labelled horizontal slider: drag left / right anywhere on it (relative,
// so a touch never makes the value jump). get / set / text work on 0..1.
typedef struct sw_slider sw_slider_t;
typedef struct {
    float (*get)(void *ud);
    void (*set)(void *ud, float v);
    void (*text)(void *ud, char *buf, int len);
} sw_slider_ops_t;
sw_slider_t *sw_slider(lv_obj_t *parent, const char *label, int x, int y, int w, int h,
                       uint32_t color, const sw_slider_ops_t *ops, void *ud);
void sw_slider_refresh(sw_slider_t *s);   // after the value changed elsewhere
