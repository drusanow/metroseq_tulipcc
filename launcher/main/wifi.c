// wifi.c - see wifi.h.
//
// Wi-Fi, lwIP and the web server run on core 0; the launcher's UI and the
// panel interrupt run on core 1, so radio activity can't make the display
// stutter.
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lsettings.h"
#include "mdns.h"

static const char *TAG = "wifi";

wifi_status_t g_wifi;

static bool s_started;
static bool s_mdns;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry;
static int s_backoff_ms = 1000;
static volatile bool s_scanning;
static wifi_ap_t s_aps[20];
static volatile int s_naps;

static void apply_config(void) {
    wifi_config_t c = {0};
    // the driver's fields are fixed-size and need not be NUL-terminated
    memcpy(c.sta.ssid, g_ls.ssid, strnlen(g_ls.ssid, sizeof c.sta.ssid));
    memcpy(c.sta.password, g_ls.pass, strnlen(g_ls.pass, sizeof c.sta.password));
    c.sta.threshold.authmode = g_ls.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);
}

static void try_connect(void) {
    if (!g_ls.ssid[0]) {
        g_wifi.state = WIFI_ST_NO_NETWORK;
        return;
    }
    snprintf(g_wifi.ssid, sizeof g_wifi.ssid, "%s", g_ls.ssid);
    g_wifi.state = WIFI_ST_CONNECTING;
    esp_wifi_connect();
}

static void retry_cb(void *arg) {
    if (s_started && g_wifi.state != WIFI_ST_CONNECTED) try_connect();
}

static void start_mdns(void) {
    if (s_mdns) return;
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set("tulip");
    mdns_instance_name_set("Tulip CC");
    mdns_service_add("Tulip file transfer", "_http", "_tcp", 80, NULL, 0);
    s_mdns = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        try_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        g_wifi.reason = d->reason;
        g_wifi.ip[0] = 0;
        if (!s_started) return;
        g_wifi.state = g_ls.ssid[0] ? WIFI_ST_FAILED : WIFI_ST_NO_NETWORK;
        // back off 1, 2, 4 ... 30 s so a missing router doesn't spin the radio
        esp_timer_start_once(s_retry, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms = s_backoff_ms * 2 > 30000 ? 30000 : s_backoff_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(g_wifi.ip, sizeof g_wifi.ip, IPSTR, IP2STR(&e->ip_info.ip));
        g_wifi.state = WIFI_ST_CONNECTED;
        s_backoff_ms = 1000;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) g_wifi.rssi = ap.rssi;
        start_mdns();
        ESP_LOGI(TAG, "connected to %s as %s (tulip.local)", g_wifi.ssid, g_wifi.ip);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 20;
        wifi_ap_record_t *recs = calloc(n, sizeof *recs);
        int k = 0;
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n && k < 20; i++) {
                if (!recs[i].ssid[0]) continue;
                bool dup = false;   // one row per network name (strongest first)
                for (int j = 0; j < k; j++)
                    if (strcmp(s_aps[j].ssid, (char *)recs[i].ssid) == 0) dup = true;
                if (dup) continue;
                snprintf(s_aps[k].ssid, sizeof s_aps[k].ssid, "%s", (char *)recs[i].ssid);
                s_aps[k].rssi = recs[i].rssi;
                s_aps[k].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                k++;
            }
        }
        free(recs);
        s_naps = k;
        s_scanning = false;
    }
}

static void driver_up(void) {
    static bool once;
    if (once) return;
    once = true;
    esp_netif_init();
    esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "tulip");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // credentials live in our settings
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_timer_create_args_t t = {.callback = retry_cb, .name = "wifi_retry"};
    esp_timer_create(&t, &s_retry);
}

static void start(void) {
    driver_up();
    if (s_started) return;
    apply_config();
    s_started = true;
    s_backoff_ms = 1000;
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);   // snappy web transfers; the launcher isn't on battery
}

static void stop(void) {
    if (!s_started) return;
    s_started = false;
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    esp_wifi_stop();
    g_wifi.state = WIFI_ST_OFF;
    g_wifi.ip[0] = 0;
}

void wifi_init(void) {
    g_wifi.state = WIFI_ST_OFF;
    if (g_ls.wifi_on) start();
}

void wifi_set_enabled(bool on) {
    g_ls.wifi_on = on;
    lsettings_save();
    if (on)
        start();
    else
        stop();
}

void wifi_connect(const char *ssid, const char *pass) {
    snprintf(g_ls.ssid, sizeof g_ls.ssid, "%s", ssid);
    snprintf(g_ls.pass, sizeof g_ls.pass, "%s", pass ? pass : "");
    g_ls.wifi_on = 1;
    lsettings_save();
    if (!s_started) {
        start();
        return;
    }
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    apply_config();
    s_backoff_ms = 1000;
    try_connect();
}

void wifi_forget(void) {
    g_ls.ssid[0] = 0;
    g_ls.pass[0] = 0;
    lsettings_save();
    if (s_started) {
        esp_wifi_disconnect();
        g_wifi.state = WIFI_ST_NO_NETWORK;
    }
}

bool wifi_scan_start(void) {
    if (s_scanning) return false;
    start();   // scanning needs the radio on
    s_scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        s_scanning = false;
        return false;
    }
    return true;
}

int wifi_scan_results(wifi_ap_t *out, int max, bool *busy) {
    *busy = s_scanning;
    int n = s_naps < max ? s_naps : max;
    memcpy(out, s_aps, n * sizeof *out);
    return n;
}
