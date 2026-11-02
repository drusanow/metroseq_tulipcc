// wifi.h - station mode, scanning, saved credentials, tulip.local (mDNS).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_ST_OFF = 0,
    WIFI_ST_NO_NETWORK,   // on, but no network saved
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,       // retrying in the background
} wifi_state_t;

typedef struct {
    volatile int state;   // wifi_state_t
    char ssid[33];
    char ip[16];
    volatile int rssi;
    volatile int reason;  // last disconnect reason
} wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_ap_t;

extern wifi_status_t g_wifi;

void wifi_init(void);   // starts if enabled in settings and a network is saved
void wifi_set_enabled(bool on);
void wifi_connect(const char *ssid, const char *pass);   // saves + connects
void wifi_forget(void);

bool wifi_scan_start(void);   // async; false if one is already running
// Copies the latest results; *busy is true while a scan is running.
int wifi_scan_results(wifi_ap_t *out, int max, bool *busy);
