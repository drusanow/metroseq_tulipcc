// fs.c - FAT on wear levelling, mounted at /files.
#include "tulip/fs.h"

#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "fs";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

bool tulip_fs_mount(bool format_if_needed) {
    if (s_wl != WL_INVALID_HANDLE) return true;
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(TULIP_FS_ROOT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
        return false;
    }
    mkdir(TULIP_APPS_DIR, 0775);   // harmless if it exists
    uint64_t total = 0, fr = 0;
    tulip_fs_usage(&total, &fr);
    ESP_LOGI(TAG, "/files mounted: %llu KB free of %llu KB", fr / 1024, total / 1024);
    return true;
}

bool tulip_fs_mounted(void) { return s_wl != WL_INVALID_HANDLE; }

bool tulip_fs_usage(uint64_t *total, uint64_t *free_bytes) {
    return esp_vfs_fat_info(TULIP_FS_ROOT, total, free_bytes) == ESP_OK;
}

bool tulip_fs_format(void) {
    esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl(TULIP_FS_ROOT, "storage");
    if (err == ESP_OK) mkdir(TULIP_APPS_DIR, 0775);
    return err == ESP_OK;
}
