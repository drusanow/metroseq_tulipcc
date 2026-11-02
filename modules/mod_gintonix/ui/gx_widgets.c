// gx_widgets.c - GinTonix's touch controls: the rotary pot (drag up/down),
// the wide fader, the full-screen list picker and the name dialogs.
// (Knob, FatBar, PickBtn / open_picker, text_dialog in gintonix.py.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_ui_int.h"

static lv_obj_t *plain(lv_obj_t *o, int32_t bg, int radius) { return sw_plain(o, bg, radius); }

static int32_t touch_x(void) {
    lv_point_t p = {0, 0};
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    return p.x;
}

static int32_t touch_y(void) {
    lv_point_t p = {0, 0};
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    return p.y;
}

// --- the rotary pot ----------------------------------------------------------------------

static void knob_event(lv_event_t *e) {
    gx_knob_t *k = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        k->y0 = touch_y();
        k->v0 = get_val(k->key);
        k->dragging = true;
        set_focus(k->key);
        if (A.learn_on) set_learn_target(k->key);
        break;
    case LV_EVENT_PRESSING: {
        if (!k->dragging) break;
        float v = k->v0 + (float)(k->y0 - touch_y()) / DRAG_PX;
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        if (v - get_val(k->key) > 0.0005f || get_val(k->key) - v > 0.0005f) {
            set_val(k->key, v);
            knob_refresh(k);
        }
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        k->dragging = false;
        break;
    default:
        break;
    }
}

