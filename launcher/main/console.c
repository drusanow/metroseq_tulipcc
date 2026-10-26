// console.c - see console.h.
#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps.h"
#include "core/path.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fsutil.h"
#include "lsettings.h"
#include "nvs_flash.h"
#include "tulip/display.h"
#include "tulip/fs.h"
#include "tulip/input.h"
#include "tulip/settings.h"
#include "web.h"
#include "wifi.h"

// --- ring buffer ---------------------------------------------------------------

#define RING 16384
static char s_ring[RING];
static uint32_t s_written;   // total bytes ever written
static SemaphoreHandle_t s_rmx;
static vprintf_like_t s_orig_vprintf;

static void ring_put(const char *s, size_t n) {
    if (!s_rmx) return;
    xSemaphoreTake(s_rmx, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\033') {   // drop ANSI colour codes from the log
            while (i < n && s[i] != 'm') i++;
            continue;
        }
        s_ring[s_written % RING] = c;
        s_written++;
    }
    xSemaphoreGive(s_rmx);
}

size_t console_text_read(uint32_t *cursor, char *out, size_t max) {
    if (!s_rmx) return 0;
    xSemaphoreTake(s_rmx, portMAX_DELAY);
    if (s_written - *cursor > RING) *cursor = s_written - RING;   // fell behind
    size_t n = 0;
    while (*cursor < s_written && n < max) out[n++] = s_ring[(*cursor)++ % RING];
    xSemaphoreGive(s_rmx);
    return n;
}

void con_printf(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = sizeof b - 1;
    fputs(b, stdout);
    ring_put(b, n);
}

static int log_vprintf(const char *fmt, va_list ap) {
    char b[256];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(b, sizeof b, fmt, cp);
    va_end(cp);
    if (n > 0) ring_put(b, n >= (int)sizeof b ? sizeof b - 1 : (size_t)n);
    return s_orig_vprintf(fmt, ap);
}

// --- commands --------------------------------------------------------------------

static bool resolve(const char *in, char *out) {
    if (!path_resolve(TULIP_FS_ROOT, in ? in : "/", out, 300)) {
        con_printf("bad path: %s\n", in ? in : "");
        return false;
    }
    return true;
}

static int c_free(int argc, char **argv) {
    con_printf("internal: %u free (min %u)\npsram:    %u free\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int c_ver(int argc, char **argv) {
    const esp_app_desc_t *d = esp_app_get_description();
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    con_printf("Tulip launcher %s (%s %s), ESP-IDF %s, ESP32-S3 rev %d.%d, %d cores\n", d->version,
               d->date, d->time, d->idf_ver, ci.revision / 100, ci.revision % 100, ci.cores);
    return 0;
}

static int c_uptime(int argc, char **argv) {
    int64_t s = esp_timer_get_time() / 1000000;
    con_printf("up %lldh %02lldm %02llds\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}

static int c_tasks(int argc, char **argv) {
    char *b = malloc(2048);
    if (!b) return 1;
    vTaskList(b);
    con_printf("name           state prio stack num core\n");
    // vTaskList output can exceed con_printf's line buffer; send in pieces
    for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) con_printf("%s\n", line);
    free(b);
    return 0;
}

static int c_df(int argc, char **argv) {
    uint64_t t = 0, f = 0;
    if (!tulip_fs_usage(&t, &f)) {
        con_printf("/files not mounted\n");
        return 1;
    }
    con_printf("/files: %llu KB used, %llu KB free, %llu KB total\n", (t - f) / 1024, f / 1024,
               t / 1024);
    return 0;
}

static int c_ls(int argc, char **argv) {
    char p[300];
    if (!resolve(argc > 1 ? argv[1] : "/", p)) return 1;
    static fs_entry_t e[128];
    int n = fs_list(p, e, 128);
    if (n < 0) {
        con_printf("no such folder: %s\n", p);
        return 1;
    }
    con_printf("%s\n", p);
    for (int i = 0; i < n; i++)
        e[i].dir ? con_printf("  %s/\n", e[i].name) : con_printf("  %-40s %8lu\n", e[i].name, (unsigned long)e[i].size);
    return 0;
}

static int c_rm(int argc, char **argv) {
    char p[300];
    if (argc < 2 || !resolve(argv[1], p)) return 1;
    if (strcmp(p, TULIP_FS_ROOT) == 0 || strcmp(p, TULIP_APPS_DIR) == 0) {
        con_printf("refusing to delete %s\n", p);
        return 1;
    }
    bool ok = fs_remove(p);
    con_printf(ok ? "deleted %s\n" : "could not delete %s\n", p);
    return ok ? 0 : 1;
}

static int c_mkdir(int argc, char **argv) {
    char p[300];
    if (argc < 2 || !resolve(argv[1], p)) return 1;
    bool ok = path_valid_name(path_basename(p)) && fs_mkdir(p);
    con_printf(ok ? "created %s\n" : "could not create %s\n", p);
    return ok ? 0 : 1;
}

static int c_mv(int argc, char **argv) {
    char a[300], b[300];
    if (argc < 3 || !resolve(argv[1], a) || !resolve(argv[2], b)) return 1;
    bool ok = fs_rename(a, b);
    con_printf(ok ? "moved\n" : "move failed\n");
    return ok ? 0 : 1;
}

static int c_cat(int argc, char **argv) {
    char p[300];
    if (argc < 2 || !resolve(argv[1], p)) return 1;
    FILE *f = fopen(p, "r");
    if (!f) {
        con_printf("no such file\n");
        return 1;
    }
    char line[200];
    int lines = 0;
    while (fgets(line, sizeof line, f) && lines++ < 200) con_printf("%s", line);
    fclose(f);
    con_printf("\n");
    return 0;
}

static int c_apps(int argc, char **argv) {
    static app_entry_t a[MAX_APPS];
    int n = apps_scan(a, MAX_APPS);
    for (int i = 0; i < n; i++)
        con_printf("  %-20s %-12s %7lu  %s%s\n", a[i].info.name, a[i].info.version,
                   (unsigned long)a[i].size, a[i].path, a[i].slot >= 0 ? "  [ready]" : "");
    if (!n) con_printf("no apps in /files/apps\n");
    return 0;
}

static int c_slots(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "clear") == 0) {
        apps_clear_slots();
        con_printf("app slots cleared\n");
        return 0;
    }
    app_slot_t s[APP_SLOTS];
    apps_slots(s);
    for (int i = 0; i < APP_SLOTS; i++)
        s[i].used ? con_printf("  ota_%d: %s %s (%lu bytes, last run #%lu)\n", i, s[i].name,
                               s[i].version, (unsigned long)s[i].size, (unsigned long)s[i].last_used)
                  : con_printf("  ota_%d: empty\n", i);
    return 0;
}

