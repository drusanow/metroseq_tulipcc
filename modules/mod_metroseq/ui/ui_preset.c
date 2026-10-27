// ui_preset.c - the 10 presets under the SEQ tracks.
//
//  * The lit (orange) number is the preset you're playing, and it keeps
//    every change you make - steps, tracks, MOD lanes, settings - saved to
//    its slot as you go. There is no SAVE button.
//  * Tap another number: it comes in on the top of TRK 1's sequence (lit
//    yellow until then), so the change lands on the start of a loop. While
//    stopped it comes in at once.
//  * Tap an empty number: the current pattern is copied there, and that
//    number becomes the one you're editing.
//  * COPY takes the current pattern; PASTE puts it over the current preset
//    (also on the top of the sequence).
#include <stdio.h>
#include <string.h>

#include "platform/storage.h"
#include "ui_common.h"

static struct {
    ui_btn_t *num[NUM_PRESETS];
    ui_btn_t *copy, *paste;
    int cur;            // the preset being played and edited
    int pending;        // waiting for the top of the sequence, -1 = none
    pattern_t saved;    // what the current slot holds (for change detection)
    uint32_t swaps;     // pat_swap_count() last seen
    bool have_clip;
    pattern_t clip;
    bool blink;
    uint32_t blink_ms;
} P = {.cur = -1, .pending = -1};

int preset_current(void) { return P.cur; }

// paint() is defined below
static void paint(void);
static void adopt(int slot);

void preset_set_current(int slot) {
    pat_swap_cancel();
    P.pending = -1;
    adopt(slot >= 0 && slot < NUM_PRESETS ? slot : 0);
    P.swaps = pat_swap_count();
    paint();
}

static void paint(void) {
    for (int i = 0; i < NUM_PRESETS; i++) {
        uint8_t c = C_BTN_DK;
        if (i == P.pending)
            c = P.blink ? C_SEL : C_BTN;
        else if (i == P.cur)
            c = C_PRESET_CUR;
        else if (storage_preset_used(i))
            c = C_BTN;
        else
            c = C_BTN_DK;
        if (!P.num[i]) continue;
        ui_btn_set_color(P.num[i], c);
        // an outlined number: white when the slot holds a pattern, dim when empty
        if (c == C_BTN || c == C_BTN_DK)
            lv_obj_set_style_text_color(P.num[i]->label,
                                        c == C_BTN ? lv_color_white() : c332(C_DIM), 0);
    }
    if (P.paste) ui_btn_set_color(P.paste, P.have_clip ? C_BTN : C_BTN_DK);
}

static void say(const char *t) { ui_header_info(t); }

// The pattern now playing is slot `slot`'s. What the slot holds is the
// reference: if the pattern differs (a PASTE), the next poll saves it.
static void adopt(int slot) {
    P.cur = slot;
    if (!storage_preset_get(slot, &P.saved)) memset(&P.saved, 0, sizeof P.saved);
    storage_set_cur_preset(slot);
}

static void pick(intptr_t i) {
    char msg[40];
    if (i == P.pending) return;
    if (i == P.cur) {
        // changed your mind: stay on this one (unless it has just gone in)
        if (P.pending >= 0 && pat_swap_cancel()) {
            P.pending = -1;
            say("stays on the current preset");
        }
        paint();
        return;
    }
    static pattern_t p;
    if (!storage_preset_get((int)i, &p)) {
        // an empty number: it takes a copy of what's playing
        pat_load(&p);
        storage_preset_save((int)i, &p);
        adopt((int)i);
        P.pending = -1;
        snprintf(msg, sizeof msg, "preset %d: a copy of this one", (int)i + 1);
        say(msg);
        paint();
        return;
    }
    P.pending = (int)i;
    pat_swap(&p);   // at once when stopped: preset_poll() sees it go in
    snprintf(msg, sizeof msg, g_status.playing ? "preset %d on the top" : "preset %d", (int)i + 1);
    say(msg);
    paint();
}

static void clip_copy(intptr_t a) {
    pat_load(&P.clip);
    P.have_clip = true;
    ui_btn_flash(P.copy);
    say("copied");
    paint();
}

static void clip_paste(intptr_t a) {
    if (!P.have_clip) {
        say("COPY first");
        return;
    }
    P.pending = P.cur;
    pat_swap(&P.clip);
    ui_btn_flash(P.paste);
    say(g_status.playing ? "pasted on the top" : "pasted");
}

void preset_after_load(void) {
    pattern_t p;
    pat_load(&p);
    P.swaps = pat_swap_count();
    if (P.cur >= 0) {
        storage_preset_save(P.cur, &p);
        P.saved = p;
    }
    paint();
}

void preset_init(void) {
    static pattern_t p;
    int cur = storage_cur_preset();
    if (cur < 0) cur = 0;
    if (storage_preset_get(cur, &p)) {
        p.bpm = g_pat.bpm;   // the tempo is the Studio's
        pat_store(&p);
    } else {
        pat_load(&p);        // first start: the default pattern is preset 1
        storage_preset_save(cur, &p);
    }
    adopt(cur);
    P.swaps = pat_swap_count();
}

void preset_poll(void) {
    pat_swap_poll();
    static pattern_t now;
    uint32_t n = pat_snapshot(&now);
    if (n != P.swaps) {
        // a new pattern went in: it belongs to the slot that was waiting
        P.swaps = n;
        if (P.pending >= 0) adopt(P.pending);
        P.pending = -1;
        ui_refresh_all();
        paint();
        return;
    }
    // the current slot keeps every change (RAM now; the flash write
    // follows when it's quiet - and waits for STOP)
    if (P.cur >= 0) {
        now.bpm = P.saved.bpm;   // the tempo is the Studio's, not the preset's
        if (memcmp(&now, &P.saved, sizeof now)) {
            P.saved = now;
            storage_preset_save(P.cur, &now);
        }
    }
    if (P.pending >= 0) {   // the waiting one blinks
        uint32_t ms = lv_tick_get();
        if (ms - P.blink_ms > 250) {
            P.blink_ms = ms;
            P.blink = !P.blink;
            paint();
        }
    }
}

void preset_row_create(lv_obj_t *parent, int y) {
    ui_section(parent, "PRESETS", SEQ_X0, y);
    lv_obj_t *h = ui_label(parent, "changes save to the lit preset; another comes in on the top of TRK 1",
                           SEQ_X0 + 86, y);
    lv_obj_set_style_text_color(h, c332(C_DIM), 0);
    const int bw = 64, gap = 6, by = y + 22, bh = 44;
    for (int i = 0; i < NUM_PRESETS; i++) {
        char nm[4];
        snprintf(nm, sizeof nm, "%d", i + 1);
        P.num[i] = ui_btn(parent, nm, SEQ_X0 + i * (bw + gap), bw, bh, pick, i, C_BTN_DK, by, NULL);
    }
    int x = SEQ_X0 + NUM_PRESETS * (bw + gap) + 12;
    int w = (SEQ_X0 + NUM_STAGES * COL_W - (COL_W - COL_INNER) - x - gap) / 2;
    P.copy = ui_btn(parent, "COPY", x, w, bh, clip_copy, 0, C_BTN, by, NULL);
    P.paste = ui_btn(parent, "PASTE", x + w + gap, w, bh, clip_paste, 0, C_BTN_DK, by, NULL);
    paint();
}
