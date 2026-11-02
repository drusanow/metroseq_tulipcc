// studio_preview.c - render the Studio's real UI code on the host: the
// Studio home page, metroseq, GinTonix and Sloe, through the panel's
// RGB332 conversion.   make -C tools/ui_preview -> build/*.png
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

#include "app.h"
#include "gx_app.h"
#include "harness.h"
#include "lvgl.h"
#include "mod_gintonix.h"
#include "mod_sloe.h"
#include "mod_wavetron.h"
#include "mod_drumster9.h"
#include "d9_app.h"
#include "platform/preview_audio.h"
#include "platform/storage.h"
#include "studio_priv.h"
#include "tulip/settings.h"
#include "tulip/touch_cal.h"
#include "ui/ui.h"
#include "ui/ui_common.h"

void preview_ui_create(void);
void preview_set_clock(bool playing, bool armed, bool ext, int ext_bpm_x10);
void preview_set_monitor(const char *t);
void shell_ui_poll(void);

// --- stand-ins for metroseq's platform layer ---------------------------------

pattern_t g_pat;
seq_status_t g_status;
void pat_lock(void) {}
void pat_unlock(void) {}
void pat_store(const pattern_t *src) { g_pat = *src; }
void pat_load(pattern_t *dst) { *dst = g_pat; }
static uint32_t s_swaps;
static bool s_swap_pending;
static pattern_t s_next;
void pat_swap(const pattern_t *p) {
    s_next = *p;
    if (!g_status.playing) {
        g_pat = s_next;
        s_swaps++;
    } else {
        s_swap_pending = true;
    }
}
bool pat_swap_pending(void) { return s_swap_pending; }
bool pat_swap_cancel(void) {
    bool w = s_swap_pending;
    s_swap_pending = false;
    return w;
}
uint32_t pat_swap_count(void) { return s_swaps; }
void pat_swap_poll(void) {}
static int s_swap_at;
int pat_swap_at(void) { return s_swap_at; }
void pat_set_swap_at(int v) { s_swap_at = v; }
const char *pat_swap_at_name(int v) {
    static const char *const N[] = {"BAR", "2 BARS", "4 BARS", "TRK 1 END"};
    return v >= 0 && v < 4 ? N[v] : "?";
}
const char *pat_swap_when(void) { return "next bar"; }
uint32_t pat_snapshot(pattern_t *d) {
    *d = g_pat;
    return s_swaps;
}
static int s_cur = -1;
int storage_cur_preset(void) { return s_cur; }
void storage_set_cur_preset(int s) { s_cur = s; }
void status_info(const char *msg) {
    snprintf(g_status.info, sizeof g_status.info, "%s", msg);
    g_status.info_seq++;
}
void status_info_get(char *buf, int len, uint32_t *seq) {
    snprintf(buf, len, "%s", g_status.info);
    *seq = g_status.info_seq;
}
bool seq_cmd(seq_cmd_t cmd) {
    if (cmd == CMD_TOGGLE_PLAY) g_status.playing = !g_status.playing;
    return true;
}
void preview_render(float *l, float *r, const studio_block_t *b) {}
static bool s_audio = true;
void preview_set_enabled(bool on) { s_audio = on; }
bool preview_enabled(void) { return s_audio; }
static pattern_t s_presets[NUM_PRESETS];
static bool s_used[NUM_PRESETS];
bool storage_preset_used(int s) { return s_used[s]; }
bool storage_preset_get(int s, pattern_t *o) {
    if (!s_used[s]) return false;
    *o = s_presets[s];
    return true;
}
void storage_preset_save(int s, const pattern_t *p) {
    s_presets[s] = *p;
    s_used[s] = true;
}
int storage_project_count(void) { return 2; }
const char *storage_project_name(int i) { return i ? "Dark Arp" : "Live Set A"; }
bool storage_project_get(int i, pattern_t *o) {
    pattern_default(o);
    return true;
}
int storage_project_save(const char *n, const pattern_t *p) { return 0; }
bool storage_write_pending(void) { return false; }


