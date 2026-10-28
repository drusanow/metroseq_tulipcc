// d9_seqp.c - the SEQ page: 7 lanes x 32 steps (one bar of 1/32 notes).
//
//   PATTERN < name >   KIT < name >   CLEAR                 (project)
//   ................................  the playhead (32 LEDs)
//   [K ] M S R [kit]  [][][][] [][][][] ... 32 steps
//   ... 7 lanes
//   BANK  A B C D E F G H I J   COPY  PASTE   (CV)
//
// The lane name is a pad: tap it to play the lane, and it lights when the
// lane hits. M mutes, S solos, R fills the lane with a random pattern of
// its kind, and the kit button gives the lane its own kit. A step lights in
// the colour of the bus the lane plays through (FX page).
//
// A-J are pattern slots: the one lit keeps every change; tap another and
// it comes in on the next downbeat (blinking until then), with its kit.
#include <stdio.h>
#include <string.h>

#include "d9_ui_int.h"

#define LANE_Y0   62
#define LANE_DY   54
#define CELL_H    46
#define CELL_X0   240
#define CELL_DX   24
#define CELL_W    21
#define BANK_Y    444

static int cell_x(int k) { return CELL_X0 + k * CELL_DX + k / 4; }

static struct {
    lv_obj_t *cell[D9_LANES][D9_STEPS], *led[D9_STEPS];
    lv_obj_t *pad[D9_LANES], *mute[D9_LANES], *solo[D9_LANES], *kitb[D9_LANES];
    lv_obj_t *slot[D9_BANKS], *copy, *paste, *cv;
    lv_obj_t *preset_lbl, *kit_lbl, *proj_lbl;
    lv_obj_t *popup, *pop_title, *pop_btn[D9_KITS + 1];
    int pop_lane;
    uint32_t shown[D9_LANES];   // the masks drawn
    uint32_t bus_shown[D9_LANES];
    int led_lit;
    uint8_t lit;                // pads lit
    uint32_t swaps_seen;
    int blink;
    bool have_clip;
    d9_slot_t clip;
} S = {.led_lit = -1, .pop_lane = -1};

// --- drawing --------------------------------------------------------------------------------------

static void paint_cell(int l, int k, bool on) {
    lv_obj_t *c = S.cell[l][k];
    uint32_t col = D9_BUS_COLOR[g_d9.bus[l]];
    lv_obj_set_style_bg_color(c, sw_c(on ? col : SW_BG), 0);
    lv_obj_set_style_border_color(c, sw_c(on ? col : (k % 4 == 0 ? SW_EDGE : SW_LINE)), 0);
}

static void paint_lane(int l, bool all) {
    uint32_t m = g_d9.mask[l], diff = all ? 0xFFFFFFFFu : (m ^ S.shown[l]);
    for (int k = 0; k < D9_STEPS; k++)
        if (diff & (1u << k)) paint_cell(l, k, m & (1u << k));
    S.shown[l] = m;
    S.bus_shown[l] = g_d9.bus[l];
}

static void paint_strip(int l) {
    sw_btn_set(S.mute[l], g_d9.muted[l]);
    sw_btn_set(S.solo[l], g_d9.solo[l]);
    int ko = g_d9.kit_ovr[l];
    char t[8];
    snprintf(t, sizeof t, "%.4s", ko >= 0 ? D9_KIT[ko].name : "-");
    sw_btn_text(S.kitb[l], t);
    sw_btn_set(S.kitb[l], ko >= 0);
    // a lane whose kit has no sample for it is greyed
    lv_obj_set_style_text_color(sw_btn_label(S.pad[l]),
                                sw_c(d9_lane_preset(&g_d9, l) == D9_NO_SAMPLE ? SW_LINE : SW_DIM), 0);
}

static void paint_slots(void) {
    int nx = g_d9_next;
    for (int i = 0; i < D9_BANKS; i++) {
        lv_obj_t *b = S.slot[i];
        bool pend = i == nx;
        sw_btn_accent(b, pend ? SW_YELLOW : ACC);
        sw_btn_set(b, i == g_d9.bank_active || (pend && S.blink));
        if (i != g_d9.bank_active && !(pend && S.blink))
            lv_obj_set_style_text_color(sw_btn_label(b), sw_c(g_d9.bank[i].used ? SW_TEXT : SW_LINE), 0);
    }
    sw_btn_set(S.paste, S.have_clip);
}

