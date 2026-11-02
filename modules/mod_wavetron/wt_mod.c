// wt_mod.c - Wavetron as a Studio module: notes in (MIDI IN on its channel,
// or a routed sequencer track), audio out, its files, and the hand-over of
// the sound and the wavetables from the UI task to the audio task.
//
// The audio task never waits: the UI publishes a new copy of the sound and
// the two table pointers under a short critical section, and the audio
// task takes it at the top of its next block. A table that leaves RAM is
// only freed once the audio task has moved past every block that could
// still be reading it.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_wavetron.h"
#include "wt_app.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "wavetron";
#define LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#endif

wt_patch_t g_wt;
wt_catalog_t g_cat;
int g_ia, g_ib = 1;
char g_patch_name[WT_PATCH_NAME_MAX + 1];
cJSON *g_lib;
char wt_last_msg[48];

static wt_engine_t s_eng;
static QueueHandle_t s_q, s_ccq;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    wt_sound_t s;
    wt_wave_t a, b;
} s_pub;
static volatile uint32_t s_ver, s_seen;
static volatile int s_load;
static bool s_dirty;
static int64_t s_dirty_at;

enum { TN_BEND = 20, TN_PANIC };

// --- memory ---------------------------------------------------------------------------------

#ifdef ESP_PLATFORM
static void *psram_alloc(unsigned n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

// the reverb and chorus lines are read every sample: internal RAM (about
// 30 KB), so the audio task leaves the PSRAM bus to the panel
static void *fast_alloc(unsigned n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : psram_alloc(n);
}
#endif

#define NRETIRE 8
static struct {
    void *p;
    uint32_t ver;
} s_retire[NRETIRE];

// A table leaving RAM: free it once the audio task has taken the next
// published version (wt_mod_poll)
static void retire(void *p) {
    for (int i = 0; i < NRETIRE; i++)
        if (!s_retire[i].p) {
            s_retire[i].p = p;
            s_retire[i].ver = s_ver + 1;
            return;
        }
    LOGW("retire list full: a table is leaked");
}

static void retire_poll(void) {
    for (int i = 0; i < NRETIRE; i++)
        if (s_retire[i].p && (int32_t)(s_seen - s_retire[i].ver) >= 0) {
            wt_free(s_retire[i].p);
            s_retire[i].p = NULL;
        }
}

// --- publishing (UI task) -----------------------------------------------------------------------

static void publish(void) {
    taskENTER_CRITICAL(&s_mux);
    s_pub.s = g_wt.s;
    s_pub.a = g_cat.t[g_ia].w;
    s_pub.b = g_cat.t[g_ib].w;
    s_ver++;
    taskEXIT_CRITICAL(&s_mux);
}

static void mark_dirty(void) {
    s_dirty = true;
    s_dirty_at = studio_now_us();
}

void wt_changed(void) {
    publish();
    mark_dirty();
}

static void post(uint8_t type, int32_t arg) {
    tnote_t n = {.type = type, .src = 0xFF, .cents = arg};
    if (s_q) xQueueSend(s_q, &n, 0);
}

void wt_replaced(void) {
    publish();
    post(TN_ALL_OFF, 0);
    mark_dirty();
}

void wt_panic_now(void) { post(TN_PANIC, 0); }

// --- files ---------------------------------------------------------------------------------------------

const char *wt_dir(void) {
    static char d[48];
    const char *root = studio_files_root();
    if (!root) return NULL;
    if (!d[0]) {
        snprintf(d, sizeof d, "%s/wavetron", root);
        mkdir(d, 0777);
    }
    return d;
}

const char *wt_tables_dir(void) {
    static char d[64];
    if (!wt_dir()) return NULL;
    if (!d[0]) {
        snprintf(d, sizeof d, "%s/wavetables", wt_dir());
        mkdir(d, 0777);
    }
    return d;
}

static void lib_path(char *out, int n) { snprintf(out, n, "%s/wt_patches.json", wt_dir() ? wt_dir() : "/nonexistent"); }
static void state_path(char *out, int n) { snprintf(out, n, "%s/state.json", wt_dir() ? wt_dir() : "/nonexistent"); }

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// A patch names a table by path. A sketch patch carries its AMYboard path
// (/sd/...): look for the same file name in the wavetables folder, then
// at the top of /files.
static bool resolve(const char *path, char *out, int n) {
    if (exists(path)) {
        snprintf(out, n, "%s", path);
        return true;
    }
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (wt_tables_dir()) {
        snprintf(out, n, "%s/%s", wt_tables_dir(), b);
        if (exists(out)) return true;
    }
    if (studio_files_root()) {
        snprintf(out, n, "%s/%s", studio_files_root(), b);
        if (exists(out)) return true;
    }
    return false;
}

// the catalogue entry for a patch's table reference; -1 if it's gone
static int table_for(const char *path, int builtin, int keep) {
    if (!path[0]) return builtin >= 0 && builtin < WT_NBUILTIN ? builtin : 0;
    char real[160], err[40];
    if (!resolve(path, real, sizeof real)) return -1;
    return wt_cat_load(&g_cat, real, keep, keep, err, sizeof err);
}

static void ref_set(int osc, int idx) {
    char *path = osc ? g_wt.b_path : g_wt.a_path;
    int *bi = osc ? &g_wt.b_builtin : &g_wt.a_builtin;
    if (idx < WT_NBUILTIN) {
        path[0] = 0;
        *bi = idx;
    } else {
        snprintf(path, sizeof g_wt.a_path, "%s", g_cat.t[idx].path);
        *bi = 0;
    }
}

bool wt_select_table(int osc, int idx) {
    if (idx < 0 || idx >= g_cat.n) return false;
    if (osc)
        g_ib = idx;
    else
        g_ia = idx;
    wt_cat_touch(&g_cat, idx);
    ref_set(osc, idx);
    wt_changed();
    return true;
}

bool wt_load_file(int osc, const char *path, char *err, int errlen) {
    // the other oscillator's table must stay; this one's may go
    int keep = osc ? g_ia : g_ib;
    int idx = wt_cat_load(&g_cat, path, keep, keep, err, errlen);
    if (idx < 0) return false;
    // the slot reused may have been this oscillator's own table: point both
    // at valid entries before publishing
    if (g_ia >= g_cat.n) g_ia = 0;
    if (g_ib >= g_cat.n) g_ib = 1;
    return wt_select_table(osc, idx);
}

// apply a patch record to the current sound (keeps the MIDI channel: that
// belongs to the setup, not the sound)
static void apply_rec(const cJSON *rec, bool *missing) {
    float mch = g_wt.s.p[WP_MCH];
    wt_patch_t p;
    wt_patch_from_json(rec, &p);
    p.s.p[WP_MCH] = mch;
    *missing = false;
    int ia = table_for(p.a_path, p.a_builtin, -1);
    int ib = table_for(p.b_path, p.b_builtin, ia);
    if (ia < 0) ia = 0, *missing = true, p.a_path[0] = 0, p.a_builtin = 0;
    if (ib < 0) ib = 1, *missing = true, p.b_path[0] = 0, p.b_builtin = 1;
    g_wt = p;
    g_ia = ia;
    g_ib = ib;
    ref_set(0, ia);
    ref_set(1, ib);
}

static void save_state(void) {
    if (!wt_dir()) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "patch", g_patch_name);
    cJSON_AddItemToObject(o, "rec", wt_patch_to_json(&g_wt));
    char path[96];
    state_path(path, sizeof path);
    char *txt = cJSON_PrintUnformatted(o);
    FILE *f = txt ? fopen(path, "w") : NULL;
    if (f) {
        fputs(txt, f);
        fclose(f);
    }
    cJSON_free(txt);
    cJSON_Delete(o);
}

