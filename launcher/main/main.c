// main.c - the Tulip CC launcher (factory app).
//
//   core 0  Wi-Fi, lwIP, web server, console, installer
//   core 1  LVGL UI + RGB panel interrupt, touch polling
#include "apps.h"
#include "console.h"
#include "esp_log.h"
#include "lsettings.h"
#include "tulip/display.h"
#include "tulip/fs.h"
#include "tulip/input.h"
#include "tulip/settings.h"
#include "ui/ui.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "launcher";

void app_main(void) {
    tulip_settings_load();   // shared: touch calibration, brightness (also inits NVS)
    lsettings_load();        // launcher: Wi-Fi, PIN, autostart
    console_init();          // early, so the screen console sees the boot log
    if (!tulip_fs_mount(true)) ESP_LOGE(TAG, "no /files - check the partition table");
    apps_init();             // slot table; imports a preloaded app on first boot
    tulip_input_start(1, 5);
    wifi_init();
    if (g_ls.web_on) web_start();
    tulip_display_start(&(tulip_display_cfg_t){
        .core = 1, .priority = 4, .ui_create = ui_create, .ui_poll = ui_poll});
    ESP_LOGI(TAG, "launcher up - web PIN %s", g_ls.pin);
}