static void paint_labels(void) {
    lv_label_set_text(S.preset_lbl, g_d9.preset >= 0 ? D9_PRESET[g_d9.preset].name : "-");
    lv_label_set_text(S.kit_lbl, D9_KIT[g_d9.kit].name);
    lv_label_set_text(S.proj_lbl, g_d9_proj_cur[0] ? g_d9_proj_cur : "");
    if (S.cv) {
        sw_btn_set(S.cv, g_d9set.cv_on && g_d9s.cv_present);
        sw_btn_text(S.cv, g_d9set.cv_on ? (g_d9s.cv_present ? "CV ON" : "CV: NO DAC") : "CV OFF");
    }
}

void d9_seq_page_refresh(void) {
    for (int l = 0; l < D9_LANES; l++) {
        paint_lane(l, true);
        paint_strip(l);
    }
    paint_slots();
    paint_labels();
    S.swaps_seen = g_d9s.swaps;
}

// --- editing --------------------------------------------------------------------------------------

// after any change to the grid: the lit slot follows it, and it's saved
static void grid_changed(void) {
    d9_lock();
    d9_slot_store(&g_d9);
    d9_unlock();
    d9_edit();
}

static void cell_event(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int l = i >> 5, k = i & 31;
    __atomic_xor_fetch(&g_d9.mask[l], 1u << k, __ATOMIC_SEQ_CST);
    paint_lane(l, false);
    grid_changed();
}

static void pad_cb(void *ud) { d9_trigger((int)(intptr_t)ud, 1.0f); }

static void mute_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_d9.muted[l] = !g_d9.muted[l];
    paint_strip(l);
    d9_edit();
}

static void solo_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_d9.solo[l] = !g_d9.solo[l];
    paint_strip(l);
    d9_edit();
}

static void rnd_cb(void *ud) {
    static uint32_t seed;
    if (!seed) seed = (uint32_t)studio_now_us() | 1;
    int l = (int)(intptr_t)ud;
    g_d9.mask[l] = d9_random_steps(l, &seed);
    paint_lane(l, false);
    grid_changed();
}

static void preset_cb(void *ud) {
    int d = (int)(intptr_t)ud;
    d9_lock();
    d9_apply_preset(&g_d9, (g_d9.preset < 0 ? 0 : g_d9.preset + d));
    d9_unlock();
    for (int l = 0; l < D9_LANES; l++) paint_lane(l, false);
    paint_labels();
    paint_slots();
    d9_edit();
}

static void kit_cb(void *ud) {
    int d = (int)(intptr_t)ud;
    g_d9.kit = (int8_t)((g_d9.kit + d + D9_KITS) % D9_KITS);
    d9_kit_changed();
    grid_changed();
    for (int l = 0; l < D9_LANES; l++) paint_strip(l);
    paint_labels();
}

static void clear_cb(void *ud) {
    d9_lock();
    d9_clear(&g_d9);
    d9_unlock();
    for (int l = 0; l < D9_LANES; l++) paint_lane(l, false);
    paint_labels();
    d9_edit();
    d9_say("cleared");
}

// --- the A-J slots -------------------------------------------------------------------------------

static void slot_cb(void *ud) {
    int i = (int)(intptr_t)ud;
    char t[40];
    if (i == g_d9.bank_active || i == g_d9_next) {   // tap again: stay
        g_d9_next = -1;
        paint_slots();
        return;
    }
    if (g_d9s.playing) {
        g_d9_next = (int8_t)i;   // the next downbeat
        d9_ensure_lane_samples();
        snprintf(t, sizeof t, "%c on the next bar", D9_BANK_LETTERS[i]);
    } else {
        d9_lock();
        d9_slot_switch(&g_d9, i);
        d9_unlock();
        d9_kit_changed();
        d9_seq_page_refresh();
        snprintf(t, sizeof t, "pattern %c", D9_BANK_LETTERS[i]);
    }
    paint_slots();
    d9_say(t);
}