static void load_state(void) {
    char path[96];
    state_path(path, sizeof path);
    cJSON *lib = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        static char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        lib = cJSON_Parse(buf);
    }
    if (!lib) return;
    const cJSON *rec = cJSON_GetObjectItem(lib, "rec");
    const cJSON *nm = cJSON_GetObjectItem(lib, "patch");
    if (cJSON_IsObject(rec)) {
        // the channel is part of the saved state (not of a patch)
        const cJSON *p = cJSON_GetObjectItem(rec, "p");
        const cJSON *ch = cJSON_GetObjectItem(p, "mch");
        if (cJSON_IsNumber(ch)) g_wt.s.p[WP_MCH] = wt_param_fix(WP_MCH, (float)ch->valuedouble);
        bool missing;
        apply_rec(rec, &missing);
    }
    if (cJSON_IsString(nm)) snprintf(g_patch_name, sizeof g_patch_name, "%s", nm->valuestring);
    cJSON_Delete(lib);
}

// --- patches --------------------------------------------------------------------------------------------

static bool lib_write(void) {
    char path[96];
    lib_path(path, sizeof path);
    return wt_dir() && wt_lib_write(path, g_lib);
}

bool wt_patch_save(const char *name) {
    if (!wt_lib_put(g_lib, name, &g_wt)) {
        snprintf(wt_last_msg, sizeof wt_last_msg, wt_lib_count(g_lib) >= WT_MAX_PATCHES ? "library full (24)" : "give it a name");
        return false;
    }
    int i = wt_lib_find(g_lib, name);
    snprintf(g_patch_name, sizeof g_patch_name, "%s", i >= 0 ? wt_lib_name(g_lib, i) : name);
    if (!lib_write()) {
        snprintf(wt_last_msg, sizeof wt_last_msg, "could not write the file");
        return false;
    }
    snprintf(wt_last_msg, sizeof wt_last_msg, "saved %s", g_patch_name);
    mark_dirty();
    return true;
}

