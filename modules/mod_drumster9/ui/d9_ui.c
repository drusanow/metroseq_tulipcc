// d9_ui.c - Drumster9's header, pages and poll.
#include <stdio.h>
#include <string.h>

#include "d9_ui.h"
#include "d9_ui_int.h"

const uint32_t D9_BUS_COLOR[D9_BUSES] = {SW_CYAN, SW_ORANGE, 0x24dbaa, 0xdb49aa};

static struct {
    lv_obj_t *play, *bpm, *info, *tabs[PG_COUNT], *pages[PG_COUNT];
    int page;
    int play_shown, bpm_shown, load_shown;
    uint32_t say_until, reload_seen;
    bool built;
} U = {.page = -1, .play_shown = -1, .bpm_shown = -1, .load_shown = -1};

static uint32_t now_ms(void) { return (uint32_t)(studio_now_us() / 1000); }

void d9_say(const char *msg) {
    if (!U.built) return;
    lv_label_set_text(U.info, msg);
    lv_obj_set_style_text_color(U.info, sw_c(SW_TEXT), 0);
    U.say_until = now_ms() + 2500;
}

void d9_edit(void) { d9_changed(); }

void d9_kit_changed(void) {
    d9_ensure_lane_samples();
    d9_changed();
}

lv_obj_t *d9_step_btn(lv_obj_t *p, int x, int y, int w, sw_cb_t cb, void *ud) {
    lv_obj_t *b = sw_btn(p, (intptr_t)ud & 1 ? "+" : "-", x, y, w, 40, ACC, cb, ud, SW_F_MID);
    sw_btn_repeat(b);
    return b;
}

static void refresh_page(int i) {
    switch (i) {
    case PG_SEQ: d9_seq_page_refresh(); break;
    case PG_KITS: d9_kit_page_refresh(); break;
    case PG_FX: d9_fx_page_refresh(); break;
    case PG_MIX: d9_mix_page_refresh(); break;
    case PG_SET: d9_set_page_refresh(); break;
    }
}

static void show_page(int i) {
    if (i == U.page) return;
    U.page = i;
    for (int k = 0; k < PG_COUNT; k++) {
        sw_btn_set(U.tabs[k], k == i);
        if (k == i)
            lv_obj_remove_flag(U.pages[k], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(U.pages[k], LV_OBJ_FLAG_HIDDEN);
    }
    refresh_page(i);
}

static void tab_cb(void *ud) { show_page((int)(intptr_t)ud); }
static void play_cb(void *ud) { studio_toggle_play(); }

static void bpm_cb(void *ud) {
    int d = (intptr_t)ud == 1 ? 1 : -1;
    studio_set_bpm_x100((studio_bpm_x100() + 50) / 100 * 100 + d * 100);
}

static void header(lv_obj_t *s) {
    const int y = STUDIO_HDR_Y, h = STUDIO_HDR_BTN_H;
    sw_label(s, "DRUMSTER9", SW_F_MID, SW_TEXT, 12, 16, 0, false);
    U.play = sw_btn(s, "PLAY", 150, y, 80, h, SW_GREEN, play_cb, NULL, SW_F_MID);
    sw_btn_repeat(sw_btn(s, "-", 238, y, 40, h, ACC, bpm_cb, (void *)0, SW_F_MID));
    U.bpm = sw_label(s, "", SW_F_LABEL, SW_TEXT, 280, 20, 76, true);
    sw_btn_repeat(sw_btn(s, "+", 358, y, 40, h, ACC, bpm_cb, (void *)1, SW_F_MID));
    static const char *const names[PG_COUNT] = {"SEQ", "KITS", "FX", "MIX", "SET"};
    for (int i = 0; i < PG_COUNT; i++)
        U.tabs[i] = sw_btn(s, names[i], 410 + i * 64, y, 60, h, ACC, tab_cb, (void *)(intptr_t)i, NULL);
    U.info = sw_label(s, "", SW_F_SMALL, C_DIM, 736, 22, STUDIO_SQ_X - 736 - 6, false);
    lv_label_set_long_mode(U.info, LV_LABEL_LONG_CLIP);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);
}

void d9_ui_create(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, sw_c(SW_BG), 0);
    header(scr);
    void (*make[PG_COUNT])(lv_obj_t *) = {d9_seq_page_create, d9_kit_page_create, d9_fx_page_create,
                                         d9_mix_page_create, d9_set_page_create};
    for (int i = 0; i < PG_COUNT; i++) {
        lv_obj_t *g = U.pages[i] = sw_box(scr, 0, PAGE_Y, 1024, PAGE_H, SW_BG, 0);
        lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
        make[i](g);
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    }
    U.built = true;
    U.reload_seen = g_d9_reload;
    show_page(PG_SEQ);
    if (!g_d9s.samples_ok) d9_say("no samples: flash tulip-full.bin");
}

void d9_ui_show(bool shown) {
    if (shown && U.built) refresh_page(U.page);
}

void d9_ui_poll(bool visible) {
    if (!U.built) return;
    if (g_d9_reload != U.reload_seen) {   // a project / Studio patch
        U.reload_seen = g_d9_reload;
        if (visible) refresh_page(U.page);
    }
    if (!visible) return;
    uint32_t ms = now_ms();
    // transport and tempo
    studio_clock_status_t st;
    studio_clock_status(&st);
    int ps = st.playing ? 1 : st.armed ? 2 : 0;
    if (ps != U.play_shown) {
        U.play_shown = ps;
        static const char *const txt[3] = {"PLAY", "STOP", "ARMED"};
        sw_btn_text(U.play, txt[ps]);
        sw_btn_set(U.play, ps != 0);
        sw_btn_accent(U.play, ps == 2 ? SW_YELLOW : SW_GREEN);
    }
    int bv = st.ext ? -1 - st.ext_bpm_x10 : (int)((studio_bpm_x100() + 50) / 100);
    if (bv != U.bpm_shown) {
        U.bpm_shown = bv;
        char t[16];
        if (bv >= 0)
            snprintf(t, sizeof t, "%d BPM", bv);
        else if (st.ext_bpm_x10)
            snprintf(t, sizeof t, "MIDI %d", (st.ext_bpm_x10 + 5) / 10);
        else
            snprintf(t, sizeof t, "MIDI --");
        lv_label_set_text(U.bpm, t);
    }
    // the info line: a message for a while, else the DSP load
    if (U.say_until && (int32_t)(ms - U.say_until) > 0) {
        U.say_until = 0;
        U.load_shown = -1;
    }
    if (!U.say_until && g_d9s.load_pct != U.load_shown) {
        U.load_shown = g_d9s.load_pct;
        char t[40];
        snprintf(t, sizeof t, "DSP %d%%", U.load_shown);
        lv_label_set_text(U.info, t);
        lv_obj_set_style_text_color(U.info, sw_c(C_DIM), 0);
    }
    if (U.page == PG_SEQ) d9_seq_page_tick(ms);
    if (U.page == PG_MIX) d9_mix_page_tick(ms);
}
