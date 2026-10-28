// io_panel.c - the I/O every app gets from the runtime, by its role, as one
// panel an app puts on its settings page (studio_io_panel()). So the clock,
// MIDI clock out and the CV clock look and work the same in every app, on
// its own or in the Studio:
//
//   every app        CLOCK INT / MIDI (MIDI clock in), MIDI CLOCK OUT
//   DRUMS            CV CLOCK on / off, its rate, VOUT1 reset / run
//   SEQUENCER        CV gate + pitch are the app's own (a note says so)
//   SYNTH            what it takes from MIDI IN
#include <stdio.h>

#include "studio/widgets.h"
#include "studio_priv.h"

#define ROW 46
#define CTL_X 120

typedef struct {
    const tmod_t *m;
    lv_obj_t *clk_int, *clk_midi, *out, *cv, *div[3], *v1[2], *status, *midi;
    int shown;
} panel_t;

static void refresh(panel_t *p) {
    int src = studio_clock_src();
    sw_btn_set(p->clk_int, src == CLOCK_SRC_INT);
    sw_btn_set(p->clk_midi, src == CLOCK_SRC_MIDI);
    if (p->out) {
        bool on = studio_clock_out();
        sw_btn_set(p->out, on && src == CLOCK_SRC_INT);
        sw_btn_text(p->out, src == CLOCK_SRC_MIDI ? "(follows MIDI)" : on ? "ON" : "OFF");
    }
    if (p->cv) {
        sw_btn_set(p->cv, studio_cv_clock());
        sw_btn_text(p->cv, studio_cv_clock() ? "ON" : "OFF");
        static const int DIV[3] = {2, 4, 8};
        for (int i = 0; i < 3; i++) sw_btn_set(p->div[i], studio_cv_clock_div() == DIV[i]);
        for (int i = 0; i < 2; i++) sw_btn_set(p->v1[i], studio_cv_clock_vout1() == i);
        int st = studio_cv_clock_state();
        static const char *const TXT[] = {"off", "running on the DAC at 0x59", "no DAC on the I2C port",
                                          "a sequencer has the DACs", ""};
        lv_label_set_text(p->status, st >= 0 && st <= CVCLK_NO_DRUMS ? TXT[st] : "");
        p->shown = st;
    }
    if (p->midi) {
        char d[96] = "-";
        if (p->m->midi_desc) p->m->midi_desc(d, sizeof d);
        lv_label_set_text(p->midi, d);
    }
}

static void tick(lv_timer_t *t) {
    panel_t *p = lv_timer_get_user_data(t);
    lv_obj_t *box = lv_obj_get_parent(p->clk_int);
    if (lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN) || !lv_obj_is_visible(box)) return;
    refresh(p);
}

// (the panel redraws itself on its timer)
static void src_cb(void *ud) { studio_set_clock_src((int)(intptr_t)ud); }

static void out_cb(void *ud) {
    if (studio_clock_src() == CLOCK_SRC_INT) studio_set_clock_out(!studio_clock_out());
}

static void cv_cb(void *ud) { studio_set_cv_clock(!studio_cv_clock(), -1, -1); }
static void div_cb(void *ud) { studio_set_cv_clock(studio_cv_clock(), (int)(intptr_t)ud, -1); }
static void v1_cb(void *ud) { studio_set_cv_clock(studio_cv_clock(), -1, (int)(intptr_t)ud); }

static lv_obj_t *row_label(lv_obj_t *box, const char *t, int r) {
    return sw_label(box, t, SW_F_SMALL, SW_DIM, 0, r * ROW + 13, CTL_X - 8, false);
}

