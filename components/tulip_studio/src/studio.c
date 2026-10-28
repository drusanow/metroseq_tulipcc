// studio.c - the module registry, settings and note routing.
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "studio_priv.h"
#include "tulip/display.h"
#include "tulip/fs.h"
#include "tulip/settings.h"
#include "tulip/sys.h"

static const char *TAG = "studio";

const tmod_t *g_mods[STUDIO_MAX_MODULES];
int g_nmods;
volatile bool g_enabled[STUDIO_MAX_MODULES];
volatile bool g_started[STUDIO_MAX_MODULES];
char g_single[24];
studio_cfg_t g_cfg;
studio_mix_t g_mix;

static bool s_files;
static volatile bool s_cfg_dirty;
static int64_t s_cfg_dirty_at;

const char *studio_role_name(tmod_role_t r) {
    switch (r) {
    case TMOD_SEQUENCER: return "SEQUENCER";
    case TMOD_DRUMS: return "DRUMS";
    case TMOD_SYNTH: return "SYNTH";
    }
    return "";
}

int studio_module_count(void) { return g_nmods; }
const tmod_t *studio_module(int i) { return i >= 0 && i < g_nmods ? g_mods[i] : NULL; }
bool studio_module_enabled(int i) { return i >= 0 && i < g_nmods && g_enabled[i]; }
bool studio_multi(void) { return g_nmods > 1; }
bool studio_module_started(int i) { return i >= 0 && i < g_nmods && g_started[i]; }

// the UI's view of ON / OFF (the rt task's g_enabled follows a moment later)
static bool s_on[STUDIO_MAX_MODULES];
bool studio_module_on(int i) { return i >= 0 && i < g_nmods && s_on[i]; }

int studio_module_index(const tmod_t *m) {
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i] == m) return i;
    return -1;
}

const char *studio_files_root(void) { return s_files ? TULIP_FS_ROOT : NULL; }

// --- settings (NVS, one record per app image) -------------------------------------------

#define CFG_VERSION 2

static void ns_name(char *out, size_t n) {
    // the Studio and each app on its own keep separate settings
    snprintf(out, n, "s_%.12s", g_single[0] ? g_single : esp_app_get_description()->project_name);
}

static void cfg_defaults(void) {
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.bpm_x100 = 12000;
    g_cfg.clock_src = CLOCK_SRC_INT;
    g_cfg.mmc = 1;
    g_cfg.volume = 8;
    g_cfg.open_mask = 0;
    g_cfg.shown = 0;
    g_cfg.cv_clock = 1;
    g_cfg.cv_div = 2;
    for (int m = 0; m < STUDIO_MAX_MODULES; m++)
        for (int k = 0; k < STUDIO_MAX_OUTS; k++) {
            g_cfg.route[m][k] = ROUTE_NONE;
            g_cfg.route_ch[m][k] = (uint8_t)(k + 1);
        }
}

