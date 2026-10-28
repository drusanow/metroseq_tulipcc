// apps.c - see apps.h.
#include "apps.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lsettings.h"
#include "nvs.h"
#include "tulip/fs.h"
#include "tulip/sys.h"

static const char *TAG = "apps";

#define NS "launcher"
#define SLOT_MAGIC 0x534C   // "SL"
#define CHUNK 4096

typedef struct {
    uint16_t magic, count;
    uint32_t clock;   // launch counter
    app_slot_t s[APP_SLOTS];
} slot_table_t;

install_status_t g_install;
boot_status_t g_boot = {.pct = -1};

void boot_status(const char *title, const char *msg, int pct) {
    if (title) snprintf(g_boot.title, sizeof g_boot.title, "%s", title);
    if (msg) snprintf(g_boot.msg, sizeof g_boot.msg, "%s", msg);
    g_boot.pct = pct;
}
static slot_table_t s_tab;
static SemaphoreHandle_t s_mx;
static char s_launch_path[192];

static const esp_partition_t *slot_part(int i) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i),
                                    NULL);
}

static void table_save(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "slots", &s_tab, sizeof s_tab);
    nvs_commit(h);
    nvs_close(h);
}

static void table_load(void) {
    nvs_handle_t h;
    size_t len = sizeof s_tab;
    bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "slots", &s_tab, &len) == ESP_OK && len == sizeof s_tab &&
             s_tab.magic == SLOT_MAGIC && s_tab.count == APP_SLOTS;
        nvs_close(h);
    }
    if (!ok) {
        memset(&s_tab, 0, sizeof s_tab);
        s_tab.magic = SLOT_MAGIC;
        s_tab.count = APP_SLOTS;
    }
}

bool apps_read_info(const char *path, app_info_t *info, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[APPIMG_HEADER_BYTES];
    size_t n = fread(buf, 1, sizeof buf, f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (!appimg_parse(buf, n, info)) return false;
    *size = (uint32_t)sz;
    return true;
}

bool apps_read_manifest(const char *path, app_manifest_t *m) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static uint8_t buf[APP_MANIFEST_END];   // UI task only; 2 KB off its stack
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return appimg_manifest(buf, n, m);
}

int apps_loaded(app_manifest_entry_t *out, int max) {
    DIR *d = opendir(STUDIO_APPS_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    static char txt[1024];   // UI task only
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l > 40 || strcmp(e->d_name + l - 4, ".elf")) continue;
        char p[96];
        snprintf(p, sizeof p, STUDIO_APPS_DIR "/%.*s.json", (int)(l - 4), e->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        size_t k = fread(txt, 1, sizeof txt - 1, f);
        fclose(f);
        txt[k] = 0;
        // its name must be its file's, as Tulip Studio insists
        if (appimg_app_json(txt, &out[n]) && strlen(out[n].name) == l - 4 &&
            !strncmp(out[n].name, e->d_name, l - 4))
            n++;
    }
    closedir(d);
    return n;
}

// Length of the valid image in a slot (0 if none).
static uint32_t slot_image_len(const esp_partition_t *p) {
    esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    esp_image_metadata_t md;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) return 0;
    return md.image_len;
}

// The slot's app in /files/apps isn't a short copy (one cut short by a power
// off or crash before this check existed). A missing file is fine: the user
// deleted it, and it stays deleted.
static bool export_ok(const esp_app_desc_t *d, uint32_t size) {
    char path[160];
    snprintf(path, sizeof path, TULIP_APPS_DIR "/%s.bin", d->project_name);
    struct stat st;
    return stat(path, &st) != 0 || (uint32_t)st.st_size == size;
}

