// storage.c - NVS-backed preset bank and named projects.
#include "storage.h"

#include <stddef.h>
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

// Version 1 records (before RANGE and the CC lanes): the same layout with
// the shorter pattern, so a v1 blob is read field by field from these
// sizes and offsets.
#define BLOB_HDR      8                                     // magic, version, size, used
#define BLOB_V1       (BLOB_HDR + PATTERN_V1_SIZE)
#define PROJ_NAME_V1  ((MAX_PROJECT_NAME_LEN + 2) & ~1)     // name[25] + padding
#define PROJ_V1       (PROJ_NAME_V1 + BLOB_V1)
#define BANK_V1       (4 + MAX_PROJECTS * PROJ_V1)
_Static_assert(offsetof(pat_blob_t, p) == BLOB_HDR, "blob header");
_Static_assert(offsetof(proj_t, b) == PROJ_NAME_V1, "project name");

static pat_blob_t s_presets[NUM_PRESETS];
static proj_bank_t s_proj;
static SemaphoreHandle_t s_mx;
static TaskHandle_t s_task;
static volatile uint32_t s_dirty_presets;   // bitmask
static volatile bool s_dirty_projects;
static volatile bool s_dirty_cur;
static int8_t s_cur;

static bool blob_ok(const pat_blob_t *b) {
    return b->magic == BLOB_MAGIC && b->version == PATTERN_VERSION &&
           b->size == sizeof(pattern_t) && b->used;
}

// a version 1 blob (len bytes at raw) -> b; false if it isn't one
static bool blob_from_v1(const uint8_t *raw, pat_blob_t *b) {
    memset(b, 0, sizeof *b);
    memcpy(b, raw, BLOB_HDR);
    if (b->magic != BLOB_MAGIC || b->version != 1 || b->size != PATTERN_V1_SIZE || !b->used)
        return false;
    memcpy(&b->p, raw + BLOB_HDR, PATTERN_V1_SIZE);
    pattern_upgrade_v1(&b->p);
    b->version = PATTERN_VERSION;
    b->size = sizeof(pattern_t);
    return true;
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
    bool cur = s_dirty_cur;
    s_dirty_cur = false;
    int8_t cur_v = s_cur;
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
    if (cur) nvs_set_i8(h, "cur", cur_v);
    nvs_commit(h);
    nvs_close(h);
}

static void store_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // presets save themselves as you edit: write once it's been quiet
        // for a moment, not on every slider step
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500))) {
        }
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
    s_cur = -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        static uint8_t raw[BANK_V1 > sizeof(proj_bank_t) ? BANK_V1 : sizeof(proj_bank_t)];
        for (int i = 0; i < NUM_PRESETS; i++) {
            char key[8];
            snprintf(key, sizeof key, "p%02d", i);
            size_t len = sizeof raw;
            pat_blob_t *b = &s_presets[i];
            bool ok = nvs_get_blob(h, key, raw, &len) == ESP_OK;
            if (ok && len == sizeof *b) {
                memcpy(b, raw, sizeof *b);
                ok = blob_ok(b);
            } else if (ok && len == BLOB_V1) {
                ok = blob_from_v1(raw, b);
                if (ok) s_dirty_presets |= 1u << i;   // written back as version 2
            } else {
                ok = false;
            }
            if (ok)
                pattern_sanitise(&b->p);
            else
                memset(b, 0, sizeof *b);
        }
        size_t len = sizeof raw;
        memset(&s_proj, 0, sizeof s_proj);
        if (nvs_get_blob(h, "projects", raw, &len) == ESP_OK) {
            uint16_t magic, count;
            memcpy(&magic, raw, 2);
            memcpy(&count, raw + 2, 2);
            if (magic == BLOB_MAGIC && count <= MAX_PROJECTS && len == sizeof s_proj) {
                memcpy(&s_proj, raw, sizeof s_proj);
            } else if (magic == BLOB_MAGIC && count <= MAX_PROJECTS && len == BANK_V1) {
                int n = 0;
                for (int i = 0; i < count; i++) {
                    const uint8_t *it = raw + 4 + i * PROJ_V1;
                    proj_t *pr = &s_proj.items[n];
                    memcpy(pr->name, it, MAX_PROJECT_NAME_LEN + 1);
                    if (blob_from_v1(it + PROJ_NAME_V1, &pr->b)) n++;
                }
                s_proj.count = (uint16_t)n;
                s_dirty_projects = true;
                ESP_LOGI(TAG, "%d projects upgraded", n);
            }
            for (int i = 0; i < s_proj.count; i++) {
                s_proj.items[i].name[MAX_PROJECT_NAME_LEN] = 0;
                pattern_sanitise(&s_proj.items[i].b.p);
            }
        }
        int8_t c;
        if (nvs_get_i8(h, "cur", &c) == ESP_OK && c >= 0 && c < NUM_PRESETS) s_cur = c;
        nvs_close(h);
    }
    s_proj.magic = BLOB_MAGIC;
    xTaskCreatePinnedToCore(store_task, "store", 4096, NULL, STORE_PRIO, &s_task, STORE_CORE);
    if (s_dirty_presets || s_dirty_projects) xTaskNotifyGive(s_task);
}

int storage_cur_preset(void) { return s_cur; }

void storage_set_cur_preset(int slot) {
    if (slot == s_cur) return;
    s_cur = (int8_t)slot;
    s_dirty_cur = true;
    xTaskNotifyGive(s_task);
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

void storage_preset_clear(int slot) {
    if (slot < 0 || slot >= NUM_PRESETS || !storage_preset_used(slot)) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memset(&s_presets[slot], 0, sizeof s_presets[slot]);
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

bool storage_write_pending(void) {
    return s_dirty_presets != 0 || s_dirty_projects || s_dirty_cur;
}