void cfg_load(void) {
    cfg_defaults();
    for (int m = 0; m < STUDIO_MAX_MODULES; m++) g_mix.level[m] = STUDIO_MIX_UNITY;
    g_mix.mute = 0;
    char ns[16];
    ns_name(ns, sizeof ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t ver = 0;
    studio_cfg_t c;
    size_t sz = sizeof c;
    if (nvs_get_u8(h, "ver", &ver) == ESP_OK && ver == CFG_VERSION &&
        nvs_get_blob(h, "cfg", &c, &sz) == ESP_OK && sz == sizeof c) {
        g_cfg = c;
    }
    studio_mix_t mx;
    sz = sizeof mx;
    if (nvs_get_blob(h, "mix", &mx, &sz) == ESP_OK && sz == sizeof mx) g_mix = mx;
    for (int m = 0; m < STUDIO_MAX_MODULES; m++)
        if (g_mix.level[m] > 100) g_mix.level[m] = STUDIO_MIX_UNITY;
    nvs_close(h);
    if (g_cfg.bpm_x100 < TC_MIN_BPM_X100 || g_cfg.bpm_x100 > TC_MAX_BPM_X100) g_cfg.bpm_x100 = 12000;
    if (g_cfg.volume > 10) g_cfg.volume = 8;
    if (g_cfg.cv_div != 2 && g_cfg.cv_div != 4 && g_cfg.cv_div != 8) g_cfg.cv_div = 2;
}

static void cfg_save_now(void) {
    char ns[16];
    ns_name(ns, sizeof ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ver", CFG_VERSION);
    nvs_set_blob(h, "cfg", &g_cfg, sizeof g_cfg);
    nvs_set_blob(h, "mix", &g_mix, sizeof g_mix);
    nvs_commit(h);
    nvs_close(h);
}

void cfg_changed(void) {
    s_cfg_dirty_at = studio_now_us();
    s_cfg_dirty = true;
}

// UI task: write a second after the last change (a slider drag is one write).
void cfg_poll(void) {
    if (s_cfg_dirty && studio_now_us() - s_cfg_dirty_at > 1000000) {
        s_cfg_dirty = false;
        cfg_save_now();
    }
}

// --- routing ---------------------------------------------------------------------------------

int studio_route_get(int mod, int track) {
    if (mod < 0 || mod >= STUDIO_MAX_MODULES || track < 0 || track >= STUDIO_MAX_OUTS)
        return ROUTE_NONE;
    return g_cfg.route[mod][track];
}

void studio_route_set(int mod, int track, int dest) {
    if (mod < 0 || mod >= STUDIO_MAX_MODULES || track < 0 || track >= STUDIO_MAX_OUTS) return;
    int old = g_cfg.route[mod][track];
    if (old == dest) return;
    // let go of anything still sounding at the old destination
    tnote_t off = {.type = TN_ALL_OFF, .src = (uint8_t)(mod * STUDIO_MAX_OUTS + track),
                   .key = (uint8_t)track, .t = studio_now_us()};
    if (old >= 0 && old < g_nmods && g_mods[old]->note) g_mods[old]->note(&off);
    if (old == ROUTE_MIDI_OUT) {
        uint8_t m[3] = {(uint8_t)(0xB0 | ((g_cfg.route_ch[mod][track] - 1) & 15)), 123, 0};
        studio_midi_out(m, 3);   // all notes off
    }
    g_cfg.route[mod][track] = (int8_t)dest;
    cfg_changed();
}

int studio_route_midi_ch(int mod, int track) {
    if (mod < 0 || mod >= STUDIO_MAX_MODULES || track < 0 || track >= STUDIO_MAX_OUTS) return 1;
    return g_cfg.route_ch[mod][track];
}

void studio_route_set_midi_ch(int mod, int track, int ch) {
    if (mod < 0 || mod >= STUDIO_MAX_MODULES || track < 0 || track >= STUDIO_MAX_OUTS) return;
    if (ch < 1) ch = 16;
    if (ch > 16) ch = 1;
    g_cfg.route_ch[mod][track] = (uint8_t)ch;
    cfg_changed();
}

bool studio_track_routed(const tmod_t *m, int track) {
    int i = studio_module_index(m);
    int d = studio_route_get(i, track);
    if (d == ROUTE_MIDI_OUT) return true;
    return d >= 0 && d < g_nmods && g_enabled[d];
}

// Called from the sequencer's rt task at the note's due time.
bool studio_note_out(const tmod_t *m, int track, const tnote_t *n) {
    int i = studio_module_index(m);
    int d = studio_route_get(i, track);
    if (d == ROUTE_NONE) return false;
    tnote_t e = *n;
    e.src = (uint8_t)(i * STUDIO_MAX_OUTS + track);
    e.key = (uint8_t)track;
    if (d == ROUTE_MIDI_OUT) {
        // remember the note each track holds, for its note-off
        static uint8_t held[STUDIO_MAX_MODULES][STUDIO_MAX_OUTS];
        static bool on[STUDIO_MAX_MODULES][STUDIO_MAX_OUTS];
        uint8_t ch = (uint8_t)((g_cfg.route_ch[i][track] - 1) & 15);
        if (on[i][track] && (e.type == TN_ON || e.type == TN_OFF || e.type == TN_ALL_OFF)) {
            uint8_t off[3] = {(uint8_t)(0x80 | ch), held[i][track], 0};
            midi_io_write(off, 3);
            on[i][track] = false;
        }
        if (e.type == TN_ON) {
            int note = (e.cents + 50) / 100;
            if (note < 0) note = 0;
            if (note > 127) note = 127;
            uint8_t msg[3] = {(uint8_t)(0x90 | ch), (uint8_t)note, e.vel ? e.vel : 100};
            midi_io_write(msg, 3);
            held[i][track] = (uint8_t)note;
            on[i][track] = true;
        }
        return true;
    }
    if (d < 0 || d >= g_nmods || !g_enabled[d] || !g_mods[d]->note) return false;
    g_mods[d]->note(&e);
    return true;
}

// --- start ---------------------------------------------------------------------------------------

void studio_run(const tmod_t *const *mods, int n) {
    if (n > STUDIO_MAX_MODULES) n = STUDIO_MAX_MODULES;
    tulip_app_init();       // under the launcher: a reset goes back to it
    tulip_settings_load();  // device-wide touch calibration + brightness (and NVS)
    s_files = tulip_fs_mount(false);
    if (!s_files) ESP_LOGW(TAG, "no /files: app data can't be saved, no apps loaded");
    // one app on its own, or the Studio (the launcher's choice)
    char mode[24];
    tulip_boot_mode_get(mode, sizeof mode);
    // the built-in apps, then any loaded from /files/studio/apps (only the
    // one asked for, when it's one of those)
    static const tmod_t *all[STUDIO_MAX_MODULES];
    int na = 0;
    for (int i = 0; i < n; i++) all[na++] = mods[i];
    bool builtin = false;
    for (int i = 0; i < n; i++) builtin |= mode[0] && !strcmp(mode, mods[i]->name);
    if (s_files && !builtin)
        na += loader_load_apps(all + na, STUDIO_MAX_MODULES - na, mods, n, mode[0] ? mode : NULL);
    g_nmods = 0;
    for (int i = 0; i < na; i++)
        if (mode[0] && !strcmp(mode, all[i]->name)) {
            g_mods[g_nmods++] = all[i];
            snprintf(g_single, sizeof g_single, "%s", all[i]->name);
        }
    if (!g_nmods)
        for (int i = 0; i < na; i++) g_mods[g_nmods++] = all[i];
    if (g_nmods == 1 && !g_single[0]) snprintf(g_single, sizeof g_single, "%s", g_mods[0]->name);
    n = g_nmods;

    cfg_load();

    // on its own, the app starts now; in the Studio, every app is off
    // until it's switched on
    if (n == 1) {
        g_enabled[0] = s_on[0] = true;
        loader_guard_begin(g_mods[0]);
        if (g_mods[0]->init) g_mods[0]->init();
        loader_guard_end(g_mods[0]);
        g_started[0] = true;   // rt_init() runs as the rt task starts
    }
    rt_start();
    while (!rt_ready()) vTaskDelay(pdMS_TO_TICKS(5));
    audio_start();
    // core 0: panel + LVGL + every app's UI
    tulip_display_start(&(tulip_display_cfg_t){
        .core = 0, .priority = 4, .ui_create = shell_ui_create, .ui_poll = shell_ui_poll});
    if (n == 1)
        ESP_LOGI(TAG, "%s on its own", g_mods[0]->title);
    else
        ESP_LOGI(TAG, "the Studio: %d apps, all off", n);
}

// UI task. Starting an app: its init() here, then rt_init() and the
// tempo in the rt task (HC_START), then it's switched on - the queue keeps
// that order, so no hook of it runs before it's ready.
// the UI must not lose these: the rt queue is drained every ~1 ms
static void post_sure(uint8_t kind, int i, int32_t arg) {
    for (int k = 0; k < 50 && !rt_post(kind, (uint8_t)i, 0, arg); k++) vTaskDelay(1);
}

void studio_module_set_on(int i, bool on) {
    if (i < 0 || i >= g_nmods || s_on[i] == on) return;
    s_on[i] = on;
    if (on && !g_started[i]) {
        g_started[i] = true;   // (UI side: init() never runs twice)
        loader_guard_begin(g_mods[i]);
        if (g_mods[i]->init) g_mods[i]->init();
        loader_guard_end(g_mods[i]);
        post_sure(HC_START, i, 0);
    }
    post_sure(HC_ENABLE, i, on);
}
