// widgets.c - see widgets.h.
#include "studio/widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t *const SW_F_SMALL = &lv_font_montserrat_12;
const lv_font_t *const SW_F_LABEL = &lv_font_montserrat_14;
const lv_font_t *const SW_F_MID = &lv_font_montserrat_20;
const lv_font_t *const SW_F_BIG = &lv_font_montserrat_28;

lv_color_t sw_c(uint32_t hex) { return lv_color_hex(hex); }

lv_obj_t *sw_plain(lv_obj_t *o, int32_t bg, int radius) {
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    if (bg < 0) {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, sw_c((uint32_t)bg), 0);
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *sw_box(lv_obj_t *parent, int x, int y, int w, int h, int32_t bg, int radius) {
    lv_obj_t *o = sw_plain(lv_obj_create(parent), bg, radius);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *sw_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                   int x, int y, int w, bool center) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font ? font : SW_F_LABEL, 0);
    lv_obj_set_style_text_color(l, sw_c(color), 0);
    if (w > 0) lv_obj_set_width(l, w);
    if (center) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *sw_line(lv_obj_t *parent, int x, int y, int w, int h) {
    return sw_box(parent, x, y, w, h, SW_LINE, 0);
}

// --- button --------------------------------------------------------------------------

typedef struct {
    sw_cb_t cb;
    void *ud;
    uint32_t accent;
    lv_obj_t *label;
    int8_t on;
} btn_t;

static void btn_event(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    btn_t *d = lv_obj_get_user_data(b);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_DELETE) {
        free(d);
        return;
    }
    if (d && d->cb) d->cb(d->ud);
}

static void paint(lv_obj_t *b, btn_t *d) {
    bool on = d->on > 0;
    lv_obj_set_style_bg_color(b, sw_c(on ? d->accent : SW_BG), 0);
    lv_obj_set_style_border_color(b, sw_c(on ? d->accent : SW_EDGE), 0);
    lv_obj_set_style_text_color(d->label, sw_c(on ? SW_BG : SW_DIM), 0);
}

