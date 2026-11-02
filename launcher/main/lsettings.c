// lsettings.c - launcher settings in NVS.
#include "lsettings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#define NS "launcher"
#define MAGIC 0x4C53   // "LS"
#define VERSION 1

typedef struct {
    uint16_t magic, version;
    lsettings_t s;
} blob_t;

lsettings_t g_ls = {.wifi_on = 1, .web_on = 1};

void lsettings_new_pin(void) {
    snprintf(g_ls.pin, sizeof g_ls.pin, "%04u", (unsigned)(esp_random() % 10000));
}

void lsettings_load(void) {
    nvs_handle_t h;
    blob_t b;
    size_t len = sizeof b;
    bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "cfg", &b, &len) == ESP_OK && len == sizeof b && b.magic == MAGIC &&
             b.version == VERSION;
        nvs_close(h);
    }
    if (ok) {
        g_ls = b.s;
        g_ls.pin[4] = 0;
        g_ls.ssid[32] = 0;
        g_ls.pass[64] = 0;
        g_ls.last_app[sizeof g_ls.last_app - 1] = 0;
    }
    if (strlen(g_ls.pin) != 4) {
        lsettings_new_pin();
        lsettings_save();
    }
}

void lsettings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    blob_t b = {.magic = MAGIC, .version = VERSION, .s = g_ls};
    nvs_set_blob(h, "cfg", &b, sizeof b);
    nvs_commit(h);
    nvs_close(h);
}
