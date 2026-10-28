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
void studio_set_cv_clock(bool on, int div, int v1) {
    g_cfg.cv_clock = on;
    if (div > 0) g_cfg.cv_div = (uint8_t)div;
    if (v1 >= 0) g_cfg.cv_vout1 = (uint8_t)v1;
}
bool studio_cv_clock(void) { return g_cfg.cv_clock; }
int studio_cv_clock_div(void) { return g_cfg.cv_div; }
int studio_cv_clock_vout1(void) { return g_cfg.cv_vout1; }
int studio_cv_clock_state(void) { return g_cfg.cv_clock ? CVCLK_SEQUENCER : CVCLK_OFF; }
void tulip_boot_mode_set(const char *m) {}
int loader_load_apps(const tmod_t **out, int room, const tmod_t *const *mods, int n, const char *only) { return 0; }
bool loader_is_loaded(const tmod_t *m) { return false; }
void loader_guard_begin(const tmod_t *m) {}
void loader_guard_end(const tmod_t *m) {}
bool studio_register_app(const tmod_t *m, int abi) { return false; }
void tulip_boot_mode_get(char *out, int len) { snprintf(out, len, "%s", ""); }
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

// mixer (the real one is in audio.c)
static uint8_t s_lv[STUDIO_MAX_MODULES] = {80, 80, 64, 80, 80, 80}, s_mute = 0x4;
void studio_mix_set_level(int m, int p) { s_lv[m] = (uint8_t)p; }
int studio_mix_level(int m) { return s_lv[m]; }
void studio_mix_set_mute(int m, bool on) { s_mute = on ? s_mute | (1u << m) : s_mute & ~(1u << m); }
bool studio_mix_muted(int m) { return s_mute & (1u << m); }
float studio_mix_peak(int m) { return m == 1 ? 0.5f : (m == 0 ? 0.05f : 0); }

// --- Drumster9's hardware: the samples partition is the image the tests pack -----------------
#include <stdio.h>

#include "esp_partition.h"
#include "studio/studio_hw.h"

i2c_master_bus_handle_t studio_i2c_bus(void) { return NULL; }

static esp_partition_t s_samples = {.address = 0xB50000, .size = 0x400000,
                                    .path = "../../../tests/build/gamma9001.bin"};

const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s,
                                                const char *label) {
    FILE *f = fopen(s_samples.path, "rb");
    if (!f) return NULL;
    fclose(f);
    return &s_samples;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n) {
    FILE *f = fopen(p->path, "rb");
    if (!f) return ESP_FAIL;
    fseek(f, (long)off, SEEK_SET);
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    return got == n ? ESP_OK : ESP_FAIL;
}
