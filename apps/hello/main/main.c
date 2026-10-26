// hello - the smallest useful Tulip app, as a template.
//
//   idf.py build && ../../tools/tulip.py --pin PIN push build/hello.bin --launch
#include <stdio.h>

#include "lvgl.h"
#include "tulip/display.h"
#include "tulip/input.h"
#include "tulip/settings.h"
#include "tulip/sys.h"

static lv_obj_t *s_count;
static int s_n;

static void on_tap(lv_event_t *e) {
    lv_label_set_text_fmt(s_count, "tapped %d times", ++s_n);
}

static void on_home(lv_event_t *e) {
    tulip_exit_to_launcher();   // never returns
}

static lv_obj_t *button(const char *txt, int x, int y, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(lv_screen_active());
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, 200, 60);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

// Runs in the LVGL task: build the UI here.
static void ui_create(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, "Hello, Tulip");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_pos(t, 40, 40);
    s_count = lv_label_create(scr);
    lv_label_set_text(s_count, "tap the button");
    lv_obj_set_style_text_color(s_count, lv_color_white(), 0);
    lv_obj_set_pos(s_count, 40, 100);
    button("TAP ME", 40, 160, on_tap);
    button("HOME", 1024 - 240, 40, on_home);
}

// Runs every LVGL loop pass (~100+ Hz): poll your own state here.
static void ui_poll(void) {}

void app_main(void) {
    tulip_app_init();          // under the launcher: a reset goes back to it
    tulip_settings_load();     // device-wide touch calibration + brightness
    tulip_input_start(0, 5);   // I2C bus + touch polling (core 0)
    tulip_display_start(&(tulip_display_cfg_t){
        .core = 1, .priority = 4, .ui_create = ui_create, .ui_poll = ui_poll});
}
