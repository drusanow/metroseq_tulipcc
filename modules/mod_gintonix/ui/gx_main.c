// gx_main.c - the MAIN page (MainPage, ChannelStrip, Switch, FocusFader in
// gintonix.py): channel pads + mute / solo, the selected channel's drum
// patch in four tinted sections, the drum sound library, the focus fader,
// PLAY, patterns, chain, morph + ALTER A/B, master.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_ui_int.h"

// --- switches: a labelled group of latching buttons ------------------------------------------

typedef struct {
    int sw;
    lv_obj_t *btns[3];
} gx_switch_t;

static gx_switch_t *s_switches[GX_NSW];
static int s_nsw;

static void switch_refresh(gx_switch_t *s) {
    gx_patch_t p;
    gx_eff(&g_gx, A.sel, &p);
    for (int j = 0; j < GX_SWITCHES[s->sw].n; j++) sw_btn_set(s->btns[j], p.s[s->sw] == j);
}

static void switch_pick(void *ud) {
    intptr_t v = (intptr_t)ud;
    gx_switch_t *s = s_switches[v >> 4];
    int val = (int)(v & 15);
    gx_patch_t p;
    gx_eff(&g_gx, A.sel, &p);
    bool changed = p.s[s->sw] != val;
    gx_set_switch(&g_gx, A.sel, s->sw, val);
    sound_edited(A.sel);
    if (s->sw == S_MOD_MODE || s->sw == S_N_ENV) A.knobs_dirty = true;   // units change
    switch_refresh(s);
    (void)changed;
}

static void switch_new(lv_obj_t *g, int sw, int x, int y, uint32_t accent, int top) {
    gx_switch_t *s = calloc(1, sizeof *s);
    s->sw = sw;
    int idx = s_nsw;
    s_switches[s_nsw++] = s;
    const gx_switch_info_t *info = &GX_SWITCHES[sw];
    sw_label(g, info->label, F_TINY, C_TEXT, x, y + top + 2, SW_W, true);
    int n = info->n;
    int bh = n == 3 ? 24 : 32, gap = n == 3 ? 4 : 8;
    int y0 = n == 3 ? y + top + 20 : y + top + 24;
    for (int j = 0; j < n; j++)
        s->btns[j] = sw_btn(g, info->opts[j], x + 6, y0 + j * (bh + gap), SW_W - 12, bh, accent,
                            switch_pick, (void *)(intptr_t)((idx << 4) | j), F_TINY);
    switch_refresh(s);
}

// --- the channel strip ------------------------------------------------------------------------

static struct {
    lv_obj_t *sel_b[GX_NCH], *mute_b[GX_NCH], *solo_b[GX_NCH], *leds[GX_NCH];
    bool led_on[GX_NCH];
    char names_shown[GX_NCH][24];
    // drum library
    gx_pick_t *drum_pick;
    lv_obj_t *name_lbl, *del_b;
    uint32_t del_armed;
    // focus fader
    lv_obj_t *f_name, *f_val;
    gx_bar_t *f_bar;
    int f_key, f_sel;
    int f_shown;
    char f_txt[24];
    // morph
    gx_bar_t *morph;
    lv_obj_t *morph_lbl;
    int mshown;
    gx_pbar_t *pbar;
} M = {.f_key = KEY_NONE, .mshown = -1};

static void pad_cb(void *ud) { select_channel((int)(intptr_t)ud, true); }
static void mute_cb(void *ud) { toggle_mute((int)(intptr_t)ud, false); }
static void solo_cb(void *ud) { toggle_mute((int)(intptr_t)ud, true); }