lv_obj_t *sw_btn(lv_obj_t *parent, const char *text, int x, int y, int w, int h, uint32_t accent,
                 sw_cb_t cb, void *ud, const lv_font_t *font) {
    lv_obj_t *b = sw_plain(lv_obj_create(parent), SW_BG, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    // press feedback: a white rim, instantly
    lv_obj_set_style_border_color(b, sw_c(SW_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 2, LV_STATE_PRESSED);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    btn_t *d = calloc(1, sizeof *d);
    d->cb = cb;
    d->ud = ud;
    d->accent = accent;
    d->on = -1;
    d->label = lv_label_create(b);
    lv_label_set_text(d->label, text);
    lv_obj_set_style_text_font(d->label, font ? font : SW_F_LABEL, 0);
    lv_obj_set_style_text_align(d->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(d->label);
    lv_obj_set_user_data(b, d);
    lv_obj_add_event_cb(b, btn_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, btn_event, LV_EVENT_DELETE, NULL);
    d->on = 0;
    paint(b, d);
    return b;
}

static void repeat_event(lv_event_t *e) {
    btn_t *d = lv_obj_get_user_data(lv_event_get_target(e));
    if (d && d->cb) d->cb(d->ud);
}

void sw_btn_repeat(lv_obj_t *b) {
    lv_obj_add_event_cb(b, repeat_event, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
}

void sw_btn_set(lv_obj_t *b, bool on) {
    btn_t *d = lv_obj_get_user_data(b);
    if (!d || d->on == (int8_t)on) return;
    d->on = on;
    paint(b, d);
}

bool sw_btn_is_on(lv_obj_t *b) {
    btn_t *d = lv_obj_get_user_data(b);
    return d && d->on > 0;
}

void sw_btn_text(lv_obj_t *b, const char *text) {
    btn_t *d = lv_obj_get_user_data(b);
    if (d) lv_label_set_text(d->label, text);
}

void sw_btn_accent(lv_obj_t *b, uint32_t accent) {
    btn_t *d = lv_obj_get_user_data(b);
    if (!d || d->accent == accent) return;
    d->accent = accent;
    paint(b, d);
}

lv_obj_t *sw_btn_label(lv_obj_t *b) {
    btn_t *d = lv_obj_get_user_data(b);
    return d ? d->label : NULL;
}

lv_obj_t *sw_square(lv_obj_t *parent, int x, int y, int s, uint32_t color, const char *sym,
                    sw_cb_t cb, void *ud) {
    lv_obj_t *b = sw_btn(parent, sym, x, y, s, s, color, cb, ud, SW_F_LABEL);
    sw_btn_set(b, true);
    lv_obj_set_style_text_color(sw_btn_label(b), sw_c(SW_TEXT), 0);
    return b;
}

// --- slider -------------------------------------------------------------------------------

struct sw_slider {
    lv_obj_t *box, *fill, *val;
    int w;
    float v0, shown;
    int32_t x0;
    bool drag;
    sw_slider_ops_t ops;
    void *ud;
    char txt[24];
};

static void slider_draw(sw_slider_t *s) {
    float v = s->ops.get(s->ud);
    if (v != s->shown) {
        s->shown = v;
        int px = (int)(v * (s->w - 2) + 0.5f);
        lv_obj_set_width(s->fill, px > 1 ? px : 1);
    }
    char t[24] = "";
    if (s->ops.text) s->ops.text(s->ud, t, sizeof t);
    if (strcmp(t, s->txt)) {
        snprintf(s->txt, sizeof s->txt, "%s", t);
        lv_label_set_text(s->val, t);
    }
}

void sw_slider_refresh(sw_slider_t *s) { slider_draw(s); }

static void slider_event(lv_event_t *e) {
    sw_slider_t *s = lv_event_get_user_data(e);
    lv_point_t p = {0, 0};
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s->x0 = p.x;
        s->v0 = s->ops.get(s->ud);
        s->drag = true;
        break;
    case LV_EVENT_PRESSING: {
        if (!s->drag) break;
        float v = s->v0 + (float)(p.x - s->x0) / (float)(s->w > 40 ? s->w : 40);
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        s->ops.set(s->ud, v);
        slider_draw(s);
        break;
    }
    default:
        s->drag = false;
        break;
    }
}

sw_slider_t *sw_slider(lv_obj_t *parent, const char *label, int x, int y, int w, int h,
                       uint32_t color, const sw_slider_ops_t *ops, void *ud) {
    sw_slider_t *s = calloc(1, sizeof *s);
    s->ops = *ops;
    s->ud = ud;
    s->w = w;
    s->shown = -1;
    sw_label(parent, label, SW_F_SMALL, SW_DIM, x, y, w / 2, false);
    s->val = sw_label(parent, "", SW_F_SMALL, color, x + w / 2, y, w / 2, false);
    lv_obj_set_style_text_align(s->val, LV_TEXT_ALIGN_RIGHT, 0);
    s->box = sw_plain(lv_obj_create(parent), 0x242424, 3);
    lv_obj_set_pos(s->box, x, y + 17);
    lv_obj_set_size(s->box, w, h - 17);
    lv_obj_add_flag(s->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s->box, 6);
    s->fill = sw_plain(lv_obj_create(s->box), (int32_t)color, 3);
    lv_obj_set_pos(s->fill, 1, 1);
    lv_obj_set_size(s->fill, 1, h - 19);
    lv_obj_remove_flag(s->fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->box, slider_event, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->box, slider_event, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(s->box, slider_event, LV_EVENT_RELEASED, s);
    lv_obj_add_event_cb(s->box, slider_event, LV_EVENT_PRESS_LOST, s);
    slider_draw(s);
    return s;
}
