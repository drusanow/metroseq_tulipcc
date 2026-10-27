// sloe_mod.c - Sloe as a Studio module: notes in (MIDI IN on its channel,
// or a routed sequencer track), audio out, and its one-page UI.
//
// Notes are queued (any task) and started at the top of the next audio
// block - about 4 ms after they were due, the audio buffer's length.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_sloe.h"
#include "sloe_dsp.h"
#include "studio/widgets.h"

#define ACCENT 0x6dff00
#define DIM    0x9292aa

static sl_engine_t s_eng;
static QueueHandle_t s_q;
static sl_patch_t s_patch;           // the UI's copy
static volatile uint32_t s_ver;
static uint32_t s_seen;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_midi_ch = 1;            // 0 omni, 1..16, -1 off
// The preset list: the factory ones, then yours (/files/sloe/presets.json).
// s_preset indexes it; s_name is the sound's name as last picked / saved.
#define SL_USER_MAX 48
#define SL_NAME_MAX 16
typedef struct {
    char name[SL_NAME_MAX + 1];
    sl_patch_t p;
} sl_user_t;
static sl_user_t *s_user;
static int s_nuser;
static int s_preset;
static char s_name[SL_NAME_MAX + 1] = "Init";
static bool s_dirty;
static int64_t s_dirty_at;

// --- notes ------------------------------------------------------------------------------------

static void note_in(const tnote_t *n) {
    if (s_q) xQueueSend(s_q, n, 0);
}

static void midi_in(const uint8_t *m, int len, int64_t t) {
    int ch = (m[0] & 15) + 1;
    if (s_midi_ch < 0 || (s_midi_ch && s_midi_ch != ch) || len < 3) return;
    uint8_t st = m[0] & 0xF0;
    tnote_t n = {.src = (uint8_t)(0x80 | (ch - 1)), .key = m[1], .cents = m[1] * 100, .t = t};
    if (st == 0x90 && m[2]) {
        n.type = TN_ON;
        n.vel = m[2];
    } else if (st == 0x80 || st == 0x90) {
        n.type = TN_OFF;
    } else if (st == 0xB0 && (m[1] == 123 || m[1] == 120)) {
        n.type = TN_ALL_OFF;
    } else {
        return;
    }
    note_in(&n);
}

// --- audio -------------------------------------------------------------------------------------

static void render(float *l, float *r, const studio_block_t *b) {
    if (s_ver != s_seen) {
        taskENTER_CRITICAL(&s_mux);
        s_seen = s_ver;
        s_eng.p = s_patch;
        taskEXIT_CRITICAL(&s_mux);
    }
    tnote_t n;
    while (xQueueReceive(s_q, &n, 0) == pdTRUE) {
        if (n.type == TN_ON)
            sl_note_on(&s_eng, n.src, n.key, n.cents / 100.0f, n.vel / 127.0f, n.glide_us);
        else if (n.type == TN_OFF)
            sl_note_off(&s_eng, n.src, n.key);
        else
            sl_all_off(&s_eng, n.src == 0xFF ? -1 : n.src);
    }
    sl_render(&s_eng, l, r, b->n);
}

// closed in Studio: let go of everything
static void enable(bool on, int64_t now) {
    tnote_t off = {.type = TN_ALL_OFF, .src = 0xFF};
    if (!on) note_in(&off);
}

// --- the patch file -------------------------------------------------------------------------------

static void file_path(char *out, int len) {
    const char *root = studio_files_root();
    snprintf(out, len, "%s/sloe/sloe.json", root ? root : "/nonexistent");
}

static bool sloe_dir(char *out, int len) {
    const char *root = studio_files_root();
    if (!root) return false;
    snprintf(out, len, "%s/sloe", root);
    mkdir(out, 0777);
    return true;
}

static cJSON *patch_json(const sl_patch_t *pt) {
    cJSON *p = cJSON_CreateObject();
    for (int i = 0; i < SL_NPARAM; i++)   // 4 places: 0.15, not 0.15000000596
        cJSON_AddNumberToObject(p, SL_PARAMS[i].key, round(pt->v[i] * 10000.0) / 10000.0);
    return p;
}