static void strip_create(lv_obj_t *g, int y, int h, int mh) {
    int w = 128;
    for (int c = 0; c < GX_NCH; c++) {
        int x = c * w;
        char nm[24];
        M.sel_b[c] = sw_btn(g, ch_name(c, nm, sizeof nm), x + 3, y, w - 6, h, ACC, pad_cb,
                            (void *)(intptr_t)c, F_LABEL);
        lv_obj_t *led = M.leds[c] = sw_box(g, x + 9, y + 6, 10, 10, LED_OFF, 5);
        (void)led;
        int hw = (w - 14) / 2;
        M.mute_b[c] = sw_btn(g, "M", x + 3, y + h + 8, hw, mh, MUTE_C, mute_cb, (void *)(intptr_t)c,
                             F_LABEL);
        M.solo_b[c] = sw_btn(g, "S", x + 11 + hw, y + h + 8, hw, mh, SOLO_C, solo_cb,
                             (void *)(intptr_t)c, F_LABEL);
    }
}

static void strip_refresh(void) {
    uint8_t off = g_live.mute | g_live.hold_mute;
    for (int c = 0; c < GX_NCH; c++) {
        sw_btn_set(M.sel_b[c], c == A.sel);
        sw_btn_set(M.mute_b[c], off & (1u << c));
        sw_btn_set(M.solo_b[c], is_solo(c));
        char nm[24];
        ch_name(c, nm, sizeof nm);
        if (strcmp(nm, M.names_shown[c])) {
            snprintf(M.names_shown[c], sizeof M.names_shown[c], "%s", nm);
            sw_btn_text(M.sel_b[c], nm);
        }
    }
}

static void strip_tick(uint32_t now) {
    for (int c = 0; c < GX_NCH; c++) {
        int32_t d1 = (int32_t)(now - g_gxs.hit_ms[c]), d2 = (int32_t)(now - A.flash_ms[c]);
        bool on = (d1 >= 0 && d1 < 90) || (d2 >= 0 && d2 < 90);
        if (on != M.led_on[c]) {
            M.led_on[c] = on;
            lv_obj_set_style_bg_color(M.leds[c], sw_c(on ? PLAY_C : LED_OFF), 0);
        }
    }
}

// --- sections ----------------------------------------------------------------------------------

#define SEP (-1)
#define SWI(s) (100 + (s))

// a tinted panel with a title; returns the x right after it
static int section(lv_obj_t *g, const char *name, const gx_sec_t *sec, int x, int y,
                   const int *items, int n) {
    int w = 6;
    for (int i = 0; i < n; i++) w += items[i] == SEP ? 8 : (items[i] >= 100 ? SW_W : KN_W);
    sw_box(g, x, y, w, CELL_H - 2, sec->tint, 6);
    sw_label(g, name, F_TINY, sec->col, x + 8, y + 2, 0, false);
    int xx = x + 3;
    for (int i = 0; i < n; i++) {
        int it = items[i];
        if (it == SEP) {
            sw_line(g, xx + 3, y + 32, 1, CELL_H - 44);
            xx += 8;
        } else if (it >= 100) {
            switch_new(g, it - 100, xx, y, sec->col, 12);
            xx += SW_W;
        } else {
            knob_new(g, KEY_SEL(it), xx, y, sec->col, KN_W, -1, 12, false);
            xx += KN_W;
        }
    }
    return x + w;
}

// --- drum sound library -------------------------------------------------------------------------

static const char *drum_title(void) {
    static char t[40];
    snprintf(t, sizeof t, "DRUM SOUND  ->  CH %d", A.sel + 1);
    return t;
}

void main_page_drums_changed(void) {
    int n = drum_count();
    char **opts = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        const char *nm = drum_name(i);
        opts[i] = malloc(strlen(nm) + 3);
        sprintf(opts[i], i < A.n_user ? "* %s" : "%s", nm);
    }
    pick_set_options(M.drum_pick, (const char *const *)opts, n);
    for (int i = 0; i < n; i++) free(opts[i]);
    free(opts);
}