static const char *const OUTS[2] = {"TRK 1", "TRK 2"};
tmod_t mod_metroseq = {
    .name = "metroseq",
    .title = "METROSEQ",
    .blurb = "Metropolix-style 2-track sequencer: gate + 1 V/oct on the Mabee DACs",
    .role = TMOD_SEQUENCER,
    .flags = TMOD_F_CV | TMOD_F_AUDIO | TMOD_F_CLOCKED,
    .n_note_outs = 2,
    .note_out_names = OUTS,
    .ui_create = ui_create,
    .ui_poll = ui_poll,
};

#define TAB_Y 21

int main(void) {
    pattern_default(&g_pat);
    g_pat.lane_cc[0] = 74;
    storage_preset_save(0, &g_pat);
    storage_preset_save(3, &g_pat);
    storage_preset_save(5, &g_pat);
    preset_init();
    g_status.cv_enabled = true;
    g_status.dac_present[0] = true;
    g_status.cv_ready = true;
    g_status.touch_ready = true;
    g_status.late_count = 4096;
    g_status.late_sum_us = 4096 * 112;
    g_status.late_max_us = 231;

    hx_init();
    hx_poll = shell_ui_poll;
    static const tmod_t *const mods[] = {&mod_metroseq, &mod_gintonix, &mod_sloe, &mod_wavetron,
                                         &mod_drumster9};
    studio_run(mods, 5);
    g_cfg.shown = -1;   // start on the home page
    g_cfg.bpm_x100 = 12400;
    studio_route_set(0, 0, 2);            // TRK 1 -> SLOE
    studio_route_set(0, 1, ROUTE_MIDI_OUT);
    preview_ui_create();
    preview_set_clock(true, false, false, 0);
    hx_pump(300);
    hx_save("studio_0_home_all_off");   // the Studio starts with every app off
    for (int i = 0; i < 5; i++) shell_set_open(i, true);
    hx_pump(600);
    hx_save("studio_1_home");

    // metroseq (its existing pictures)
    studio_show_module(0);
    hx_pump(50);
    g_status.playing = true;   // playhead LEDs
    g_status.disp_stage[0] = 2;
    g_status.disp_stage[1] = 5;
    g_status.lane_disp[0] = 3;
    status_info("CV ready");
    hx_pump(50);
    hx_tap(SEQ_X0 + 3 * 70 + 30, PAGE_Y + 456 + 22 + 20);   // preset 4: waits for the top
    hx_pump(50);
    hx_save("metroseq_1_seq");
    hx_tap(SEQ_X0 + 3 * COL_W + 50, PAGE_Y + 4 + 14 + 15);   // stage 4 -> detail
    hx_save("metroseq_2_detail");
    hx_tap(512 - 280 + 440 + 50, 300 - 160 + 12 + 20);       // Close
    show_page(1);
    hx_pump(50);
    hx_save("metroseq_3_trk");
    show_page(2);
    hx_pump(50);
    hx_save("metroseq_4_mod");
    show_page(3);
    hx_pump(300);
    hx_save("metroseq_5_set");
    hx_tap(24 + 75, PAGE_Y + 358 + 20);   // SAVE AS...
    hx_save("metroseq_6_save_project");
    hx_tap(232 + 434 + 55, 45 + 102 + 20); // Cancel
    show_page(0);
    preview_set_clock(false, true, true, 1243);   // following MIDI clock, armed
    g_status.playing = false;
    hx_pump(100);
    hx_save("metroseq_9_midi_armed");
    preview_set_clock(true, false, false, 0);
    g_status.playing = true;

    // GinTonix: blue square = next app
    studio_next_app();
    hx_pump(100);
    g_gxs.running = true;
    g_gxs.pat = 0;
    g_gxs.step = 6;
    g_gxs.hit_ms[0] = g_gxs.hit_ms[2] = hx_now_ms();
    g_gxs.load_pct = 11;
    hx_pump(60);
    hx_save("gintonix_1_main");
    hx_touch(267, 184, 1);                    // drag the MOD AMT pot up: focus + value
    hx_touch(267, 150, 1);
    hx_touch(267, 120, 1);
    hx_touch(267, 120, 0);
    hx_pump(100);
    hx_save("gintonix_2_main_edit");
    hx_tap(128 + 62 + 29, 21);                // SEQ
    hx_pump(100);
    hx_save("gintonix_3_seq");
    hx_touch(150 + 5 * 54 + 20, 58 + 22 + 2 * 45 + 18, 1);   // add a trigger: held
    hx_pump(60);
    hx_touch(150 + 5 * 54 + 20, 58 + 22 + 2 * 45 + 18, 0);
    hx_tap(150 + 11 * 54 + 20, 58 + 9);         // LENGTH 12
    hx_pump(100);
    hx_save("gintonix_3b_seq_length");
    hx_tap(150 + 15 * 54 + 20, 58 + 9);         // back to 16
    hx_tap(128 + 2 * 62 + 29, 21);            // MIDI
    preview_set_monitor("ch10 CC74 = 81");
    hx_pump(100);
    hx_save("gintonix_4_midi");
    hx_tap(128 + 3 * 62 + 29, 21);            // TOOLS
    hx_pump(100);
    hx_save("gintonix_5_tools");
    hx_tap(380 + 35, 21);                     // KITS -> picker
    hx_pump(100);
    hx_save("gintonix_6_kits");
    hx_tap(4 + 125, 64 + 27);                 // first kit
    hx_pump(100);
    hx_tap(726 + 33, 21);                     // SAVE...
    hx_pump(100);
    hx_save("gintonix_7_save");
    hx_tap(620 + 75, 118 + 22);               // CANCEL
    hx_tap(128 + 29, 21);                     // MAIN
    hx_pump(100);

    // Sloe
    studio_next_app();
    hx_pump(100);
    hx_save("sloe_1");
    hx_tap(484 + 38, 28);       // SAVE AS
    hx_pump(100);
    hx_save("sloe_2_save_as");
    hx_tap(630 + 80, 110 + 24); // SAVE
    hx_pump(100);
    hx_save("sloe_3_saved");

    // Wavetron: a chord playing, so the displays move. A WAV in its folder
    // (24-bit stereo, 32 cycles) for the browser.
    {
        mkdir("files/wavetron", 0777);
        mkdir("files/wavetron/wavetables", 0777);
        FILE *f = fopen("files/wavetron/wavetables/Demo Growl.wav", "wb");
        int n = 32 * 256;
        uint32_t data = (uint32_t)n * 6, u;
        fwrite("RIFF", 1, 4, f);
        u = 36 + data;
        fwrite(&u, 4, 1, f);
        fwrite("WAVEfmt ", 1, 8, f);
        uint32_t fmt[4] = {16, 1 | 2 << 16, 44100, 44100 * 6};
        uint16_t fmt2[2] = {6, 24};
        fwrite(fmt, 4, 4, f);
        fwrite(fmt2, 2, 2, f);
        fwrite("data", 1, 4, f);
        fwrite(&data, 4, 1, f);
        for (int i = 0; i < n; i++) {
            float c = (float)(i / 256) / 31, ph = (float)(i % 256) / 256;
            int32_t s = (int32_t)(sinf(6.2831853f * ph + c * 3 * sinf(12.566f * ph)) * 0.9f * 8388607);
            uint8_t b[6] = {s & 255, s >> 8 & 255, s >> 16 & 255, s & 255, s >> 8 & 255, s >> 16 & 255};
            fwrite(b, 1, 6, f);
        }
        fclose(f);
    }
    studio_next_app();
    hx_pump(100);
    {
        static const int notes[3] = {48, 55, 60};
        for (int i = 0; i < 3; i++) {
            tnote_t n = {.type = TN_ON, .src = 0x80, .key = (uint8_t)notes[i], .vel = 100,
                         .cents = notes[i] * 100};
            mod_wavetron.note(&n);
        }
        static float l[STUDIO_BLOCK], r[STUDIO_BLOCK];
        studio_block_t b = {.n = STUDIO_BLOCK};
        for (int i = 0; i < 300; i++) mod_wavetron.render(l, r, &b);
    }
    hx_pump(200);
    hx_save("wavetron_1_osc");
    hx_tap(8 + 144 + 98, 60 + 24);   // OSC A's table name
    hx_pump(100);
    hx_save("wavetron_8_tables");
    hx_tap(16 + 180, 60 + 340 + 27);   // LOAD A WAV FROM /files ...
    hx_pump(100);
    hx_save("wavetron_9_files");
    hx_tap(1024 - 80, 60 + 26);        // CANCEL
    static const char *const pages[] = {"wavetron_2_filter", "wavetron_3_mod", "wavetron_4_matrix",
                                        "wavetron_5_voice", "wavetron_6_fx", "wavetron_7_patch"};
    for (int k = 1; k < 7; k++) {
        hx_tap(173 + 70 * k, 28);
        hx_pump(150);
        hx_save(pages[k - 1]);
    }
    hx_tap(173, 28);

    // Drumster9: playing, so the playhead and a pad light
    studio_next_app();
    hx_pump(100);
    g_d9.bus[L_CHAT] = 1;
    g_d9.bus[L_OHAT] = 1;
    g_d9.bus[L_SNARE] = 2;
    g_d9.kit_ovr[L_TOM] = 5;
    g_d9.muted[L_CYMBAL] = 1;
    g_d9s.playing = true;
    g_d9s.step = 6;
    g_d9s.hit_ms[L_KICK] = g_d9s.hit_ms[L_CHAT] = hx_now_ms();
    g_d9s.load_pct = 4;
    g_d9_next = 2;
    g_d9.bank[1].used = 1;
    g_d9.fx[1].rev_level = 0.35f;
    g_d9.fx[1].filter = FT_LPF;
    g_d9.fx[1].freq = 2400;
    g_d9.fx[1].eq_h = 4;
    g_d9_reload++;
    hx_pump(60);
    hx_save("drumster9_1_seq");
    g_d9_next = -1;
    hx_tap(440 + 1 * 64, 28);                 // KITS
    hx_pump(100);
    hx_save("drumster9_2_kits");
    hx_tap(577, 64 + 64 + 21);                // CHANGE the kick
    hx_pump(100);
    hx_save("drumster9_3_samples");
    hx_tap(956, 64 + 28);                     // BACK
    hx_tap(440 + 2 * 64, 28);                 // FX
    hx_pump(100);
    hx_tap(12 + 104 + 49, 64 + 28);           // BUS 1
    hx_pump(100);
    hx_save("drumster9_4_fx");
    hx_tap(440 + 3 * 64, 28);                 // MIX
    g_d9s.peak = 0.5f;
    hx_pump(100);
    hx_save("drumster9_5_mix");
    hx_tap(440 + 4 * 64, 28);                 // SET
    hx_pump(100);
    hx_save("drumster9_6_set");
    hx_tap(440, 28);                          // back to SEQ

    // a Studio patch: SAVE AS... with its keyboard, then it's in the list
    studio_show_home();
    hx_pump(100);
    hx_tap(612 + 60, 448 + 20);
    hx_pump(100);
    hx_save("studio_2_save_patch");
    hx_tap(630 + 80, 110 + 24);
    hx_pump(100);
    hx_save("studio_3_patch_saved");
    {
        char err[64] = "";
        bool ok = studio_patch_load("PATCH 1", err, sizeof err);
        printf("patch load: %s %s\n", ok ? "ok" : "FAILED", err);
    }
    return 0;
}
