// d9_mod.c - Drumster9 as a Studio module: the samples, the audio on the
// master clock, MIDI notes in, the CV clock (on its own), files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "core/d9_dsp.h"
#include "core/d9_seq.h"
#include "d9_app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_drumster9.h"
#include "studio/studio_hw.h"
#include "ui/d9_ui.h"

static const char *TAG = "drumster9";

d9_state_t g_d9;
d9_index_t *g_d9ix_p;   // allocated at init, before anything reads it
volatile int8_t g_d9_next = -1;
d9_status_t g_d9s = {.step = -1};
d9_settings_t g_d9set = {.midi_ch = 11, .cv_on = true, .cv_div = 2, .cv_vout1 = CV_RESET};
volatile uint32_t g_d9_reload;
char g_d9_proj_cur[D9_PROJECT_NAME + 1];

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
void d9_lock(void) { taskENTER_CRITICAL(&s_mux); }
void d9_unlock(void) { taskEXIT_CRITICAL(&s_mux); }

// --- samples: flash partition -> PSRAM, on demand ----------------------------------------
//
// Only what's in use is in RAM: each kit's 7 samples are about 45-380 KB.
// A sample no kit uses any more, and not played for longer than the
// longest sample lasts, can be let go when the cache is over its budget -
// so the audio task can never be left playing freed memory.

#define CACHE_BUDGET (2 * 1024 * 1024)
#define CACHE_KEEP_US 6000000   // the longest sample is 5.2 s

typedef struct {
    const int16_t *volatile pcm;
    uint32_t len;
    volatile int64_t last_us;   // last played
} cache_t;

static const esp_partition_t *s_part;
static cache_t *s_cache;   // [D9_MAX_SAMPLES], in PSRAM
static int16_t s_by_preset[512];   // preset -> index in g_d9ix, -1
static uint32_t s_cache_bytes;

static int sample_index(int preset) {
    return preset >= 0 && preset < 512 ? s_by_preset[preset] : -1;
}

const char *d9_name(int preset) {
    static char t[24];
    return d9_sample_name(&g_d9ix, preset, t, sizeof t);
}

static bool in_use(int preset) {
    for (int k = 0; k < D9_KITS; k++)
        for (int l = 0; l < D9_LANES; l++)
            if (g_d9.kit_map[k][l] == preset) return true;
    return false;
}

static void cache_trim(uint32_t need) {
    if (!s_cache) return;
    int64_t now = studio_now_us();
    for (int i = 0; i < g_d9ix.nsamples && s_cache_bytes + need > CACHE_BUDGET; i++) {
        cache_t *c = &s_cache[i];
        if (!c->pcm || in_use(g_d9ix.samples[i].preset) || now - c->last_us < CACHE_KEEP_US) continue;
        const int16_t *p = c->pcm;
        c->pcm = NULL;
        s_cache_bytes -= c->len * 2;
        heap_caps_free((void *)p);
    }
}

bool d9_ensure_sample(int preset) {
    int i = sample_index(preset);
    if (i < 0 || !s_part || !s_cache) return false;
    cache_t *c = &s_cache[i];
    if (c->pcm) return true;
    const d9_sample_t *s = &g_d9ix.samples[i];
    uint32_t bytes = s->frames * 2;
    cache_trim(bytes);
    int16_t *p = heap_caps_malloc(bytes ? bytes : 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "no room for %s (%u bytes)", s->name, (unsigned)bytes);
        return false;
    }
    if (esp_partition_read(s_part, s->offset, p, bytes) != ESP_OK) {
        heap_caps_free(p);
        return false;
    }
    c->len = s->frames;
    c->last_us = studio_now_us();
    __atomic_store_n(&c->pcm, p, __ATOMIC_RELEASE);   // len first, then the pointer
    s_cache_bytes += bytes;
    return true;
}