static void refresh_name(void) {
    int c = A.sel;
    const char *nm = g_gx.names[c];
    pick_set_text(M.drum_pick, nm);
    for (int i = 0; i < drum_count(); i++)
        if (!strcmp(drum_name(i), nm)) {
            pick_set_selected(M.drum_pick, i);
            break;
        }
    char t[48];
    snprintf(t, sizeof t, "DRUM  CH %d%s", c + 1, user_drum_index(nm) >= 0 ? "  (saved)" : "");
    lv_label_set_text(M.name_lbl, t);
    if (M.del_armed) {
        M.del_armed = 0;
        sw_btn_text(M.del_b, "DEL");
        sw_btn_set(M.del_b, false);
    }
}

static void drum_cb(int i) {
    int c = A.sel;
    gx_patch_t a, b;
    drum_patches(i, &a, &b);
    g_gx.a[c] = a;
    g_gx.b[c] = b;
    snprintf(g_gx.names[c], sizeof g_gx.names[c], "%s", drum_name(i));
    sound_edited(c);
    gx_sound_changed(c);   // now: the preview hit below must use it
    refresh_channel();
    gx_trigger(c, 1.0f);
    A.flash_ms[c] = (uint32_t)(studio_now_us() / 1000);
}

static int s_save_ch;

static const char *save_label(const char *name) {
    return user_drum_index(name) >= 0 ? "OVERWRITE" : "SAVE";
}

static void save_drum_as(const char *name) {
    int c = s_save_ch;
    char nm[GX_NAME_MAX + 1];
    if (!name[0])
        snprintf(nm, sizeof nm, "Sound %d", A.n_user + 1);
    else
        snprintf(nm, sizeof nm, "%s", name);
    save_user_drum(nm, &g_gx.a[c], &g_gx.b[c]);
    snprintf(g_gx.names[c], sizeof g_gx.names[c], "%s", nm);
    A.strip_dirty = true;
    main_page_drums_changed();
    refresh_name();
    say("sound saved");
}

static void save_drum(void *ud) {
    s_save_ch = A.sel;
    char t[40], nm[KIT_NAME + 1];
    snprintf(t, sizeof t, "SAVE SOUND  (CH %d)", A.sel + 1);
    snprintf(nm, sizeof nm, "%s", g_gx.names[A.sel]);
    text_dialog(t, nm, save_drum_as, save_label);
}

static void del_drum(void *ud) {
    int i = user_drum_index(g_gx.names[A.sel]);
    if (i < 0) {
        say("not a saved sound");
        return;
    }
    uint32_t now = (uint32_t)(studio_now_us() / 1000);
    if (!(M.del_armed && now - M.del_armed < 3000)) {
        M.del_armed = now ? now : 1;
        sw_btn_text(M.del_b, "SURE?");
        sw_btn_set(M.del_b, true);
        return;
    }
    M.del_armed = 0;
    sw_btn_text(M.del_b, "DEL");
    sw_btn_set(M.del_b, false);
    del_user_drum(i);
    main_page_drums_changed();
    refresh_name();
    say("sound deleted");
}

// --- focus fader: the last touched pot, big ------------------------------------------------------

static void focus_step(void *ud) {
    if (A.focus_key != KEY_NONE) {
        nudge(A.focus_key, (int)(intptr_t)ud);
        M.f_shown = -1;
    }
}

static void focus_default(void *ud) {
    int k = A.focus_key;
    if (k == KEY_NONE) return;
    if (KEY_IS_G(k) && (k & 7) == G_TEMPO)
        apply_tempo(120.0f);
    else
        set_val(k, key_default(k));
    gx_knob_t *kn = knob_by_key(k);
    if (kn) knob_refresh(kn);
    M.f_shown = -1;
}

static void focus_moved(float v) {
    if (A.focus_key == KEY_NONE) return;
    set_val(A.focus_key, v);
    gx_knob_t *kn = knob_by_key(A.focus_key);
    if (kn) knob_refresh(kn);
}

static float focus_get(void) { return A.focus_key == KEY_NONE ? 0 : get_val(A.focus_key); }

