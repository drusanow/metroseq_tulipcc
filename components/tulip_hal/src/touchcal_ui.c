// touchcal_ui.c - touchscreen calibration screen, shared by the launcher
// and apps. The result is stored device-wide (tulip/settings.h).
//
// Flow:
//   PROMPT  at boot: tap the LEFT half to calibrate, the RIGHT half (or
//           wait 5 s) to keep the saved calibration (Tulip's default if
//           none was ever saved).
//   TARGET  five crosses (4 corners + centre): touch each and lift.
//   RESULT  the new calibration is applied live; draw on the screen to
//           test it, then SAVE / RETRY / CANCEL. 20 s with no touch =
//           CANCEL, so a bad result can never lock you out.
//   FAILED  taps didn't fit one consistent mapping: try again or keep
//           the current calibration.
//
// PROMPT, TARGET and FAILED read RAW controller coordinates through the
// display's touch hook and offer only huge half-screen choices, so they
// work even when the current calibration is badly wrong.
#include "tulip/touchcal_ui.h"

#include <stdio.h>

#include "lvgl.h"
#include "tulip/board.h"
#include "tulip/gt911.h"
#include "tulip/settings.h"
#include "tulip/touch.h"
#include "tulip/touch_cal.h"

#define LCD_W LCD_H_RES
#define LCD_H LCD_V_RES

// --- tiny widget kit (this file must not depend on any app's UI code) ------

#define RGB332(r, g, b) ((uint8_t)((((r) >> 5) << 5) | (((g) >> 5) << 2) | ((b) >> 6)))
#define C_BG     0
#define C_BTN    RGB332(60, 110, 200)
#define C_BTN_DK RGB332(50, 55, 75)
#define C_PLAY   RGB332(40, 170, 90)
#define C_FLASH  RGB332(255, 140, 0)
#define C_ON     RGB332(0, 210, 120)

typedef void (*ui_cb_t)(intptr_t arg);
typedef struct {
    lv_obj_t *obj;
    ui_cb_t cb;
} ui_btn_t;

static lv_color_t c332(uint8_t c) {
    return lv_color_make((c >> 5) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 255 / 3);
}

static lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h, uint8_t color,
                          int radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, c332(color), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *ui_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

static void btn_event(lv_event_t *e) {
    ui_btn_t *b = lv_event_get_user_data(e);
    b->cb(0);
}

