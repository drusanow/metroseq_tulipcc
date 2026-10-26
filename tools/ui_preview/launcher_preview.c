// launcher_preview.c - render the launcher's real UI code on the host, with
// stand-in device modules holding realistic data.
//   make -C tools/ui_preview      -> build/launcher_*.png
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "console.h"
#include "fsutil.h"
#include "harness.h"
#include "lsettings.h"
#include "lvgl.h"
#include "sysinfo.h"
#include "theme.h"
#include "tulip/fs.h"
#include "tulip/settings.h"
#include "tulip/touch_cal.h"
#include "ui.h"
#include "web.h"
#include "wifi.h"

// --- stand-ins ---------------------------------------------------------------------

tulip_settings_t g_tulip = {.cal = TOUCH_CAL_DEFAULT, .brightness = 7, .cal_prompt = 1};
void tulip_settings_save(void) {}
void tulip_display_set_brightness(int level) {}
lsettings_t g_ls = {.wifi_on = 1, .web_on = 1, .pin = "4821", .ssid = "Studio-5G",
                    .last_app = "/files/apps/metroseq.bin"};
void lsettings_save(void) {}
void lsettings_new_pin(void) { strcpy(g_ls.pin, "3907"); }
wifi_status_t g_wifi = {.state = WIFI_ST_CONNECTED, .ssid = "Studio-5G", .ip = "192.168.1.42",
                        .rssi = -51};
void wifi_set_enabled(bool on) { g_ls.wifi_on = on; }
void wifi_connect(const char *s, const char *p) {}
void wifi_forget(void) {}
static const wifi_ap_t APS[] = {{"Studio-5G", -51, true}, {"Studio-2G", -58, true},
                                {"BTHub-7F21", -71, true}, {"Cafe Guest", -80, false}};
static bool s_scanned;
bool wifi_scan_start(void) {
    s_scanned = true;
    return true;
}
int wifi_scan_results(wifi_ap_t *out, int max, bool *busy) {
    *busy = false;
    int n = s_scanned ? 4 : 0;
    memcpy(out, APS, n * sizeof *out);
    return n;
}
bool web_running(void) { return true; }
bool web_start(void) { return true; }
void web_stop(void) {}

install_status_t g_install;
typedef struct {
    const char *file, *name, *ver;
    uint32_t size;
    int slot;
} fake_app_t;
static const fake_app_t APPS[] = {
    {"gintonix.bin", "gintonix", "1.0.0", 815104, 2},
    {"hello.bin", "hello", "1.0.0", 588800, -1},
    {"metroseq.bin", "metroseq", "2.0.0", 740976, 1},
    {"studio.bin", "studio", "1.0.0", 891600, 0},
};
#define NAPPS 4
int apps_scan(app_entry_t *out, int max) {
    for (int i = 0; i < NAPPS; i++) {
        memset(&out[i], 0, sizeof out[i]);
        snprintf(out[i].path, sizeof out[i].path, "/files/apps/%s", APPS[i].file);
        strcpy(out[i].info.name, APPS[i].name);
        strcpy(out[i].info.version, APPS[i].ver);
        out[i].size = APPS[i].size;
        out[i].slot = APPS[i].slot;
    }
    return NAPPS;
}
bool apps_read_info(const char *path, app_info_t *info, uint32_t *size) {
    for (int i = 0; i < NAPPS; i++) {
        if (strstr(path, APPS[i].file)) {
            memset(info, 0, sizeof *info);
            strcpy(info->name, APPS[i].name);
            strcpy(info->version, APPS[i].ver);
            *size = APPS[i].size;
            return true;
        }
    }
    return false;
}
bool apps_launch(const char *path) { return true; }
int apps_slots(app_slot_t out[APP_SLOTS]) {
    memset(out, 0, sizeof(app_slot_t) * APP_SLOTS);
    out[0].used = 1;
    strcpy(out[0].name, "studio");
    strcpy(out[0].version, "1.0.0");
    out[1].used = 1;
    strcpy(out[1].name, "metroseq");
    strcpy(out[1].version, "2.0.0");
    out[2].used = 1;
    strcpy(out[2].name, "gintonix");
    strcpy(out[2].version, "1.0.0");
    return APP_SLOTS;
}
void apps_clear_slots(void) {}