static void focus_create(lv_obj_t *g, int y, int h) {
    M.f_name = sw_label(g, "", F_SMALL, C_DIM, 8, y + 6, 150, false);
    M.f_val = sw_label(g, "", F_MID, ACC, 8, y + 24, 150, false);
    int bw = 52;
    lv_obj_t *b = sw_btn(g, "-", 166, y + 2, bw, h - 4, ACC, focus_step, (void *)-1, F_BIG);
    sw_btn_repeat(b);
    int x1 = 1016 - 76 - 8 - bw;
    b = sw_btn(g, "+", x1, y + 2, bw, h - 4, ACC, focus_step, (void *)1, F_BIG);
    sw_btn_repeat(b);
    int bx = 166 + bw + 8;
    M.f_bar = bar_new(g, bx, y + 2, x1 - 8 - bx, h - 4, ACC, ACC_DIM, focus_moved, 28);
    M.f_bar->get = focus_get;
    sw_btn(g, "DEFAULT", 1016 - 76, y + 2, 76, h - 4, C_DIM, focus_default, NULL, F_LABEL);
}

static void focus_tick(void) {
    int key = A.focus_key;
    if (key == KEY_NONE) return;
    if (key != M.f_key || M.f_sel != A.sel) {
        M.f_key = key;
        M.f_sel = A.sel;
        char nm[48], lab[32];
        key_label(key, lab, sizeof lab);
        if (KEY_IS_SEL(key))
            snprintf(nm, sizeof nm, "CH%d  %s", A.sel + 1, lab);
        else
            snprintf(nm, sizeof nm, "%s", lab);
        lv_label_set_text(M.f_name, nm);
        uint32_t col, dim;
        if (!KEY_IS_G(key)) {
            const gx_sec_t *s = gx_sec_of(key & 31);
            col = s->col;
            dim = s->dim;
        } else if ((key & 7) == G_MASTER || (key & 7) == G_MORPH) {
            col = ACC;
            dim = ACC_DIM;
        } else {
            col = SEQ_ACC;
            dim = SEQ_DIM;
        }
        lv_obj_set_style_text_color(M.f_val, sw_c(col), 0);
        bar_set_color(M.f_bar, col, dim);
        M.f_shown = -1;
        M.f_txt[0] = 0;
    }
    float v = get_val(key);
    int iv = (int)(v * 2000 + 0.5f);
    if (iv != M.f_shown && !M.f_bar->dragging) bar_show(M.f_bar, v);
    M.f_shown = iv;
    char t[24];
    val_text(key, t, sizeof t);
    if (strcmp(t, M.f_txt)) {
        snprintf(M.f_txt, sizeof M.f_txt, "%s", t);
        lv_label_set_text(M.f_val, t);
    }
}

// --- morph --------------------------------------------------------------------------------------

static void morph_cb(float v) { set_val(KEY_G(G_MORPH), v); }
static float morph_get(void) { return g_gx.morph; }
static void alter_a(void *ud) { tools_alter_ab(false); }
static void alter_b(void *ud) { tools_alter_ab(true); }

// --- page -----------------------------------------------------------------------------------------