static void copy_cb(void *ud) {
    S.clip = (d9_slot_t){.used = 1, .kit = g_d9.kit};
    memcpy(S.clip.m, g_d9.mask, sizeof S.clip.m);
    S.have_clip = true;
    paint_slots();
    char t[24];
    snprintf(t, sizeof t, "copied %c", D9_BANK_LETTERS[g_d9.bank_active]);
    d9_say(t);
}

static void paste_cb(void *ud) {
    if (!S.have_clip) return d9_say("COPY first");
    d9_lock();
    memcpy(g_d9.mask, S.clip.m, sizeof g_d9.mask);
    g_d9.kit = S.clip.kit;
    d9_slot_store(&g_d9);
    d9_unlock();
    d9_kit_changed();
    d9_seq_page_refresh();
    char t[24];
    snprintf(t, sizeof t, "pasted to %c", D9_BANK_LETTERS[g_d9.bank_active]);
    d9_say(t);
}

static void cv_cb(void *ud) {
    g_d9set.cv_on = !g_d9set.cv_on;
    d9_cv_apply();
    paint_labels();
}

// --- a lane's own kit ------------------------------------------------------------------------------

static void pop_close(void *ud) {
    S.pop_lane = -1;
    lv_obj_add_flag(S.popup, LV_OBJ_FLAG_HIDDEN);
}

static void pop_pick(void *ud) {
    int l = S.pop_lane;
    if (l < 0) return;
    g_d9.kit_ovr[l] = (int8_t)((int)(intptr_t)ud - 1);   // 0 = the global kit
    d9_kit_changed();
    paint_strip(l);
    pop_close(NULL);
}