bool wt_patch_load(int i) {
    const cJSON *rec = wt_lib_rec(g_lib, i);
    if (!rec) return false;
    bool missing;
    apply_rec(rec, &missing);
    snprintf(g_patch_name, sizeof g_patch_name, "%s", wt_lib_name(g_lib, i));
    snprintf(wt_last_msg, sizeof wt_last_msg, missing ? "%s: a wavetable is missing" : "%s", g_patch_name);
    wt_replaced();
    return !missing;
}

bool wt_patch_delete(int i) {
    if (i < 0 || i >= wt_lib_count(g_lib)) return false;
    if (!strcmp(wt_lib_name(g_lib, i), g_patch_name)) g_patch_name[0] = 0;
    wt_lib_delete(g_lib, i);
    mark_dirty();
    return lib_write();
}

bool wt_patch_rename(int i, const char *name) {
    bool cur = i >= 0 && i < wt_lib_count(g_lib) && !strcmp(wt_lib_name(g_lib, i), g_patch_name);
    if (!wt_lib_rename(g_lib, i, name)) {
        snprintf(wt_last_msg, sizeof wt_last_msg, "name taken or empty");
        return false;
    }
    if (cur) snprintf(g_patch_name, sizeof g_patch_name, "%s", wt_lib_name(g_lib, i));
    mark_dirty();
    return lib_write();
}

void wt_patch_init(void) {
    float mch = g_wt.s.p[WP_MCH];
    memset(&g_wt, 0, sizeof g_wt);
    wt_sound_default(&g_wt.s);
    g_wt.s.p[WP_MCH] = mch;
    g_wt.b_builtin = 1;
    g_ia = 0;
    g_ib = 1;
    g_patch_name[0] = 0;
    wt_replaced();
}

// --- live state -----------------------------------------------------------------------------------------

void wt_live(wt_live_t *o) {
    int last = -1;
    o->sounding = false;
    for (int v = 0; v < WT_NV; v++) {
        const wt_voice_t *vo = &s_eng.v[v];
        o->active[v] = vo->active;
        o->gate[v] = vo->gate;
        o->note[v] = vo->target;
        if (vo->active) {
            o->sounding = true;
            if (last < 0 || vo->age > s_eng.v[last].age || (vo->gate && !s_eng.v[last].gate)) last = v;
        }
    }
    o->pos_a = last >= 0 ? s_eng.v[last].pos_a : -1;
    o->pos_b = last >= 0 ? s_eng.v[last].pos_b : -1;
}

int wt_scope_read(float *out, int n) {
    if (n > WT_SCOPE) n = WT_SCOPE;
    uint32_t w = s_eng.scope_w;
    for (int i = 0; i < n; i++) out[i] = s_eng.scope[(w - n + i) % WT_SCOPE];
    return n;
}

int wt_load_pct(void) { return s_load; }

bool wt_midi_next(wt_cc_t *m) { return s_ccq && xQueueReceive(s_ccq, m, 0) == pdTRUE; }

// --- notes and MIDI ---------------------------------------------------------------------------------------

static void note_in(const tnote_t *n) {
    if (s_q) xQueueSend(s_q, n, 0);
}

static void midi_in(const uint8_t *m, int len, int64_t t) {
    int ch = (m[0] & 15) + 1;
    if (ch != (int)g_wt.s.p[WP_MCH] || len < 2) return;
    uint8_t st = m[0] & 0xF0;
    tnote_t n = {.src = (uint8_t)(0x80 | (ch - 1)), .key = m[1], .cents = m[1] * 100, .t = t};
    if (st == 0x90 && len == 3 && m[2]) {
        n.type = TN_ON;
        n.vel = m[2];
        note_in(&n);
    } else if ((st == 0x80 || st == 0x90) && len == 3) {
        n.type = TN_OFF;
        note_in(&n);
    } else if (st == 0xE0 && len == 3) {
        n.type = TN_BEND;
        n.cents = ((m[2] << 7) | m[1]) - 8192;
        note_in(&n);
    } else if (st == 0xB0 && len == 3) {
        if (m[1] == 123 || m[1] == 120) {
            n.type = TN_ALL_OFF;
            note_in(&n);
        } else if (s_ccq) {
            wt_cc_t c = {m[1], m[2]};
            xQueueSend(s_ccq, &c, 0);
        }
    }
}

// --- audio --------------------------------------------------------------------------------------------------

