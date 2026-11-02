// lsettings.h - launcher settings (NVS namespace "launcher").
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t wifi_on;        // join the saved network at boot
    uint8_t web_on;         // run the file-transfer web server
    uint8_t autostart;      // launch last_app at power-on
    char pin[5];            // web/API PIN, 4 digits
    char last_app[192];     // path of the last launched app
    char ssid[33];
    char pass[65];
} lsettings_t;

extern lsettings_t g_ls;

void lsettings_load(void);
void lsettings_save(void);
void lsettings_new_pin(void);   // random 4-digit PIN
