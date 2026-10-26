// shell.c - screens, the Tulip-style app squares and the Studio home page.
//
// Each app gets its own LVGL screen (in PSRAM), built at start for the
// running apps and kept from then on, so switching is instant and an app's
// page state is still there when you come back. Hidden apps keep playing:
// only their screen is off.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "studio/widgets.h"
#include "studio_priv.h"
#include "tulip/display.h"
#include "tulip/settings.h"
#include "tulip/sys.h"
#include "tulip/touchcal_ui.h"

static lv_obj_t *s_scr[STUDIO_MAX_MODULES];
static lv_obj_t *s_home;
static int s_cur = -2;   // shown: module index, -1 = home
static bool s_leaving;

// --- switching ----------------------------------------------------------------------

static void set_enabled(int i, bool on) {
    if (g_enabled[i] == on) return;
    rt_post(HC_ENABLE, (uint8_t)i, 0, on);
    if (on)
        g_cfg.open_mask |= (uint8_t)(1u << i);
    else
        g_cfg.open_mask &= (uint8_t)~(1u << i);
    cfg_changed();
}

static lv_obj_t *new_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, sw_c(SW_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, sw_c(SW_TEXT), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void home_refresh(void);

static void build(int i) {
    if (s_scr[i]) return;
    s_scr[i] = new_screen();
    if (g_mods[i]->ui_create) g_mods[i]->ui_create(s_scr[i]);
}

static void show(int i) {
    if (i == s_cur) return;
    int old = s_cur;
    if (old >= 0 && g_mods[old]->ui_show) g_mods[old]->ui_show(false);
    s_cur = i;
    if (i < 0) {
        home_refresh();
        lv_screen_load(s_home);
    } else {
        if (!g_enabled[i]) set_enabled(i, true);
        build(i);
        lv_screen_load(s_scr[i]);
        if (g_mods[i]->ui_show) g_mods[i]->ui_show(true);
    }
    if (g_cfg.shown != i) {
        g_cfg.shown = (int8_t)i;
        cfg_changed();
    }
}

void studio_show_module(int i) {
    if (i >= 0 && i < g_nmods) show(i);
}

void studio_show_home(void) {
    if (studio_multi()) show(-1);
}

void studio_next_app(void) {
    // the running apps in order, then the home page, round and round
    int n = g_nmods;
    for (int step = 1; step <= n + 1; step++) {
        int k = s_cur + step;   // -1 (home) .. n-1
        k = ((k + 1) % (n + 1)) - 1;
        if (k == -1) {
            if (studio_multi()) {
                show(-1);
                return;
            }
            continue;
        }
        if (g_enabled[k]) {
            show(k);
            return;
        }
    }
}

// --- leaving for the launcher ------------------------------------------------------------

static void leave_wait(lv_timer_t *t) {
    static int ticks;
    studio_clock_status_t st;
    studio_clock_status(&st);
    bool busy = st.playing;
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i]->busy && g_mods[i]->busy()) busy = true;
    if (!busy || ++ticks > 50) tulip_exit_to_launcher();
}

void studio_exit_to_launcher(void) {
    if (s_leaving) return;
    s_leaving = true;
    studio_stop();
    lv_timer_create(leave_wait, 100, NULL);
}

// --- the app squares -----------------------------------------------------------------------

static void sq_next(void *ud) { studio_next_app(); }

static void sq_red(void *ud) {
    if (studio_multi() && s_cur >= 0)
        studio_show_home();
    else
        studio_exit_to_launcher();
}

static void sq_blue_long(lv_event_t *e) { studio_show_home(); }

lv_obj_t *studio_chrome(lv_obj_t *parent) {
    const int h = STUDIO_SQ, x = STUDIO_SQ_X, y = STUDIO_HDR_Y;
    int w = studio_multi() ? 2 * h + 6 : h;
    lv_obj_t *box = sw_box(lv_obj_get_screen(parent), x + (studio_multi() ? 0 : h + 6), y, w, h,
                           -1, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   // (children take the taps)
    if (studio_multi()) {
        lv_obj_t *b = sw_square(box, 0, 0, h, SW_SQ_BLUE, LV_SYMBOL_NEXT, sq_next, NULL);
        lv_obj_add_event_cb(b, sq_blue_long, LV_EVENT_LONG_PRESSED, NULL);
        sw_square(box, h + 6, 0, h, SW_SQ_RED, LV_SYMBOL_HOME, sq_red, NULL);
    } else if (tulip_under_launcher()) {
        sw_square(box, 0, 0, h, SW_SQ_RED, LV_SYMBOL_HOME, sq_red, NULL);
    } else {
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);   // flashed on its own: nowhere to go
    }
    return box;
}