// keys a file doesn't have keep what's there
static void patch_from(const cJSON *p, sl_patch_t *pt) {
    for (int i = 0; i < SL_NPARAM; i++) {
        const cJSON *x = cJSON_GetObjectItem(p, SL_PARAMS[i].key);
        if (cJSON_IsNumber(x)) pt->v[i] = x->valuedouble < 0 ? 0 : (x->valuedouble > 1 ? 1 : x->valuedouble);
    }
}

static cJSON *read_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < 256 * 1024 ? malloc(n + 1) : NULL;
    cJSON *o = NULL;
    if (buf && fread(buf, 1, n, f) == (size_t)n) {
        buf[n] = 0;
        o = cJSON_Parse(buf);
    }
    free(buf);
    fclose(f);
    return o;
}

static bool write_json(const char *path, const cJSON *o) {
    char *txt = cJSON_PrintUnformatted(o);
    FILE *f = txt ? fopen(path, "w") : NULL;
    bool ok = f && fputs(txt, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    cJSON_free(txt);
    return ok;
}

static void save_now(void) {
    char path[64];
    if (!sloe_dir(path, sizeof path)) return;
    file_path(path, sizeof path);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddStringToObject(o, "name", s_name);
    cJSON_AddItemToObject(o, "patch", patch_json(&s_patch));
    write_json(path, o);
    cJSON_Delete(o);
}

// --- your presets -----------------------------------------------------------------------------

static void user_path(char *out, int len) {
    const char *root = studio_files_root();
    snprintf(out, len, "%s/sloe/presets.json", root ? root : "/nonexistent");
}

static void user_load(void) {
    if (!s_user) s_user = calloc(SL_USER_MAX, sizeof *s_user);
    s_nuser = 0;
    if (!s_user) return;
    char path[64];
    user_path(path, sizeof path);
    cJSON *o = read_json(path);
    const cJSON *e;
    cJSON_ArrayForEach(e, o) {
        const cJSON *n = cJSON_GetObjectItem(e, "name");
        if (!cJSON_IsString(n) || s_nuser >= SL_USER_MAX) continue;
        sl_user_t *u = &s_user[s_nuser++];
        snprintf(u->name, sizeof u->name, "%s", n->valuestring);
        sl_patch_default(&u->p);
        patch_from(cJSON_GetObjectItem(e, "patch"), &u->p);
    }
    cJSON_Delete(o);
}

static bool user_write(void) {
    char path[64];
    if (!sloe_dir(path, sizeof path)) return false;
    user_path(path, sizeof path);
    cJSON *o = cJSON_CreateArray();
    for (int i = 0; i < s_nuser; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", s_user[i].name);
        cJSON_AddItemToObject(e, "patch", patch_json(&s_user[i].p));
        cJSON_AddItemToArray(o, e);
    }
    bool ok = write_json(path, o);
    cJSON_Delete(o);
    return ok;
}

static int npresets(void) { return SL_NPRESETS + s_nuser; }
static bool is_user(int i) { return i >= SL_NPRESETS && i < npresets(); }
static const char *preset_name(int i) {
    return i < SL_NPRESETS ? SL_PRESETS[i].name : s_user[i - SL_NPRESETS].name;
}
static int find_preset(const char *name) {
    for (int i = npresets() - 1; i >= 0; i--)   // yours first
        if (!strcmp(preset_name(i), name)) return i;
    return -1;
}

static void load(void) {
    sl_patch_default(&s_patch);
    user_load();
    char path[64];
    file_path(path, sizeof path);
    cJSON *o = read_json(path);
    if (!o) return;
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "name");
    if (cJSON_IsString(v)) {
        snprintf(s_name, sizeof s_name, "%s", v->valuestring);
    } else {   // older files: the factory preset's number
        v = cJSON_GetObjectItem(o, "preset");
        if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint < SL_NPRESETS)
            snprintf(s_name, sizeof s_name, "%s", SL_PRESETS[v->valueint].name);
    }
    int i = find_preset(s_name);
    s_preset = i < 0 ? 0 : i;
    patch_from(cJSON_GetObjectItem(o, "patch"), &s_patch);
    cJSON_Delete(o);
}

