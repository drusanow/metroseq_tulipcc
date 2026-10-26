// gx_midip.c - the MIDI page (MidiPage in gintonix.py): note / CC channel,
// the note map, the clock (shared by every Studio app), pattern MIDI out,
// CC learn, sync trim.
#include <stdio.h>
#include <string.h>

#include "gx_ui_int.h"

static struct {
    lv_obj_t *mon;
    gx_pick_t *ch_pick, *out_pick;
    lv_obj_t *b_int, *b_ext, *b_out, *b_mmc, *b_learn, *b_sel;
    lv_obj_t *learn_lbl, *cc_lbl, *sync_lbl;
    uint32_t mon_seq;
} I;

// MIDI OMNI, CH 1..16, OFF  <->  0, 1..16, -1
static int ch_index(int ch) { return ch < 0 ? 17 : ch; }

static void ch_cb(int i) {
    g_gxset.midi_ch = (int8_t)(i == 17 ? -1 : i);
    g_live.hold_mute = 0;   // their note-offs may not come any more
    A.strip_dirty = true;
    gx_settings_save();
}

static void out_cb(int i) {
    g_gxset.midi_out = (uint8_t)i;
    gx_settings_save();
}

static void set_clock(void *ud) {
    studio_set_clock_src((int)(intptr_t)ud);
    A.knobs_dirty = true;
    midi_page_refresh();
    gx_settings_save();
}

static void toggle_out(void *ud) {
    if (studio_clock_src() == CLOCK_SRC_MIDI) {
        say("EXT: no clock out");
        return;
    }
    studio_set_clock_out(!studio_clock_out());
    midi_page_refresh();
    gx_settings_save();
}

static void toggle_mmc(void *ud) {
    studio_set_mmc(!studio_mmc());
    midi_page_refresh();
    gx_settings_save();
}

static void sync_step(void *ud) {
    int v = g_gxset.sync_ms + (int)(intptr_t)ud;
    g_gxset.sync_ms = (int8_t)(v < -50 ? -50 : (v > 50 ? 50 : v));
    midi_page_refresh();
    gx_settings_save();
}

static void toggle_learn(void *ud) {
    A.learn_on = !A.learn_on;
    set_learn_target(KEY_NONE);
    midi_page_refresh();
}

static void toggle_sel(void *ud) {
    g_gxset.cc_sel = !g_gxset.cc_sel;
    midi_page_refresh();
    gx_settings_save();
}

static void forget(void *ud) {
    int k = A.learn_target;
    if (k == KEY_NONE) {
        lv_label_set_text(I.learn_lbl, "Turn CC LEARN on and touch a knob first.");
        return;
    }
    // the knob itself, and what it would map to either way
    char names[3][20];
    key_name(k, names[0], 20);
    key_name(KEY_IS_SEL(k) ? KEY_CH(A.sel, k & 31) : k, names[1], 20);
    key_name(k, names[2], 20);
    int w = 0;
    for (int i = 0; i < g_gxset.ncc; i++) {
        bool hit = false;
        for (int j = 0; j < 3; j++) hit |= !strcmp(g_gxset.cc[i].key, names[j]);
        if (!hit) g_gxset.cc[w++] = g_gxset.cc[i];
    }
    g_gxset.ncc = w;
    gx_settings_save();
    midi_page_refresh();
}

static void clear_all(void *ud) {
    g_gxset.ncc = 0;
    gx_settings_save();
    midi_page_refresh();
}

