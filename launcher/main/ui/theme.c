// theme.c - see theme.h.
#include "theme.h"

lv_color_t th_c(uint8_t c) {
    return lv_color_make((c >> 5) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 255 / 3);
}

void th_init(void) {
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *t = lv_theme_default_init(d, th_c(TH_ORANGE), th_c(TH_CYAN), true,
                                          &lv_font_montserrat_14);
    lv_display_set_theme(d, t);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, th_c(TH_BG), 0);
    lv_obj_set_style_text_color(scr, th_c(TH_FG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *th_box(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *th_hline(lv_obj_t *parent, int x, int y, int w) {
    lv_obj_t *o = th_box(parent, x, y, w, 1);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, th_c(TH_LINE), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *th_text(lv_obj_t *parent, const char *txt, int x, int y, uint8_t color,
                  const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, th_c(color), 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    return l;
}

lv_obj_t *th_title(lv_obj_t *parent, const char *txt, int x, int y, uint8_t color) {
    lv_obj_t *l = th_text(parent, txt, x, y, color, &lv_font_montserrat_20);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

static void style_btn(lv_obj_t *b) {
    lv_obj_set_style_bg_color(b, th_c(TH_BG), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, th_c(TH_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, th_c(TH_FG), 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    // press feedback: orange outline + white text, instantly
    lv_obj_set_style_border_color(b, th_c(TH_ORANGE), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 2, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, th_c(TH_WHITE), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, th_c(TH_LINE), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, th_c(RGB332(36, 36, 85)), LV_STATE_DISABLED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *th_btn(lv_obj_t *parent, const char *txt, int x, int y, int w, int h, lv_event_cb_t cb,
                 void *ud) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    style_btn(b);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    // act on PRESS: the UI reacts the moment a finger lands
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, ud);
    return b;
}

void th_btn_set_text(lv_obj_t *btn, const char *txt) {
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) lv_label_set_text(l, txt);
}

void th_btn_set_active(lv_obj_t *btn, bool on) {
    lv_obj_set_style_bg_color(btn, th_c(on ? TH_ORANGE : TH_BG), 0);
    lv_obj_set_style_border_color(btn, th_c(on ? TH_ORANGE : TH_LINE), 0);
    lv_obj_set_style_text_color(btn, th_c(on ? TH_WHITE : TH_FG), 0);
}

void th_btn_set_enabled(lv_obj_t *btn, bool on) {
    if (on)
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
    else
        lv_obj_add_state(btn, LV_STATE_DISABLED);
}

lv_obj_t *th_square(lv_obj_t *parent, const char *symbol, int x, int y, int w, int h,
                    uint8_t color, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = th_btn(parent, symbol, x, y, w, h, cb, ud);
    lv_obj_set_style_bg_color(b, th_c(color), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_text_color(b, th_c(TH_WHITE), 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_20, 0);
    return b;
}