static ui_btn_t *ui_btn(lv_obj_t *parent, const char *text, int x, int w, int h, ui_cb_t cb,
                        intptr_t arg, uint8_t color, int y, void *unused) {
    static ui_btn_t pool[3];
    static int n;
    ui_btn_t *b = &pool[n++ % 3];
    b->cb = cb;
    b->obj = ui_panel(parent, x, y, w, h, color, 5);
    lv_obj_add_flag(b->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(b->obj, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b->obj, 3, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_PRESSED, b);
    lv_obj_t *l = ui_label(b->obj, text, 0, 0);
    lv_obj_center(l);
    return b;
}

static void (*s_msg)(const char *);
static void (*s_done)(void);

void tulip_touchcal_set_notify(void (*msg)(const char *)) { s_msg = msg; }

static void ui_header_info(const char *t) {
    if (s_msg) s_msg(t);
}

#define NPTS              5
#define PROMPT_MS         5000
#define TOUCH_WAIT_MS     10000   // give up on the prompt if touch never comes up
#define RESULT_TIMEOUT_MS 20000
#define MAX_SAMPLES       48
#define QLEN              16

static const int16_t TX[NPTS] = {64, LCD_W - 64, LCD_W - 64, 64, LCD_W / 2};
static const int16_t TY[NPTS] = {64, 64, LCD_H - 64, LCD_H - 64, LCD_H / 2};

typedef enum { TC_OFF, TC_PROMPT, TC_TARGET, TC_FAILED, TC_RESULT } tc_state_t;

static struct {
    tc_state_t st;
    lv_obj_t *ov;
    lv_obj_t *title, *sub;
    lv_obj_t *left, *right, *left_lbl, *right_lbl;
    lv_obj_t *ring, *hline, *vline;
    lv_obj_t *marks[NPTS], *dot;
    ui_btn_t *save_b, *retry_b, *cancel_b;
    int target;
    float rx[NPTS], ry[NPTS];
    float sx[MAX_SAMPLES], sy[MAX_SAMPLES];
    int ns;
    bool finger_down;   // a press that began before this step: ignore it
    bool pressing;
    float old_cal[6], new_cal[6];
    touch_cal_fit_t fit;
    uint32_t t0, last_touch, created;
    int countdown;
    touch_event_t q[QLEN];
    uint8_t qh, qt;
} T;

// --- raw touch hook (runs in the LVGL task, inside the indev read) ---------

static bool hook(const touch_event_t *ev) {
    uint8_t n = (T.qh + 1) % QLEN;
    if (n != T.qt) {
        T.q[T.qh] = *ev;
        T.qh = n;
    }
    T.last_touch = lv_tick_get();
    return T.st != TC_RESULT;   // RESULT: let LVGL use the new calibration
}

static bool next_raw(touch_event_t *ev) {
    if (T.qt == T.qh) return false;
    *ev = T.q[T.qt];
    T.qt = (T.qt + 1) % QLEN;
    return true;
}

// --- building blocks ---------------------------------------------------------

static void show(lv_obj_t *o, bool on) {
    if (on)
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *line(int w, int h) {
    lv_obj_t *o = ui_panel(T.ov, 0, 0, w, h, RGB332(255, 255, 255), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void finish(void);
static void begin_targets(bool finger_down);

static void btn_save(intptr_t a) {
    for (int i = 0; i < 6; i++) g_tulip.cal[i] = T.new_cal[i];
    g_tulip.cal_valid = 1;
    tulip_settings_save();
    ui_header_info("touch calibration saved");
    finish();
}

static void btn_retry(intptr_t a) {
    gt911_set_cal(T.old_cal);
    begin_targets(true);   // the RETRY press itself is still down
}

static void btn_cancel(intptr_t a) {
    gt911_set_cal(T.old_cal);
    ui_header_info("touch calibration unchanged");
    finish();
}

// RESULT: a dot follows the finger so the new calibration can be judged
static void test_draw(lv_event_t *e) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_set_pos(T.dot, p.x - 8, p.y - 8);
    show(T.dot, true);
    T.last_touch = lv_tick_get();
}

static void build(void) {
    if (T.ov) return;
    T.ov = ui_panel(lv_layer_top(), 0, 0, LCD_W, LCD_H, C_BG, 0);
    lv_obj_add_flag(T.ov, LV_OBJ_FLAG_CLICKABLE);   // swallow taps meant for the UI below
    lv_obj_add_event_cb(T.ov, test_draw, LV_EVENT_PRESSING, NULL);

    T.left = ui_panel(T.ov, 0, 0, LCD_W / 2 - 2, LCD_H, C_BTN, 0);
    T.right = ui_panel(T.ov, LCD_W / 2 + 2, 0, LCD_W / 2 - 2, LCD_H, C_BTN_DK, 0);
    lv_obj_remove_flag(T.left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(T.right, LV_OBJ_FLAG_CLICKABLE);
    T.left_lbl = lv_label_create(T.left);
    T.right_lbl = lv_label_create(T.right);
    lv_obj_set_style_text_align(T.left_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(T.right_lbl, LV_TEXT_ALIGN_CENTER, 0);

    T.ring = ui_panel(T.ov, 0, 0, 44, 44, C_BG, 22);
    lv_obj_set_style_border_width(T.ring, 3, 0);
    lv_obj_set_style_border_color(T.ring, c332(C_FLASH), 0);
    lv_obj_remove_flag(T.ring, LV_OBJ_FLAG_CLICKABLE);
    T.hline = line(70, 2);
    T.vline = line(2, 70);
    for (int i = 0; i < NPTS; i++) {
        T.marks[i] = ui_panel(T.ov, TX[i] - 4, TY[i] - 4, 8, 8, C_FLASH, 4);
        lv_obj_remove_flag(T.marks[i], LV_OBJ_FLAG_CLICKABLE);
    }
    T.dot = ui_panel(T.ov, 0, 0, 16, 16, C_ON, 8);
    lv_obj_remove_flag(T.dot, LV_OBJ_FLAG_CLICKABLE);

    T.title = ui_label(T.ov, "", 0, 150);
    T.sub = ui_label(T.ov, "", 0, 190);
    lv_obj_set_width(T.title, LCD_W);
    lv_obj_set_width(T.sub, LCD_W);
    lv_obj_set_style_text_align(T.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(T.sub, LV_TEXT_ALIGN_CENTER, 0);

    int bw = 240, by = LCD_H - 110, gap = (LCD_W - 3 * bw) / 4;
    T.save_b = ui_btn(T.ov, "SAVE", gap, bw, 70, btn_save, 0, C_PLAY, by, NULL);
    T.retry_b = ui_btn(T.ov, "RETRY", 2 * gap + bw, bw, 70, btn_retry, 0, C_BTN, by, NULL);
    T.cancel_b = ui_btn(T.ov, "CANCEL", 3 * gap + 2 * bw, bw, 70, btn_cancel, 0, C_BTN_DK, by,
                        NULL);
}

static void hide_all(void) {
    show(T.left, false);
    show(T.right, false);
    show(T.ring, false);
    show(T.hline, false);
    show(T.vline, false);
    for (int i = 0; i < NPTS; i++) show(T.marks[i], false);
    show(T.dot, false);
    show(T.save_b->obj, false);
    show(T.retry_b->obj, false);
    show(T.cancel_b->obj, false);
    lv_label_set_text(T.title, "");
    lv_label_set_text(T.sub, "");
}

static void open_overlay(void) {
    build();
    show(T.ov, true);
    lv_obj_move_foreground(T.ov);
    T.qh = T.qt = 0;
    display_set_touch_hook(hook);
    gt911_get_cal(T.old_cal);
}

static void finish(void) {
    display_set_touch_hook(NULL);
    T.st = TC_OFF;
    if (T.ov) show(T.ov, false);
    void (*done)(void) = s_done;
    s_done = NULL;
    if (done) done();
}

// Raw x on the left half of the panel? (works whatever the calibration)
static bool raw_left(const touch_event_t *ev) {
    int xm, ym;
    gt911_raw_max(&xm, &ym);
    return ev->rx < xm / 2;
}

// --- states ------------------------------------------------------------------

static void halves(const char *l, const char *r) {
    show(T.left, true);
    show(T.right, true);
    lv_label_set_text(T.left_lbl, l);
    lv_obj_center(T.left_lbl);
    lv_label_set_text(T.right_lbl, r);
    lv_obj_center(T.right_lbl);
}

static void draw_prompt(int secs) {
    char r[96];
    const char *which = g_tulip.cal_valid ? "SAVED" : "DEFAULT";
    if (secs >= 0)
        snprintf(r, sizeof r, "USE %s CALIBRATION\n\ntap this side\nor wait %d s", which, secs);
    else
        snprintf(r, sizeof r, "USE %s CALIBRATION\n\ntap this side", which);
    halves("CALIBRATE TOUCH\n\ntap this side", r);
}

static void draw_target(void) {
    int x = TX[T.target], y = TY[T.target];
    lv_obj_set_pos(T.ring, x - 22, y - 22);
    lv_obj_set_pos(T.hline, x - 35, y - 1);
    lv_obj_set_pos(T.vline, x - 1, y - 35);
    show(T.ring, true);
    show(T.hline, true);
    show(T.vline, true);
    lv_obj_set_style_border_color(T.ring, c332(C_FLASH), 0);
    lv_label_set_text_fmt(T.title, "Touch the centre of the cross, then lift   (%d / %d)",
                          T.target + 1, NPTS);
    lv_label_set_text(T.sub, "Use a fingertip or stylus, as you will when playing.");
}

static void begin_targets(bool finger_down) {
    hide_all();
    display_set_touch_hook(hook);
    T.st = TC_TARGET;
    T.target = 0;
    T.ns = 0;
    T.pressing = false;
    T.finger_down = finger_down;
    T.qh = T.qt = 0;
    draw_target();
}

static void solve(void) {
    static const float sx[NPTS] = {TX[0], TX[1], TX[2], TX[3], TX[4]};
    static const float sy[NPTS] = {TY[0], TY[1], TY[2], TY[3], TY[4]};
    hide_all();
    bool ok = touch_cal_solve(T.rx, T.ry, sx, sy, NPTS, T.new_cal, &T.fit);
    if (!ok) {
        T.st = TC_FAILED;
        char l[64];
        snprintf(l, sizeof l, "TRY AGAIN\n\ntap this side");
        halves(l, "KEEP CURRENT\nCALIBRATION\n\ntap this side");
        return;
    }
    gt911_set_cal(T.new_cal);   // live, so the test below uses it
    T.st = TC_RESULT;
    T.last_touch = lv_tick_get();
    for (int i = 0; i < NPTS; i++) show(T.marks[i], true);
    show(T.save_b->obj, true);
    show(T.retry_b->obj, true);
    show(T.cancel_b->obj, true);
    char buf[96];
    snprintf(buf, sizeof buf, "Calibrated: average error %.1f px, worst %.1f px", T.fit.rms_px,
             T.fit.max_px);
    lv_label_set_text(T.title, buf);
    lv_label_set_text(T.sub, "Touch around the screen to test - the green dot should sit under "
                             "your finger.\nNo touch for 20 s cancels.");
}

// Average a press's samples, skipping the first one (the contact edge).
static void take_point(void) {
    int from = T.ns >= 3 ? 1 : 0;
    float ax = 0, ay = 0;
    for (int i = from; i < T.ns; i++) {
        ax += T.sx[i];
        ay += T.sy[i];
    }
    T.rx[T.target] = ax / (T.ns - from);
    T.ry[T.target] = ay / (T.ns - from);
}

// --- public ----------------------------------------------------------------------

void tulip_touchcal_boot_prompt(void (*done)(void)) {
    s_done = done;
    open_overlay();
    hide_all();
    T.st = TC_PROMPT;
    T.created = lv_tick_get();
    T.t0 = 0;   // countdown starts once the touch controller is up
    T.countdown = -2;
    T.finger_down = false;
    draw_prompt(-1);
    lv_label_set_text(T.title, "");
}

void tulip_touchcal_start(void (*done)(void)) {
    s_done = done;
    open_overlay();
    begin_targets(true);   // the SET-page press is still down
}

bool tulip_touchcal_active(void) { return T.st != TC_OFF; }

void tulip_touchcal_poll(void) {
    if (T.st == TC_OFF) return;
    uint32_t now = lv_tick_get();
    touch_event_t ev;

    switch (T.st) {
    case TC_PROMPT:
        if (!T.t0) {
            if (gt911_ready()) {
                T.t0 = now;
            } else if (now - T.created > TOUCH_WAIT_MS) {
                finish();   // no touchscreen: nothing to calibrate
                return;
            }
        } else {
            int left = (int)(PROMPT_MS - (now - T.t0) + 999) / 1000;
            if (left <= 0) {
                finish();
                return;
            }
            if (left != T.countdown) {
                T.countdown = left;
                draw_prompt(left);
            }
        }
        while (next_raw(&ev)) {
            if (ev.pressed) continue;   // decide on lift
            if (raw_left(&ev)) {
                begin_targets(false);
            } else {
                finish();
            }
            return;
        }
        break;

    case TC_TARGET:
        while (next_raw(&ev)) {
            if (T.finger_down) {   // wait out a press from before this step
                if (!ev.pressed) T.finger_down = false;
                continue;
            }
            if (ev.pressed) {
                T.pressing = true;
                if (T.ns < MAX_SAMPLES) {
                    T.sx[T.ns] = ev.rx;
                    T.sy[T.ns] = ev.ry;
                    T.ns++;
                }
                lv_obj_set_style_border_color(T.ring, c332(C_ON), 0);
            } else if (T.pressing && T.ns > 0) {
                take_point();
                T.pressing = false;
                T.ns = 0;
                if (++T.target == NPTS) {
                    solve();
                    return;
                }
                draw_target();
            }
        }
        break;

    case TC_FAILED:
        while (next_raw(&ev)) {
            if (ev.pressed) continue;
            if (raw_left(&ev))
                begin_targets(false);
            else
                finish();
            return;
        }
        break;

    case TC_RESULT:
        while (next_raw(&ev)) {
        }
        if (now - T.last_touch > RESULT_TIMEOUT_MS) btn_cancel(0);
        break;

    default:
        break;
    }
}
