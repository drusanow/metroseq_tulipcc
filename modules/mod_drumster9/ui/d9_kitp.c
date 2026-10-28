// d9_kitp.c - the KITS page: which sample each kit plays for each part.
//
// Pick a kit, then CHANGE on a part: every bank's samples are a tap away
// (the Python offered the kit's own bank plus Acoustic and Extras; here
// it's all nine). Tapping a sample plays it and puts it in the kit at once.
// RESET KIT puts the kit's own samples back. The choices are part of the
// project (the Python's kit_overrides).
#include <stdio.h>
#include <string.h>

#include "d9_ui_int.h"

#define ROWS 11
#define COLS 4
#define ROW_Y0 64
#define ROW_DY 54

static struct {
    lv_obj_t *kit_lbl, *name[D9_LANES], *len[D9_LANES];
    lv_obj_t *picker, *ptitle, *ptab[D9_MAX_BANKS], *poff, *item[ROWS * COLS];
    int kit, role, bank;
} K = {.role = -1};

static void refresh_rows(void) {
    lv_label_set_text(K.kit_lbl, D9_KIT[K.kit].name);
    for (int l = 0; l < D9_LANES; l++) {
        int p = g_d9.kit_map[K.kit][l];
        bool own = p == D9_KIT[K.kit].s[l];
        lv_label_set_text(K.name[l], d9_name(p));
        lv_obj_set_style_text_color(K.name[l], sw_c(own ? SW_TEXT : ACC), 0);
        int ms = d9_sample_ms(&g_d9ix, p);
        if (ms)
            lv_label_set_text_fmt(K.len[l], "%d ms%s", ms, own ? "" : "   (changed)");
        else
            lv_label_set_text(K.len[l], p == D9_NO_SAMPLE ? "silent" : "");
    }
}

