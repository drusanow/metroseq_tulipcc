// gx_tools.c - the TOOLS page (ToolsPage in gintonix.py): the Microtonic's
// menu functions - randomize, alter, init, copy / paste / exchange for a
// drum patch and for patterns, shift, reverse, morph tools - and UNDO.
#include <stdio.h>
#include <string.h>

#include "gx_ui_int.h"

static struct {
    lv_obj_t *msg, *only_b;
    bool only;
    bool have_patch, have_pat, have_undo;
    gx_patch_t clip_a, clip_b;
    char clip_name[GX_NAME_MAX + 1];
    bool clip_only;
    int clip_ch;
    gx_pat_t clip_pat;
    gx_state_t undo;
    uint32_t rng;
} T = {.rng = 0x1234567u};

static float rnd(void *x) {
    (void)x;
    uint32_t v = T.rng;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    T.rng = v;
    return (float)(v >> 8) / 16777216.0f;
}

static void snap(void) {
    T.undo = g_gx;
    T.have_undo = true;
    T.rng ^= (uint32_t)studio_now_us();
}

static void msg(const char *m) { lv_label_set_text(T.msg, m); }

static void done(const char *m, bool sound, bool pattern) {
    if (sound) sound_edited(-1);
    if (pattern) pattern_edited();
    refresh_all();
    edited();
    msg(m);
}

static uint8_t chans(void) { return T.only ? (uint8_t)(1u << A.sel) : 0xFF; }

// --- drum patch -----------------------------------------------------------------------------

static void play_sel(void) {
    gx_sound_changed(A.sel);
    gx_trigger(A.sel, 1.0f);
}

static void rnd_patch(void *ud) {
    snap();
    int c = A.sel;
    gx_random_patch(&g_gx.a[c], rnd, NULL);
    g_gx.b[c] = g_gx.a[c];
    snprintf(g_gx.names[c], sizeof g_gx.names[c], "Random");
    char t[48];
    snprintf(t, sizeof t, "channel %d: random drum patch", c + 1);
    done(t, true, false);
    play_sel();
}

static void alter_patch(void *ud) {
    snap();
    int c = A.sel;
    gx_alter_patch(&g_gx.a[c], 0.08f, rnd, NULL);
    gx_alter_patch(&g_gx.b[c], 0.08f, rnd, NULL);
    char t[48];
    snprintf(t, sizeof t, "channel %d: altered", c + 1);
    done(t, true, false);
    play_sel();
}

static void init_patch(void *ud) {
    snap();
    int c = A.sel;
    gx_patch_default(&g_gx.a[c]);
    gx_patch_default(&g_gx.b[c]);
    snprintf(g_gx.names[c], sizeof g_gx.names[c], "Init");
    char t[48];
    snprintf(t, sizeof t, "channel %d: init", c + 1);
    done(t, true, false);
}

static void copy_patch(void *ud) {
    int c = A.sel;
    T.clip_a = g_gx.a[c];
    T.clip_b = g_gx.b[c];
    snprintf(T.clip_name, sizeof T.clip_name, "%s", g_gx.names[c]);
    T.have_patch = true;
    char t[48];
    snprintf(t, sizeof t, "channel %d copied", c + 1);
    msg(t);
}

static void paste_patch(void *ud) {
    if (!T.have_patch) {
        msg("nothing copied yet");
        return;
    }
    snap();
    int c = A.sel;
    bool swap = ud != NULL;
    gx_patch_t oa = g_gx.a[c], ob = g_gx.b[c];
    char on[GX_NAME_MAX + 1];
    snprintf(on, sizeof on, "%s", g_gx.names[c]);
    g_gx.a[c] = T.clip_a;
    g_gx.b[c] = T.clip_b;
    snprintf(g_gx.names[c], sizeof g_gx.names[c], "%s", T.clip_name);
    if (swap) {
        T.clip_a = oa;
        T.clip_b = ob;
        snprintf(T.clip_name, sizeof T.clip_name, "%s", on);
    }
    char t[48];
    snprintf(t, sizeof t, swap ? "channel %d <-> clipboard" : "pasted into channel %d", c + 1);
    done(t, true, false);
}