static void changed(void) {
    taskENTER_CRITICAL(&s_mux);
    s_ver++;
    taskEXIT_CRITICAL(&s_mux);
    s_dirty = true;
    s_dirty_at = studio_now_us();
}

// --- UI ---------------------------------------------------------------------------------------------

static struct {
    sw_slider_t *sl[SL_NPARAM];
    lv_obj_t *wave_b[2][4], *mono_b[2];
    lv_obj_t *preset_lbl, *ch_lbl, *voices_lbl, *del_b, *msg;
    lv_obj_t *keys[25];
    int held_key;
    bool del_armed;
    bool built;
} U = {.held_key = -1};

static float get_p(void *ud) { return s_patch.v[(intptr_t)ud]; }
static void set_p(void *ud, float v) {
    s_patch.v[(intptr_t)ud] = v;
    changed();
}
static void text_p(void *ud, char *b, int n) { sl_param_text(&s_patch, (int)(intptr_t)ud, b, n); }
static const sw_slider_ops_t OPS = {get_p, set_p, text_p};

static void refresh_switches(void) {
    for (int o = 0; o < 2; o++)
        for (int w = 0; w < 4; w++)
            sw_btn_set(U.wave_b[o][w], sl_param_step(&s_patch, o ? P_WAVE2 : P_WAVE1) == w);
    int mono = sl_param_step(&s_patch, P_MONO);
    sw_btn_set(U.mono_b[0], !mono);
    sw_btn_set(U.mono_b[1], mono);
}

static void refresh_all(void) {
    for (int i = 0; i < SL_NPARAM; i++)
        if (U.sl[i]) sw_slider_refresh(U.sl[i]);
    refresh_switches();
    char nm[32];
    snprintf(nm, sizeof nm, "%s%s", is_user(s_preset) ? "* " : "", s_name);
    lv_label_set_text(U.preset_lbl, nm);
    char t[16];
    if (s_midi_ch == 0)
        snprintf(t, sizeof t, "OMNI");
    else if (s_midi_ch < 0)
        snprintf(t, sizeof t, "OFF");
    else
        snprintf(t, sizeof t, "CH %d", s_midi_ch);
    lv_label_set_text(U.ch_lbl, t);
}

static void wave_cb(void *ud) {
    int v = (int)(intptr_t)ud;
    s_patch.v[(v >> 2) ? P_WAVE2 : P_WAVE1] = (v & 3) / 3.0f;
    changed();
    refresh_switches();
}

static void mono_cb(void *ud) {
    s_patch.v[P_MONO] = ud ? 1.0f : 0.0f;
    changed();
    refresh_switches();
}

static void say(const char *t) {
    if (U.msg) lv_label_set_text(U.msg, t);
}

static void del_disarm(void) {
    U.del_armed = false;
    sw_btn_text(U.del_b, "DEL");
    sw_btn_set(U.del_b, false);
}

static void preset_cb(void *ud) {
    del_disarm();
    s_preset = (s_preset + (int)(intptr_t)ud + npresets()) % npresets();
    s_patch = is_user(s_preset) ? s_user[s_preset - SL_NPRESETS].p : SL_PRESETS[s_preset].p;
    snprintf(s_name, sizeof s_name, "%s", preset_name(s_preset));
    changed();
    refresh_all();
    say(is_user(s_preset) ? "yours" : "factory");
}

