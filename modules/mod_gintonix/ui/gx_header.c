// gx_header.c - the header: pages, kits, the preset bank, status and the
// Studio squares. (Header in gintonix.py.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_ui_int.h"

static struct {
    lv_obj_t *pages[4];
    gx_pick_t *kit_pick, *slot_pick;
    lv_obj_t *del_btn, *status, *cpu;
    uint32_t del_armed;
    char cpu_shown[24];
    char kit_txt[12];
    const char **kit_opts;
} H;

static void page_cb(void *ud) { show_page((int)(intptr_t)ud); }

void header_set_page(int i) {
    for (int j = 0; j < 4; j++) sw_btn_set(H.pages[j], j == i);
}

void header_status(const char *msg) {
    char t[40];
    if (!msg) {
        snprintf(t, sizeof t, "%s%.17s", A.dirty ? "*" : "", A.patch_name);
        msg = t;
    }
    lv_label_set_text(H.status, msg);
    lv_obj_set_style_text_color(H.status, sw_c(A.dirty ? ACC : C_DIM), 0);
}

static void disarm_del(void) {
    H.del_armed = 0;
    sw_btn_text(H.del_btn, "DEL");
    sw_btn_set(H.del_btn, false);
}

static char s_slot_txt[GX_BANK_SLOTS][32];
static const char *s_slot_opts[GX_BANK_SLOTS];

void header_slot_ui(void) {
    for (int i = 0; i < GX_BANK_SLOTS; i++) {
        const char *n = slot_name(i + 1);
        snprintf(s_slot_txt[i], sizeof s_slot_txt[i], "%03d  %s", i + 1, n ? n : "---");
        s_slot_opts[i] = s_slot_txt[i];
    }
    pick_set_options(H.slot_pick, s_slot_opts, GX_BANK_SLOTS);
    pick_set_selected(H.slot_pick, g_gxset.slot - 1);
    disarm_del();
    header_status(NULL);
}

static void kit_cb(int i) {
    load_kit(i);
    pick_set_text(H.kit_pick, "KITS");
}

static void open_kits(void) {
    int n = kit_count();
    static const char **opts;
    free(opts);
    opts = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) opts[i] = kit_name(i);
    pick_set_options(H.kit_pick, opts, n);
    pick_set_text(H.kit_pick, "KITS");
}

static void slot_cb(int i) { load_slot(i + 1); }

static void step_slot(void *ud) {
    load_slot((g_gxset.slot - 1 + (int)(intptr_t)ud + GX_BANK_SLOTS) % GX_BANK_SLOTS + 1);
}

static void load_cb(void *ud) { load_slot(g_gxset.slot); }

static void save_cb(void *ud) {
    disarm_del();
    save_kit_dialog();
}

static void del_cb(void *ud) {
    if (!slot_name(g_gxset.slot)) {
        char t[24];
        snprintf(t, sizeof t, "%03d empty", g_gxset.slot);
        header_status(t);
        return;
    }
    uint32_t now = (uint32_t)(studio_now_us() / 1000);
    if (!(H.del_armed && now - H.del_armed < 3000)) {
        H.del_armed = now ? now : 1;
        sw_btn_text(H.del_btn, "SURE?");
        sw_btn_set(H.del_btn, true);
        char t[24];
        snprintf(t, sizeof t, "delete %03d?", g_gxset.slot);
        header_status(t);
        return;
    }
    delete_slot(g_gxset.slot);
}

#define Y  STUDIO_HDR_Y       // the same header shape as every Studio app
#define H_ STUDIO_HDR_BTN_H

void header_create(lv_obj_t *s) {
    lv_obj_t *g = sw_box(s, 0, 0, 1024, PAGE_Y - 2, C_BG, 0);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    sw_label(g, "GINTONIX", F_BIG, C_TEXT, 6, 13, 0, false);
    static const char *const names[4] = {"MAIN", "SEQ", "MIDI", "TOOLS"};
    for (int j = 0; j < 4; j++)
        H.pages[j] = sw_btn(g, names[j], 128 + j * 62, Y, 58, H_, ACC, page_cb, (void *)(intptr_t)j,
                            NULL);
    H.kit_pick = pick_new(g, 380, Y, 70, H_, ACC, "LOAD KIT", kit_cb);
    open_kits();
    sw_btn(g, "<", 456, Y, 36, H_, ACC, step_slot, (void *)-1, NULL);
    H.slot_pick = pick_new(g, 496, Y, 134, H_, ACC, "KIT SLOT", slot_cb);
    sw_btn(g, ">", 634, Y, 36, H_, ACC, step_slot, (void *)1, NULL);
    sw_btn(g, "LOAD", 674, Y, 48, H_, ACC, load_cb, NULL, NULL);
    sw_btn(g, "SAVE...", 726, Y, 66, H_, ACC, save_cb, NULL, NULL);
    H.del_btn = sw_btn(g, "DEL", 796, Y, 46, H_, ACC, del_cb, NULL, NULL);
    H.cpu = sw_label(g, "", F_SMALL, C_DIM, 848, 10, 78, false);
    H.status = sw_label(g, "", F_SMALL, C_DIM, 848, 28, 78, false);
    lv_label_set_long_mode(H.status, LV_LABEL_LONG_CLIP);
    // the Studio squares: blue = next app, red = Studio home / launcher
    studio_chrome(g);
    sw_line(g, 0, STUDIO_HDR_LINE_Y, 1024, 1);
}