void d9_ensure_lane_samples(void) {
    bool kits[D9_KITS] = {0};
    kits[g_d9.kit] = true;
    for (int l = 0; l < D9_LANES; l++) kits[d9_lane_kit(&g_d9, l)] = true;
    int nx = g_d9_next;
    if (nx >= 0 && g_d9.bank[nx].used) kits[g_d9.bank[nx].kit] = true;
    for (int k = 0; k < D9_KITS; k++)
        if (kits[k])
            for (int l = 0; l < D9_LANES; l++) d9_ensure_sample(g_d9.kit_map[k][l]);
}

// audio task: a preset's samples, or NULL if it isn't in RAM
static const int16_t *pcm_for(int preset, uint32_t *len, int64_t now) {
    int i = sample_index(preset);
    if (i < 0 || !s_cache) return NULL;
    cache_t *c = &s_cache[i];
    const int16_t *p = __atomic_load_n(&c->pcm, __ATOMIC_ACQUIRE);
    if (!p) return NULL;
    *len = c->len;
    c->last_us = now;
    return p;
}

static bool samples_open(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "samples");
    memset(s_by_preset, 0xff, sizeof s_by_preset);
    if (!s_part) {
        ESP_LOGW(TAG, "no samples partition: flash tulip-full.bin");
        return false;
    }
    uint8_t h[32];
    uint32_t n = 0;
    if (esp_partition_read(s_part, 0, h, sizeof h) == ESP_OK) n = d9_index_bytes(h);
    uint8_t *buf = n ? malloc(n) : NULL;
    bool ok = buf && esp_partition_read(s_part, 0, buf, n) == ESP_OK && d9_index_parse(&g_d9ix, buf, n);
    free(buf);
    if (!ok || g_d9ix.total > s_part->size) {
        ESP_LOGW(TAG, "the samples partition holds no samples: flash tulip-full.bin");
        s_part = NULL;
        memset(&g_d9ix, 0, sizeof g_d9ix);
        return false;
    }
    for (int i = 0; i < g_d9ix.nsamples; i++)
        if (g_d9ix.samples[i].preset < 512) s_by_preset[g_d9ix.samples[i].preset] = (int16_t)i;
    ESP_LOGI(TAG, "%d samples in %d banks", g_d9ix.nsamples, g_d9ix.nbanks);
    return true;
}

// --- audio -----------------------------------------------------------------------------------------

typedef struct {
    uint8_t lane;
    int16_t preset;   // the audition voice's sample
    float vel;
} trig_t;

static QueueHandle_t s_trigq;
static d9_engine_t s_eng;
static d9_seq_t s_seq;
static uint32_t s_epoch_seen;
static bool s_was_running;
static volatile bool s_reseq;   // closed in Studio: pick up where the clock is when reopened

#define BASE_GAIN 0.5f

void d9_trigger(int lane, float vel) {
    if (lane < 0 || lane >= D9_LANES || !s_trigq) return;
    trig_t t = {(uint8_t)lane, 0, vel};
    xQueueSend(s_trigq, &t, 0);
}

void d9_audition(int preset) {
    if (!d9_ensure_sample(preset) || !s_trigq) return;
    trig_t t = {D9_AUDITION, (int16_t)preset, 1.0f};
    xQueueSend(s_trigq, &t, 0);
}