int fs_list(const char *dir, fs_entry_t *out, int max) {
    static const fs_entry_t ROOT[] = {{"apps", true, 0}, {"gintonix", true, 0},
                                      {"sloe", true, 0}, {"readme.txt", false, 1874}};
    static const fs_entry_t APPD[] = {{"experiments", true, 0},
                                      {"gintonix.bin", false, 815104},
                                      {"hello.bin", false, 588800},
                                      {"metroseq.bin", false, 740976},
                                      {"studio.bin", false, 891600}};
    const fs_entry_t *src = ROOT;
    int n = 4;
    if (strcmp(dir, "/files/apps") == 0) {
        src = APPD;
        n = 5;
    } else if (strcmp(dir, "/files") != 0) {
        n = 0;
    }
    memcpy(out, src, n * sizeof *out);
    return n;
}
bool fs_mkdir(const char *p) { return true; }
bool fs_remove(const char *p) { return true; }
bool fs_rename(const char *a, const char *b) { return true; }
bool fs_is_dir(const char *p) { return false; }
bool fs_exists(const char *p) { return false; }
bool tulip_fs_usage(uint64_t *t, uint64_t *f) {
    *t = 17498112;
    *f = 13631488;
    return true;
}

void sysinfo_text(char *b, size_t n) {
    snprintf(b, n,
             "Launcher 1.0.0  (built Oct 26 2026)\n"
             "ESP-IDF v5.3.2   ESP32-S3 rev 0.2   32 MB flash\n"
             "Memory: 212 KB internal + 7650 KB PSRAM free\n"
             "Storage: 13312 KB free of 17088 KB\n"
             "MAC 34:85:18:7A:2C:10   up 0h 04m 12s");
}
void sys_restart(void) {}
void sys_factory_reset(void) {}
bool sys_format_storage(void) { return true; }

void console_submit(const char *line) {}
size_t console_text_read(uint32_t *cursor, char *out, size_t max) {
    static const char *T =
        "I (412) fs: /files mounted: 13312 KB free of 17088 KB\n"
        "I (1630) touch: GT911 at 0x5d, raw range 1024x750\n"
        "I (2215) wifi: connected to Studio-5G as 192.168.1.42 (tulip.local)\n"
        "I (2216) web: file transfer at http://tulip.local (PIN 4821)\n"
        "> free\n"
        "internal: 217600 free (min 198144)\n"
        "psram:    7833600 free\n"
        "> slots\n"
        "  ota_0: studio 1.0.0 (891600 bytes, last run #12)\n"
        "  ota_1: metroseq 2.0.0 (740976 bytes, last run #9)\n"
        "  ota_2: gintonix 1.0.0 (815104 bytes, last run #4)\n"
        "  ota_3: empty\n"
        "> i2cscan\n"
        "  0x58 (Mabee DAC, A0 cut)\n"
        "  0x59 (Mabee DAC)\n"
        "  0x5d (GT911 touch)\n";
    size_t len = strlen(T);
    if (*cursor >= len) return 0;
    size_t n = len - *cursor < max ? len - *cursor : max;
    memcpy(out, T + *cursor, n);
    *cursor += n;
    return n;
}

#define TAB(i) (150 + (i) * 106 + 50), 28

int main(void) {
    hx_init();
    hx_poll = ui_poll;
    ui_create();
    hx_pump(1200);
    hx_save("launcher_0_cal_prompt");
    hx_tap(800, 300);   // right half: keep the saved calibration
    hx_pump(200);
    hx_save("launcher_1_home");

    hx_tap(TAB(PG_FILES));
    hx_tap(300, PAGE_Y + 108 + 3 * 48 + 22);   // select metroseq.bin
    hx_save("launcher_2_files");
    hx_tap(540, PAGE_Y + 52 + 21);              // DELETE -> confirm
    hx_save("launcher_3_delete_confirm");
    hx_tap(546, 348);                           // CANCEL
    hx_tap(220, PAGE_Y + 52 + 21);              // NEW FOLDER -> prompt + keyboard
    hx_save("launcher_4_new_folder");
    hx_tap(576, 186);                           // CANCEL

    hx_tap(TAB(PG_WIFI));
    hx_tap(516, PAGE_Y + 156 + 18);             // SCAN
    hx_pump(200);
    hx_save("launcher_5_wifi");
    hx_tap(300, PAGE_Y + 204 + 22);             // Studio-5G -> password
    hx_save("launcher_6_wifi_password");
    hx_tap(576, 186);                           // CANCEL

    hx_tap(TAB(PG_SETTINGS));
    hx_pump(1100);
    hx_save("launcher_7_settings");
    hx_tap(TAB(PG_CONSOLE));
    hx_pump(300);
    hx_save("launcher_8_console");

    hx_tap(TAB(PG_HOME));
    strcpy(g_install.name, "hello");
    g_install.state = INSTALL_WRITING;
    g_install.pct = 62;
    hx_pump(200);
    hx_save("launcher_9_installing");
    return 0;
}
