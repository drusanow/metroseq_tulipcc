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
    // The screen first: it shows the progress of what follows (the first
    // start after flashing formats /files and copies the preloaded apps in,
    // which can take a minute) and builds the launcher once g_boot.done.
    tulip_input_start(1, 5);
    tulip_display_start(&(tulip_display_cfg_t){
        .core = 1, .priority = 4, .ui_create = ui_create, .ui_poll = ui_poll});
    boot_status("STARTING", "opening /files", -1);
    if (!tulip_fs_mount(false)) {
        boot_status("PREPARING STORAGE",
                    "first start after flashing: formatting /files (the old Tulip /user area)",
                    -1);
        if (!tulip_fs_mount(true)) ESP_LOGE(TAG, "no /files - check the partition table");
    }
    apps_init();             // slot table; imports the preloaded apps on first boot
    boot_status("STARTING", "Wi-Fi", -1);
    wifi_init();
    if (g_ls.web_on) web_start();
    g_boot.done = true;
    ESP_LOGI(TAG, "launcher up - web PIN %s", g_ls.pin);
}
