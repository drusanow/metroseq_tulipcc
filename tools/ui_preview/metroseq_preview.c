// metroseq_preview.c - render metroseq's real UI code on the host.
//   make -C tools/ui_preview      -> build/metroseq_*.png
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "harness.h"
#include "lvgl.h"
#include "platform/preview_audio.h"
#include "platform/storage.h"
#include "tulip/settings.h"
#include "tulip/touch_cal.h"
#include "ui/ui.h"
#include "ui/ui_common.h"

// --- stand-ins for metroseq's platform layer ---------------------------------

pattern_t g_pat;
settings_t g_settings;
seq_status_t g_status;
tulip_settings_t g_tulip = {.cal = TOUCH_CAL_DEFAULT, .brightness = 9};
void tulip_settings_save(void) {}
bool tulip_under_launcher(void) { return true; }   // shows the HOME button
void tulip_exit_to_launcher(void) {}
void pat_lock(void) {}
void pat_unlock(void) {}
void pat_store(const pattern_t *src) { g_pat = *src; }
void pat_load(pattern_t *dst) { *dst = g_pat; }
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
void storage_settings_save(void) {}
int storage_project_count(void) { return 2; }
const char *storage_project_name(int i) { return i ? "Dark Arp" : "Live Set A"; }
bool storage_project_get(int i, pattern_t *o) {
    pattern_default(o);
    return true;
}
int storage_project_save(const char *n, const pattern_t *p) { return 0; }
bool storage_write_pending(void) { return false; }

int main(void) {
    pattern_default(&g_pat);
    storage_preset_save(0, &g_pat);
    storage_preset_save(3, &g_pat);
    g_status.cv_enabled = true;
    g_status.dac_present[0] = true;
    g_status.cv_ready = true;
    g_status.touch_ready = true;
    g_status.late_count = 4096;
    g_status.late_sum_us = 4096 * 112;
    g_status.late_max_us = 231;

    hx_init();
    ui_create();
    hx_pump(50);

    g_status.playing = true;   // playhead LEDs
    g_status.disp_stage[0] = 2;
    g_status.disp_stage[1] = 5;
    g_status.lane_disp[0] = 3;
    status_info("CV ready");
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
    hx_tap(110 + 60, PAGE_Y + 428 + 20);   // SAVE AS...
    hx_save("metroseq_6_save_project");
    hx_tap(232 + 434 + 55, 45 + 102 + 20); // Cancel
    show_page(0);
    g_settings.clock_src = CLOCK_MIDI;     // following MIDI clock, armed
    g_status.playing = false;
    g_status.ext_armed = true;
    g_status.ext_bpm_x10 = 1243;
    hx_pump(100);
    hx_save("metroseq_9_midi_armed");
    return 0;
}
