// storage.c - NVS-backed preset bank and named projects.
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "store";

#define NS            "metro"
#define BLOB_MAGIC    0x504d   // "MP"
#define STORE_CORE    0
#define STORE_PRIO    2

typedef struct {
    uint16_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t used;
    pattern_t p;
} pat_blob_t;

typedef struct {
    char name[MAX_PROJECT_NAME_LEN + 1];
    pat_blob_t b;
} proj_t;

typedef struct {
    uint16_t magic;
    uint16_t count;
    proj_t items[MAX_PROJECTS];
} proj_bank_t;

typedef struct {
    uint16_t magic;
    uint16_t version;
    settings_t s;
} settings_blob_t;
#define SETTINGS_VERSION 1

static pat_blob_t s_presets[NUM_PRESETS];
static proj_bank_t s_proj;
static SemaphoreHandle_t s_mx;
static TaskHandle_t s_task;
static volatile uint32_t s_dirty_presets;   // bitmask
static volatile bool s_dirty_projects;
static volatile bool s_dirty_settings;
static settings_blob_t s_settings;

static bool blob_ok(const pat_blob_t *b) {
    return b->magic == BLOB_MAGIC && b->version == PATTERN_VERSION &&
           b->size == sizeof(pattern_t) && b->used;
}

static void blob_set(pat_blob_t *b, const pattern_t *p) {
    b->magic = BLOB_MAGIC;
    b->version = PATTERN_VERSION;
    b->size = sizeof(pattern_t);
    b->used = 1;
    b->p = *p;
}

static void flush_now(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t dirty = s_dirty_presets;
    s_dirty_presets = 0;
    bool proj = s_dirty_projects;
    s_dirty_projects = false;
    bool sets = s_dirty_settings;
    s_dirty_settings = false;
    settings_blob_t sb = s_settings;
    static pat_blob_t pb[NUM_PRESETS];
    static proj_bank_t bank;
    memcpy(pb, s_presets, sizeof pb);
    if (proj) bank = s_proj;
    xSemaphoreGive(s_mx);

    for (int i = 0; i < NUM_PRESETS; i++) {
        if (!(dirty & (1u << i))) continue;
        char key[8];
        snprintf(key, sizeof key, "p%02d", i);
        if (nvs_set_blob(h, key, &pb[i], sizeof pb[i]) != ESP_OK)
            ESP_LOGW(TAG, "preset %d write failed", i + 1);
    }
    if (proj && nvs_set_blob(h, "projects", &bank, sizeof bank) != ESP_OK)
        ESP_LOGW(TAG, "project write failed");
    if (sets && nvs_set_blob(h, "settings", &sb, sizeof sb) != ESP_OK)
        ESP_LOGW(TAG, "settings write failed");
    nvs_commit(h);
    nvs_close(h);
}

static void store_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if CONFIG_METRO_DEFER_FLASH_WHILE_PLAYING
        while (g_status.playing) vTaskDelay(pdMS_TO_TICKS(200));
#endif
        flush_now();
    }
}

void storage_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_mx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < NUM_PRESETS; i++) {
            char key[8];
            snprintf(key, sizeof key, "p%02d", i);
            size_t len = sizeof s_presets[i];
            if (nvs_get_blob(h, key, &s_presets[i], &len) != ESP_OK || len != sizeof s_presets[i] ||
                !blob_ok(&s_presets[i]))
                memset(&s_presets[i], 0, sizeof s_presets[i]);
            else
                pattern_sanitise(&s_presets[i].p);
        }
        size_t slen = sizeof s_settings;
        if (nvs_get_blob(h, "settings", &s_settings, &slen) == ESP_OK &&
            slen == sizeof s_settings && s_settings.magic == BLOB_MAGIC &&
            s_settings.version == SETTINGS_VERSION) {
            g_settings = s_settings.s;
            if (g_settings.clock_src > CLOCK_MIDI) g_settings.clock_src = CLOCK_INT;
        }
        size_t len = sizeof s_proj;
        if (nvs_get_blob(h, "projects", &s_proj, &len) != ESP_OK || len != sizeof s_proj ||
            s_proj.magic != BLOB_MAGIC || s_proj.count > MAX_PROJECTS) {
            memset(&s_proj, 0, sizeof s_proj);
        } else {
            for (int i = 0; i < s_proj.count; i++) {
                s_proj.items[i].name[MAX_PROJECT_NAME_LEN] = 0;
                pattern_sanitise(&s_proj.items[i].b.p);
            }
        }
        nvs_close(h);
    }
    s_proj.magic = BLOB_MAGIC;
    xTaskCreatePinnedToCore(store_task, "store", 4096, NULL, STORE_PRIO, &s_task, STORE_CORE);
}

bool storage_preset_used(int slot) {
    return slot >= 0 && slot < NUM_PRESETS && blob_ok(&s_presets[slot]);
}

bool storage_preset_get(int slot, pattern_t *out) {
    if (!storage_preset_used(slot)) return false;
    *out = s_presets[slot].p;
    return true;
}

void storage_preset_save(int slot, const pattern_t *p) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    blob_set(&s_presets[slot], p);
    s_dirty_presets |= 1u << slot;
    xSemaphoreGive(s_mx);
    xTaskNotifyGive(s_task);
}

int storage_project_count(void) { return s_proj.count; }

const char *storage_project_name(int i) {
    return (i >= 0 && i < s_proj.count) ? s_proj.items[i].name : "?";
}

bool storage_project_get(int i, pattern_t *out) {
    if (i < 0 || i >= s_proj.count || !blob_ok(&s_proj.items[i].b)) return false;
    *out = s_proj.items[i].b.p;
    return true;
}

int storage_project_save(const char *name, const pattern_t *p) {
    char nm[MAX_PROJECT_NAME_LEN + 1];
    snprintf(nm, sizeof nm, "%s", name);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_proj.count; i++)
        if (strcmp(s_proj.items[i].name, nm) == 0) idx = i;
    if (idx < 0) {
        if (s_proj.count >= MAX_PROJECTS) {
            memmove(&s_proj.items[0], &s_proj.items[1], (MAX_PROJECTS - 1) * sizeof(proj_t));
            s_proj.count--;
        }
        idx = s_proj.count++;
    }
    snprintf(s_proj.items[idx].name, sizeof s_proj.items[idx].name, "%s", nm);
    blob_set(&s_proj.items[idx].b, p);
    s_dirty_projects = true;
    xSemaphoreGive(s_mx);
    xTaskNotifyGive(s_task);
    return idx;
}

void storage_settings_save(void) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_settings.magic = BLOB_MAGIC;
    s_settings.version = SETTINGS_VERSION;
    s_settings.s = g_settings;
    s_dirty_settings = true;
    xSemaphoreGive(s_mx);
    xTaskNotifyGive(s_task);
}

bool storage_write_pending(void) {
    return s_dirty_presets != 0 || s_dirty_projects || s_dirty_settings;
}
