// main.c - Metropolix-style CV sequencer for the Tulip CC, native C.
//
// A port of metropolix_tulip.py that runs directly on the Tulip CC's
// ESP32-S3 with no Tulip/MicroPython/AMY firmware. Task layout:
//
//   core 1  seq     prio 23  engine + timer + I2C bus (DACs, touch)  <- real time
//   core 1  midi_in prio 22  MIDI IN bytes, timestamped -> seq
//   core 0  audio   prio 6   preview synth -> I2S
//   core 0  lvgl    prio 4   UI rendering + RGB panel flush
//   core 0  store   prio 2   NVS commits of presets / projects
//
// See docs/INVESTIGATION.md for why it is arranged this way.
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tulip/display.h"
#include "tulip/settings.h"
#include "tulip/sys.h"
#include "ui/ui.h"
#include "platform/preview_audio.h"
#include "platform/seq_task.h"
#include "platform/storage.h"

static const char *TAG = "metro";

pattern_t g_pat;
settings_t g_settings;
seq_status_t g_status;

static portMUX_TYPE s_pat_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;

void pat_lock(void) { taskENTER_CRITICAL(&s_pat_mux); }
void pat_unlock(void) { taskEXIT_CRITICAL(&s_pat_mux); }

void pat_store(const pattern_t *src) {
    pat_lock();
    memcpy(&g_pat, src, sizeof g_pat);
    pat_unlock();
}

void pat_load(pattern_t *dst) {
    pat_lock();
    memcpy(dst, &g_pat, sizeof g_pat);
    pat_unlock();
}

void status_info(const char *msg) {
    taskENTER_CRITICAL(&s_info_mux);
    strlcpy(g_status.info, msg, sizeof g_status.info);
    g_status.info_seq++;
    taskEXIT_CRITICAL(&s_info_mux);
}

void status_info_get(char *buf, int len, uint32_t *seq) {
    taskENTER_CRITICAL(&s_info_mux);
    strlcpy(buf, g_status.info, len);
    *seq = g_status.info_seq;
    taskEXIT_CRITICAL(&s_info_mux);
}

void app_main(void) {
    tulip_app_init();       // under the launcher: next reboot goes back to it
    tulip_settings_load();  // device-wide touch calibration + brightness
    pattern_default(&g_pat);
    storage_init();
    seq_task_start();       // core 1: real-time sequencer, owns the I2C bus
    preview_audio_start();  // core 0
    // core 0: panel + LVGL + UI
    tulip_display_start(&(tulip_display_cfg_t){
        .core = 0, .priority = 4, .ui_create = ui_create, .ui_poll = ui_poll});
    ESP_LOGI(TAG, "metroseq ready - press PLAY");
}