// A slot holds an app nobody knows about (preloaded at first flash, or the
// table was lost): add it to the table and export it to /files/apps.
static void adopt_slot(int i, const esp_partition_t *p, const esp_app_desc_t *d, int nth,
                       int total) {
    uint32_t len = slot_image_len(p);
    if (!len) return;
    app_slot_t *s = &s_tab.s[i];
    memset(s, 0, sizeof *s);
    s->used = 1;
    memcpy(s->elf_sha, d->app_elf_sha256, 32);
    s->size = len;
    snprintf(s->name, sizeof s->name, "%s", d->project_name);
    snprintf(s->version, sizeof s->version, "%s", d->version);

    char path[160];
    snprintf(path, sizeof path, TULIP_APPS_DIR "/%s.bin", d->project_name);
    // A copy cut short (power off, a crash) left a short file behind, which
    // the launcher can't read as an app: only a complete one counts. The
    // copy goes to a temporary name, renamed once it is all there.
    struct stat st;
    if (stat(path, &st) == 0 && (uint32_t)st.st_size == len) return;   // already there
    char tmp[168];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    static uint8_t buf[CHUNK];
    bool ok = true;
    char msg[96];
    snprintf(msg, sizeof msg, "copying %s into /files/apps  (%d of %d)", d->project_name, nth,
             total);
    for (uint32_t off = 0; ok && off < len; off += CHUNK) {
        uint32_t n = len - off < CHUNK ? len - off : CHUNK;
        ok = esp_partition_read(p, off, buf, n) == ESP_OK && fwrite(buf, 1, n, f) == n;
        boot_status(NULL, msg, (int)((uint64_t)(off + n) * 100 / len));
    }
    if (fclose(f) != 0) ok = false;
    unlink(path);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        ESP_LOGE(TAG, "could not copy %s to %s", d->project_name, path);
        return;
    }
    ESP_LOGI(TAG, "imported %s from slot %d to %s", d->project_name, i, path);
}

void apps_init(void) {
    s_mx = xSemaphoreCreateMutex();
    // where loadable Studio apps go, so an upload there just works
    mkdir(TULIP_FS_ROOT "/studio", 0775);
    mkdir(STUDIO_APPS_DIR, 0775);
    table_load();
    bool changed = false;
    // static: 6 x 256 bytes would overflow app_main's stack
    static bool adopt[APP_SLOTS];
    static esp_app_desc_t desc[APP_SLOTS];
    memset(adopt, 0, sizeof adopt);
    int total = 0;
    for (int i = 0; i < APP_SLOTS; i++) {
        const esp_partition_t *p = slot_part(i);
        bool has = p && esp_ota_get_partition_description(p, &desc[i]) == ESP_OK;
        app_slot_t *s = &s_tab.s[i];
        if (!has) {
            if (s->used) {
                memset(s, 0, sizeof *s);
                changed = true;
            }
        } else if (!s->used || memcmp(s->elf_sha, desc[i].app_elf_sha256, 32) != 0 ||
                   !export_ok(&desc[i], s->size)) {
            adopt[i] = true;
            total++;
        }
    }
    if (total) boot_status("SETTING UP APPS", "checking the app slots", 0);
    for (int i = 0, nth = 0; i < APP_SLOTS; i++) {
        if (!adopt[i]) continue;
        adopt_slot(i, slot_part(i), &desc[i], ++nth, total);
        changed = true;
    }
    if (changed) table_save();
}

static void scan_dir(const char *dir, app_entry_t *out, int max, int *n, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[192];
    while ((e = readdir(d)) && *n < max) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (e->d_type == DT_DIR) {
            if (depth == 0) scan_dir(p, out, max, n, 1);
            continue;
        }
        if (!path_has_ext(p, ".bin")) continue;
        app_entry_t *a = &out[*n];
        if (!apps_read_info(p, &a->info, &a->size)) continue;
        snprintf(a->path, sizeof a->path, "%s", p);
        a->slot = slots_find(s_tab.s, APP_SLOTS, a->info.elf_sha, a->size);
        (*n)++;
    }
    closedir(d);
}

static int cmp_app(const void *a, const void *b) {
    return strcasecmp(((const app_entry_t *)a)->info.name, ((const app_entry_t *)b)->info.name);
}

int apps_scan(app_entry_t *out, int max) {
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    scan_dir(TULIP_APPS_DIR, out, max, &n, 0);
    xSemaphoreGive(s_mx);
    qsort(out, n, sizeof *out, cmp_app);
    return n;
}

int apps_slots(app_slot_t out[APP_SLOTS]) {
    memcpy(out, s_tab.s, sizeof s_tab.s);
    return APP_SLOTS;
}

void apps_clear_slots(void) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < APP_SLOTS; i++) {
        const esp_partition_t *p = slot_part(i);
        if (p) esp_partition_erase_range(p, 0, 0x10000);   // kills the image header
    }
    memset(s_tab.s, 0, sizeof s_tab.s);
    table_save();
    xSemaphoreGive(s_mx);
}

static void fail(const char *msg) {
    snprintf(g_install.msg, sizeof g_install.msg, "%s", msg);
    g_install.state = INSTALL_FAILED;
    ESP_LOGE(TAG, "%s", msg);
}

