// gx_mod.c - GinTonix as a Studio module: audio, MIDI, settings.
//
// Audio (core 1, audio task): each 1.1 ms block asks the pattern player
// which hits sound in it - from the master clock's timeline, so the drums
// sit exactly on the grid metroseq's gates use - starts them at their
// exact frame, and renders the 8 voices. Pattern MIDI out leaves at the
// grid time through the rt task (the drums are rendered a few ms ahead).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "core/gx_engine.h"
#include "core/gx_json.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gx_app.h"
#include "mod_gintonix.h"
#include "ui/gx_ui.h"

static const char *TAG = "gintonix";

#define NOTE_TRIG 36   // C1..G1: play channel 1..8
#define NOTE_MUTE 48   // C2..G2: mute channel 1..8 while held
#define NOTE_PAT  60   // C3..B3: pattern A..L

gx_state_t g_gx;
gx_live_t g_live = {.next = -1};
gx_status_t g_gxs = {.step = -1};
gx_settings_t g_gxset;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_trigq, s_midiq;

// coefficients: the UI's latest, picked up by the audio task per block
static gx_vp_t s_vp_new[GX_NCH];
static volatile uint32_t s_vp_ver;
static uint32_t s_vp_seen;
static volatile bool s_silence;

// audio-task state
static gx_engine_t s_eng;
static gx_seq_t s_seq;

void gx_lock(void) { taskENTER_CRITICAL(&s_mux); }
void gx_unlock(void) { taskEXIT_CRITICAL(&s_mux); }

void gx_sound_changed(int c) {
    gx_vp_t vp[GX_NCH];
    uint8_t choke = 0;
    for (int i = 0; i < GX_NCH; i++) {
        gx_patch_t p;
        gx_eff(&g_gx, i, &p);
        if (p.s[S_CHOKE]) choke |= (uint8_t)(1u << i);
        if (c < 0 || c == i) gx_vp_from_patch(&vp[i], &p, g_gx.master);
    }
    gx_lock();
    for (int i = 0; i < GX_NCH; i++)
        if (c < 0 || c == i) s_vp_new[i] = vp[i];
    s_vp_ver++;
    gx_unlock();
    g_live.choke = choke;
}

typedef struct {
    uint8_t ch;
    float vel;
} trig_t;

void gx_trigger(int c, float vel) {
    if (c < 0 || c >= GX_NCH || !s_trigq) return;
    if ((g_live.mute | g_live.hold_mute) & (1u << c)) return;
    trig_t t = {(uint8_t)c, vel};
    xQueueSend(s_trigq, &t, 0);
    g_gxs.hit_ms[c] = (uint32_t)(studio_now_us() / 1000);
}

void gx_silence(void) { s_silence = true; }

// --- audio ----------------------------------------------------------------------------------

static void midi_note_out(int64_t t, int c, float vel, int64_t gate_us) {
    uint8_t ch = (uint8_t)((g_gxset.midi_out - 1) & 15);
    // accent 127, normal 64 (the Python's 1.008 x 0.5), fills fade
    int v = vel >= 0.99f ? 127 : (int)(vel * 128.0f + 0.5f);
    if (v < 1) v = 1;
    uint8_t on[3] = {(uint8_t)(0x90 | ch), (uint8_t)(NOTE_TRIG + c), (uint8_t)v};
    uint8_t off[3] = {(uint8_t)(0x80 | ch), (uint8_t)(NOTE_TRIG + c), 0};
    studio_midi_out_at(t, on, 3);
    studio_midi_out_at(t + gate_us, off, 3);
}

static void render(float *l, float *r, const studio_block_t *b) {
    int64_t c0 = studio_now_us();
    if (s_vp_ver != s_vp_seen) {
        gx_lock();
        s_vp_seen = s_vp_ver;
        memcpy(s_eng.vp, s_vp_new, sizeof s_eng.vp);
        gx_unlock();
    }
    if (s_silence) {
        s_silence = false;
        gx_engine_silence(&s_eng);
    }
    gx_ev_t ev[72];
    int nev = 0;
    trig_t t;
    while (nev < 8 && xQueueReceive(s_trigq, &t, 0) == pdTRUE) ev[nev++] = (gx_ev_t){0, t.ch, t.vel};

    // the hits whose grid time, moved by the sync offset, sounds in this block
    tclock_tl_t tl;
    studio_timeline(&tl);
    int64_t sync = (int64_t)g_gxset.sync_ms * 1000;
    int64_t t_end = studio_time_at(b, b->n) + sync;
    gx_hit_t hits[64];
    gx_lock();
    int nh = gx_seq_run(&s_seq, &g_gx, &g_live, &tl, tclock_tl_pos(&tl, t_end - 1), hits, 64);
    int step = gx_seq_step_at(&s_seq, &g_gx, tclock_tl_pos(&tl, b->t0 + sync));
    int pat = s_seq.pat, rate = g_gx.rate;
    gx_unlock();
    g_gxs.running = s_seq.active;
    g_gxs.step = (int8_t)(s_seq.active ? step : -1);
    g_gxs.pat = (int8_t)pat;

    int64_t gate = (gx_step_units(rate) * tl.p16_us / TC_U16) / 2;
    for (int i = 0; i < nh; i++) {
        int64_t tg = tclock_tl_time(&tl, hits[i].pos);
        int fr = studio_frame_at(b, tg - sync);
        if (fr < 0) fr = 0;   // late (the first beat after a MIDI Start)
        if (fr >= b->n) fr = b->n - 1;
        // after any live triggers at frame 0: keep frame order
        int k = nev;
        while (k > 0 && ev[k - 1].frame > fr) {
            ev[k] = ev[k - 1];
            k--;
        }
        ev[k] = (gx_ev_t){fr, hits[i].ch, hits[i].vel};
        nev++;
        g_gxs.hit_ms[hits[i].ch] = (uint32_t)(tg / 1000);
        if (g_gxset.midi_out) midi_note_out(tg, hits[i].ch, hits[i].vel, gate);
    }
    gx_engine_render(&s_eng, ev, nev, l, r, b->n);
    int64_t c1 = studio_now_us();
    static float load;
    load += ((float)(c1 - c0) * (100.0f * STUDIO_SR / 1e6f / STUDIO_BLOCK) - load) * 0.02f;
    g_gxs.load_pct = (int)(load + 0.5f);
}