static void render(float *l, float *r, const studio_block_t *b) {
    int64_t c0 = studio_now_us();
    tclock_tl_t tl;
    studio_timeline(&tl);
    int64_t t_end = studio_time_at(b, b->n);
    d9_hit_t hits[48];
    d9_ev_t ev[64];
    d9_mix_t m;
    int nev = 0;
    uint32_t sw = 0;

    if (s_reseq) {
        s_reseq = false;
        d9_seq_init(&s_seq);
    }
    d9_lock();
    int nh = d9_seq_run(&s_seq, &g_d9, &tl, tclock_tl_pos(&tl, t_end - 1), &g_d9_next, &sw, hits, 48);
    int step = d9_seq_step_at(&s_seq, tclock_tl_pos(&tl, b->t0));
    float master = g_d9.master / D9_MASTER_DEF * BASE_GAIN;
    for (int L = 0; L < D9_LANES; L++) {
        int bus = g_d9.bus[L];
        m.lane_bus[L] = (uint8_t)bus;
        m.lane_gain[L] = d9_audible(&g_d9, L) ? g_d9.vol[L] * g_d9.fx[bus].drive * master : 0.0f;
    }
    m.lane_bus[D9_AUDITION] = 0;
    m.lane_gain[D9_AUDITION] = master;
    memcpy(m.bus, g_d9.fx, sizeof m.bus);
    int16_t preset[48];
    for (int i = 0; i < nh; i++) preset[i] = (int16_t)d9_lane_preset(&g_d9, hits[i].lane);
    d9_unlock();

    m.stop = s_was_running && !tl.running;
    s_was_running = tl.running;
    if (sw) g_d9s.swaps += sw;
    // live hits first, at frame 0
    trig_t t;
    while (nev < 8 && xQueueReceive(s_trigq, &t, 0) == pdTRUE) {
        int p = t.lane == D9_AUDITION ? t.preset : d9_lane_preset(&g_d9, t.lane);
        uint32_t len = 0;
        const int16_t *pcm = pcm_for(p, &len, c0);
        if (!pcm) continue;
        ev[nev++] = (d9_ev_t){0, t.lane, t.vel, pcm, len};
        if (t.lane < D9_LANES) g_d9s.hit_ms[t.lane] = (uint32_t)(c0 / 1000);
    }
    for (int i = 0; i < nh && nev < 64; i++) {
        uint32_t len = 0;
        const int16_t *pcm = pcm_for(preset[i], &len, c0);
        if (!pcm) continue;
        int64_t tg = tclock_tl_time(&tl, hits[i].pos);
        int fr = studio_frame_at(b, tg);
        if (fr < 0) fr = 0;   // late (the first beat after a MIDI Start)
        if (fr >= b->n) fr = b->n - 1;
        ev[nev++] = (d9_ev_t){fr, hits[i].lane, 1.0f, pcm, len};
        g_d9s.hit_ms[hits[i].lane] = (uint32_t)(tg / 1000);
    }
    d9_engine_render(&s_eng, &m, ev, nev, l, r, b->n);
    if (s_eng.peak > g_d9s.peak) g_d9s.peak = s_eng.peak;
    s_eng.peak = 0;
    g_d9s.playing = tl.running;
    g_d9s.step = (int8_t)(s_seq.active ? step : -1);
    int64_t c1 = studio_now_us();
    static float load;
    load += ((float)(c1 - c0) * (100.0f * STUDIO_SR / 1e6f / STUDIO_BLOCK) - load) * 0.02f;
    g_d9s.load_pct = (int)(load + 0.5f);
}

static void enable(bool on, int64_t now) {
    (void)now;
    if (!on) {
        // closed in Studio: let go of everything (the next block fades it),
        // and don't play the steps missed meanwhile when it's opened again
        s_was_running = true;
        s_reseq = true;
    }
}

// --- MIDI: notes on our channel play the lanes (GM drum notes) ----------------------------

static void midi_in(const uint8_t *m, int len, int64_t t) {
    (void)t;
    int ch = (m[0] & 15) + 1;
    int8_t want = g_d9set.midi_ch;
    if (want < 0 || (want && want != ch) || len < 3) return;
    if ((m[0] & 0xF0) != 0x90 || !m[2]) return;
    for (int l = 0; l < D9_LANES; l++)
        if (m[1] == D9_GM_NOTE[l]) d9_trigger(l, m[2] / 127.0f);
}

static void midi_desc(char *b, int n) {
    char ch[8];
    if (g_d9set.midi_ch < 0) {
        snprintf(b, n, "off");
        return;
    }
    if (g_d9set.midi_ch == 0)
        snprintf(ch, sizeof ch, "omni");
    else
        snprintf(ch, sizeof ch, "ch %d", g_d9set.midi_ch);
    snprintf(b, n, "%s: GM drum notes play the lanes (36 kick, 38 snare, 42 hat ...)", ch);
}

