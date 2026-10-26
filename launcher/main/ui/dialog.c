// dialog.c - see dialog.h.
#include "dialog.h"

#include <stdio.h>
#include <string.h>

#include "theme.h"

static struct {
    lv_obj_t *veil, *box, *ta, *kb;
    dlg_ok_cb ok;
    dlg_text_cb ok_text;
    void *ud;
} D;

bool dlg_open(void) { return D.veil != NULL; }

void dlg_close(void) {
    // async: we are often inside an event of one of the dialog's own objects
    if (D.veil) lv_obj_delete_async(D.veil);
    memset(&D, 0, sizeof D);
}

static void on_cancel(lv_event_t *e) { dlg_close(); }

static void on_ok(lv_event_t *e) {
    dlg_ok_cb ok = D.ok;
    dlg_text_cb okt = D.ok_text;
    void *ud = D.ud;
    char text[96] = {0};
    if (D.ta) snprintf(text, sizeof text, "%s", lv_textarea_get_text(D.ta));
    dlg_close();
    if (ok) ok(ud);
    if (okt) okt(text, ud);
}

static void ta_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) on_ok(e);
    if (c == LV_EVENT_CANCEL) on_cancel(e);
}

static void open_box(const char *title, int h, int y) {
    dlg_close();
    D.veil = th_box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(D.veil, th_c(TH_BG), 0);
    lv_obj_set_style_bg_opa(D.veil, LV_OPA_80, 0);
    lv_obj_add_flag(D.veil, LV_OBJ_FLAG_CLICKABLE);   // block the page below
    D.box = th_box(D.veil, (SCR_W - 620) / 2, y, 620, h);
    lv_obj_set_style_bg_opa(D.box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(D.box, th_c(TH_BG), 0);
    lv_obj_set_style_border_color(D.box, th_c(TH_ORANGE), 0);
    lv_obj_set_style_border_width(D.box, 1, 0);
    th_title(D.box, title, 20, 16, TH_ORANGE);
}

void dlg_confirm(const char *title, const char *body, const char *ok_label, bool danger,
                 dlg_ok_cb ok, void *ud) {
    open_box(title, 220, 170);
    D.ok = ok;
    D.ud = ud;
    lv_obj_t *t = th_text(D.box, body, 20, 60, TH_FG, NULL);
    lv_obj_set_width(t, 580);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_t *b = th_btn(D.box, ok_label, 620 - 20 - 160, 220 - 20 - 44, 160, 44, on_ok, NULL);
    if (danger) {
        lv_obj_set_style_bg_color(b, th_c(TH_RED), 0);
        lv_obj_set_style_border_color(b, th_c(TH_RED), 0);
        lv_obj_set_style_text_color(b, th_c(TH_WHITE), 0);
    } else {
        th_btn_set_active(b, true);
    }
    th_btn(D.box, "CANCEL", 620 - 20 - 160 - 16 - 160, 220 - 20 - 44, 160, 44, on_cancel, NULL);
}

void dlg_prompt(const char *title, const char *initial, const char *ok_label, bool password,
                dlg_text_cb ok, void *ud) {
    open_box(title, 190, 40);
    D.ok_text = ok;
    D.ud = ud;
    D.ta = lv_textarea_create(D.box);
    lv_obj_set_pos(D.ta, 20, 60);
    lv_obj_set_size(D.ta, 580, 50);
    lv_textarea_set_one_line(D.ta, true);
    lv_textarea_set_max_length(D.ta, 64);
    lv_textarea_set_password_mode(D.ta, password);
    lv_textarea_set_text(D.ta, initial ? initial : "");
    lv_obj_set_style_bg_color(D.ta, th_c(TH_BG), 0);
    lv_obj_set_style_border_color(D.ta, th_c(TH_LINE), 0);
    lv_obj_set_style_text_color(D.ta, th_c(TH_WHITE), 0);
    lv_obj_add_state(D.ta, LV_STATE_FOCUSED);   // show the cursor
    lv_obj_add_event_cb(D.ta, ta_event, LV_EVENT_ALL, NULL);
    lv_obj_t *b = th_btn(D.box, ok_label, 620 - 20 - 140, 124, 140, 44, on_ok, NULL);
    th_btn_set_active(b, true);
    th_btn(D.box, "CANCEL", 620 - 20 - 140 - 16 - 140, 124, 140, 44, on_cancel, NULL);
    D.kb = lv_keyboard_create(D.veil);
    lv_obj_set_size(D.kb, SCR_W, 300);
    lv_obj_align(D.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(D.kb, D.ta);
}