static int c_launch(int argc, char **argv) {
    char p[300];
    if (argc < 2 || !resolve(argv[1], p)) return 1;
    if (!apps_launch(p)) {
        con_printf("not an app (or an install is running)\n");
        return 1;
    }
    con_printf("launching %s...\n", p);
    return 0;
}

static int c_parts(int argc, char **argv) {
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        con_printf("  %-9s type %d/%-3d 0x%07lx  %6lu KB\n", p->label, p->type, p->subtype,
                   (unsigned long)p->address, (unsigned long)p->size / 1024);
    }
    esp_partition_iterator_release(it);
    return 0;
}

static int c_wifi(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "on") == 0) {
        wifi_set_enabled(true);
    } else if (strcmp(sub, "off") == 0) {
        wifi_set_enabled(false);
    } else if (strcmp(sub, "connect") == 0 && argc >= 3) {
        wifi_connect(argv[2], argc > 3 ? argv[3] : "");
    } else if (strcmp(sub, "forget") == 0) {
        wifi_forget();
    } else if (strcmp(sub, "scan") == 0) {
        wifi_scan_start();
        bool busy = true;
        for (int i = 0; i < 100 && busy; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wifi_ap_t tmp[1];
            wifi_scan_results(tmp, 0, &busy);
        }
        wifi_ap_t aps[20];
        int n = wifi_scan_results(aps, 20, &busy);
        for (int i = 0; i < n; i++)
            con_printf("  %4d dBm %s %s\n", aps[i].rssi, aps[i].secure ? "*" : " ", aps[i].ssid);
        return 0;
    }
    static const char *const st[] = {"off", "no network saved", "connecting", "connected", "retrying"};
    con_printf("wifi %s, network '%s', ip %s, rssi %d, last reason %d\n", st[g_wifi.state],
               g_ls.ssid, g_wifi.ip[0] ? g_wifi.ip : "-", g_wifi.rssi, g_wifi.reason);
    con_printf("web transfer %s, PIN %s\n", web_running() ? "on (http://tulip.local)" : "off", g_ls.pin);
    return 0;
}

static int c_i2cscan(int argc, char **argv) {
    i2c_master_bus_handle_t bus = tulip_i2c_bus();
    if (!bus) {
        con_printf("i2c bus not started\n");
        return 1;
    }
    int found = 0;
    for (int a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            const char *who = a == 0x5D || a == 0x14 ? " (GT911 touch)" : a == 0x59 ? " (Mabee DAC)"
                              : a == 0x58 ? " (Mabee DAC, A0 cut)" : "";
            con_printf("  0x%02x%s\n", a, who);
            found++;
        }
    }
    if (!found) con_printf("nothing found\n");
    return 0;
}

static int c_bright(int argc, char **argv) {
    if (argc > 1) {
        g_tulip.brightness = (uint8_t)atoi(argv[1]);
        if (g_tulip.brightness < 1) g_tulip.brightness = 1;
        if (g_tulip.brightness > 9) g_tulip.brightness = 9;
        tulip_display_set_brightness(g_tulip.brightness);
        tulip_settings_save();
    }
    con_printf("brightness %d\n", g_tulip.brightness);
    return 0;
}

