// ui_common.c - shared widget helpers.
#include "ui_common.h"

#include <assert.h>

#include "studio/widgets.h"

const uint8_t C_TRK[NUM_TRACKS] = {C_TRK0, C_TRK1};
// MULT / SING / HOLD fill with their colour; REST stays an outline
const uint8_t C_GATE[GATE_COUNT] = {RGB332(0x6d, 0xff, 0x00), RGB332(0x24, 0xb6, 0xff),
                                    RGB332(0xff, 0xdb, 0x00), C_BTN_DK};

// Expand RGB332 so that the flush's RGB565 -> RGB332 conversion maps it
// back to exactly the same byte.
lv_color_t c332(uint8_t c) {
    return lv_color_make((c >> 5) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 255 / 3);
}

void ui_depad(lv_obj_t *o) {
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

// radius > 0 asks for a popup: square, black, with an outline (the Studio look)
lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h, uint8_t color, int radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    ui_depad(p);
    lv_obj_set_style_shadow_width(p, 0, 0);
    if (radius > 0) {
        lv_obj_set_style_bg_color(p, c332(C_BG), 0);
        lv_obj_set_style_border_width(p, 1, 0);
        lv_obj_set_style_border_color(p, c332(C_EDGE), 0);
        lv_obj_set_style_radius(p, 0, 0);
    } else {
        lv_obj_set_style_bg_color(p, c332(color), 0);
        lv_obj_set_style_radius(p, 0, 0);
    }
    return p;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, SW_F_LABEL, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *ui_section(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *l = ui_label(parent, text, x, y);
    lv_obj_set_style_text_color(l, c332(C_SECTION), 0);
    return l;
}

lv_obj_t *ui_led(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = ui_panel(parent, x, y, w, h, C_LED_OFF, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void ui_led_set(lv_obj_t *led, uint8_t color) {
    lv_obj_set_style_bg_color(led, c332(color), 0);
}

// --- Button ---------------------------------------------------------------------

#define MAX_BTNS 320
static ui_btn_t s_btns[MAX_BTNS];
static int s_nbtns;

// Neutral colours draw as an outline with dim text; any other colour fills
// the button, with black text (GinTonix's latching buttons).
static bool neutral(uint8_t c) { return c == C_BTN || c == C_BTN_DK || c == C_BG; }

static void paint(ui_btn_t *b) {
    bool fill = !neutral(b->color);
    lv_obj_set_style_bg_color(b->obj, c332(fill ? b->color : C_BG), 0);
    lv_obj_set_style_border_color(b->obj, c332(fill ? b->color : C_EDGE), 0);
    lv_obj_set_style_text_color(b->label, fill ? lv_color_black() : c332(C_DIM), 0);
}

static void btn_event(lv_event_t *e) {
    ui_btn_t *b = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED_REPEAT) {
        if (b->repeat_cb) b->repeat_cb(b->arg);
    } else if (b->cb) {
        b->cb(b->arg);
    }
}

ui_btn_t *ui_btn(lv_obj_t *parent, const char *text, int x, int w, int h, ui_cb_t cb,
                 intptr_t arg, uint8_t color, int y, ui_cb_t repeat_cb) {
    assert(s_nbtns < MAX_BTNS);
    ui_btn_t *b = &s_btns[s_nbtns++];
    b->cb = cb;
    b->repeat_cb = repeat_cb;
    b->arg = arg;
    b->color = color;
    b->obj = lv_obj_create(parent);
    lv_obj_set_size(b->obj, w, h);
    lv_obj_set_pos(b->obj, x, y);
    lv_obj_set_style_radius(b->obj, 0, 0);
    lv_obj_set_style_bg_opa(b->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b->obj, 0, 0);
    lv_obj_remove_flag(b->obj, LV_OBJ_FLAG_SCROLLABLE);
    ui_depad(b->obj);
    lv_obj_set_style_border_width(b->obj, 1, 0);
    lv_obj_set_style_border_opa(b->obj, LV_OPA_COVER, 0);
    // Instant feedback: a white rim while the finger is down.
    lv_obj_set_style_border_color(b->obj, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b->obj, 2, LV_STATE_PRESSED);
    // Act on PRESS, not release: the touch UI reacts the moment the finger
    // lands (one touch poll, ~5-10 ms) instead of after it lifts. There is
    // no scrolling anywhere in this UI, so there is nothing to mistake a
    // press for. Hold-to-repeat still works on top (LONG_PRESSED_REPEAT).
    lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_PRESSED, b);
    if (repeat_cb) lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_LONG_PRESSED_REPEAT, b);
    b->label = lv_label_create(b->obj);
    lv_label_set_text(b->label, text);
    lv_obj_set_style_text_font(b->label, SW_F_LABEL, 0);
    lv_obj_center(b->label);
    paint(b);
    return b;
}

void ui_btn_set_color(ui_btn_t *b, uint8_t c) {
    if (b->color == c) return;
    b->color = c;
    paint(b);
}

void ui_btn_set_text(ui_btn_t *b, const char *t) {
    lv_label_set_text(b->label, t);
}

static void flash_revert(lv_timer_t *t) { paint(lv_timer_get_user_data(t)); }

void ui_btn_flash(ui_btn_t *b) {
    lv_obj_set_style_bg_color(b->obj, c332(C_FLASH), 0);
    lv_obj_set_style_border_color(b->obj, c332(C_FLASH), 0);
    lv_obj_set_style_text_color(b->label, lv_color_black(), 0);
    lv_timer_t *t = lv_timer_create(flash_revert, 500, b);
    lv_timer_set_repeat_count(t, 1);   // auto-deleted after firing
}

// --- slider look ---------------------------------------------------------------

// The Studio's bar look, upright: a dark track that fills with the
// colour from the bottom (from the middle for a bipolar range), and a
// white line for the value.
void ui_style_bar_slider(lv_obj_t *sl, uint8_t fill) {
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, c332(C_SLIDER_TRACK), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, c332(fill), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_ver(sl, -7, LV_PART_KNOB);   // a 6 px line across the track
    lv_obj_set_style_pad_hor(sl, 4, LV_PART_KNOB);
    // The track is only 20 px wide; let a finger anywhere within 24 px of
    // it grab the slider.
    lv_obj_set_ext_click_area(sl, 24);
}