// --- the Studio home page ------------------------------------------------------------------

#define TILE_W 326
#define TILE_H 150

static struct {
    lv_obj_t *play, *bpm, *clk_int, *clk_midi, *clk_out, *mmc, *status;
    lv_obj_t *tile[STUDIO_MAX_MODULES], *open_b[STUDIO_MAX_MODULES], *close_b[STUDIO_MAX_MODULES];
    lv_obj_t *state[STUDIO_MAX_MODULES];
    // routing rows
    int nrows;
    struct {
        int mod, track;
        lv_obj_t *dest, *ch, *chm, *chp;
    } row[8];
    lv_obj_t *vol, *cpu, *midi_mon;
    lv_obj_t *midi_rows[STUDIO_MAX_MODULES];
    // mixer
    lv_obj_t *mute[STUDIO_MAX_MODULES], *meter[STUDIO_MAX_MODULES];
    sw_slider_t *fader[STUDIO_MAX_MODULES];
    int meter_px[STUDIO_MAX_MODULES];
    int play_shown, bpm_shown;
    uint32_t mon_seq;
} H = {.play_shown = -1, .bpm_shown = -1};

static void h_play(void *ud) { studio_toggle_play(); }

// --- mixer ---------------------------------------------------------------------------------

#define METER_W 280

static float mix_get(void *ud) { return studio_mix_level((int)(intptr_t)ud) / 100.0f; }
static void mix_set(void *ud, float v) { studio_mix_set_level((int)(intptr_t)ud, (int)(v * 100 + 0.5f)); }
static void mix_text(void *ud, char *b, int n) {
    int v = studio_mix_level((int)(intptr_t)ud);
    if (v <= 0) {
        snprintf(b, n, "OFF");
        return;
    }
    float db = v >= STUDIO_MIX_UNITY
                   ? 2.0f * (v - STUDIO_MIX_UNITY) / (100 - STUDIO_MIX_UNITY)
                   : -40.0f * (float)(STUDIO_MIX_UNITY - v) / (STUDIO_MIX_UNITY - 1);
    snprintf(b, n, "%+.1f dB", (double)db);
}
static const sw_slider_ops_t MIX_OPS = {mix_get, mix_set, mix_text};

static void h_mute(void *ud) {
    int m = (int)(intptr_t)ud;
    studio_mix_set_mute(m, !studio_mix_muted(m));
    sw_btn_set(H.mute[m], studio_mix_muted(m));
}

static void mixer_create(lv_obj_t *s, int y) {
    sw_label(s, "MIXER", SW_F_LABEL, SW_ORANGE, 14, y, 0, false);
    sw_label(s, "each app's level into the master VOLUME; M mutes", SW_F_SMALL, SW_DIM, 110, y + 2,
             600, false);
    int j = 0;
    for (int i = 0; i < g_nmods; i++) {
        if (!(g_mods[i]->flags & TMOD_F_AUDIO)) continue;
        int x = 14 + j++ * 336, yy = y + 24;
        H.mute[i] = sw_btn(s, "M", x, yy + 17, 38, 34, SW_RED, h_mute, (void *)(intptr_t)i, NULL);
        sw_btn_set(H.mute[i], studio_mix_muted(i));
        H.fader[i] = sw_slider(s, g_mods[i]->title, x + 46, yy, METER_W, 44, SW_GREEN, &MIX_OPS,
                               (void *)(intptr_t)i);
        sw_box(s, x + 46, yy + 47, METER_W, 4, 0x242424, 0);
        H.meter[i] = sw_box(s, x + 46, yy + 47, 1, 4, SW_GREEN, 0);
        H.meter_px[i] = -1;
    }
}