static void kitb_cb(void *ud) {
    int l = S.pop_lane = (int)(intptr_t)ud;
    char t[48];
    snprintf(t, sizeof t, "%s: ITS OWN KIT", D9_ROLE[l]);
    lv_label_set_text(S.pop_title, t);
    for (int i = 0; i <= D9_KITS; i++) sw_btn_set(S.pop_btn[i], g_d9.kit_ovr[l] == i - 1);
    lv_obj_remove_flag(S.popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(S.popup);
}

// --- the page -----------------------------------------------------------------------------------

void d9_seq_page_tick(uint32_t ms) {
    // a queued slot came in: the new grid
    if (g_d9s.swaps != S.swaps_seen) {
        S.swaps_seen = g_d9s.swaps;
        d9_kit_changed();
        d9_seq_page_refresh();
    }
    for (int l = 0; l < D9_LANES; l++)
        if (g_d9.mask[l] != S.shown[l] || g_d9.bus[l] != S.bus_shown[l]) paint_lane(l, g_d9.bus[l] != S.bus_shown[l]);
    // playhead
    int st = g_d9s.playing ? g_d9s.step : -1;
    if (st != S.led_lit) {
        if (S.led_lit >= 0) lv_obj_set_style_bg_color(S.led[S.led_lit], sw_c(S.led_lit % 4 ? 0x242424 : SW_LINE), 0);
        if (st >= 0) lv_obj_set_style_bg_color(S.led[st], sw_c(ACC), 0);
        S.led_lit = st;
    }
    // the pads light for a moment on each hit
    for (int l = 0; l < D9_LANES; l++) {
        bool on = (uint32_t)(ms - g_d9s.hit_ms[l]) < 90;
        if (on != ((S.lit >> l) & 1)) {
            sw_btn_set(S.pad[l], on);
            S.lit ^= (uint8_t)(1u << l);
        }
    }
    // the queued slot blinks
    int b = g_d9_next >= 0 ? (int)((ms / 250) & 1) : 0;
    if (b != S.blink || (g_d9_next < 0 && S.blink)) {
        S.blink = b;
        paint_slots();
    }
}

void d9_seq_page_create(lv_obj_t *p) {
    // pattern presets, the kit, clear
    sw_label(p, "PATTERN", SW_F_SMALL, C_DIM, 8, 20, 0, false);
    sw_btn(p, "<", 66, 8, 40, 40, ACC, preset_cb, (void *)-1, SW_F_MID);
    S.preset_lbl = sw_label(p, "", SW_F_LABEL, SW_TEXT, 108, 20, 120, true);
    sw_btn(p, ">", 230, 8, 40, 40, ACC, preset_cb, (void *)1, SW_F_MID);
    sw_label(p, "KIT", SW_F_SMALL, C_DIM, 288, 20, 0, false);
    sw_btn(p, "<", 316, 8, 40, 40, ACC, kit_cb, (void *)-1, SW_F_MID);
    S.kit_lbl = sw_label(p, "", SW_F_LABEL, SW_TEXT, 358, 20, 110, true);
    sw_btn(p, ">", 470, 8, 40, 40, ACC, kit_cb, (void *)1, SW_F_MID);
    sw_btn(p, "CLEAR", 528, 8, 84, 40, SW_RED, clear_cb, NULL, NULL);
    S.proj_lbl = sw_label(p, "", SW_F_LABEL, ACC, 628, 20, 384, false);
    lv_label_set_long_mode(S.proj_lbl, LV_LABEL_LONG_CLIP);

    // the playhead
    for (int k = 0; k < D9_STEPS; k++) {
        S.led[k] = sw_box(p, cell_x(k), LANE_Y0 - 12, CELL_W, 6, k % 4 ? 0x242424 : SW_LINE, 0);
    }
    // the lanes
    for (int l = 0; l < D9_LANES; l++) {
        int y = LANE_Y0 + l * LANE_DY;
        S.pad[l] = sw_btn(p, D9_SHORT[l], 8, y, 40, CELL_H, ACC, pad_cb, (void *)(intptr_t)l, SW_F_LABEL);
        S.mute[l] = sw_btn(p, "M", 52, y, 38, CELL_H, SW_RED, mute_cb, (void *)(intptr_t)l, NULL);
        S.solo[l] = sw_btn(p, "S", 94, y, 38, CELL_H, SW_YELLOW, solo_cb, (void *)(intptr_t)l, NULL);
        sw_btn(p, "R", 136, y, 38, CELL_H, ACC, rnd_cb, (void *)(intptr_t)l, NULL);
        S.kitb[l] = sw_btn(p, "-", 178, y, 54, CELL_H, ACC, kitb_cb, (void *)(intptr_t)l, SW_F_SMALL);
        for (int k = 0; k < D9_STEPS; k++) {
            lv_obj_t *c = S.cell[l][k] = sw_box(p, cell_x(k), y, CELL_W, CELL_H, SW_BG, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(c, cell_event, LV_EVENT_PRESSED, (void *)(intptr_t)(l << 5 | k));
        }
    }
    // the A-J slots
    sw_label(p, "BANK", SW_F_SMALL, C_DIM, 8, BANK_Y + 15, 0, false);
    for (int i = 0; i < D9_BANKS; i++) {
        char t[2] = {D9_BANK_LETTERS[i], 0};
        S.slot[i] = sw_btn(p, t, 52 + i * 56, BANK_Y, 50, 44, ACC, slot_cb, (void *)(intptr_t)i, SW_F_MID);
    }
    S.copy = sw_btn(p, "COPY", 628, BANK_Y, 90, 44, ACC, copy_cb, NULL, NULL);
    S.paste = sw_btn(p, "PASTE", 724, BANK_Y, 90, 44, ACC, paste_cb, NULL, NULL);
    if (d9_cv_available()) S.cv = sw_btn(p, "CV", 832, BANK_Y, 180, 44, SW_GREEN, cv_cb, NULL, NULL);
    sw_label(p, "tap a step to set it  -  the lane name plays it  -  M mute  S solo  R random  -  "
                "kit: the lane's own  -  A-J: patterns, the next one on the next bar",
             SW_F_SMALL, C_DIM, 8, BANK_Y + 58, 1000, false);

    // a lane's kit: GLOBAL or one of the 7
    lv_obj_t *d = S.popup = sw_box(p, 120, 120, 784, 150, SW_BG, 0);
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_style_border_color(d, sw_c(SW_EDGE), 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    S.pop_title = sw_label(d, "", SW_F_LABEL, ACC, 16, 18, 0, false);
    sw_btn(d, "CLOSE", 664, 8, 108, 40, SW_RED, pop_close, NULL, NULL);
    for (int i = 0; i <= D9_KITS; i++)
        S.pop_btn[i] = sw_btn(d, i ? D9_KIT[i - 1].name : "GLOBAL", 16 + i * 95, 70, 89, 56, ACC, pop_pick,
                              (void *)(intptr_t)i, SW_F_SMALL);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    d9_seq_page_refresh();
}