// --- MIDI (MIDI task) -------------------------------------------------------------------------

static void midi_in(const uint8_t *m, int len, int64_t t) {
    int ch = (m[0] & 15) + 1;
    int8_t want = g_gxset.midi_ch;
    if (want < 0 || (want != 0 && want != ch)) return;
    uint8_t st = m[0] & 0xF0;
    if (st == 0x90 && len == 3 && m[2] > 0) {
        int n = m[1];
        if (n >= NOTE_TRIG && n < NOTE_TRIG + GX_NCH) {
            gx_trigger(n - NOTE_TRIG, m[2] / 127.0f);
            return;
        }
        if (n >= NOTE_MUTE && n < NOTE_MUTE + GX_NCH) {
            __atomic_or_fetch(&g_live.hold_mute, (uint8_t)(1u << (n - NOTE_MUTE)), __ATOMIC_SEQ_CST);
            return;
        }
    } else if ((st == 0x80 || (st == 0x90 && m[2] == 0)) && len == 3) {
        int n = m[1];
        if (n >= NOTE_MUTE && n < NOTE_MUTE + GX_NCH) {
            __atomic_and_fetch(&g_live.hold_mute, (uint8_t)~(1u << (n - NOTE_MUTE)),
                               __ATOMIC_SEQ_CST);
        }
        return;
    }
    // pattern keys, program change, CC: the UI task does those
    gx_midi_msg_t mm = {m[0], m[1], len > 2 ? m[2] : 0};
    if (s_midiq) xQueueSend(s_midiq, &mm, 0);
}

bool gx_midi_next(gx_midi_msg_t *m) { return s_midiq && xQueueReceive(s_midiq, m, 0) == pdTRUE; }

// --- files ---------------------------------------------------------------------------------------

const char *gx_dir(void) {
    static char dir[32];
    const char *root = studio_files_root();
    if (!root) return NULL;
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "%s/gintonix", root);
        mkdir(dir, 0777);
        char b[64];
        snprintf(b, sizeof b, "%s/gintonic_bank", dir);
        mkdir(b, 0777);
    }
    return dir;
}

void gx_path(char *out, int len, const char *name) {
    const char *d = gx_dir();
    snprintf(out, len, "%s/%s", d ? d : "/nonexistent", name);
}

// --- settings ---------------------------------------------------------------------------------

#define SETTINGS_FILE "gintonic_settings.json"
static volatile bool s_set_dirty;

static void settings_defaults(void) {
    memset(&g_gxset, 0, sizeof g_gxset);
    g_gxset.midi_ch = 10;   // drums' usual channel (the Python: omni)
    g_gxset.cc_sel = true;
    g_gxset.slot = 1;
    g_gxset.ncc = 2;
    g_gxset.cc[0].cc = 1;
    snprintf(g_gxset.cc[0].key, sizeof g_gxset.cc[0].key, "g:morph");
    g_gxset.cc[1].cc = 7;
    snprintf(g_gxset.cc[1].key, sizeof g_gxset.cc[1].key, "g:master");
}