// keep the sound as one of yours, under this name (replaces one of yours
// with the same name)
static void saved_as(const char *name, void *ud) {
    char nm[SL_NAME_MAX + 1];
    snprintf(nm, sizeof nm, "%s", name);
    int i = -1;
    for (int k = 0; k < s_nuser; k++)
        if (!strcmp(s_user[k].name, nm)) i = k;
    if (i < 0) {
        if (!s_user || s_nuser >= SL_USER_MAX) return say("full: delete one first");
        i = s_nuser++;
    }
    snprintf(s_user[i].name, sizeof s_user[i].name, "%s", nm);
    s_user[i].p = s_patch;
    bool ok = user_write();
    snprintf(s_name, sizeof s_name, "%s", nm);
    s_preset = SL_NPRESETS + i;
    s_dirty = true;
    s_dirty_at = studio_now_us();
    refresh_all();
    say(ok ? "saved" : "couldn't write the file");
}

static void save_as_cb(void *ud) {
    del_disarm();
    sw_text_entry("SAVE SLOE PRESET", is_user(s_preset) ? s_name : "MY SOUND", SL_NAME_MAX, ACCENT,
                  saved_as, NULL);
}

// SAVE: over the one of yours you're on; from a factory one, a new name
static void save_cb(void *ud) {
    del_disarm();
    if (is_user(s_preset))
        saved_as(s_user[s_preset - SL_NPRESETS].name, NULL);
    else
        save_as_cb(NULL);
}

static void del_cb(void *ud) {
    if (!is_user(s_preset)) return say("factory: can't delete");
    if (!U.del_armed) {   // tap twice
        U.del_armed = true;
        sw_btn_text(U.del_b, "SURE?");
        sw_btn_set(U.del_b, true);
        return;
    }
    del_disarm();
    int i = s_preset - SL_NPRESETS;
    memmove(&s_user[i], &s_user[i + 1], (s_nuser - i - 1) * sizeof *s_user);
    s_nuser--;
    bool ok = user_write();
    s_preset = 0;   // the sound stays; it's just not one of yours any more
    refresh_all();
    say(ok ? "deleted" : "couldn't write the file");
}

static void ch_cb(void *ud) {
    int c = s_midi_ch + (int)(intptr_t)ud;
    s_midi_ch = c < -1 ? 16 : (c > 16 ? -1 : c);
    s_dirty = true;
    s_dirty_at = studio_now_us();
    refresh_all();
}

// the on-screen keys: C3..C5, for trying sounds without a keyboard
static void key_event(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    tnote_t n = {.src = 0xF0, .key = (uint8_t)(48 + k), .cents = (48 + k) * 100, .vel = 100};
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        n.type = TN_ON;
        U.held_key = k;
    } else {
        n.type = TN_OFF;
        U.held_key = -1;
    }
    note_in(&n);
}

static void section(lv_obj_t *s, const char *title, int x, int y) {
    sw_label(s, title, SW_F_LABEL, SW_ORANGE, x, y, 0, false);
}

