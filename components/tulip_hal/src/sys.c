// sys.c - app lifecycle helpers (see sys.h).
#include "tulip/sys.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "tulip";

bool tulip_under_launcher(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

// Erasing otadata makes the bootloader start the factory app (the
// launcher) next time. Done directly rather than via
// esp_ota_set_boot_partition(factory), which would first hash-verify the
// whole launcher image and slow every app start.
static void boot_launcher_next(void) {
    const esp_partition_t *ota =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (ota) esp_partition_erase_range(ota, 0, ota->size);
}

void tulip_app_init(void) {
    if (!tulip_under_launcher()) return;
    // We started, so this image is good: tell the rollback logic, then
    // make sure the next boot goes home.
    esp_ota_mark_app_valid_cancel_rollback();
    boot_launcher_next();
    ESP_LOGI(TAG, "running under the launcher; next boot returns to it");
}

void tulip_boot_mode_set(const char *mode) {
    nvs_handle_t h;
    if (nvs_open("tulip", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "mode", mode ? mode : "");
    nvs_commit(h);
    nvs_close(h);
}

void tulip_boot_mode_get(char *out, int len) {
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("tulip", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = (size_t)len;
    if (nvs_get_str(h, "mode", out, &n) != ESP_OK) out[0] = 0;
    nvs_close(h);
}

void tulip_exit_to_launcher(void) {
    boot_launcher_next();
    esp_restart();
}

bool tulip_cold_start(void) {
    switch (esp_reset_reason()) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_DEEPSLEEP:
        return false;
    default:
        return true;
    }
}