void gx_settings_load(void) {
    settings_defaults();
    char path[96];
    gx_path(path, sizeof path, SETTINGS_FILE);
    cJSON *o = gx_json_load(path);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return;
    }
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "midi_ch"))) {
        int c = v->valueint;
        g_gxset.midi_ch = (int8_t)(c >= -1 && c <= 16 ? c : 0);
    }
    if ((v = cJSON_GetObjectItem(o, "cc_sel"))) g_gxset.cc_sel = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "midi_out")))
        g_gxset.midi_out = (uint8_t)(v->valueint % 17);
    // "sync_ms" in the Python made up for AMY's ~30 ms of audio delay; that
    // delay is compensated exactly here, so this is a separate fine trim
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "sync_adj_ms"))) {
        int s = v->valueint;
        g_gxset.sync_ms = (int8_t)(s < -50 ? -50 : (s > 50 ? 50 : s));
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "slot"))) {
        int s = v->valueint;
        g_gxset.slot = (uint8_t)(s >= 1 && s <= GX_BANK_SLOTS ? s : 1);
    }
    const cJSON *cc = cJSON_GetObjectItem(o, "cc");
    if (cJSON_IsArray(cc)) {
        g_gxset.ncc = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, cc) {
            const cJSON *n = cJSON_GetArrayItem(e, 0), *k = cJSON_GetArrayItem(e, 1);
            if (!cJSON_IsNumber(n) || !cJSON_IsString(k) || g_gxset.ncc >= GX_MAX_CC) continue;
            g_gxset.cc[g_gxset.ncc].cc = (uint8_t)(n->valueint & 127);
            snprintf(g_gxset.cc[g_gxset.ncc].key, sizeof g_gxset.cc[0].key, "%s", k->valuestring);
            g_gxset.ncc++;
        }
    }
    cJSON_Delete(o);
}

static void settings_write(void) {
    cJSON *o = cJSON_CreateObject();
    studio_clock_status_t st;
    studio_clock_status(&st);
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddNumberToObject(o, "midi_ch", g_gxset.midi_ch);
    cJSON_AddBoolToObject(o, "clock_ext", studio_clock_src() == CLOCK_SRC_MIDI);
    cJSON_AddBoolToObject(o, "clock_out", studio_clock_out());
    cJSON_AddNumberToObject(o, "tempo", studio_bpm_x100() / 100.0);
    cJSON_AddNumberToObject(o, "slot", g_gxset.slot);
    cJSON_AddBoolToObject(o, "cc_sel", g_gxset.cc_sel);
    cJSON_AddNumberToObject(o, "midi_out", g_gxset.midi_out);
    cJSON_AddBoolToObject(o, "mmc", studio_mmc());
    cJSON_AddNumberToObject(o, "sync_adj_ms", g_gxset.sync_ms);
    cJSON *cc = cJSON_AddArrayToObject(o, "cc");
    for (int i = 0; i < g_gxset.ncc; i++) {
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateNumber(g_gxset.cc[i].cc));
        cJSON_AddItemToArray(e, cJSON_CreateString(g_gxset.cc[i].key));
        cJSON_AddItemToArray(cc, e);
    }
    char path[96];
    gx_path(path, sizeof path, SETTINGS_FILE);
    if (gx_dir()) gx_json_save(path, o);
    cJSON_Delete(o);
}

// The Python only wrote settings while stopped; there's no stall to fear
// here (code runs from PSRAM during flash writes) but it costs nothing.
void gx_settings_save(void) { s_set_dirty = true; }

void gx_settings_poll(void) {
    if (s_set_dirty && !g_gxs.running) {
        s_set_dirty = false;
        settings_write();
    }
}

// --- module ---------------------------------------------------------------------------------------

static void m_init(void) {
    s_trigq = xQueueCreate(32, sizeof(trig_t));
    s_midiq = xQueueCreate(64, sizeof(gx_midi_msg_t));
    gx_state_init(&g_gx);
    gx_kit_state(&GX_KITS[0], &g_gx);
    g_gx.tempo = studio_bpm_x100() / 100.0f;   // the master's tempo stays
    gx_engine_init(&s_eng);
    gx_seq_init(&s_seq);
    gx_settings_load();
    gx_boot_load();   // the last kit slot, as the Python did
    gx_sound_changed(-1);
    ESP_LOGI(TAG, "%d drums, %d kits, MIDI ch %d", GX_NDRUMS, GX_NKITS, g_gxset.midi_ch);
}

static void m_enable(bool on, int64_t now) {
    (void)now;
    if (!on) gx_silence();
}

static bool m_busy(void) { return s_set_dirty; }

static void m_midi_desc(char *b, int n) {
    char ch[12];
    if (g_gxset.midi_ch < 0)
        snprintf(ch, sizeof ch, "off");
    else if (g_gxset.midi_ch == 0)
        snprintf(ch, sizeof ch, "omni");
    else
        snprintf(ch, sizeof ch, "ch %d", g_gxset.midi_ch);
    snprintf(b, n, "%s: C1-G1 play, C2-G2 mute, C3-B3 patterns, CCs, program change = kit slot",
             ch);
}

tmod_t mod_gintonix = {
    .name = "gintonix",
    .title = "GINTONIX",
    .blurb = "Microtonic-style drum machine: 8 synth drums, 12 patterns, morph, fills",
    .role = TMOD_DRUMS,
    .flags = TMOD_F_AUDIO | TMOD_F_CLOCKED,
    .init = m_init,
    .rt_enable = m_enable,
    .render = render,
    .midi = midi_in,
    .midi_desc = m_midi_desc,
    .ui_create = gx_ui_create,
    .ui_poll = gx_ui_poll,
    .ui_show = gx_ui_show,
    .busy = m_busy,
    .patch_save = gx_patch_save,
    .patch_load = gx_patch_load,
};