// peak meters, ~15 Hz: a quick rise, a slow fall
static void mixer_poll(void) {
    static float hold[STUDIO_MAX_MODULES];
    for (int i = 0; i < g_nmods; i++) {
        if (!H.meter[i]) continue;
        float p = studio_mix_peak(i);
        hold[i] = p > hold[i] ? p : hold[i] * 0.8f;
        // -48 .. 0 dB across the bar
        float db = hold[i] > 1e-5f ? 20.0f * log10f(hold[i]) : -96.0f;
        float f = (db + 48.0f) / 48.0f;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        int px = (int)(f * METER_W);
        if (px < 1) px = 1;
        if (px == H.meter_px[i]) continue;
        H.meter_px[i] = px;
        lv_obj_set_width(H.meter[i], px);
        lv_obj_set_style_bg_color(H.meter[i], sw_c(db > -1.0f ? SW_RED : db > -9.0f ? SW_YELLOW : SW_GREEN), 0);
    }
}
static void h_rst(void *ud) { studio_restart(); }

static void h_bpm(void *ud) {
    int d = (int)(intptr_t)ud;
    int32_t v = (studio_bpm_x100() + 50) / 100 * 100 + d * 100;
    studio_set_bpm_x100(v);
}

static void h_clock(void *ud) {
    studio_set_clock_src((int)(intptr_t)ud);
    home_refresh();
}

static void h_clock_out(void *ud) {
    studio_set_clock_out(!studio_clock_out());
    home_refresh();
}

static void h_mmc(void *ud) {
    studio_set_mmc(!studio_mmc());
    home_refresh();
}

static void h_open(void *ud) { show((int)(intptr_t)ud); }

static void h_close(void *ud) {
    int i = (int)(intptr_t)ud;
    set_enabled(i, false);
    home_refresh();
}

// destinations a track can go to: preview (none), each synth module, MIDI OUT
static int dest_next(int cur) {
    int opts[STUDIO_MAX_MODULES + 2], n = 0;
    opts[n++] = ROUTE_NONE;
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i]->flags & TMOD_F_NOTES_IN) opts[n++] = i;
    opts[n++] = ROUTE_MIDI_OUT;
    for (int k = 0; k < n; k++)
        if (opts[k] == cur) return opts[(k + 1) % n];
    return ROUTE_NONE;
}

static void dest_text(char *buf, int len, int d) {
    if (d == ROUTE_NONE)
        snprintf(buf, len, "OWN SOUND");
    else if (d == ROUTE_MIDI_OUT)
        snprintf(buf, len, "MIDI OUT");
    else if (d >= 0 && d < g_nmods)
        snprintf(buf, len, "%s", g_mods[d]->title);
    else
        snprintf(buf, len, "?");
}

static void h_dest(void *ud) {
    int r = (int)(intptr_t)ud;
    int m = H.row[r].mod, k = H.row[r].track;
    studio_route_set(m, k, dest_next(studio_route_get(m, k)));
    home_refresh();
}

static void h_ch(void *ud) {
    int r = (int)(intptr_t)ud >> 1, d = ((int)(intptr_t)ud & 1) ? 1 : -1;
    int m = H.row[r].mod, k = H.row[r].track;
    studio_route_set_midi_ch(m, k, studio_route_midi_ch(m, k) + d);
    home_refresh();
}

static void h_vol(void *ud) {
    studio_set_volume(studio_volume() + (int)(intptr_t)ud);
    home_refresh();
}

static void h_cal(void *ud) { tulip_touchcal_start(NULL); }