lv_obj_t *studio_io_panel(lv_obj_t *parent, int x, int y, const tmod_t *m, uint32_t accent) {
    panel_t *p = lv_malloc_zeroed(sizeof *p);
    p->m = m;
    p->shown = -1;
    lv_obj_t *box = sw_box(parent, x, y, 330, 6 * ROW, -1, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(box, p);
    int r = 0;
    row_label(box, "CLOCK", r);
    p->clk_int = sw_btn(box, "INT", CTL_X, r * ROW, 100, 40, SW_CYAN, src_cb, (void *)CLOCK_SRC_INT, NULL);
    p->clk_midi = sw_btn(box, "MIDI IN", CTL_X + 106, r * ROW, 100, 40, SW_CYAN, src_cb,
                         (void *)CLOCK_SRC_MIDI, NULL);
    r++;
    if (m->flags & TMOD_F_CLOCKED || m->role != TMOD_SYNTH) {
        row_label(box, "MIDI CLOCK OUT", r);
        p->out = sw_btn(box, "OFF", CTL_X, r * ROW, 206, 40, SW_CYAN, out_cb, NULL, NULL);
        r++;
    }
    if (m->role == TMOD_DRUMS) {
        row_label(box, "CV CLOCK", r);
        p->cv = sw_btn(box, "OFF", CTL_X, r * ROW, 206, 40, SW_GREEN, cv_cb, NULL, NULL);
        r++;
        row_label(box, "VOUT0 RATE", r);
        static const char *const DN[3] = {"1/16", "1/8", "1/4"};
        static const int DV[3] = {2, 4, 8};
        for (int i = 0; i < 3; i++)
            p->div[i] = sw_btn(box, DN[i], CTL_X + i * 70, r * ROW, 66, 40, accent, div_cb, (void *)(intptr_t)DV[i], NULL);
        r++;
        row_label(box, "VOUT1", r);
        p->v1[0] = sw_btn(box, "RESET", CTL_X, r * ROW, 100, 40, accent, v1_cb, (void *)CV_V1_RESET, NULL);
        p->v1[1] = sw_btn(box, "RUN", CTL_X + 106, r * ROW, 100, 40, accent, v1_cb, (void *)CV_V1_RUN, NULL);
        r++;
        p->status = sw_label(box, "", SW_F_SMALL, SW_TEXT, 0, r * ROW + 4, 330, false);
    } else if (m->role == TMOD_SEQUENCER) {
        sw_label(box, (m->flags & TMOD_F_CV) ? "CV: gate + pitch on the Mabee DACs.\nMIDI OUT per track."
                                             : "MIDI OUT per track.",
                 SW_F_SMALL, SW_DIM, 0, r * ROW + 6, 330, false);
    } else {
        row_label(box, "MIDI IN", r);
        p->midi = sw_label(box, "", SW_F_SMALL, SW_TEXT, CTL_X, r * ROW + 13, 210, false);
        lv_label_set_long_mode(p->midi, LV_LABEL_LONG_WRAP);
    }
    refresh(p);
    lv_timer_create(tick, 300, p);
    return box;
}

// --- the CV clock on one row, for a page with no room for the panel ----------------------
//
//   [CV CLOCK ON] [1/16] [RESET]  status        (430 x 40)

typedef struct {
    lv_obj_t *on, *div, *v1, *status;
} strip_t;

static void strip_refresh(strip_t *s) {
    bool on = studio_cv_clock();
    sw_btn_set(s->on, on);
    sw_btn_text(s->on, on ? "CV CLOCK ON" : "CV CLOCK OFF");
    int d = studio_cv_clock_div();
    sw_btn_text(s->div, d == 8 ? "1/4" : d == 4 ? "1/8" : "1/16");
    sw_btn_text(s->v1, studio_cv_clock_vout1() == CV_V1_RUN ? "RUN" : "RESET");
    static const char *const TXT[] = {"", "DAC 0x59", "no DAC", "seq has it", ""};
    int st = studio_cv_clock_state();
    lv_label_set_text(s->status, st >= 0 && st <= CVCLK_NO_DRUMS ? TXT[st] : "");
}

static void strip_tick(lv_timer_t *t) {
    strip_t *s = lv_timer_get_user_data(t);
    if (lv_obj_is_visible(s->on)) strip_refresh(s);
}

static void strip_div_cb(void *ud) {
    int d = studio_cv_clock_div();
    studio_set_cv_clock(studio_cv_clock(), d == 2 ? 4 : d == 4 ? 8 : 2, -1);
}

static void strip_v1_cb(void *ud) {
    studio_set_cv_clock(studio_cv_clock(), -1, studio_cv_clock_vout1() == CV_V1_RUN ? CV_V1_RESET : CV_V1_RUN);
}

lv_obj_t *studio_cv_strip(lv_obj_t *parent, int x, int y, int h, uint32_t accent) {
    strip_t *s = lv_malloc_zeroed(sizeof *s);
    s->on = sw_btn(parent, "CV CLOCK OFF", x, y, 140, h, SW_GREEN, cv_cb, NULL, NULL);
    s->div = sw_btn(parent, "1/16", x + 146, y, 66, h, accent, strip_div_cb, NULL, NULL);
    s->v1 = sw_btn(parent, "RESET", x + 218, y, 80, h, accent, strip_v1_cb, NULL, NULL);
    s->status = sw_label(parent, "", SW_F_SMALL, SW_DIM, x + 306, y + h / 2 - 8, 124, false);
    strip_refresh(s);
    lv_timer_create(strip_tick, 300, s);
    return s->on;
}