static void install_task(void *arg) {
    app_info_t info;
    uint32_t size;
    if (!apps_read_info(s_launch_path, &info, &size)) {
        fail("not a Tulip app (no ESP32-S3 app header)");
        vTaskDelete(NULL);
    }
    if (!g_install.name[0]) snprintf(g_install.name, sizeof g_install.name, "%s", info.name);
    xSemaphoreTake(s_mx, portMAX_DELAY);

    int slot = slots_find(s_tab.s, APP_SLOTS, info.elf_sha, size);
    const esp_partition_t *part = NULL;
    if (slot >= 0) {
        // Cached - but make sure the slot really still holds it.
        part = slot_part(slot);
        esp_app_desc_t d;
        if (!part || esp_ota_get_partition_description(part, &d) != ESP_OK ||
            memcmp(d.app_elf_sha256, info.elf_sha, 32) != 0)
            slot = -1;
    }
    if (slot < 0) {
        slot = slots_pick(s_tab.s, APP_SLOTS);
        part = slot_part(slot);
        if (!part || size > part->size) {
            xSemaphoreGive(s_mx);
            fail(part ? "app is larger than an app slot (1.5 MB)" : "no app slots in partition table");
            vTaskDelete(NULL);
        }
        g_install.state = INSTALL_ERASING;
        g_install.pct = 0;
        snprintf(g_install.msg, sizeof g_install.msg, "preparing slot %d", slot);
        s_tab.s[slot].used = 0;   // half-written slot must never look valid
        table_save();
        esp_ota_handle_t h;
        if (esp_ota_begin(part, size, &h) != ESP_OK) {
            xSemaphoreGive(s_mx);
            fail("could not erase the app slot");
            vTaskDelete(NULL);
        }
        FILE *f = fopen(s_launch_path, "rb");
        static uint8_t buf[CHUNK];
        uint32_t done = 0;
        bool ok = f != NULL;
        g_install.state = INSTALL_WRITING;
        while (ok && done < size) {
            size_t n = fread(buf, 1, CHUNK, f);
            if (n == 0) break;
            ok = esp_ota_write(h, buf, n) == ESP_OK;
            done += n;
            g_install.pct = (int)((uint64_t)done * 100 / size);
        }
        if (f) fclose(f);
        // esp_ota_end() checks the whole image (segments + SHA-256) and
        // frees the handle whatever the result; abort only if we never got
        // that far.
        esp_err_t end;
        if (ok && done == size) {
            end = esp_ota_end(h);
        } else {
            esp_ota_abort(h);
            end = ESP_FAIL;
        }
        if (end != ESP_OK) {
            xSemaphoreGive(s_mx);
            fail("image check failed - the file is damaged or incomplete");
            vTaskDelete(NULL);
        }
        app_slot_t *s = &s_tab.s[slot];
        s->used = 1;
        memcpy(s->elf_sha, info.elf_sha, 32);
        s->size = size;
        snprintf(s->name, sizeof s->name, "%s", info.name);
        snprintf(s->version, sizeof s->version, "%s", info.version);
    }
    s_tab.s[slot].last_used = ++s_tab.clock;
    table_save();
    snprintf(g_ls.last_app, sizeof g_ls.last_app, "%s", s_launch_path);
    lsettings_save();
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        xSemaphoreGive(s_mx);
        fail("could not select the app slot");
        vTaskDelete(NULL);
    }
    xSemaphoreGive(s_mx);
    g_install.pct = 100;
    g_install.state = INSTALL_STARTING;
    snprintf(g_install.msg, sizeof g_install.msg, "starting %s", g_install.name);
    ESP_LOGI(TAG, "launching %s from slot %d", info.name, slot);
    vTaskDelay(pdMS_TO_TICKS(400));   // let the screen show it
    esp_restart();
}

bool apps_launch(const char *path) { return apps_launch_as(path, "", NULL); }

bool apps_launch_as(const char *path, const char *mode, const char *title) {
    int st = g_install.state;
    if (st == INSTALL_ERASING || st == INSTALL_WRITING || st == INSTALL_STARTING) return false;
    app_info_t info;
    uint32_t size;
    if (!apps_read_info(path, &info, &size)) return false;
    snprintf(s_launch_path, sizeof s_launch_path, "%s", path);
    // read by Tulip Studio at its start (other apps ignore it)
    tulip_boot_mode_set(mode ? mode : "");
    memset(&g_install, 0, sizeof g_install);
    g_install.state = INSTALL_ERASING;
    snprintf(g_install.name, sizeof g_install.name, "%s", title ? title : info.name);
    snprintf(g_install.msg, sizeof g_install.msg, "checking");
    xTaskCreatePinnedToCore(install_task, "install", 6144, NULL, 5, NULL, 0);
    return true;
}