static void home_create(void) {
    lv_obj_t *s = s_home = new_screen();
    // top bar
    sw_label(s, "TULIP STUDIO", SW_F_MID, SW_ORANGE, 14, 16, 0, false);
    H.play = sw_btn(s, "PLAY", 200, 8, 96, 40, SW_GREEN, h_play, NULL, SW_F_MID);
    sw_btn(s, "RST", 302, 8, 56, 40, SW_CYAN, h_rst, NULL, NULL);
    lv_obj_t *b = sw_btn(s, "-", 372, 8, 44, 40, SW_CYAN, h_bpm, (void *)-1, SW_F_MID);
    sw_btn_repeat(b);
    H.bpm = sw_label(s, "", SW_F_MID, SW_TEXT, 418, 18, 110, true);
    b = sw_btn(s, "+", 530, 8, 44, 40, SW_CYAN, h_bpm, (void *)1, SW_F_MID);
    sw_btn_repeat(b);
    H.clk_int = sw_btn(s, "INT", 590, 8, 58, 40, SW_CYAN, h_clock, (void *)CLOCK_SRC_INT, NULL);
    H.clk_midi = sw_btn(s, "MIDI", 652, 8, 64, 40, SW_CYAN, h_clock, (void *)CLOCK_SRC_MIDI, NULL);
    H.clk_out = sw_btn(s, "CLK OUT", 722, 8, 82, 40, SW_CYAN, h_clock_out, NULL, NULL);
    H.mmc = sw_btn(s, "MMC", 808, 8, 92, 40, SW_CYAN, h_mmc, NULL, NULL);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);

    // apps
    sw_label(s, "APPS", SW_F_LABEL, SW_ORANGE, 14, 68, 0, false);
    for (int i = 0; i < g_nmods; i++) {
        const tmod_t *m = g_mods[i];
        int x = 14 + (i % 3) * (TILE_W + 8), y = 92 + (i / 3) * (TILE_H + 8);
        lv_obj_t *t = H.tile[i] = sw_box(s, x, y, TILE_W, TILE_H, SW_BG, 4);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, sw_c(SW_LINE), 0);
        sw_label(t, m->title, SW_F_MID, SW_TEXT, 12, 10, 0, false);
        char tag[48];
        snprintf(tag, sizeof tag, "%s%s%s", studio_role_name(m->role),
                 (m->flags & TMOD_F_CV) ? "  CV" : "", (m->flags & TMOD_F_AUDIO) ? "  AUDIO" : "");
        uint32_t tc = m->role == TMOD_SEQUENCER ? SW_CYAN : m->role == TMOD_DRUMS ? SW_ORANGE
                                                                                 : SW_GREEN;
        sw_label(t, tag, SW_F_SMALL, tc, 12, 40, 0, false);
        lv_obj_t *bl = sw_label(t, m->blurb ? m->blurb : "", SW_F_SMALL, SW_DIM, 12, 60, TILE_W - 24,
                                false);
        lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
        H.state[i] = sw_label(t, "", SW_F_SMALL, SW_DIM, TILE_W - 90, 14, 78, false);
        lv_obj_set_style_text_align(H.state[i], LV_TEXT_ALIGN_RIGHT, 0);
        H.open_b[i] = sw_btn(t, "OPEN", 12, TILE_H - 46, 150, 36, SW_ORANGE, h_open,
                             (void *)(intptr_t)i, NULL);
        H.close_b[i] = sw_btn(t, "CLOSE", TILE_W - 112, TILE_H - 46, 100, 36, SW_RED, h_close,
                              (void *)(intptr_t)i, NULL);
    }

    // routing
    int y = 92 + ((g_nmods + 2) / 3) * (TILE_H + 8) + 6;
    sw_label(s, "ROUTING", SW_F_LABEL, SW_ORANGE, 14, y, 0, false);
    sw_label(s, "a sequencer track plays its own sound, a synth on the Tulip, or MIDI OUT",
             SW_F_SMALL, SW_DIM, 110, y + 2, 0, false);
    y += 24;
    H.nrows = 0;
    for (int i = 0; i < g_nmods && H.nrows < 8; i++) {
        const tmod_t *m = g_mods[i];
        for (int k = 0; k < m->n_note_outs && H.nrows < 8; k++) {
            int r = H.nrows++;
            int x = 14 + (r % 2) * 504, yy = y + (r / 2) * 46;
            H.row[r].mod = i;
            H.row[r].track = k;
            char t[40];
            snprintf(t, sizeof t, "%s %s", m->title,
                     m->note_out_names ? m->note_out_names[k] : "OUT");
            sw_label(s, t, SW_F_LABEL, SW_TEXT, x, yy + 11, 170, false);
            sw_label(s, LV_SYMBOL_RIGHT, SW_F_LABEL, SW_DIM, x + 170, yy + 11, 0, false);
            H.row[r].dest = sw_btn(s, "", x + 194, yy, 140, 38, SW_CYAN, h_dest,
                                   (void *)(intptr_t)r, NULL);
            H.row[r].chm = sw_btn(s, "-", x + 342, yy, 38, 38, SW_CYAN, h_ch,
                                  (void *)(intptr_t)(r << 1), NULL);
            H.row[r].ch = sw_label(s, "", SW_F_LABEL, SW_TEXT, x + 382, yy + 11, 70, true);
            H.row[r].chp = sw_btn(s, "+", x + 454, yy, 38, 38, SW_CYAN, h_ch,
                                  (void *)(intptr_t)((r << 1) | 1), NULL);
        }
    }

    // who listens to MIDI IN
    y += ((H.nrows + 1) / 2) * 46 + 10;
    sw_label(s, "MIDI IN", SW_F_LABEL, SW_ORANGE, 14, y, 0, false);
    sw_label(s, "clock + Start / Stop / Continue / Song Position / MMC go to every clocked app "
                "when the clock is MIDI; notes and CCs by channel:",
             SW_F_SMALL, SW_DIM, 110, y + 2, 900, false);
    for (int i = 0; i < g_nmods; i++) {
        int yy = y + 26 + i * 22;
        sw_label(s, g_mods[i]->title, SW_F_SMALL, SW_TEXT, 30, yy, 0, false);
        H.midi_rows[i] = sw_label(s, "", SW_F_SMALL, SW_DIM, 150, yy, 860, false);
    }
    mixer_create(s, y + 26 + g_nmods * 22 + 8);

    // bottom line: volume, load, MIDI monitor, touch calibration
    sw_line(s, 0, 540, 1024, 1);
    sw_label(s, "VOLUME", SW_F_SMALL, SW_DIM, 14, 562, 0, false);
    sw_btn(s, "-", 76, 550, 38, 38, SW_ORANGE, h_vol, (void *)-1, NULL);
    H.vol = sw_label(s, "", SW_F_LABEL, SW_TEXT, 116, 560, 40, true);
    sw_btn(s, "+", 158, 550, 38, 38, SW_ORANGE, h_vol, (void *)1, NULL);
    H.cpu = sw_label(s, "", SW_F_SMALL, SW_DIM, 214, 562, 170, false);
    sw_label(s, "MIDI IN", SW_F_SMALL, SW_DIM, 400, 562, 0, false);
    H.midi_mon = sw_label(s, "--", SW_F_SMALL, SW_TEXT, 460, 562, 300, false);
    sw_btn(s, "TOUCH CAL", 880, 550, 130, 38, SW_ORANGE, h_cal, NULL, NULL);
    home_refresh();
}

