// sloe_mod.c - Sloe as a Studio module: notes in (MIDI IN on its channel,
// or a routed sequencer track), audio out, and its one-page UI.
//
// Notes are queued (any task) and started at the top of the next audio
// block - about 4 ms after they were due, the audio buffer's length.
#include <stdio.h>
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
static int s_preset;
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

static void save_now(void) {
    const char *root = studio_files_root();
    if (!root) return;
    char dir[48], path[64];
    snprintf(dir, sizeof dir, "%s/sloe", root);
    mkdir(dir, 0777);
    file_path(path, sizeof path);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddNumberToObject(o, "preset", s_preset);
    cJSON *p = cJSON_AddObjectToObject(o, "patch");
    for (int i = 0; i < SL_NPARAM; i++) cJSON_AddNumberToObject(p, SL_PARAMS[i].key, s_patch.v[i]);
    char *txt = cJSON_PrintUnformatted(o);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
    }
    cJSON_free(txt);
    cJSON_Delete(o);
}

static void load(void) {
    sl_patch_default(&s_patch);
    char path[64];
    file_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    if (!o) return;
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "preset");
    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint < SL_NPRESETS) s_preset = v->valueint;
    const cJSON *p = cJSON_GetObjectItem(o, "patch");
    for (int i = 0; i < SL_NPARAM; i++) {
        const cJSON *x = cJSON_GetObjectItem(p, SL_PARAMS[i].key);
        if (cJSON_IsNumber(x)) s_patch.v[i] = x->valuedouble < 0 ? 0 : (x->valuedouble > 1 ? 1 : x->valuedouble);
    }
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
    lv_obj_t *preset_lbl, *ch_lbl, *voices_lbl;
    lv_obj_t *keys[25];
    int held_key;
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
    lv_label_set_text(U.preset_lbl, SL_PRESETS[s_preset].name);
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

static void preset_cb(void *ud) {
    s_preset = (s_preset + (int)(intptr_t)ud + SL_NPRESETS) % SL_NPRESETS;
    s_patch = SL_PRESETS[s_preset].p;
    changed();
    refresh_all();
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
    sw_btn(s, "<", 180, 8, 40, 40, ACCENT, preset_cb, (void *)-1, SW_F_MID);
    U.preset_lbl = sw_label(s, "", SW_F_MID, SW_TEXT, 224, 18, 170, true);
    sw_btn(s, ">", 398, 8, 40, 40, ACCENT, preset_cb, (void *)1, SW_F_MID);
    sw_label(s, "MIDI IN", SW_F_SMALL, DIM, 470, 22, 0, false);
    sw_btn(s, "-", 530, 8, 40, 40, ACCENT, ch_cb, (void *)-1, SW_F_MID);
    U.ch_lbl = sw_label(s, "", SW_F_MID, SW_TEXT, 572, 18, 76, true);
    sw_btn(s, "+", 650, 8, 40, 40, ACCENT, ch_cb, (void *)1, SW_F_MID);
    sw_label(s, "or route a sequencer track to SLOE\non the Studio home page", SW_F_SMALL, DIM,
             704, 12, 220, false);
    studio_chrome(s, 1024 - 2 * 40 - 6 - 8, 8, 40);
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
};