static void refresh_picker(void) {
    if (K.role < 0) return;
    char t[64];
    snprintf(t, sizeof t, "%s  /  %s", D9_KIT[K.kit].name, D9_ROLE[K.role]);
    lv_label_set_text(K.ptitle, t);
    int cur = g_d9.kit_map[K.kit][K.role];
    for (int b = 0; b < g_d9ix.nbanks; b++) sw_btn_set(K.ptab[b], b == K.bank);
    sw_btn_set(K.poff, cur == D9_NO_SAMPLE);
    const d9_bank_t *bk = K.bank < g_d9ix.nbanks ? &g_d9ix.banks[K.bank] : NULL;
    for (int i = 0; i < ROWS * COLS; i++) {
        lv_obj_t *b = K.item[i];
        if (!bk || i >= bk->count) {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        sw_btn_text(b, d9_name(bk->first + i));
        sw_btn_set(b, cur == bk->first + i);
    }
}

void d9_kit_page_refresh(void) {
    refresh_rows();
    refresh_picker();
}

static void kit_step(void *ud) {
    K.kit = (K.kit + (int)(intptr_t)ud + D9_KITS) % D9_KITS;
    refresh_rows();
}

static void set_sample(int preset) {
    d9_audition(preset);
    g_d9.kit_map[K.kit][K.role] = (int16_t)preset;
    d9_kit_changed();
    d9_kit_page_refresh();
}

static void reset_cb(void *ud) {
    memcpy(g_d9.kit_map[K.kit], D9_KIT[K.kit].s, sizeof g_d9.kit_map[K.kit]);
    d9_kit_changed();
    refresh_rows();
    d9_say("kit reset");
}

static void play_cb(void *ud) {
    int p = g_d9.kit_map[K.kit][(int)(intptr_t)ud];
    if (p != D9_NO_SAMPLE) d9_audition(p);
}

static void change_cb(void *ud) {
    K.role = (int)(intptr_t)ud;
    // start on the bank of the sample it has, else the kit's own
    const d9_sample_t *s = d9_sample(&g_d9ix, g_d9.kit_map[K.kit][K.role]);
    K.bank = s ? s->bank : d9_bank_find(&g_d9ix, D9_KIT[K.kit].bank);
    if (K.bank < 0) K.bank = 0;
    refresh_picker();
    lv_obj_remove_flag(K.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(K.picker);
}

static void back_cb(void *ud) {
    K.role = -1;
    lv_obj_add_flag(K.picker, LV_OBJ_FLAG_HIDDEN);
    refresh_rows();
}

static void tab_cb(void *ud) {
    K.bank = (int)(intptr_t)ud;
    refresh_picker();
}

static void off_cb(void *ud) {
    g_d9.kit_map[K.kit][K.role] = D9_NO_SAMPLE;
    d9_kit_changed();
    refresh_picker();
}

static void item_cb(void *ud) {
    if (K.bank >= g_d9ix.nbanks) return;
    set_sample(g_d9ix.banks[K.bank].first + (int)(intptr_t)ud);
}

void d9_kit_page_create(lv_obj_t *p) {
    K.kit = g_d9.kit;
    sw_label(p, "KIT", SW_F_SMALL, C_DIM, 12, 20, 0, false);
    sw_btn(p, "<", 44, 8, 40, 40, ACC, kit_step, (void *)-1, SW_F_MID);
    K.kit_lbl = sw_label(p, "", SW_F_MID, SW_TEXT, 86, 16, 150, true);
    sw_btn(p, ">", 238, 8, 40, 40, ACC, kit_step, (void *)1, SW_F_MID);
    sw_btn(p, "RESET KIT", 296, 8, 120, 40, SW_RED, reset_cb, NULL, NULL);
    sw_label(p, "CHANGE a part to pick its sample from any bank; tap one to hear it", SW_F_SMALL, C_DIM,
             436, 22, 0, false);
    for (int l = 0; l < D9_LANES; l++) {
        int y = ROW_Y0 + l * ROW_DY;
        sw_label(p, D9_ROLE[l], SW_F_LABEL, SW_TEXT, 12, y + 13, 90, false);
        K.name[l] = sw_label(p, "", SW_F_LABEL, SW_TEXT, 110, y + 13, 330, false);
        lv_label_set_long_mode(K.name[l], LV_LABEL_LONG_CLIP);
        sw_btn(p, LV_SYMBOL_PLAY, 450, y, 64, 42, ACC, play_cb, (void *)(intptr_t)l, NULL);
        sw_btn(p, "CHANGE", 522, y, 110, 42, ACC, change_cb, (void *)(intptr_t)l, NULL);
        K.len[l] = sw_label(p, "", SW_F_SMALL, C_DIM, 648, y + 15, 360, false);
        sw_line(p, 12, y + 48, 1000, 1);
    }

    // the sample picker
    lv_obj_t *d = K.picker = sw_box(p, 0, 0, 1024, PAGE_H, SW_BG, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    K.ptitle = sw_label(d, "", SW_F_MID, ACC, 12, 16, 0, false);
    sw_btn(d, "BACK", 900, 8, 112, 40, ACC, back_cb, NULL, NULL);
    for (int b = 0; b < D9_MAX_BANKS; b++) {
        if (b < g_d9ix.nbanks)
            K.ptab[b] = sw_btn(d, g_d9ix.banks[b].label, 8 + b * 104, 56, 100, 40, ACC, tab_cb,
                               (void *)(intptr_t)b, SW_F_SMALL);
    }
    K.poff = sw_btn(d, "OFF", 944, 56, 72, 40, SW_RED, off_cb, NULL, NULL);
    for (int i = 0; i < ROWS * COLS; i++) {
        int c = i / ROWS, r = i % ROWS;   // down the columns, in bank order
        K.item[i] = sw_btn(d, "", 8 + c * 252, 108 + r * 38, 246, 34, ACC, item_cb, (void *)(intptr_t)i,
                           SW_F_SMALL);
    }
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    refresh_rows();
}