static void home_refresh(void) {
    if (!s_home) return;
    int src = studio_clock_src();
    sw_btn_set(H.clk_int, src == CLOCK_SRC_INT);
    sw_btn_set(H.clk_midi, src == CLOCK_SRC_MIDI);
    sw_btn_set(H.clk_out, studio_clock_out() && src == CLOCK_SRC_INT);
    sw_btn_set(H.mmc, studio_mmc());
    sw_btn_text(H.mmc, src == CLOCK_SRC_MIDI ? "MMC IN" : "MMC OUT");
    for (int i = 0; i < g_nmods; i++) {
        bool on = g_enabled[i];
        lv_label_set_text(H.state[i], on ? "RUNNING" : "CLOSED");
        lv_obj_set_style_text_color(H.state[i], sw_c(on ? SW_GREEN : SW_DIM), 0);
        lv_obj_set_style_border_color(H.tile[i], sw_c(on ? SW_EDGE : SW_LINE), 0);
        sw_btn_text(H.open_b[i], on ? "SHOW" : "OPEN");
        if (on)
            lv_obj_remove_flag(H.close_b[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(H.close_b[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int r = 0; r < H.nrows; r++) {
        int m = H.row[r].mod, k = H.row[r].track;
        int d = studio_route_get(m, k);
        char t[24];
        dest_text(t, sizeof t, d);
        sw_btn_text(H.row[r].dest, t);
        sw_btn_set(H.row[r].dest, d != ROUTE_NONE);
        bool midi = d == ROUTE_MIDI_OUT;
        snprintf(t, sizeof t, "CH %d", studio_route_midi_ch(m, k));
        lv_label_set_text(H.row[r].ch, t);
        lv_obj_t *objs[3] = {H.row[r].ch, H.row[r].chm, H.row[r].chp};
        for (int j = 0; j < 3; j++) {
            if (midi)
                lv_obj_remove_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char v[8];
    snprintf(v, sizeof v, "%d", studio_volume());
    lv_label_set_text(H.vol, v);
    for (int i = 0; i < g_nmods; i++) {
        char d[96] = "-";
        if (g_mods[i]->midi_desc)
            g_mods[i]->midi_desc(d, sizeof d);
        else if (g_mods[i]->flags & TMOD_F_CLOCKED)
            snprintf(d, sizeof d, "clock and transport only");
        lv_label_set_text(H.midi_rows[i], d);
    }
}

static void home_poll(void) {
    static int64_t next_meter;
    if (studio_now_us() > next_meter) {
        next_meter = studio_now_us() + 66000;
        mixer_poll();
    }
    studio_clock_status_t st;
    studio_clock_status(&st);
    int ps = st.playing ? 1 : st.armed ? 2 : 0;
    if (ps != H.play_shown) {
        H.play_shown = ps;
        static const char *const txt[3] = {"PLAY", "STOP", "ARMED"};
        sw_btn_text(H.play, txt[ps]);
        sw_btn_set(H.play, ps != 0);
        sw_btn_accent(H.play, ps == 2 ? SW_YELLOW : SW_GREEN);
    }
    int bv = st.ext ? -1 - st.ext_bpm_x10 : (int)studio_bpm_x100();
    if (bv != H.bpm_shown) {
        H.bpm_shown = bv;
        char t[24];
        if (bv >= 0) {
            int32_t b = studio_bpm_x100();
            if (b % 100)
                snprintf(t, sizeof t, "%d.%d BPM", (int)(b / 100), (int)(b % 100) / 10);
            else
                snprintf(t, sizeof t, "%d BPM", (int)(b / 100));
        } else if (st.ext_bpm_x10) {
            snprintf(t, sizeof t, "MIDI %d", (st.ext_bpm_x10 + 5) / 10);
        } else {
            snprintf(t, sizeof t, "MIDI --");
        }
        lv_label_set_text(H.bpm, t);
    }
    static int64_t next;
    int64_t now = studio_now_us();
    if (now > next) {
        next = now + 500000;
        char t[72];
        const char *bad = studio_audio_problem();
        if (bad)
            snprintf(t, sizeof t, "%s", bad);
        else
            snprintf(t, sizeof t, "AUDIO %d%%   XRUN %u", studio_audio_load_pct(),
                     (unsigned)studio_audio_underruns());
        lv_obj_set_style_text_color(H.cpu, sw_c(bad ? SW_RED : SW_DIM), 0);
        lv_label_set_text(H.cpu, t);
    }
    char mon[40];
    uint32_t seq = studio_midi_monitor(mon, sizeof mon);
    if (seq != H.mon_seq) {
        H.mon_seq = seq;
        lv_label_set_text(H.midi_mon, mon);
    }
}

// --- UI task entry points ---------------------------------------------------------------------

void shell_ui_create(void) {
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *t = lv_theme_default_init(d, sw_c(SW_ORANGE), sw_c(SW_CYAN), true,
                                          &lv_font_montserrat_14);
    lv_display_set_theme(d, t);
    lv_obj_t *boot = lv_screen_active();   // LVGL's default screen: replaced below
    if (studio_multi()) home_create();
    // every running app gets its screen now (its MIDI CC / program change
    // handling runs in its UI poll, shown or not); closed ones when opened
    for (int i = 0; i < g_nmods; i++)
        if (g_enabled[i]) build(i);
    int first = studio_multi() ? g_cfg.shown : 0;
    if (first >= g_nmods || (first >= 0 && !g_enabled[first])) first = studio_multi() ? -1 : 0;
    show(first);
    if (boot != lv_screen_active()) lv_obj_delete(boot);
}

// No sound would otherwise fail silently: on every page, a red line at the
// bottom says why while audio is down.
static void audio_banner(void) {
    static lv_obj_t *lbl;
    static int64_t next;
    int64_t now = studio_now_us();
    if (now < next) return;
    next = now + 500000;
    const char *bad = studio_audio_problem();
    if (!bad) {
        if (lbl) lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!lbl) {
        lbl = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(lbl, sw_c(SW_RED), 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lbl, sw_c(0xffffff), 0);
        lv_obj_set_style_pad_hor(lbl, 8, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    lv_label_set_text(lbl, bad);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

void shell_ui_poll(void) {
    audio_banner();
    for (int i = 0; i < g_nmods; i++)
        if (s_scr[i] && g_mods[i]->ui_poll) g_mods[i]->ui_poll(s_cur == i);
    if (s_cur == -1) home_poll();
    tulip_touchcal_poll();
    cfg_poll();
}
