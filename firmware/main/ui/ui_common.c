// ui_common.c - shared widget helpers.
#include "ui_common.h"

const uint8_t C_TRK[NUM_TRACKS] = {C_TRK0, C_TRK1};
const uint8_t C_GATE[GATE_COUNT] = {RGB332(0, 200, 110), RGB332(70, 150, 255),
                                    RGB332(230, 200, 40), RGB332(90, 90, 100)};

// Expand RGB332 so that the flush's RGB565 -> RGB332 conversion maps it
// back to exactly the same byte.
lv_color_t c332(uint8_t c) {
    return lv_color_make((c >> 5) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 255 / 3);
}

void ui_depad(lv_obj_t *o) {
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h, uint8_t color, int radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, c332(color), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    ui_depad(p);
    return p;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *ui_led(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = ui_panel(parent, x, y, w, h, C_LED_OFF, 2);
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
    lv_obj_set_style_radius(b->obj, 5, 0);
    lv_obj_set_style_bg_color(b->obj, c332(color), 0);
    lv_obj_set_style_bg_opa(b->obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(b->obj, LV_OBJ_FLAG_SCROLLABLE);
    ui_depad(b->obj);
    if (repeat_cb) {
        // a hold must not also fire a click when released
        lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_SHORT_CLICKED, b);
        lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_LONG_PRESSED_REPEAT, b);
    } else {
        lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_CLICKED, b);
    }
    b->label = lv_label_create(b->obj);
    lv_label_set_text(b->label, text);
    lv_obj_center(b->label);
    return b;
}

void ui_btn_set_color(ui_btn_t *b, uint8_t c) {
    b->color = c;
    lv_obj_set_style_bg_color(b->obj, c332(c), 0);
}

void ui_btn_set_text(ui_btn_t *b, const char *t) {
    lv_label_set_text(b->label, t);
}

static void flash_revert(lv_timer_t *t) {
    ui_btn_t *b = lv_timer_get_user_data(t);
    lv_obj_set_style_bg_color(b->obj, c332(b->color), 0);
}

void ui_btn_flash(ui_btn_t *b) {
    lv_obj_set_style_bg_color(b->obj, c332(C_FLASH), 0);
    lv_timer_t *t = lv_timer_create(flash_revert, 500, b);
    lv_timer_set_repeat_count(t, 1);   // auto-deleted after firing
}

// --- slider look ---------------------------------------------------------------

void ui_style_bar_slider(lv_obj_t *sl, uint8_t center) {
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, c332(C_SLIDER_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, c332(C_SLIDER_TRACK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, c332(center), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, c332(C_KNOB_FRAME), LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 4, LV_PART_KNOB);
    lv_obj_set_style_border_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_ver(sl, -4, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(sl, 3, LV_PART_KNOB);
}