// --- the CV clock (rt task) -------------------------------------------------------------
//
// As the Python did it, on the Mabee DAC at 0x59: VOUT0 a square-wave clock
// locked to the step grid (both edges on 1/32 steps), VOUT1 a one-step reset
// pulse on each downbeat (or high while running). Only when no other app in
// the image drives the DACs (in Studio, metroseq owns them).

#define CV_ADDR 0x59
#define CV_HZ 400000
#define CV_HIGH 0x8000   // 5 V of the 0-10 V range
enum { CV_CMD_APPLY = 1 };

static bool s_cv_avail;
static i2c_master_dev_handle_t s_cv_dev;
static int8_t s_cv_lvl[2] = {-1, -1};
static uint8_t s_cv_fails;
static int s_ramp = -1;
static int64_t s_ramp_next = INT64_MAX, s_cv_init_at = INT64_MAX;

bool d9_cv_available(void) { return s_cv_avail; }

static bool cv_write(int ch, uint16_t code) {
    if (!s_cv_dev) return false;
    uint8_t b[3] = {(uint8_t)(ch ? 0x04 : 0x02), (uint8_t)(code & 0xff), (uint8_t)(code >> 8)};
    if (i2c_master_transmit(s_cv_dev, b, 3, 5) == ESP_OK) {
        s_cv_fails = 0;
        return true;
    }
    if (++s_cv_fails >= 5) {
        ESP_LOGW(TAG, "CV DAC stopped answering: clock off");
        g_d9s.cv_present = false;
    }
    return false;
}

static void cv_level(int ch, bool high) {
    if (!g_d9s.cv_present || s_cv_lvl[ch] == high) return;
    if (cv_write(ch, high ? CV_HIGH : 0)) s_cv_lvl[ch] = high;
}

static void cv_bringup(void) {
    if (!s_cv_dev) {
        i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = CV_ADDR,
                                   .scl_speed_hz = CV_HZ};
        if (i2c_master_bus_add_device(studio_i2c_bus(), &cfg, &s_cv_dev) != ESP_OK) return;
    }
    uint8_t range[2] = {0x01, 0x11};   // 0-10 V
    i2c_master_transmit(s_cv_dev, range, 2, 5);
    g_d9s.cv_present = cv_write(0, 0) && cv_write(1, 0);
    s_cv_lvl[0] = s_cv_lvl[1] = 0;
    if (g_d9s.cv_present) {
        s_ramp = 0;   // ease VOUT0 up and down once before any clock
        s_ramp_next = studio_now_us();
        ESP_LOGI(TAG, "CV clock on the DAC at 0x%02x", CV_ADDR);
    }
}

static void m_rt_init(void) {
    if (s_cv_avail) s_cv_init_at = studio_now_us() + 1000000;
}

// the next step boundary after now, while the transport runs
static int64_t cv_next_edge(int64_t now) {
    tclock_tl_t tl;
    studio_timeline(&tl);
    if (!tl.running || !g_d9s.cv_present || s_ramp >= 0 || !g_d9set.cv_on) return INT64_MAX;
    int64_t pos = tclock_tl_pos(&tl, now);
    int64_t nxt = (pos < 0 ? 0 : (pos / D9_STEP_UNITS + 1)) * D9_STEP_UNITS;
    return tclock_tl_time(&tl, nxt);
}

static int64_t m_next_due(void) {
    int64_t due = cv_next_edge(studio_now_us());
    if (s_ramp_next < due) due = s_ramp_next;
    if (s_cv_init_at < due) due = s_cv_init_at;
    return due;
}

