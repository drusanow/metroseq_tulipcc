idf_component_register(
    SRCS
        "main.c"
        "apps.c"
        "console.c"
        "fsutil.c"
        "lsettings.c"
        "sysinfo.c"
        "web.c"
        "wifi.c"
        "core/appimg.c"
        "core/path.c"
        "ui/dialog.c"
        "ui/page_console.c"
        "ui/page_files.c"
        "ui/page_home.c"
        "ui/page_settings.c"
        "ui/page_wifi.c"
        "ui/theme.c"
        "ui/ui.c"
    INCLUDE_DIRS "." "ui"
    EMBED_TXTFILES "web/index.html"
    REQUIRES
        tulip_hal lvgl app_update bootloader_support esp_partition esp_wifi esp_netif
        esp_event esp_http_server esp_timer console nvs_flash fatfs mdns esp_driver_i2c spi_flash
)