static void ui_create(lv_obj_t *s) {
    // header
    sw_label(s, "SLOE", SW_F_BIG, SW_TEXT, 12, 8, 0, false);
    sw_label(s, "SYNTH", SW_F_SMALL, ACCENT, 104, 22, 0, false);
    sw_btn(s, "<", 170, 8, 40, 40, ACCENT, preset_cb, (void *)-1, SW_F_MID);
    U.preset_lbl = sw_label(s, "", SW_F_LABEL, SW_TEXT, 212, 20, 156, true);
    lv_label_set_long_mode(U.preset_lbl, LV_LABEL_LONG_CLIP);
    sw_btn(s, ">", 370, 8, 40, 40, ACCENT, preset_cb, (void *)1, SW_F_MID);
    sw_btn(s, "SAVE", 418, 8, 62, 40, ACCENT, save_cb, NULL, NULL);
    sw_btn(s, "SAVE AS", 484, 8, 76, 40, ACCENT, save_as_cb, NULL, NULL);
    U.del_b = sw_btn(s, "DEL", 564, 8, 52, 40, SW_RED, del_cb, NULL, NULL);
    sw_label(s, "MIDI IN", SW_F_SMALL, DIM, 632, 22, 0, false);
    sw_btn(s, "-", 690, 8, 40, 40, ACCENT, ch_cb, (void *)-1, SW_F_MID);
    U.ch_lbl = sw_label(s, "", SW_F_MID, SW_TEXT, 732, 18, 76, true);
    sw_btn(s, "+", 810, 8, 40, 40, ACCENT, ch_cb, (void *)1, SW_F_MID);
    U.msg = sw_label(s, "", SW_F_SMALL, DIM, 858, 22, 70, false);
    lv_label_set_long_mode(U.msg, LV_LABEL_LONG_CLIP);
    studio_chrome(s);
    sw_line(s, 0, 56, 1024, 1);

    // oscillators
    section(s, "OSCILLATORS", 12, 66);
    for (int o = 0; o < 2; o++) {
        sw_label(s, o ? "OSC 2" : "OSC 1", SW_F_SMALL, DIM, 12, 94 + o * 44, 0, false);
        for (int w = 0; w < 4; w++)
            U.wave_b[o][w] = sw_btn(s, SL_WAVES[w], 64 + w * 84, 88 + o * 44, 78, 36, ACCENT, wave_cb,
                                    (void *)(intptr_t)((o << 2) | w), NULL);
    }
    const int col = 252, sw = 232;
    int x0 = 12;
    U.sl[P_SEMI2] = sw_slider(s, "OSC 2 SEMI", 410, 84, sw, 46, ACCENT, &OPS, (void *)P_SEMI2);
    U.sl[P_DETUNE] = sw_slider(s, "DETUNE", 410 + col, 84, sw, 46, ACCENT, &OPS, (void *)P_DETUNE);
    U.sl[P_MIX] = sw_slider(s, "OSC 1 / 2", 410, 136, sw, 46, ACCENT, &OPS, (void *)P_MIX);
    U.sl[P_NOISE] = sw_slider(s, "NOISE", 410 + col, 136, sw, 46, ACCENT, &OPS, (void *)P_NOISE);

    // filter + its envelope
    section(s, "FILTER", x0, 196);
    static const int frow[] = {P_CUTOFF, P_RESO, P_ENVAMT, P_KEYTRK};
    for (int i = 0; i < 4; i++)
        U.sl[frow[i]] = sw_slider(s, SL_PARAMS[frow[i]].label, x0 + i * col, 216, sw, 46, SW_CYAN,
                                  &OPS, (void *)(intptr_t)frow[i]);
    static const int fenv[] = {P_FA, P_FD, P_FS, P_FR};
    for (int i = 0; i < 4; i++)
        U.sl[fenv[i]] = sw_slider(s, SL_PARAMS[fenv[i]].label, x0 + i * col, 268, sw, 46, SW_CYAN,
                                  &OPS, (void *)(intptr_t)fenv[i]);
    // amp
    section(s, "AMP", x0, 324);
    static const int aenv[] = {P_AA, P_AD, P_AS, P_AR};
    for (int i = 0; i < 4; i++)
        U.sl[aenv[i]] = sw_slider(s, SL_PARAMS[aenv[i]].label, x0 + i * col, 344, sw, 46, SW_ORANGE,
                                  &OPS, (void *)(intptr_t)aenv[i]);
    U.sl[P_GLIDE] = sw_slider(s, "GLIDE", x0, 396, sw, 46, SW_ORANGE, &OPS, (void *)P_GLIDE);
    U.sl[P_VEL] = sw_slider(s, "VELOCITY", x0 + col, 396, sw, 46, SW_ORANGE, &OPS, (void *)P_VEL);
    U.sl[P_LEVEL] = sw_slider(s, "LEVEL", x0 + 2 * col, 396, sw, 46, SW_ORANGE, &OPS, (void *)P_LEVEL);
    sw_label(s, "VOICES", SW_F_SMALL, DIM, x0 + 3 * col, 396, 0, false);
    U.mono_b[0] = sw_btn(s, "POLY 6", x0 + 3 * col, 413, 112, 29, SW_ORANGE, mono_cb, NULL, NULL);
    U.mono_b[1] = sw_btn(s, "MONO", x0 + 3 * col + 120, 413, 112, 29, SW_ORANGE, mono_cb, (void *)1,
                         NULL);

    // two octaves of keys
    sw_line(s, 0, 456, 1024, 1);
    static const bool black[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int nw = 0;
    for (int k = 0; k < 25; k++)
        if (!black[k % 12]) nw++;
    int ww = (1024 - 24) / nw, wx = 12, kh = 600 - 470 - 10;
    for (int pass = 0; pass < 2; pass++) {
        wx = 12;
        for (int k = 0; k < 25; k++) {
            bool b = black[k % 12];
            if (pass == 0 && !b) {
                U.keys[k] = sw_btn(s, k % 12 == 0 ? (k ? "C4" : "C3") : "", wx, 470, ww - 4, kh,
                                   ACCENT, NULL, NULL, SW_F_SMALL);
                if (k == 24) sw_btn_text(U.keys[k], "C5");
            }
            if (pass == 1 && b)
                U.keys[k] = sw_btn(s, "", wx - ww / 3, 470, ww * 2 / 3 - 2, kh * 6 / 10, ACCENT, NULL,
                                   NULL, NULL);
            if (!b) wx += ww;
            if (pass == 1 && b) lv_obj_set_style_bg_color(U.keys[k], sw_c(0x242424), 0);
        }
    }
    for (int k = 0; k < 25; k++) {
        lv_obj_add_event_cb(U.keys[k], key_event, LV_EVENT_PRESSED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(U.keys[k], key_event, LV_EVENT_RELEASED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(U.keys[k], key_event, LV_EVENT_PRESS_LOST, (void *)(intptr_t)k);
    }
    U.built = true;
    refresh_all();
}

static void ui_poll(bool visible) {
    if (s_dirty && studio_now_us() - s_dirty_at > 1500000) {
        s_dirty = false;
        save_now();
    }
}

static void init(void) {
    s_q = xQueueCreate(64, sizeof(tnote_t));
    sl_init(&s_eng);
    load();
    s_eng.p = s_patch;
    s_seen = s_ver;
}

static void midi_desc(char *b, int n) {
    if (s_midi_ch < 0)
        snprintf(b, n, "off (routed tracks only)");
    else if (s_midi_ch == 0)
        snprintf(b, n, "notes on every channel");
    else
        snprintf(b, n, "notes on ch %d", s_midi_ch);
}

// --- Studio patches ---------------------------------------------------------------------------

static cJSON *patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s_name);
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddItemToObject(o, "patch", patch_json(&s_patch));
    return o;
}

static void patch_load(const cJSON *o) {
    sl_patch_default(&s_patch);
    patch_from(cJSON_GetObjectItem(o, "patch"), &s_patch);
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "name");
    if (cJSON_IsString(v)) snprintf(s_name, sizeof s_name, "%s", v->valuestring);
    int i = find_preset(s_name);
    s_preset = i < 0 ? 0 : i;
    changed();
    if (U.built) refresh_all();
}

static bool busy(void) {
    if (s_dirty) {
        s_dirty = false;
        save_now();
    }
    return false;
}

tmod_t mod_sloe = {
    .name = "sloe",
    .title = "SLOE",
    .blurb = "Subtractive synth: 2 oscillators, filter, 6 voices or mono with glide",
    .role = TMOD_SYNTH,
    .flags = TMOD_F_AUDIO | TMOD_F_NOTES_IN,
    .init = init,
    .rt_enable = enable,
    .render = render,
    .note = note_in,
    .midi = midi_in,
    .midi_desc = midi_desc,
    .ui_create = ui_create,
    .ui_poll = ui_poll,
    .busy = busy,
    .patch_save = patch_save,
    .patch_load = patch_load,
};