static void rnd_all(void *ud) {
    snap();
    for (int c = 0; c < GX_NCH; c++) {
        gx_random_patch(&g_gx.a[c], rnd, NULL);
        g_gx.b[c] = g_gx.a[c];
        snprintf(g_gx.names[c], sizeof g_gx.names[c], "Random");
    }
    done("all drum patches random", true, false);
}

static void alter_all(void *ud) {
    snap();
    for (int c = 0; c < GX_NCH; c++) {
        gx_alter_patch(&g_gx.a[c], 0.08f, rnd, NULL);
        gx_alter_patch(&g_gx.b[c], 0.08f, rnd, NULL);
    }
    done("all drum patches altered", true, false);
}

// ALTER A / ALTER B (also on MAIN): one end point of all 8 sounds
void tools_alter_ab(bool b) {
    snap();
    for (int c = 0; c < GX_NCH; c++) gx_alter_patch(b ? &g_gx.b[c] : &g_gx.a[c], 0.25f, rnd, NULL);
    done(b ? "morph B end-points altered (move MORPH to hear)"
           : "morph A end-points altered (move MORPH to hear)",
         true, false);
}

static void alter_ab_cb(void *ud) { tools_alter_ab(ud != NULL); }

static void morph_copy(void *ud) {
    snap();
    gx_morph_copy(&g_gx, ud != NULL);
    done(ud ? "A > B" : "B > A", true, false);
}

// --- pattern -------------------------------------------------------------------------------------

static void pat_msg(const char *what) {
    char t[48];
    snprintf(t, sizeof t, "pattern %c: %s", GX_PAT_NAMES[g_gx.cur], what);
    done(t, false, true);
}

static void toggle_only(void *ud) {
    T.only = !T.only;
    sw_btn_set(T.only_b, T.only);
}

static void rnd_pat(void *ud) {
    snap();
    gx_pat_t *p = &g_gx.pats[g_gx.cur], r;
    gx_random_pattern(&r, p->len, rnd, NULL);
    uint8_t m = chans();
    for (int c = 0; c < GX_NCH; c++) {
        if (!(m & (1u << c))) continue;
        p->trig[c] = r.trig[c];
        p->acc[c] = r.acc[c];
        p->fill[c] = r.fill[c];
    }
    pat_msg("random");
}

static void alter_pat(void *ud) {
    snap();
    gx_pat_alter(&g_gx.pats[g_gx.cur], chans(), rnd, NULL);
    pat_msg("altered");
}

static void rnd_af(void *ud) {
    snap();
    gx_random_accents_fills(&g_gx.pats[g_gx.cur], chans(), rnd, NULL);
    pat_msg("new accents + fills");
}

static void shift(void *ud) {
    snap();
    gx_pat_shift(&g_gx.pats[g_gx.cur], ud ? 1 : -1, chans());
    pat_msg("shifted");
}

static void reverse(void *ud) {
    snap();
    gx_pat_reverse(&g_gx.pats[g_gx.cur], chans());
    pat_msg("reversed");
}

static void clear(void *ud) {
    snap();
    gx_pat_t *p = &g_gx.pats[g_gx.cur];
    uint8_t m = chans();
    for (int c = 0; c < GX_NCH; c++)
        if (m & (1u << c)) p->trig[c] = p->acc[c] = p->fill[c] = 0;
    pat_msg("cleared");
}

static void copy_pat(void *ud) {
    T.clip_pat = g_gx.pats[g_gx.cur];
    T.clip_only = T.only;
    T.clip_ch = A.sel;
    T.have_pat = true;
    char t[48];
    if (T.only)
        snprintf(t, sizeof t, "pattern %c copied (channel %d)", GX_PAT_NAMES[g_gx.cur], A.sel + 1);
    else
        snprintf(t, sizeof t, "pattern %c copied", GX_PAT_NAMES[g_gx.cur]);
    msg(t);
}

static void paste_pat(void *ud) {
    if (!T.have_pat) {
        msg("nothing copied yet");
        return;
    }
    snap();
    bool swap = ud != NULL;
    gx_pat_t *p = &g_gx.pats[g_gx.cur];
    gx_pat_t old = *p;
    if (T.clip_only) {
        p->trig[A.sel] = T.clip_pat.trig[T.clip_ch];
        p->acc[A.sel] = T.clip_pat.acc[T.clip_ch];
        p->fill[A.sel] = T.clip_pat.fill[T.clip_ch];
    } else {
        uint8_t chain = p->chain;
        gx_lock();   // the audio task reads patterns
        *p = T.clip_pat;
        p->chain = chain;
        gx_unlock();
    }
    if (swap) {
        T.clip_pat = old;
        T.clip_ch = A.sel;
    }
    pat_msg(swap ? "exchanged" : "pasted");
}

