// settings.c - shared device settings in NVS.
#include "tulip/settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tulip/touch_cal.h"

static const char *TAG = "tulip";

#define NS "tulip"
#define KEY "settings"
#define MAGIC 0x5453   // "TS"
#define VERSION 1

typedef struct {
    uint16_t magic;
    uint16_t version;
    tulip_settings_t s;
} blob_t;

tulip_settings_t g_tulip = {
    .cal = TOUCH_CAL_DEFAULT,
    .brightness = 9,
    .cal_prompt = 1,
};

void tulip_settings_load(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    blob_t b;
    size_t len = sizeof b;
    if (nvs_get_blob(h, KEY, &b, &len) == ESP_OK && len == sizeof b && b.magic == MAGIC &&
        b.version == VERSION) {
        g_tulip = b.s;
        if (g_tulip.brightness < 1 || g_tulip.brightness > 9) g_tulip.brightness = 9;
        if (!g_tulip.cal_valid) {
            const float def[6] = TOUCH_CAL_DEFAULT;
            memcpy(g_tulip.cal, def, sizeof def);
        }
    }
    nvs_close(h);
}

void tulip_settings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    blob_t b = {.magic = MAGIC, .version = VERSION, .s = g_tulip};
    if (nvs_set_blob(h, KEY, &b, sizeof b) != ESP_OK || nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "settings write failed");
    nvs_close(h);
}