void midi_page_create(lv_obj_t *g) {
    sw_label(g, "MIDI & CLOCK", F_MID, ACC, 12, 8, 0, false);
    sw_label(g, "MIDI IN", F_SMALL, C_DIM, 700, 10, 0, false);
    I.mon = sw_label(g, "--", F_SMALL, C_TEXT, 760, 10, 250, false);

    sw_label(g, "NOTES / CC", F_LABEL, C_TEXT, 12, 56, 0, false);
    static const char *chs[18];
    static char chbuf[18][16];
    for (int i = 0; i < 18; i++) {
        if (i == 0)
            snprintf(chbuf[i], 16, "MIDI OMNI");
        else if (i == 17)
            snprintf(chbuf[i], 16, "MIDI OFF");
        else
            snprintf(chbuf[i], 16, "MIDI CH %d", i);
        chs[i] = chbuf[i];
    }
    I.ch_pick = pick_new(g, 110, 46, 140, 32, ACC, "MIDI NOTES / CC CHANNEL", ch_cb);
    pick_set_options(I.ch_pick, chs, 18);
    sw_label(g,
             "C1-G1 (36-43) = play channel 1-8 (velocity)    C2-G2 (48-55) = mute channel 1-8 "
             "while held\nC3-B3 (60-71) = pattern A-L (switches at the end of the pass)    "
             "Program Change 0-127 = kit slot 1-128",
             F_LABEL, C_DIM, 270, 44, 740, false);

    sw_label(g, "CLOCK", F_LABEL, C_TEXT, 12, 106, 0, false);
    I.b_int = sw_btn(g, "INTERNAL", 110, 96, 100, 32, SEQ_ACC, set_clock,
                     (void *)(intptr_t)CLOCK_SRC_INT, NULL);
    I.b_ext = sw_btn(g, "EXT MIDI", 216, 96, 100, 32, SEQ_ACC, set_clock,
                     (void *)(intptr_t)CLOCK_SRC_MIDI, NULL);
    I.b_out = sw_btn(g, "SEND CLOCK OUT", 330, 96, 150, 32, SEQ_ACC, toggle_out, NULL, NULL);
    I.b_mmc = sw_btn(g, "MMC IN", 488, 96, 100, 32, SEQ_ACC, toggle_mmc, NULL, NULL);
    sw_label(g,
             "One clock for every Studio app. EXT: PLAY arms, the\nmaster's Start / Continue / "
             "Stop runs it. MMC: IN with\nEXT (MPC transport), OUT with INTERNAL.",
             F_LABEL, C_DIM, 600, 88, 416, false);

    sw_label(g, "PATTERN OUT", F_LABEL, C_TEXT, 12, 156, 0, false);
    static const char *outs[17];
    static char obuf[17][8];
    for (int i = 0; i < 17; i++) {
        if (i)
            snprintf(obuf[i], 8, "CH %d", i);
        else
            snprintf(obuf[i], 8, "OFF");
        outs[i] = obuf[i];
    }
    I.out_pick = pick_new(g, 110, 146, 100, 32, ACC, "PATTERN MIDI OUT", out_cb);
    pick_set_options(I.out_pick, outs, 17);
    sw_label(g,
             "the pattern's hits go out as MIDI notes C1-G1 (36-43) on the clock grid, accent = "
             "velocity 127, normal 64",
             F_LABEL, C_DIM, 222, 154, 790, false);

    sw_label(g, "MIDI CC", F_LABEL, C_TEXT, 12, 208, 0, false);
    I.b_learn = sw_btn(g, "CC LEARN", 110, 198, 100, 32, LEARN_C, toggle_learn, NULL, NULL);
    sw_btn(g, "FORGET KNOB", 216, 198, 120, 32, ACC, forget, NULL, NULL);
    sw_btn(g, "CLEAR ALL", 342, 198, 100, 32, ACC, clear_all, NULL, NULL);
    I.b_sel = sw_btn(g, "CC -> SELECTED CH", 450, 198, 170, 32, ACC, toggle_sel, NULL, NULL);
    I.learn_lbl = sw_label(g, "", F_LABEL, C_DIM, 630, 206, 390, false);
    sw_label(g,
             "CC -> SELECTED CH on: a learned drum knob always moves the channel that is "
             "selected (handy for one hardware controller).  Off: it stays on the channel it was "
             "learned on.",
             F_LABEL, C_DIM, 12, 240, 1000, false);
    I.cc_lbl = sw_label(g, "", F_SMALL, C_TEXT, 12, 284, 1000, false);
    lv_obj_set_style_text_line_space(I.cc_lbl, 4, 0);

    sw_label(g, "SYNC", F_LABEL, C_TEXT, 12, 456, 0, false);
    sw_btn(g, "-", 110, 446, 48, 32, SEQ_ACC, sync_step, (void *)-1, F_MID);
    I.sync_lbl = sw_label(g, "", F_LABEL, C_TEXT, 164, 454, 110, true);
    sw_btn(g, "+", 280, 446, 48, 32, SEQ_ACC, sync_step, (void *)1, F_MID);
    sw_label(g,
             "SYNC: the drums play this much earlier (+) or later (-) than the clock grid. The\n"
             "Tulip's own audio delay is already compensated: leave it at 0 unless a device needs it.",
             F_LABEL, C_DIM, 340, 444, 680, false);
    midi_page_refresh();
}

void midi_page_refresh(void) {
    pick_set_selected(I.ch_pick, ch_index(g_gxset.midi_ch));
    bool ext = studio_clock_src() == CLOCK_SRC_MIDI;
    sw_btn_set(I.b_int, !ext);
    sw_btn_set(I.b_ext, ext);
    sw_btn_set(I.b_out, studio_clock_out() && !ext);
    sw_btn_set(I.b_mmc, studio_mmc());
    sw_btn_text(I.b_mmc, ext ? "MMC IN" : "MMC OUT");
    char t[32];
    snprintf(t, sizeof t, "%+d ms", g_gxset.sync_ms);
    lv_label_set_text(I.sync_lbl, t);
    pick_set_selected(I.out_pick, g_gxset.midi_out);
    sw_btn_set(I.b_learn, A.learn_on);
    sw_btn_set(I.b_sel, g_gxset.cc_sel);
    if (A.learn_on) {
        if (A.learn_target == KEY_NONE) {
            lv_label_set_text(I.learn_lbl,
                              "Touch a knob or MORPH (MAIN / SEQ page), then move a controller.");
        } else {
            char lab[32], m[64];
            int k = A.learn_target;
            key_label(KEY_IS_SEL(k) && !g_gxset.cc_sel ? KEY_CH(A.sel, k & 31) : k, lab,
                      sizeof lab);
            snprintf(m, sizeof m, "%s: move a controller now.", lab);
            lv_label_set_text(I.learn_lbl, m);
        }
    } else {
        lv_label_set_text(I.learn_lbl, "");
    }
    if (!g_gxset.ncc) {
        lv_label_set_text(I.cc_lbl, "(no CC assigned)");
        return;
    }
    static char txt[GX_MAX_CC * 26 + 16];
    int n = 0;
    for (int i = 0; i < g_gxset.ncc && n < (int)sizeof txt - 32; i++) {
        int key = key_parse(g_gxset.cc[i].key);
        char lab[32] = "?";
        if (key != KEY_NONE) key_label(key, lab, sizeof lab);
        n += snprintf(txt + n, sizeof txt - n, "CC%-3d %-14.14s%s", g_gxset.cc[i].cc, lab,
                      (i % 5 == 4) ? "\n" : "  ");
    }
    lv_label_set_text(I.cc_lbl, txt);
}

void midi_page_tick(uint32_t now) {
    (void)now;
    if (A.midi_dirty) {
        A.midi_dirty = false;
        midi_page_refresh();
    }
    char mon[40];
    uint32_t seq = studio_midi_monitor(mon, sizeof mon);
    if (seq != I.mon_seq) {
        I.mon_seq = seq;
        lv_label_set_text(I.mon, mon);
    }
}