gx_knob_t *knob_new(lv_obj_t *parent, int key, int x, int y, uint32_t accent, int w, int bipolar,
                    int top, bool big) {
    gx_knob_t *k = calloc(1, sizeof *k);
    k->key = key;
    k->accent = accent;
    k->shown = -1;
    k->big = big;
    lv_obj_t *cell = k->cell = plain(lv_obj_create(parent), -1, 0);
    lv_obj_set_size(cell, w, CELL_H);
    lv_obj_set_pos(cell, x, y);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(cell, sw_c(accent), 0);
    k->title = sw_label(cell, knob_title(key), F_TINY, C_TEXT, 0, top + 2, w, true);
    int kx = (w - KNOB) / 2;
    // thin value ring (the arc's own knob is hidden) ...
    lv_obj_t *a = k->arc = lv_arc_create(cell);
    lv_obj_set_size(a, KNOB, KNOB);
    lv_obj_set_pos(a, kx, top + KNOB_Y);
    lv_arc_set_bg_angles(a, 135, 45);
    lv_arc_set_range(a, 0, 100);
    lv_obj_set_style_arc_width(a, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, sw_c(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, sw_c(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(a, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    if (bipolar < 0) bipolar = (KEY_IS_SEL(key) || KEY_IS_CH(key)) && GX_KNOBS[key & 31].bipolar;
    if (bipolar) lv_arc_set_mode(a, LV_ARC_MODE_SYMMETRICAL);
    // ... a dark cap inside it ...
    lv_obj_t *cap = plain(lv_obj_create(cell), CAP_C, LV_RADIUS_CIRCLE);
    lv_obj_set_size(cap, CAP, CAP);
    lv_obj_set_pos(cap, (w - CAP) / 2, top + KNOB_Y + (KNOB - CAP) / 2);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_color(cap, sw_c(CAP_EDGE), 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    // ... and a white pointer: a narrow wedge of a second arc, centre to rim
    int ps = CAP - 6;
    lv_obj_t *pr = k->ptr = lv_arc_create(cell);
    lv_obj_set_size(pr, ps, ps);
    lv_obj_set_pos(pr, (w - ps) / 2, top + KNOB_Y + (KNOB - ps) / 2);
    lv_arc_set_bg_angles(pr, 0, 360);
    lv_obj_set_style_arc_opa(pr, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(pr, ps / 2 - 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(pr, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pr, sw_c(C_TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(pr, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(pr, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(pr, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pr, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(pr, LV_OBJ_FLAG_CLICKABLE);
    if (big)   // an important value: large, in the accent colour
        k->val = sw_label(cell, "", F_MID, accent, 0, top + KNOB_Y + KNOB + 2, w, true);
    else
        k->val = sw_label(cell, "", F_SMALL, C_DIM, 0, top + KNOB_Y + KNOB + 4, w, true);
    lv_obj_add_event_cb(cell, knob_event, LV_EVENT_PRESSED, k);
    lv_obj_add_event_cb(cell, knob_event, LV_EVENT_PRESSING, k);
    lv_obj_add_event_cb(cell, knob_event, LV_EVENT_RELEASED, k);
    lv_obj_add_event_cb(cell, knob_event, LV_EVENT_PRESS_LOST, k);
    if (g_nknobs < MAX_KNOBS) g_knobs[g_nknobs++] = k;
    knob_refresh(k);
    return k;
}

void knob_refresh(gx_knob_t *k) {
    int iv = (int)(get_val(k->key) * 100 + 0.5f);
    if (iv != k->shown) {
        k->shown = iv;
        lv_arc_set_value(k->arc, iv);
        int av = 135 + 270 * iv / 100;
        lv_arc_set_angles(k->ptr, (av - 5 + 360) % 360, (av + 5) % 360);
    }
    char t[20];
    val_text(k->key, t, sizeof t);
    if (strcmp(t, k->shown_txt)) {
        snprintf(k->shown_txt, sizeof k->shown_txt, "%s", t);
        lv_label_set_text(k->val, t);
    }
}

void knob_retitle(gx_knob_t *k) { lv_label_set_text(k->title, knob_title(k->key)); }

void knob_set_learn(gx_knob_t *k, bool on) {
    if (on == k->learn_mark) return;
    k->learn_mark = on;
    lv_obj_set_style_text_color(k->title, sw_c(on ? LEARN_C : C_TEXT), 0);
}

void knob_set_focus(gx_knob_t *k, bool on) {
    if (on == k->focus_mark) return;
    k->focus_mark = on;
    lv_obj_set_style_border_width(k->cell, on ? 2 : 0, 0);
    lv_obj_set_style_radius(k->cell, on ? 6 : 0, 0);
}

// --- the wide fader ------------------------------------------------------------------------

static void bar_event(lv_event_t *e) {
    gx_bar_t *b = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        b->x0 = touch_x();
        b->v0 = b->get ? b->get() : b->v;
        b->dragging = true;
        if (b->key != KEY_NONE) {   // MORPH: focus fader + MIDI CC learn
            set_focus(b->key);
            if (A.learn_on) set_learn_target(b->key);
        }
        break;
    case LV_EVENT_PRESSING: {
        if (!b->dragging) break;
        float v = b->v0 + (float)(touch_x() - b->x0) / (float)(b->w - b->thumb);
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        if (v - b->v > 0.0005f || b->v - v > 0.0005f) {
            bar_show(b, v);
            if (b->cb) b->cb(v);
        }
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        b->dragging = false;
        break;
    default:
        break;
    }
}

gx_bar_t *bar_new(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, uint32_t dim,
                  bar_cb_t cb, int thumb) {
    gx_bar_t *b = calloc(1, sizeof *b);
    b->w = w;
    b->thumb = thumb;
    b->cb = cb;
    b->v = -1;
    b->key = KEY_NONE;
    b->box = plain(lv_obj_create(parent), STEP_OFF, 4);
    lv_obj_set_size(b->box, w, h);
    lv_obj_set_pos(b->box, x, y);
    lv_obj_add_flag(b->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b->box, 8);
    b->fill = plain(lv_obj_create(b->box), dim, 0);
    lv_obj_set_size(b->fill, 1, h);
    lv_obj_remove_flag(b->fill, LV_OBJ_FLAG_CLICKABLE);
    b->knob = plain(lv_obj_create(b->box), color, 4);
    lv_obj_set_size(b->knob, thumb, h - 8);
    lv_obj_set_pos(b->knob, 0, 4);
    lv_obj_remove_flag(b->knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b->box, bar_event, LV_EVENT_PRESSED, b);
    lv_obj_add_event_cb(b->box, bar_event, LV_EVENT_PRESSING, b);
    lv_obj_add_event_cb(b->box, bar_event, LV_EVENT_RELEASED, b);
    lv_obj_add_event_cb(b->box, bar_event, LV_EVENT_PRESS_LOST, b);
    return b;
}

void bar_show(gx_bar_t *b, float v) {
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    int px = (int)(v * (b->w - b->thumb) + 0.5f);
    int old = (int)(b->v * (b->w - b->thumb) + 0.5f);
    if (px != old || b->v < 0) {
        lv_obj_set_width(b->fill, px + b->thumb / 2 > 1 ? px + b->thumb / 2 : 1);
        lv_obj_set_x(b->knob, px);
    }
    b->v = v;
}

void bar_set_color(gx_bar_t *b, uint32_t color, uint32_t dim) {
    lv_obj_set_style_bg_color(b->knob, sw_c(color), 0);
    lv_obj_set_style_bg_color(b->fill, sw_c(dim), 0);
}

// --- full-screen list picker -----------------------------------------------------------------

#define PICK_COLS 4
#define PICK_ROWS 9
#define PER (PICK_COLS * PICK_ROWS)

static struct {
    lv_obj_t *d, *pg_lbl, *prev, *next;
    lv_obj_t *btns[PER];
    const char *const *opts;
    int n, sel, page, pages;
    pick_cb_t cb;
} P;

bool picker_active(void) { return P.d != NULL; }

static void picker_fill(void) {
    int base = P.page * PER;
    for (int j = 0; j < PER; j++) {
        int i = base + j;
        lv_obj_t *b = P.btns[j];
        if (i < P.n) {
            lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
            sw_btn_text(b, P.opts[i]);
            sw_btn_set(b, i == P.sel);
            if (i != P.sel) lv_obj_set_style_text_color(sw_btn_label(b), sw_c(C_TEXT), 0);
        } else {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
    char t[32] = "";
    if (P.pages > 1) snprintf(t, sizeof t, "%d / %d", P.page + 1, P.pages);
    lv_label_set_text(P.pg_lbl, t);
    if (P.pages > 1) {
        lv_obj_remove_flag(P.prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(P.next, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(P.prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(P.next, LV_OBJ_FLAG_HIDDEN);
    }
}

static void picker_close(void) {
    if (!P.d) return;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);   // the touch must not land on the page below
    lv_obj_delete_async(P.d);
    P.d = NULL;
}

static void picker_page(void *ud) {
    P.page = (P.page + (int)(intptr_t)ud + P.pages) % P.pages;
    picker_fill();
}

static void picker_tap(void *ud) {
    int i = P.page * PER + (int)(intptr_t)ud;
    if (i >= P.n) return;
    pick_cb_t cb = P.cb;
    picker_close();
    if (cb) cb(i);
}

static void picker_cancel(void *ud) { picker_close(); }

void picker_open(const char *title, const char *const *opts, int n, int sel, uint32_t accent,
                 pick_cb_t cb) {
    if (P.d || n <= 0) return;
    P.opts = opts;
    P.n = n;
    P.sel = sel;
    P.cb = cb;
    P.pages = (n + PER - 1) / PER;
    P.page = sel >= 0 ? sel / PER : 0;
    if (P.page >= P.pages) P.page = P.pages - 1;
    lv_obj_t *d = P.d = plain(lv_obj_create(A.scr), C_BG, 0);
    lv_obj_set_size(d, 1024, 600);
    lv_obj_set_pos(d, 0, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    sw_label(d, title, F_BIG, accent, 12, 16, 560, false);
    P.pg_lbl = sw_label(d, "", F_MID, C_DIM, 590, 20, 90, false);
    P.prev = sw_btn(d, "<", 680, 6, 70, 48, accent, picker_page, (void *)-1, F_BIG);
    P.next = sw_btn(d, ">", 756, 6, 70, 48, accent, picker_page, (void *)1, F_BIG);
    sw_btn(d, "CANCEL", 846, 6, 170, 48, ACC, picker_cancel, NULL, F_MID);
    int bw = (1024 - 8 - (PICK_COLS - 1) * 6) / PICK_COLS, bh = 54;
    for (int j = 0; j < PER; j++) {
        int r = j / PICK_COLS, c = j % PICK_COLS;
        lv_obj_t *b = P.btns[j] = sw_btn(d, "", 4 + c * (bw + 6), 64 + r * (bh + 4), bw, bh, accent,
                                         picker_tap, (void *)(intptr_t)j, F_LABEL);
        lv_obj_t *l = sw_btn_label(b);
        lv_obj_set_width(l, bw - 10);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_center(l);
    }
    picker_fill();
}

// --- the button that opens a picker ---------------------------------------------------------

static void pick_show(gx_pick_t *p) {
    const char *t = p->fixed ? p->fixed : (p->n ? p->opts[p->sel] : "");
    sw_btn_text(p->btn, t);
}

static gx_pick_t *s_open_pick;

static void pick_done(int i) {
    gx_pick_t *p = s_open_pick;
    if (!p) return;
    p->sel = i;
    pick_show(p);
    if (p->cb) p->cb(i);
}

static void pick_open(void *ud) {
    gx_pick_t *p = ud;
    s_open_pick = p;
    const char *t = p->title ? p->title() : p->title_txt;
    picker_open(t, (const char *const *)p->opts, p->n, p->sel, p->accent, pick_done);
}

gx_pick_t *pick_new(lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent,
                    const char *title, pick_cb_t cb) {
    gx_pick_t *p = calloc(1, sizeof *p);
    p->accent = accent;
    p->cb = cb;
    snprintf(p->title_txt, sizeof p->title_txt, "%s", title ? title : "");
    p->btn = sw_btn(parent, "", x, y, w, h, accent, pick_open, p, F_LABEL);
    lv_obj_set_style_border_color(p->btn, sw_c(C_BTN_EDGE), 0);
    lv_obj_t *l = sw_btn_label(p->btn);
    lv_obj_set_style_text_color(l, sw_c(C_TEXT), 0);
    lv_obj_set_width(l, w - 10);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_center(l);
    return p;
}

void pick_set_options(gx_pick_t *p, const char *const *opts, int n) {
    for (int i = 0; i < p->n; i++) free(p->opts[i]);
    free(p->opts);
    p->opts = calloc(n > 0 ? n : 1, sizeof(char *));
    for (int i = 0; i < n; i++) p->opts[i] = strdup(opts[i]);
    p->n = n;
    if (p->sel >= n) p->sel = 0;
    pick_show(p);
}

void pick_set_selected(gx_pick_t *p, int i) {
    if (i >= 0 && i < p->n) {
        p->sel = i;
        pick_show(p);
    }
}

void pick_set_text(gx_pick_t *p, const char *fixed) {
    p->fixed = fixed;
    pick_show(p);
}

// --- name dialogs -----------------------------------------------------------------------------

static struct {
    lv_obj_t *d, *ta, *ok, *info;
    text_cb_t cb;
    const char *(*label)(const char *);
    int slot;
    gx_pick_t *slot_pick;
} D;

bool dialog_active(void) { return D.d != NULL; }

static void style_keyboard(lv_obj_t *kb) {
    // black keys with thin outlines, like the buttons
    lv_obj_set_style_bg_color(kb, sw_c(C_BG), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, sw_c(C_BG), LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, sw_c(C_BTN_EDGE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, sw_c(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, F_MID, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, sw_c(ACC_DIM), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, sw_c(ACC), LV_PART_ITEMS | LV_STATE_PRESSED);
}

static void dialog_close(void) {
    if (!D.d) return;
    lv_obj_delete_async(D.d);
    D.d = NULL;
    D.slot_pick = NULL;
}

static void clean_name(const char *in, char *out, int len) {
    // printable ASCII only, trimmed, at most KIT_NAME
    int n = 0;
    while (*in == ' ') in++;
    for (; *in && n < len - 1 && n < KIT_NAME; in++)
        out[n++] = (*in >= 32 && *in < 127) ? *in : '?';
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void text_check(void) {
    if (!D.d || !D.label) return;
    char nm[KIT_NAME + 1];
    clean_name(lv_textarea_get_text(D.ta), nm, sizeof nm);
    const char *t = D.label(nm);
    sw_btn_text(D.ok, t);
    sw_btn_set(D.ok, strcmp(t, "SAVE") != 0);
}

static void ta_event(lv_event_t *e) { text_check(); }

static void text_ok(void *ud) {
    if (!D.d) return;
    char nm[KIT_NAME + 1];
    clean_name(lv_textarea_get_text(D.ta), nm, sizeof nm);
    text_cb_t cb = D.cb;
    dialog_close();
    if (cb) cb(nm);
}

static void text_cancel(void *ud) { dialog_close(); }

static void kb_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_READY)
        text_ok(NULL);
    else
        text_cancel(NULL);
}

static lv_obj_t *dialog_base(const char *title) {
    lv_obj_t *d = D.d = plain(lv_obj_create(A.scr), C_BG, 0);
    lv_obj_set_size(d, 1024, 600);
    lv_obj_set_pos(d, 0, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    sw_label(d, title, F_BIG, ACC, 0, 14, 1024, true);
    sw_label(d, "NAME", F_MID, C_TEXT, 110, 130, 0, false);
    lv_obj_t *ta = D.ta = lv_textarea_create(d);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, KIT_NAME);
    lv_obj_set_style_text_font(ta, F_MID, 0);
    lv_obj_set_style_bg_color(ta, sw_c(C_BG), 0);
    lv_obj_set_style_text_color(ta, sw_c(C_TEXT), 0);
    lv_obj_set_style_border_color(ta, sw_c(C_BTN_EDGE), 0);
    lv_obj_set_size(ta, 400, 48);
    lv_obj_set_pos(ta, 200, 116);
    sw_btn(d, "CANCEL", 620, 118, 150, 44, ACC, text_cancel, NULL, F_MID);
    D.ok = sw_btn(d, "SAVE", 780, 118, 150, 44, ACC, text_ok, NULL, F_MID);
    lv_obj_t *kb = lv_keyboard_create(d);
    lv_obj_set_size(kb, 984, 330);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 190);
    lv_keyboard_set_textarea(kb, ta);
    style_keyboard(kb);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, NULL);
    return d;
}

void text_dialog(const char *title, const char *text, text_cb_t ok,
                 const char *(*label)(const char *name)) {
    if (D.d) return;
    dialog_base(title);
    D.cb = ok;
    D.label = label;
    lv_textarea_set_text(D.ta, text ? text : "");
    lv_obj_add_event_cb(D.ta, ta_event, LV_EVENT_VALUE_CHANGED, NULL);
    text_check();
}

// SAVE KIT: slot + name

static void kit_set_slot(int slot) {
    D.slot = slot;
    if (D.slot_pick) pick_set_selected(D.slot_pick, slot - 1);
    const char *old = slot_name(slot);
    char ph[16];
    snprintf(ph, sizeof ph, "KIT %03d", slot);
    lv_textarea_set_placeholder_text(D.ta, ph);
    if (!old) {
        lv_label_set_text(D.info, "empty slot");
        lv_obj_set_style_text_color(D.info, sw_c(C_DIM), 0);
        sw_btn_text(D.ok, "SAVE");
        sw_btn_set(D.ok, false);
    } else {
        char t[64];
        snprintf(t, sizeof t, "overwrites:\n%s", old);
        lv_label_set_text(D.info, t);
        lv_obj_set_style_text_color(D.info, sw_c(ACC), 0);
        sw_btn_text(D.ok, "OVERWRITE");
        sw_btn_set(D.ok, true);
    }
}

static void kit_step(void *ud) {
    kit_set_slot((D.slot - 1 + (int)(intptr_t)ud + GX_BANK_SLOTS) % GX_BANK_SLOTS + 1);
}

static void kit_pick(int i) { kit_set_slot(i + 1); }

static void kit_ok(const char *name) {
    save_slot(D.slot, name);
    char t[24];
    snprintf(t, sizeof t, "saved %03d", D.slot);
    say(t);
}

void save_kit_dialog(void) {
    if (D.d) return;
    lv_obj_t *d = dialog_base("SAVE KIT");
    D.cb = kit_ok;
    D.label = NULL;
    sw_label(d, "SLOT", F_MID, C_TEXT, 110, 72, 0, false);
    sw_btn(d, "<", 200, 62, 44, 40, ACC, kit_step, (void *)-1, F_MID);
    static char *names[GX_BANK_SLOTS];
    static char buf[GX_BANK_SLOTS][32];
    for (int i = 0; i < GX_BANK_SLOTS; i++) {
        const char *n = slot_name(i + 1);
        snprintf(buf[i], sizeof buf[i], "%03d  %s", i + 1, n ? n : "---");
        names[i] = buf[i];
    }
    D.slot_pick = pick_new(d, 250, 62, 300, 40, ACC, "SAVE TO SLOT", kit_pick);
    pick_set_options(D.slot_pick, (const char *const *)names, GX_BANK_SLOTS);
    sw_btn(d, ">", 556, 62, 44, 40, ACC, kit_step, (void *)1, F_MID);
    D.info = sw_label(d, "", F_LABEL, C_DIM, 616, 64, 380, false);
    char nm[KIT_NAME + 1];
    clean_name(A.patch_name, nm, sizeof nm);
    lv_textarea_set_text(D.ta, nm);
    kit_set_slot(suggest_save_slot());
}
