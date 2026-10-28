// loader.c - apps loaded at start-up from /files/studio/apps/<name>.elf
// (an EXPERIMENT, see docs/APPS.md section 7).
//
// Each .elf is an app built on its own (tools/build_elf_app.sh). Espressif's
// elf_loader copies it into PSRAM, links it against the functions Tulip
// Studio exports to apps (tools/tulip_abi.py) and calls its app_main(),
// which hands over its tmod_t (studio_register_app). From then on it is an
// app like the built-in ones: the same hooks, run by the same tasks.
//
// With CONFIG_SPIRAM_XIP_FROM_PSRAM the cache stays on during flash writes,
// so code in PSRAM keeps running while files are saved - as the built-in
// apps' code does.
//
// A crash guard: before a loaded app's code first runs (its app_main(), its
// init(), its screen) the app's name goes in NVS, and it's cleared after. If
// the Tulip restarts with a name still there, that app crashed: its file is
// renamed <name>.elf.crashed and Tulip Studio starts without it.
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "studio_priv.h"

static const char *TAG = "loader";

#define APPS_DIR "/files/studio/apps"
#define GUARD_NS "tulip"
#define GUARD_KEY "app_guard"
#define MAX_ELF_BYTES (512 * 1024)

extern const struct esp_elfsym g_tulip_abi_syms[];   // abi_exports.c (generated)
extern const int g_tulip_abi_count;

static const tmod_t *s_registered;   // set by the app's app_main()
static int s_reg_abi;
static const tmod_t *s_loaded[STUDIO_MAX_MODULES];
static int s_nloaded;

bool studio_register_app(const tmod_t *m, int abi_version) {
    if (s_registered || !m) return false;
    s_reg_abi = abi_version;
    s_registered = m;
    return abi_version == TULIP_ABI_VERSION;
}

bool loader_is_loaded(const tmod_t *m) {
    for (int i = 0; i < s_nloaded; i++)
        if (s_loaded[i] == m) return true;
    return false;
}

// --- the crash guard ---------------------------------------------------------------------

static void guard_set(const char *name) {
    nvs_handle_t h;
    if (nvs_open(GUARD_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (name)
        nvs_set_str(h, GUARD_KEY, name);
    else
        nvs_erase_key(h, GUARD_KEY);
    nvs_commit(h);
    nvs_close(h);
}

static void guard_get(char *out, size_t len) {
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(GUARD_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_str(h, GUARD_KEY, out, &len) != ESP_OK) out[0] = 0;
    nvs_close(h);
}

void loader_guard_begin(const tmod_t *m) {
    if (loader_is_loaded(m)) guard_set(m->name);
}

void loader_guard_end(const tmod_t *m) {
    if (loader_is_loaded(m)) guard_set(NULL);
}

// --- loading ----------------------------------------------------------------------------------

static bool has_ext(const char *n, const char *ext) {
    size_t a = strlen(n), b = strlen(ext);
    return a > b && !strcmp(n + a - b, ext);
}

static bool name_taken(const char *name, const tmod_t *const *mods, int n) {
    for (int i = 0; i < n; i++)
        if (!strcmp(mods[i]->name, name)) return true;
    return false;
}

// One .elf: read, relocate, run its app_main(). The app, or NULL.
static const tmod_t *load_one(const char *stem, const tmod_t *const *mods, int n) {
    char path[96];
    snprintf(path, sizeof path, APPS_DIR "/%s.elf", stem);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > MAX_ELF_BYTES) {
        ESP_LOGW(TAG, "%s: missing, empty or over %d KB", path, MAX_ELF_BYTES / 1024);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    bool ok = f && fread(buf, 1, st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    if (!ok) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "%s: can't read it", path);
        return NULL;
    }
    esp_elf_t *elf = heap_caps_calloc(1, sizeof *elf, MALLOC_CAP_SPIRAM);
    if (!elf || esp_elf_init(elf) < 0 || esp_elf_relocate(elf, buf, st.st_size) < 0) {
        // most often: it uses a function Tulip Studio doesn't export (the
        // log above names it), or it was built for another chip
        ESP_LOGE(TAG, "%s: not a Tulip Studio app for this firmware", path);
        heap_caps_free(buf);
        if (elf) esp_elf_deinit(elf);
        heap_caps_free(elf);
        return NULL;
    }
    heap_caps_free(buf);   // relocate copied what it needs
    s_registered = NULL;
    guard_set(stem);
    esp_elf_request(elf, 0, 0, NULL);
    guard_set(NULL);
    const tmod_t *m = s_registered;
    const char *why = !m                          ? "its app_main() didn't call studio_register_app()"
                      : s_reg_abi != TULIP_ABI_VERSION ? "it was built for another version of Tulip Studio"
                      : strcmp(m->name, stem)          ? "its name isn't its file's name"
                      : name_taken(m->name, mods, n)   ? "an app of that name is already in Tulip Studio"
                                                       : NULL;
    if (why) {
        ESP_LOGE(TAG, "%s: %s", path, why);
        esp_elf_deinit(elf);   // nothing of it runs any more
        heap_caps_free(elf);
        return NULL;
    }
    ESP_LOGI(TAG, "loaded %s (%s) from %s, ABI %d", m->title, studio_role_name(m->role), path, s_reg_abi);
    return m;   // the elf stays loaded for good
}

int loader_load_apps(const tmod_t **out, int room, const tmod_t *const *mods, int n, const char *only) {
    static esp_elf_symbol_table_t *registered;
    if (!registered) {
        registered = g_tulip_abi_syms;
        esp_elf_register_symbol(g_tulip_abi_syms);
        ESP_LOGI(TAG, "%d functions exported to apps (ABI %d)", g_tulip_abi_count, TULIP_ABI_VERSION);
    }
    // the app that crashed last time stays out
    char crashed[24];
    guard_get(crashed, sizeof crashed);
    if (crashed[0]) {
        char a[96], b[104];
        snprintf(a, sizeof a, APPS_DIR "/%s.elf", crashed);
        snprintf(b, sizeof b, "%s.crashed", a);
        unlink(b);
        rename(a, b);
        guard_set(NULL);
        ESP_LOGE(TAG, "%s crashed the Tulip last time: renamed to %s", crashed, b);
    }
    DIR *d = opendir(APPS_DIR);
    if (!d) return 0;
    int k = 0;
    struct dirent *e;
    char stems[STUDIO_MAX_MODULES][24];
    int ns = 0;
    while ((e = readdir(d)) && ns < STUDIO_MAX_MODULES) {
        if (!has_ext(e->d_name, ".elf") || strlen(e->d_name) - 4 >= sizeof stems[0]) continue;
        snprintf(stems[ns], sizeof stems[0], "%.*s", (int)(strlen(e->d_name) - 4), e->d_name);
        ns++;
    }
    closedir(d);
    // in name order, so their place in the Studio stays put
    for (int i = 1; i < ns; i++)
        for (int j = i; j > 0 && strcmp(stems[j - 1], stems[j]) > 0; j--) {
            char t[24];
            memcpy(t, stems[j], sizeof t);
            memcpy(stems[j], stems[j - 1], sizeof t);
            memcpy(stems[j - 1], t, sizeof t);
        }
    for (int i = 0; i < ns && k < room; i++) {
        if (only && strcmp(only, stems[i])) continue;
        const tmod_t *m = load_one(stems[i], mods, n);
        if (!m) continue;
        out[k++] = m;
        s_loaded[s_nloaded++] = m;
    }
    return k;
}