static void render(float *l, float *r, const studio_block_t *b) {
    int64_t c0 = studio_now_us();
    if (s_ver != s_seen) {
        taskENTER_CRITICAL(&s_mux);
        uint32_t v = s_ver;
        wt_set_sound(&s_eng, &s_pub.s);
        wt_set_tables(&s_eng, &s_pub.a, &s_pub.b);
        taskEXIT_CRITICAL(&s_mux);
        s_seen = v;
    }
    s_eng.bpm = studio_bpm_x100() / 100.0f;
    tnote_t n;
    while (xQueueReceive(s_q, &n, 0) == pdTRUE) {
        switch (n.type) {
        case TN_ON: wt_note_on(&s_eng, n.src, n.key, n.cents / 100.0f, n.vel / 127.0f, n.glide_us); break;
        case TN_OFF: wt_note_off(&s_eng, n.src, n.key); break;
        case TN_ALL_OFF: wt_all_off(&s_eng); break;
        case TN_BEND: s_eng.bend = n.cents / 8192.0f; break;
        case TN_PANIC: wt_panic(&s_eng); break;
        }
    }
    wt_render(&s_eng, l, r, b->n);
    int64_t c1 = studio_now_us();
    static float load;
    load += ((float)(c1 - c0) * (100.0f * STUDIO_SR / 1e6f / STUDIO_BLOCK) - load) * 0.02f;
    s_load = (int)(load + 0.5f);
}

// closed in Studio: let go of everything
static void enable(bool on, int64_t now) {
    if (!on) post(TN_ALL_OFF, 0);
}

// --- module ---------------------------------------------------------------------------------------------------

static void init(void) {
#ifdef ESP_PLATFORM
    wt_alloc = psram_alloc;
    wt_alloc_fast = fast_alloc;
    wt_free = heap_caps_free;
#endif
    s_q = xQueueCreate(64, sizeof(tnote_t));
    s_ccq = xQueueCreate(32, sizeof(wt_cc_t));
    g_cat.retire = retire;
    if (!wt_cat_init(&g_cat) || !wt_init(&s_eng)) LOGW("out of memory");
    wt_sound_default(&g_wt.s);
    g_wt.b_builtin = 1;
    if (wt_dir()) {
        wt_tables_dir();
        load_state();
        char path[96];
        lib_path(path, sizeof path);
        g_lib = wt_lib_read(path);
    } else {
        g_lib = cJSON_CreateArray();
    }
    publish();
    s_seen = s_ver - 1;   // the audio task takes it on its first block
    LOGI("%d tables, %d patches", g_cat.n, wt_lib_count(g_lib));
}

static void ui_poll(bool visible) {
    retire_poll();
    if (s_dirty && studio_now_us() - s_dirty_at > 1500000) {
        s_dirty = false;
        save_state();
    }
    wt_ui_poll(visible);
}

static bool busy(void) { return s_dirty; }

// --- Studio patches: the sound (tables by path), its name, the MIDI channel --------------------

static cJSON *patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "patch", g_patch_name);
    cJSON_AddNumberToObject(o, "midi_ch", (int)g_wt.s.p[WP_MCH]);
    cJSON_AddItemToObject(o, "rec", wt_patch_to_json(&g_wt));
    return o;
}

static void patch_load(const cJSON *o) {
    const cJSON *rec = cJSON_GetObjectItem(o, "rec");
    if (!cJSON_IsObject(rec)) return;
    const cJSON *ch = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(ch)) g_wt.s.p[WP_MCH] = wt_param_fix(WP_MCH, (float)ch->valuedouble);
    bool missing;
    apply_rec(rec, &missing);
    const cJSON *nm = cJSON_GetObjectItem(o, "patch");
    snprintf(g_patch_name, sizeof g_patch_name, "%s", cJSON_IsString(nm) ? nm->valuestring : "");
    if (missing) snprintf(wt_last_msg, sizeof wt_last_msg, "a wavetable is missing");
    wt_replaced();
    mark_dirty();
    wt_ui_reload();
}

static void midi_desc(char *b, int n) {
    snprintf(b, n, "notes on ch %d, bend; CC1 position, CC74 cutoff, CC71 reso, CC76 detune",
             (int)g_wt.s.p[WP_MCH]);
}

tmod_t mod_wavetron = {
    .name = "wavetron",
    .title = "WAVETRON",
    .blurb = "Dual wavetable synth: 4 voices, unison, a mod matrix, effects",
    .role = TMOD_SYNTH,
    .flags = TMOD_F_AUDIO | TMOD_F_NOTES_IN,
    .init = init,
    .rt_enable = enable,
    .render = render,
    .note = note_in,
    .midi = midi_in,
    .midi_desc = midi_desc,
    .ui_create = wt_ui_create,
    .ui_poll = ui_poll,
    .ui_show = wt_ui_show,
    .busy = busy,
    .patch_save = patch_save,
    .patch_load = patch_load,
};
