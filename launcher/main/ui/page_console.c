// page_console.c - the on-screen debug REPL (same commands as the USB
// serial console; type "help").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "theme.h"
#include "ui.h"

#define MAX_TEXT 12000   // keep the scrollback bounded

static struct {
    lv_obj_t *out, *in, *kb, *b_run, *quick;
    uint32_t cursor;
    bool kb_open;
} C;

static void layout(void) {
    // With the keyboard up, the output shrinks and the input sits above it.
    int out_h = C.kb_open ? 188 : 380;
    lv_obj_set_height(C.out, out_h);
    lv_obj_set_y(C.in, 8 + out_h + 8);
    lv_obj_set_y(C.b_run, 8 + out_h + 8);
    if (C.kb_open) {
        lv_obj_remove_flag(C.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(C.quick, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(C.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(C.quick, LV_OBJ_FLAG_HIDDEN);
    }
}

static void run(const char *line) {
    if (!line[0]) return;
    console_submit(line);
}

static void run_cb(lv_event_t *e) {
    run(lv_textarea_get_text(C.in));
    lv_textarea_set_text(C.in, "");
}

static void in_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_FOCUSED || c == LV_EVENT_CLICKED) {
        C.kb_open = true;
        layout();
    } else if (c == LV_EVENT_READY) {
        run_cb(e);
    } else if (c == LV_EVENT_CANCEL) {
        C.kb_open = false;
        layout();
    }
}

static void quick_cb(lv_event_t *e) { run((const char *)lv_event_get_user_data(e)); }

static void clear_cb(lv_event_t *e) { lv_textarea_set_text(C.out, ""); }

void console_page_create(lv_obj_t *p) {
    C.out = lv_textarea_create(p);
    lv_obj_set_pos(C.out, 16, 8);
    lv_obj_set_size(C.out, SCR_W - 32, 380);
    lv_obj_set_style_text_font(C.out, &lv_font_unscii_16, 0);
    lv_obj_set_style_bg_color(C.out, th_c(TH_BG), 0);
    lv_obj_set_style_border_color(C.out, th_c(TH_LINE), 0);
    lv_obj_set_style_border_width(C.out, 1, 0);
    lv_obj_set_style_text_color(C.out, th_c(TH_FG), 0);
    lv_obj_remove_flag(C.out, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_textarea_set_cursor_click_pos(C.out, false);
    lv_obj_set_style_opa(C.out, LV_OPA_TRANSP, LV_PART_CURSOR);

    C.in = lv_textarea_create(p);
    lv_obj_set_pos(C.in, 16, 396);
    lv_obj_set_size(C.in, SCR_W - 32 - 128 - 12, 44);
    lv_textarea_set_one_line(C.in, true);
    lv_textarea_set_placeholder_text(C.in, "type a command (help)");
    lv_obj_set_style_text_font(C.in, &lv_font_unscii_16, 0);
    lv_obj_set_style_bg_color(C.in, th_c(TH_BG), 0);
    lv_obj_set_style_border_color(C.in, th_c(TH_ORANGE), 0);
    lv_obj_set_style_text_color(C.in, th_c(TH_WHITE), 0);
    lv_obj_add_event_cb(C.in, in_event, LV_EVENT_ALL, NULL);
    C.b_run = th_btn(p, "RUN", SCR_W - 16 - 128, 396, 128, 44, run_cb, NULL);
    th_btn_set_active(C.b_run, true);

    static const char *const Q[] = {"help", "free", "tasks", "df", "ls /apps", "apps",
                                    "slots", "wifi", "i2cscan"};
    C.quick = th_box(p, 16, 452, SCR_W - 32, 80);
    for (int i = 0; i < 9; i++) {
        char up[16];
        snprintf(up, sizeof up, "%s", Q[i]);
        for (char *c = up; *c; c++) *c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
        th_btn(C.quick, up, (i % 9) * 100, 0, 92, 40, quick_cb, (void *)Q[i]);
    }
    th_btn(C.quick, "CLEAR", 0, 46, 92, 34, clear_cb, NULL);
    th_text(C.quick, "also on the USB serial port at 115200 baud", 108, 54, TH_DIM,
            &lv_font_montserrat_14);

    C.kb = lv_keyboard_create(p);
    lv_obj_set_size(C.kb, SCR_W, 280);
    lv_obj_align(C.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(C.kb, C.in);
    lv_obj_add_flag(C.kb, LV_OBJ_FLAG_HIDDEN);
}

void console_page_show(void) {
    console_page_poll();
}

void console_page_poll(void) {
    static char buf[2048];
    size_t n;
    bool added = false;
    while ((n = console_text_read(&C.cursor, buf, sizeof buf - 1)) > 0) {
        buf[n] = 0;
        lv_textarea_add_text(C.out, buf);
        added = true;
    }
    if (added) {
        const char *t = lv_textarea_get_text(C.out);
        size_t len = strlen(t);
        if (len > MAX_TEXT) {
            // copy first: set_text may free the buffer `t` points into
            char *keep = strdup(t + len - MAX_TEXT / 2);
            if (keep) {
                lv_textarea_set_text(C.out, keep);
                free(keep);
            }
        }
        lv_textarea_set_cursor_pos(C.out, LV_TEXTAREA_CURSOR_LAST);   // keep the newest visible
    }
}