void main_page_create(lv_obj_t *g) {
    strip_create(g, 4, 52, 32);
    int y1 = 100, y2 = y1 + CELL_H + 2;
    static const int osc[] = {SWI(S_WAVE), K_OSC_FREQ, SEP, SWI(S_MOD_MODE), K_MOD_AMT, K_MOD_RATE,
                              SEP, K_OSC_ATT, K_OSC_DEC};
    static const int noise[] = {SWI(S_N_MODE), K_N_FREQ, K_N_Q, SEP, SWI(S_N_ENV), K_N_ATT, K_N_DEC};
    static const int mix[] = {K_MIX, K_EQ_FREQ, K_EQ_GAIN, K_DIST, K_LEVEL, K_PAN, SEP, SWI(S_CHOKE)};
    static const int vel[] = {K_VEL_OSC, K_VEL_NOISE, K_VEL_MOD};
    int x = section(g, "OSC", &SEC_OSC, 2, y1, osc, 9);
    section(g, "NOISE", &SEC_NOISE, x + 4, y1, noise, 7);
    x = section(g, "MIX / OUT", &SEC_MIX, 2, y2, mix, 8);
    x = section(g, "VELOCITY", &SEC_VEL, x + 4, y2, vel, 3);
    // the drum sound library (saved + factory) showing the channel's sound;
    // SAVE = store the selected channel's sound in the list, DEL = remove it
    x += 4;
    int dw = 1020 - x - 6;
    M.name_lbl = sw_label(g, "DRUM", F_TINY, C_TEXT, x + 4, y2 + 4, dw, false);
    M.drum_pick = pick_new(g, x + 4, y2 + 22, dw, 40, ACC, NULL, drum_cb);
    M.drum_pick->title = drum_title;
    main_page_drums_changed();
    int bw = (dw - 6) * 3 / 5;
    sw_btn(g, "SAVE SOUND", x + 4, y2 + 70, bw, 40, ACC, save_drum, NULL, F_LABEL);
    M.del_b = sw_btn(g, "DEL", x + 10 + bw, y2 + 70, dw - 6 - bw, 40, MUTE_C, del_drum, NULL, F_LABEL);
    // focus fader: the last touched pot
    int y3 = y2 + CELL_H + 2;
    focus_create(g, y3, 56);
    sw_line(g, 2, y3 + 60, 1020, 1);
    // bottom: PLAY (left, square), patterns, chain, morph, MASTER (right)
    int y4 = y3 + 66, H = 120;
    int yo = y4 + (PAGE_H - 12 - y4 - H) / 2;
    int bh = (H - 6) / 2, px = 4 + H + 10, rx = px + 6 * 52 + 2;
    M.pbar = pbar_new(g, px, yo, 6, 46, bh, 6, rx, yo, 84, H, 4, yo, H, H);
    knob_new(g, KEY_G(G_MASTER), 1020 - 72, yo - 4, ACC, 72, 0, 0, false);
    // MORPH: a wide fader, % and ALTER A / ALTER B
    int mx = rx + 84 + 12, mw = 1020 - 72 - 10 - mx;
    sw_label(g, "MORPH", F_TINY, C_TEXT, mx, yo + 2, 0, false);
    M.morph_lbl = sw_label(g, "", F_SMALL, ACC, mx + mw - 40, yo + 2, 40, false);
    M.morph = bar_new(g, mx, yo + 18, mw, 48, ACC, ACC_DIM, morph_cb, 24);
    M.morph->get = morph_get;
    M.morph->key = KEY_G(G_MORPH);
    sw_label(g, "A", F_MID, ACC, mx + 6, yo + 32, 0, false);
    sw_label(g, "B", F_MID, ACC, mx + mw - 20, yo + 32, 0, false);
    int hw = (mw - 6) / 2;
    sw_btn(g, "ALTER A", mx, yo + 72, hw, H - 72, ACC, alter_a, NULL, F_LABEL);
    sw_btn(g, "ALTER B", mx + hw + 6, yo + 72, hw, H - 72, ACC, alter_b, NULL, F_LABEL);
}

void main_page_refresh(bool knobs) {
    if (knobs) A.knobs_dirty = true;
    for (int i = 0; i < s_nsw; i++) switch_refresh(s_switches[i]);
    strip_refresh();
    refresh_name();
    M.mshown = -1;
}

void main_page_tick(uint32_t now) {
    if (A.strip_dirty) {
        A.strip_dirty = false;
        strip_refresh();
    }
    strip_tick(now);
    pbar_tick(M.pbar);
    focus_tick();
    int m = (int)(g_gx.morph * 1000 + 0.5f);
    if (m != M.mshown) {
        M.mshown = m;
        if (!M.morph->dragging) bar_show(M.morph, g_gx.morph);
        char t[8];
        snprintf(t, sizeof t, "%d%%", (int)(g_gx.morph * 100 + 0.5f));
        lv_label_set_text(M.morph_lbl, t);
    }
}
