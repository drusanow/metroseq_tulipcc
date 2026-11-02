// sysinfo.c - see sysinfo.h.
#include <stdbool.h>
#include <stdio.h>

#include "sysinfo.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "tulip/fs.h"

void sysinfo_text(char *buf, size_t n) {
    const esp_app_desc_t *d = esp_app_get_description();
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint64_t total = 0, fr = 0;
    tulip_fs_usage(&total, &fr);
    int64_t s = esp_timer_get_time() / 1000000;
    snprintf(buf, n,
             "Launcher %s  (built %s)\n"
             "ESP-IDF %s   ESP32-S3 rev %d.%d   %lu MB flash\n"
             "Memory: %u KB internal + %u KB PSRAM free\n"
             "Storage: %llu KB free of %llu KB\n"
             "MAC %02X:%02X:%02X:%02X:%02X:%02X   up %lldh %02lldm %02llds",
             d->version, d->date, d->idf_ver, ci.revision / 100, ci.revision % 100,
             (unsigned long)(flash >> 20),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), fr / 1024, total / 1024,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], s / 3600, (s / 60) % 60, s % 60);
}

void sys_restart(void) {
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

void sys_factory_reset(void) {
    nvs_flash_erase();
    sys_restart();
}

bool sys_format_storage(void) { return tulip_fs_format(); }