static int c_cal(int argc, char **argv) {
    con_printf("touch calibration (%s): x = %.4f*rx %+.4f*ry %+.2f ; y = %.4f*rx %+.4f*ry %+.2f\n",
               g_tulip.cal_valid ? "saved" : "default", g_tulip.cal[0], g_tulip.cal[1], g_tulip.cal[2],
               g_tulip.cal[3], g_tulip.cal[4], g_tulip.cal[5]);
    return 0;
}

static int c_reboot(int argc, char **argv) {
    con_printf("restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static int c_factory(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        con_printf("erases all settings (Wi-Fi, calibration, app settings). Type: factory-reset yes\n");
        return 1;
    }
    nvs_flash_erase();
    return c_reboot(0, NULL);
}

static int c_format(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        con_printf("erases every file in /files. Type: format yes\n");
        return 1;
    }
    bool ok = tulip_fs_format();
    con_printf(ok ? "storage formatted\n" : "format failed\n");
    return ok ? 0 : 1;
}

static int c_help(int argc, char **argv);

typedef struct {
    const char *name, *args, *help;
    int (*fn)(int, char **);
} cmd_t;

static const cmd_t CMDS[] = {
    {"help", "", "this list", c_help},
    {"ver", "", "launcher / IDF / chip version", c_ver},
    {"free", "", "free memory", c_free},
    {"tasks", "", "FreeRTOS tasks", c_tasks},
    {"uptime", "", "time since boot", c_uptime},
    {"df", "", "storage usage", c_df},
    {"ls", "[path]", "list a folder (paths are inside /files)", c_ls},
    {"cat", "<path>", "print a text file", c_cat},
    {"mkdir", "<path>", "create a folder", c_mkdir},
    {"rm", "<path>", "delete a file or folder (recursive)", c_rm},
    {"mv", "<from> <to>", "rename / move", c_mv},
    {"apps", "", "apps found in /files/apps", c_apps},
    {"slots", "[clear]", "what the 4 app slots hold", c_slots},
    {"launch", "<path>", "install + run an app", c_launch},
    {"parts", "", "partition table", c_parts},
    {"wifi", "[on|off|scan|connect <ssid> [pass]|forget]", "Wi-Fi status / control", c_wifi},
    {"i2cscan", "", "find devices on the I2C port", c_i2cscan},
    {"bright", "[1-9]", "backlight", c_bright},
    {"cal", "", "show the touch calibration", c_cal},
    {"reboot", "", "restart", c_reboot},
    {"format", "yes", "erase all files", c_format},
    {"factory-reset", "yes", "erase all settings", c_factory},
};
#define NCMDS (sizeof CMDS / sizeof CMDS[0])

static int c_help(int argc, char **argv) {
    for (size_t i = 0; i < NCMDS; i++)
        con_printf("  %-14s %-30s %s\n", CMDS[i].name, CMDS[i].args, CMDS[i].help);
    return 0;
}

// --- screen console task -----------------------------------------------------

static QueueHandle_t s_lines;
static SemaphoreHandle_t s_cmx;   // one command at a time (UART or screen)

static void run_line(char *line) {
    char *argv[8];
    int argc = (int)esp_console_split_argv(line, argv, 8);
    if (argc == 0) return;
    for (size_t i = 0; i < NCMDS; i++) {
        if (strcmp(argv[0], CMDS[i].name) == 0) {
            xSemaphoreTake(s_cmx, portMAX_DELAY);
            CMDS[i].fn(argc, argv);
            xSemaphoreGive(s_cmx);
            return;
        }
    }
    con_printf("unknown command '%s' - try help\n", argv[0]);
}

static void console_task(void *arg) {
    char line[160];
    for (;;) {
        if (xQueueReceive(s_lines, line, portMAX_DELAY) != pdTRUE) continue;
        con_printf("> %s\n", line);
        run_line(line);
    }
}

void console_submit(const char *line) {
    char l[160];
    snprintf(l, sizeof l, "%s", line);
    if (s_lines) xQueueSend(s_lines, l, 0);
}

// UART REPL: esp_console calls these; same lock as the screen.
static int uart_trampoline(int argc, char **argv) {
    for (size_t i = 0; i < NCMDS; i++) {
        if (strcmp(argv[0], CMDS[i].name) == 0) {
            xSemaphoreTake(s_cmx, portMAX_DELAY);
            int r = CMDS[i].fn(argc, argv);
            xSemaphoreGive(s_cmx);
            return r;
        }
    }
    return 1;
}

void console_init(void) {
    s_rmx = xSemaphoreCreateMutex();
    s_cmx = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    s_lines = xQueueCreate(4, 160);
    xTaskCreatePinnedToCore(console_task, "console", 6144, NULL, 3, NULL, 0);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "tulip>";
    rc.task_core_id = 0;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&uc, &rc, &repl) == ESP_OK) {
        for (size_t i = 0; i < NCMDS; i++) {
            esp_console_cmd_t c = {.command = CMDS[i].name, .help = CMDS[i].help,
                                   .hint = CMDS[i].args, .func = uart_trampoline};
            esp_console_cmd_register(&c);
        }
        esp_console_start_repl(repl);
    }
}