void header_tick(uint32_t now) {
    if (A.status_dirty) {
        A.status_dirty = false;
        header_status(NULL);
    }
    if (H.del_armed && now - H.del_armed > 3000) disarm_del();
    char t[24];
    snprintf(t, sizeof t, "DSP %d%%", g_gxs.load_pct);
    if (strcmp(t, H.cpu_shown)) {
        snprintf(H.cpu_shown, sizeof H.cpu_shown, "%s", t);
        lv_label_set_text(H.cpu, t);
    }
}

// --- the pattern bar (MAIN + SEQ) -----------------------------------------------------------------

struct gx_pbar {
    lv_obj_t *btns[GX_NPAT], *lbls[GX_NPAT];
    lv_obj_t *chain_b, *play_b;
    uint32_t shown;
};

static void pat_tap(lv_event_t *e) { select_pattern((int)(intptr_t)lv_event_get_user_data(e)); }

static void chain_cb(void *ud) {
    gx_pat_t *p = &g_gx.pats[g_gx.cur];
    if (g_gx.cur < GX_NPAT - 1) {
        p->chain = !p->chain;
        pattern_edited();
    }
}

static void play_cb(void *ud) { transport_toggle(); }

gx_pbar_t *pbar_new(lv_obj_t *g, int x, int y, int cols, int bw, int bh, int gap, int cx, int cy,
                    int cw, int ch, int px, int py, int pw, int ph) {
    gx_pbar_t *p = calloc(1, sizeof *p);
    for (int i = 0; i < GX_NPAT; i++) {
        lv_obj_t *b = p->btns[i] = sw_plain(lv_obj_create(g), C_BG, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, x + (i % cols) * (bw + gap), y + (i / cols) * (bh + gap));
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        char t[2] = {GX_PAT_NAMES[i], 0};
        lv_obj_t *l = p->lbls[i] = lv_label_create(b);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, F_MID, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, pat_tap, LV_EVENT_PRESSED, (void *)(intptr_t)i);
    }
    p->chain_b = sw_btn(g, "CHAIN >", cx, cy, cw, ch, CHAIN_C, chain_cb, NULL, F_LABEL);
    p->play_b = sw_btn(g, "PLAY", px, py, pw, ph, PLAY_C, play_cb, NULL, F_BIG);
    p->shown = UINT32_MAX;
    return p;
}

void pbar_tick(gx_pbar_t *p) {
    studio_clock_status_t st;
    studio_clock_status(&st);
    int first, n;
    gx_chain_of(g_gx.pats, g_gx.cur, &first, &n);
    uint32_t nonempty = 0;
    for (int i = 0; i < GX_NPAT; i++)
        if (!gx_pat_empty(&g_gx.pats[i])) nonempty |= 1u << i;
    bool play = audible();
    // a signature of everything drawn, so nothing is redrawn needlessly
    uint32_t sig = (uint32_t)g_gx.cur | ((uint32_t)(play ? g_gxs.pat : 15) << 4) |
                   ((uint32_t)(g_live.next & 15) << 8) | ((uint32_t)first << 12) |
                   ((uint32_t)n << 16) | ((uint32_t)st.playing << 20) | ((uint32_t)st.armed << 21) |
                   ((uint32_t)g_gx.pats[g_gx.cur].chain << 22);
    sig ^= nonempty * 2654435761u;
    if (sig == p->shown) return;
    p->shown = sig;
    for (int i = 0; i < GX_NPAT; i++) {
        bool sel = i == g_gx.cur;
        bool playing = play && i == g_gxs.pat;
        lv_obj_set_style_bg_color(p->btns[i], sw_c(sel ? SEQ_ACC : C_BG), 0);
        lv_obj_set_style_border_color(
            p->btns[i], sw_c(playing ? PLAY_C : (i == g_live.next ? SEQ_ACC : C_BTN_EDGE)), 0);
        uint32_t tc;
        if (sel)
            tc = C_BG;
        else if (n > 1 && i >= first && i < first + n)
            tc = CHAIN_C;
        else if (nonempty & (1u << i))
            tc = C_TEXT;
        else
            tc = C_LINE;
        lv_obj_set_style_text_color(p->lbls[i], sw_c(tc), 0);
    }
    sw_btn_set(p->chain_b, g_gx.pats[g_gx.cur].chain);
    sw_btn_set(p->play_b, st.playing || st.armed);
    sw_btn_accent(p->play_b, st.armed ? ACC_C : PLAY_C);
    sw_btn_text(p->play_b, st.armed ? "ARMED" : (st.playing ? "STOP" : "PLAY"));
}