static void undo(void *ud) {
    if (!T.have_undo) {
        msg("nothing to undo");
        return;
    }
    T.have_undo = false;
    float tempo = g_gx.tempo;
    gx_lock();
    g_gx = T.undo;
    g_gx.tempo = tempo;
    gx_unlock();
    done("undone", true, true);
}

void tools_page_create(lv_obj_t *g) {
    sw_label(g, "DRUM PATCH  (selected channel)", F_MID, ACC, 12, 8, 0, false);
    struct {
        const char *n;
        sw_cb_t f;
        void *ud;
    } row1[] = {{"RANDOMIZE", rnd_patch, 0}, {"ALTER", alter_patch, 0}, {"INIT", init_patch, 0},
                {"COPY", copy_patch, 0},     {"PASTE", paste_patch, 0}, {"EXCHANGE", paste_patch, (void *)1}};
    for (int j = 0; j < 6; j++)
        sw_btn(g, row1[j].n, 12 + j * 140, 40, 132, 40, ACC, row1[j].f, row1[j].ud, F_LABEL);
    sw_label(g, "ALL 8 DRUM PATCHES", F_MID, ACC, 12, 98, 0, false);
    struct {
        const char *n;
        sw_cb_t f;
        void *ud;
    } row2[] = {{"RANDOMIZE ALL", rnd_all, 0},     {"ALTER ALL", alter_all, 0},
                {"A > B (morph)", morph_copy, (void *)1}, {"B > A (morph)", morph_copy, 0},
                {"ALTER A", alter_ab_cb, 0},       {"ALTER B", alter_ab_cb, (void *)1}};
    for (int j = 0; j < 6; j++)
        sw_btn(g, row2[j].n, 12 + j * 168, 128, 160, 40, ACC, row2[j].f, row2[j].ud, F_LABEL);
    sw_label(g, "PATTERN  (selected pattern)", F_MID, SEQ_ACC, 12, 188, 0, false);
    T.only_b = sw_btn(g, "SELECTED CHANNEL ONLY", 340, 182, 220, 32, SEQ_ACC, toggle_only, NULL,
                      F_LABEL);
    struct {
        const char *n;
        sw_cb_t f;
        void *ud;
    } row3[] = {{"RANDOMIZE", rnd_pat, 0},  {"ALTER", alter_pat, 0}, {"ACCENTS + FILLS", rnd_af, 0},
                {"<< SHIFT", shift, 0},     {"SHIFT >>", shift, (void *)1}, {"REVERSE", reverse, 0},
                {"CLEAR", clear, 0}};
    for (int j = 0; j < 7; j++)
        sw_btn(g, row3[j].n, 12 + j * 142, 222, 134, 40, SEQ_ACC, row3[j].f, row3[j].ud, F_LABEL);
    sw_btn(g, "COPY", 12, 272, 134, 40, SEQ_ACC, copy_pat, NULL, F_LABEL);
    sw_btn(g, "PASTE", 154, 272, 134, 40, SEQ_ACC, paste_pat, NULL, F_LABEL);
    sw_btn(g, "EXCHANGE", 296, 272, 134, 40, SEQ_ACC, paste_pat, (void *)1, F_LABEL);
    sw_line(g, 12, 330, 1000, 1);
    sw_btn(g, "UNDO", 12, 344, 160, 48, C_TEXT, undo, NULL, F_MID);
    T.msg = sw_label(g, "", F_LABEL, C_DIM, 190, 360, 820, false);
    sw_label(g,
             "Morph: every drum knob has an A and a B end-point; the MORPH fader (MAIN page) fades "
             "between them.  Turning a knob with MORPH between A and B moves both end-points.  A > B "
             "copies A into B (then change B, e.g. at MORPH = B).  ALTER A / ALTER B (also on MAIN) "
             "randomize one end-point of all 8 sounds; UNDO undoes it.",
             F_LABEL, C_DIM, 12, 410, 1000, false);
    lv_label_set_long_mode(lv_obj_get_child(g, -1), LV_LABEL_LONG_WRAP);
}