static void m_advance(int64_t now) {
    if (now >= s_cv_init_at) {
        s_cv_init_at = INT64_MAX;
        if (g_d9set.cv_on) cv_bringup();
    }
    if (s_ramp >= 0 && now >= s_ramp_next) {
        if (s_ramp > 32) {
            cv_write(0, 0);
            s_cv_lvl[0] = 0;
            s_ramp = -1;
            s_ramp_next = INT64_MAX;
        } else {
            int i = s_ramp <= 16 ? s_ramp : 32 - s_ramp;
            cv_write(0, (uint16_t)(CV_HIGH * i / 16));
            s_cv_lvl[0] = -1;
            s_ramp++;
            s_ramp_next += 40000;
        }
        return;
    }
    tclock_tl_t tl;
    studio_timeline(&tl);
    if (!g_d9s.cv_present || !g_d9set.cv_on) return;
    if (!tl.running) {
        cv_level(0, false);
        cv_level(1, false);
        return;
    }
    int64_t pos = tclock_tl_pos(&tl, now + TC_LOOKAHEAD_US);
    if (pos < 0) return;
    int step = (int)((pos / D9_STEP_UNITS) % D9_STEPS);
    int div = g_d9set.cv_div >= 2 ? g_d9set.cv_div : 2;
    cv_level(0, (step % div) < div / 2);
    cv_level(1, g_d9set.cv_vout1 == CV_RUN ? true : step == 0);
}

static void m_poll(int64_t now) {
    (void)now;
    // stopped: everything low, at once
    tclock_tl_t tl;
    studio_timeline(&tl);
    if (tl.epoch != s_epoch_seen) {
        s_epoch_seen = tl.epoch;
        if (!tl.running && g_d9s.cv_present) {
            cv_level(0, false);
            cv_level(1, false);
        }
    }
}

static void m_cmd(uint8_t cmd, int32_t arg, int64_t now) {
    (void)arg;
    (void)now;
    if (cmd != CV_CMD_APPLY || !s_cv_avail) return;
    if (g_d9set.cv_on && !g_d9s.cv_present) {
        cv_bringup();
    } else if (!g_d9set.cv_on && g_d9s.cv_present) {
        cv_level(0, false);
        cv_level(1, false);
    }
}

void d9_cv_apply(void) {
    studio_cmd(&mod_drumster9, CV_CMD_APPLY, 0);
    d9_settings_save();
}

// --- files ---------------------------------------------------------------------------------

static bool d9_dir(char *out, int len, const char *name) {
    const char *root = studio_files_root();
    if (!root) return false;
    char d[48];
    snprintf(d, sizeof d, "%s/drumster9", root);
    mkdir(d, 0777);
    snprintf(out, len, "%s/%s", d, name);
    return true;
}

static cJSON *read_json(const char *name) {
    char path[96];
    if (!d9_dir(path, sizeof path, name)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < 1024 * 1024 ? malloc(n + 1) : NULL;
    cJSON *o = NULL;
    if (buf && fread(buf, 1, n, f) == (size_t)n) {
        buf[n] = 0;
        o = cJSON_Parse(buf);
    }
    free(buf);
    fclose(f);
    return o;
}

static bool write_json(const char *name, const cJSON *o) {
    char path[96], tmp[104];
    if (!d9_dir(path, sizeof path, name)) return false;
    snprintf(tmp, sizeof tmp, "%s.part", path);
    char *txt = cJSON_PrintUnformatted(o);
    FILE *f = txt ? fopen(tmp, "w") : NULL;
    bool ok = f && fputs(txt, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    cJSON_free(txt);
    // written in full before it replaces the old one
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}

static float tempo(void) { return studio_bpm_x100() / 100.0f; }

static void settings_load(void) {
    cJSON *o = read_json("settings.json");
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "midi_ch")) && v->valueint >= -1 && v->valueint <= 16)
        g_d9set.midi_ch = (int8_t)v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "cv"))) g_d9set.cv_on = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "cv_div")) &&
        (v->valueint == 2 || v->valueint == 4 || v->valueint == 8))
        g_d9set.cv_div = (uint8_t)v->valueint;
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "cv_vout1")))
        g_d9set.cv_vout1 = strcmp(v->valuestring, "run") ? CV_RESET : CV_RUN;
    cJSON_Delete(o);
}

