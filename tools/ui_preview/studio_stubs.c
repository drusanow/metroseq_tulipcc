// studio_stubs.c - the Studio runtime's real-time, audio and MIDI layers,
// faked for the UI preview: the real shell, widgets, settings/routing
// (studio.c) and every module's UI run on top of these.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "harness.h"
#include "studio_priv.h"
#include "tulip/display.h"
#include "tulip/fs.h"
#include "tulip/settings.h"
#include "tulip/sys.h"
#include "tulip/touch_cal.h"

// --- FreeRTOS queue ------------------------------------------------------------------------
struct hq {
    int item, cap, head, n;
    uint8_t *buf;
};
QueueHandle_t xQueueCreate(int len, int item) {
    struct hq *q = calloc(1, sizeof *q);
    q->item = item;
    q->cap = len;
    q->buf = calloc(len, item);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *it, TickType_t t) {
    if (q->n == q->cap) return pdFALSE;
    memcpy(q->buf + ((q->head + q->n) % q->cap) * q->item, it, q->item);
    q->n++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *it, TickType_t t) {
    if (!q->n) return pdFALSE;
    memcpy(it, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->n--;
    return pdTRUE;
}
void vTaskDelay(TickType_t t) {}

// --- tulip_hal ------------------------------------------------------------------------------
tulip_settings_t g_tulip = {.cal = TOUCH_CAL_DEFAULT, .brightness = 9};
void tulip_settings_save(void) {}
void tulip_settings_load(void) {}
bool tulip_under_launcher(void) { return true; }
void tulip_exit_to_launcher(void) {}
void tulip_app_init(void) {}
bool tulip_fs_mount(bool f) {
    mkdir("files", 0777);
    return true;
}
static void (*s_ui_create)(void);
void tulip_display_start(const tulip_display_cfg_t *cfg) { s_ui_create = cfg->ui_create; }
void preview_ui_create(void) { s_ui_create(); }
void tulip_display_set_brightness(int l) {}

// --- the fake real-time layer ---------------------------------------------------------------
static studio_clock_status_t s_st;
static tclock_tl_t s_tl = {.p16_us = 125000, .max_pos = INT64_MAX};
static char s_mon[40] = "ch10 note 36 v100";

void preview_set_clock(bool playing, bool armed, bool ext, int ext_bpm_x10) {
    s_st.playing = playing;
    s_st.armed = armed;
    s_st.ext = ext;
    s_st.ext_bpm_x10 = ext_bpm_x10;
    g_cfg.clock_src = ext;
}
void preview_set_monitor(const char *t) { snprintf(s_mon, sizeof s_mon, "%s", t); }

int64_t studio_now_us(void) { return (int64_t)hx_now_ms() * 1000; }
bool rt_ready(void) { return true; }
void rt_start(void) {}
void audio_start(void) {}
void midi_io_start(void) {}
void midi_io_write(const uint8_t *b, int n) {}
void midi_monitor_set(const char *t) {}

bool rt_post(uint8_t kind, uint8_t mod, uint8_t cmd, int32_t arg) {
    if (kind == HC_ENABLE && mod < g_nmods) g_enabled[mod] = arg != 0;
    if (kind == HC_TOGGLE) s_st.playing = !s_st.playing;
    return true;
}
void studio_play(void) { s_st.playing = true; }
void studio_stop(void) { s_st.playing = false; }
void studio_toggle_play(void) { s_st.playing = !s_st.playing; }
void studio_restart(void) {}
void studio_set_bpm_x100(int32_t v) { g_cfg.bpm_x100 = v; }
int32_t studio_bpm_x100(void) { return g_cfg.bpm_x100; }
void studio_set_clock_src(int s) { g_cfg.clock_src = (uint8_t)s; s_st.ext = s; }
int studio_clock_src(void) { return g_cfg.clock_src; }
void studio_set_clock_out(bool on) { g_cfg.clock_out = on; }
bool studio_clock_out(void) { return g_cfg.clock_out; }
void studio_set_mmc(bool on) { g_cfg.mmc = on; }
bool studio_mmc(void) { return g_cfg.mmc; }
void studio_clock_status(studio_clock_status_t *st) { *st = s_st; }
void studio_timeline(tclock_tl_t *out) {
    *out = s_tl;
    out->running = s_st.playing;
}
bool studio_cmd(const tmod_t *m, uint8_t cmd, int32_t arg) { return true; }
bool studio_midi_out(const uint8_t *b, int n) { return true; }
bool studio_midi_out_at(int64_t t, const uint8_t *b, int n) { return true; }
uint32_t studio_midi_monitor(char *buf, int len) {
    snprintf(buf, len, "%s", s_mon);
    return 1;
}
void studio_set_volume(int v) { g_cfg.volume = (uint8_t)v; }
int studio_volume(void) { return g_cfg.volume; }
int studio_audio_load_pct(void) { return 23; }
uint32_t studio_audio_underruns(void) { return 0; }
const char *studio_audio_problem(void) { return NULL; }