void d9_settings_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "midi_ch", g_d9set.midi_ch);
    cJSON_AddBoolToObject(o, "cv", g_d9set.cv_on);
    cJSON_AddNumberToObject(o, "cv_div", g_d9set.cv_div);
    cJSON_AddStringToObject(o, "cv_vout1", g_d9set.cv_vout1 == CV_RUN ? "run" : "reset");
    write_json("settings.json", o);
    cJSON_Delete(o);
}

// the session, saved by itself
static volatile bool s_dirty;
static int64_t s_dirty_at;

void d9_changed(void) {
    s_dirty = true;
    s_dirty_at = studio_now_us();
}

static void state_save(void) {
    s_dirty = false;
    cJSON *o = d9_project_to_json(&g_d9, g_d9_proj_cur, tempo());
    write_json("state.json", o);
    cJSON_Delete(o);
}

// a whole new state (a project, a Studio patch, the saved session)
static void state_apply(const d9_state_t *s) {
    g_d9_next = -1;
    d9_lock();
    g_d9 = *s;
    d9_unlock();
    d9_ensure_lane_samples();
    g_d9_reload++;
}

static void state_load(void) {
    static d9_state_t s;
    cJSON *o = read_json("state.json");
    if (o) {
        float bpm;
        d9_project_from_json(o, &s, &bpm);
        const cJSON *nm = cJSON_GetObjectItem(o, "name");
        if (cJSON_IsString(nm)) snprintf(g_d9_proj_cur, sizeof g_d9_proj_cur, "%s", nm->valuestring);
    } else {
        d9_state_init(&s);   // first start: the sketch's "4 on Floor" on the TR-808
        d9_apply_preset(&s, 0);
    }
    cJSON_Delete(o);
    g_d9 = s;
}

// --- projects --------------------------------------------------------------------------------

#define PROJECTS "tulipdrums_projects.json"
static cJSON *s_lib;

static cJSON *lib(void) {
    if (!s_lib) {
        s_lib = read_json(PROJECTS);
        if (!cJSON_IsArray(s_lib)) {
            cJSON_Delete(s_lib);
            s_lib = cJSON_CreateArray();
        }
    }
    return s_lib;
}

int d9_proj_count(void) { return cJSON_GetArraySize(lib()); }

const char *d9_proj_name(int i) {
    const cJSON *n = cJSON_GetObjectItem(cJSON_GetArrayItem(lib(), i), "name");
    return cJSON_IsString(n) ? n->valuestring : "?";
}

int d9_proj_find(const char *name) {
    for (int i = 0; i < d9_proj_count(); i++)
        if (!strcmp(d9_proj_name(i), name)) return i;
    return -1;
}

bool d9_proj_save(const char *name) {
    char nm[D9_PROJECT_NAME + 1];
    snprintf(nm, sizeof nm, "%s", name);
    cJSON *p = d9_project_to_json(&g_d9, nm, tempo());
    int i = d9_proj_find(nm);
    if (i >= 0) {
        cJSON_ReplaceItemInArray(lib(), i, p);
    } else {
        while (d9_proj_count() >= D9_MAX_PROJECTS) cJSON_DeleteItemFromArray(lib(), 0);   // oldest goes
        cJSON_AddItemToArray(lib(), p);
    }
    snprintf(g_d9_proj_cur, sizeof g_d9_proj_cur, "%s", nm);
    d9_changed();
    return write_json(PROJECTS, lib());
}

bool d9_proj_load(int i) {
    const cJSON *p = cJSON_GetArrayItem(lib(), i);
    if (!cJSON_IsObject(p)) return false;
    static d9_state_t s;
    float bpm;
    d9_project_from_json(p, &s, &bpm);
    if (bpm >= 20 && studio_clock_src() == CLOCK_SRC_INT) studio_set_bpm_x100((int32_t)(bpm * 100 + 0.5f));
    state_apply(&s);
    snprintf(g_d9_proj_cur, sizeof g_d9_proj_cur, "%s", d9_proj_name(i));
    d9_changed();
    return true;
}

bool d9_proj_delete(int i) {
    if (i < 0 || i >= d9_proj_count()) return false;
    if (!strcmp(d9_proj_name(i), g_d9_proj_cur)) g_d9_proj_cur[0] = 0;
    cJSON_DeleteItemFromArray(lib(), i);
    return write_json(PROJECTS, lib());
}

// --- Studio patches: the whole state, plus the MIDI channel ---------------------------------

static cJSON *patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "project", d9_project_to_json(&g_d9, g_d9_proj_cur, tempo()));
    cJSON_AddNumberToObject(o, "midi_ch", g_d9set.midi_ch);
    return o;
}

static void patch_load(const cJSON *o) {
    const cJSON *p = cJSON_GetObjectItem(o, "project");
    if (!cJSON_IsObject(p)) return;
    static d9_state_t s;
    float bpm;
    d9_project_from_json(p, &s, &bpm);   // the tempo is the Studio patch's
    state_apply(&s);
    const cJSON *nm = cJSON_GetObjectItem(p, "name");
    snprintf(g_d9_proj_cur, sizeof g_d9_proj_cur, "%s", cJSON_IsString(nm) ? nm->valuestring : "");
    const cJSON *ch = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(ch) && ch->valueint >= -1 && ch->valueint <= 16) {
        g_d9set.midi_ch = (int8_t)ch->valueint;
        d9_settings_save();
    }
    d9_changed();
}

// --- module ------------------------------------------------------------------------------------

static void *psram_alloc(unsigned n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void init(void) {
    s_trigq = xQueueCreate(16, sizeof(trig_t));
    g_d9ix_p = heap_caps_calloc(1, sizeof *g_d9ix_p, MALLOC_CAP_SPIRAM);
    if (!g_d9ix_p) g_d9ix_p = calloc(1, sizeof *g_d9ix_p);   // 8 KB at boot: can't fail
    s_cache = heap_caps_calloc(D9_MAX_SAMPLES, sizeof *s_cache, MALLOC_CAP_SPIRAM);
    if (!d9_engine_init(&s_eng, psram_alloc)) ESP_LOGW(TAG, "no room for echo / reverb");
    d9_seq_init(&s_seq);
    // only on our own: in Studio, metroseq owns the DACs
    s_cv_avail = true;
    for (int i = 0; i < studio_module_count(); i++) {
        const tmod_t *m = studio_module(i);
        if (m != &mod_drumster9 && (m->flags & TMOD_F_CV)) s_cv_avail = false;
    }
    settings_load();
    state_load();
    g_d9s.samples_ok = samples_open();
    if (g_d9s.samples_ok) d9_ensure_lane_samples();
    ESP_LOGI(TAG, "%u KB of samples in RAM, CV clock %s", (unsigned)(s_cache_bytes / 1024),
             s_cv_avail ? "available" : "off (another app has the DACs)");
}

static void ui_poll(bool visible) {
    if (s_dirty && studio_now_us() - s_dirty_at > 1500000) state_save();
    d9_ui_poll(visible);
}

static bool busy(void) {
    if (s_dirty) state_save();
    return false;
}

tmod_t mod_drumster9 = {
    .name = "drumster9",
    .title = "DRUMSTER9",
    .blurb = "32-step sample drum machine: 7 Gamma 9001 kits, A-J patterns, 4 FX buses",
    .role = TMOD_DRUMS,
    .flags = TMOD_F_AUDIO | TMOD_F_CLOCKED,
    .init = init,
    .rt_init = m_rt_init,
    .rt_next_due = m_next_due,
    .rt_advance = m_advance,
    .rt_cmd = m_cmd,
    .rt_poll = m_poll,
    .rt_enable = enable,
    .render = render,
    .midi = midi_in,
    .midi_desc = midi_desc,
    .ui_create = d9_ui_create,
    .ui_poll = ui_poll,
    .ui_show = d9_ui_show,
    .busy = busy,
    .patch_save = patch_save,
    .patch_load = patch_load,
};
