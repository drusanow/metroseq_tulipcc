// example.c - the smallest complete Tulip Studio app: a sine synth (role
// SYNTH) that plays notes from MIDI IN and from a sequencer track routed to
// it. Copy this folder to modules/mod_<yours>/, rename everything that says
// example, rebuild apps/studio - it then shows on the launcher's start page
// and on the Studio home page. docs/APPS.md says what each part is for.
#include <math.h>
#include <stdio.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_example.h"
#include "studio/widgets.h"

#define VOICES 4
#define ACCENT SW_CYAN

typedef struct {
    uint8_t src, key;   // who plays it (a track, a MIDI channel) and its key
    bool on;
    float phase, inc, amp, env;
} voice_t;

// render() runs from internal RAM (linker.lf) and calls nothing in flash -
// no sinf / powf there: tables made in init() instead
#define SINE_N 256
static float s_sine[SINE_N + 1];
static float s_note_inc[128];      // phase step per frame of each MIDI note

static QueueHandle_t s_q;          // notes in: rt / MIDI task -> audio task
static voice_t s_v[VOICES];        // the audio task's alone
static volatile int s_midi_ch;     // 0 omni, 1..16, -1 off
static volatile float s_level = 0.3f;
static lv_obj_t *s_ch_lbl, *s_note_lbl;
static volatile uint8_t s_last_key;

// --- notes: queue them, never play them here (rt / MIDI task) ----------------------------

static void note_in(const tnote_t *n) { xQueueSend(s_q, n, 0); }

static void midi_in(const uint8_t *m, int len, int64_t t) {
    int ch = (m[0] & 15) + 1;
    if (len < 3 || s_midi_ch < 0 || (s_midi_ch && s_midi_ch != ch)) return;
    tnote_t n = {.src = (uint8_t)(0x80 | (ch - 1)), .key = m[1], .cents = m[1] * 100, .vel = m[2], .t = t};
    uint8_t st = m[0] & 0xF0;
    if (st == 0x90 && m[2])
        n.type = TN_ON;
    else if (st == 0x80 || st == 0x90)
        n.type = TN_OFF;
    else
        return;
    note_in(&n);
}

// --- audio (audio task, core 1): add a block into l / r, never block ------------------------

static void render(float *l, float *r, const studio_block_t *b) {
    tnote_t n;
    while (xQueueReceive(s_q, &n, 0) == pdTRUE) {
        if (n.type == TN_ON) {
            voice_t *v = &s_v[0];
            for (int i = 0; i < VOICES; i++)
                if (!s_v[i].on && s_v[i].env < v->env) v = &s_v[i];   // the quietest free one
            int k = n.cents / 100;
            k = k < 0 ? 0 : k > 127 ? 127 : k;
            *v = (voice_t){.src = n.src, .key = n.key, .on = true, .phase = 0, .inc = s_note_inc[k],
                           .amp = n.vel / 127.0f, .env = 0};
            s_last_key = n.key;
        } else {
            for (int i = 0; i < VOICES; i++)
                if (n.type == TN_ALL_OFF || (s_v[i].src == n.src && s_v[i].key == n.key)) s_v[i].on = false;
        }
    }
    float lvl = s_level;
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &s_v[i];
        if (!v->on && v->env < 1e-4f) continue;
        for (int f = 0; f < b->n; f++) {
            v->env += ((v->on ? v->amp : 0.0f) - v->env) * 0.002f;   // a soft attack / release
            float x = v->phase * SINE_N;
            int i0 = (int)x;
            float s = (s_sine[i0] + (s_sine[i0 + 1] - s_sine[i0]) * (x - i0)) * v->env * lvl;
            v->phase += v->inc;
            if (v->phase >= 1.0f) v->phase -= 1.0f;
            l[f] += s;
            r[f] += s;
        }
    }
}

// switched OFF in the Studio: let go of everything
static void enable(bool on, int64_t now) {
    tnote_t off = {.type = TN_ALL_OFF};
    if (!on) note_in(&off);
}

// --- UI (core 0) -------------------------------------------------------------------------------

static void refresh(void) {
    char t[24];
    if (s_midi_ch < 0)
        snprintf(t, sizeof t, "OFF");
    else if (s_midi_ch == 0)
        snprintf(t, sizeof t, "OMNI");
    else
        snprintf(t, sizeof t, "CH %d", s_midi_ch);
    lv_label_set_text(s_ch_lbl, t);
}

static void ch_cb(void *ud) {
    int c = s_midi_ch + (int)(intptr_t)ud;
    s_midi_ch = c < -1 ? 16 : c > 16 ? -1 : c;
    refresh();
}

static void level_cb(void *ud) {
    float v = s_level + (intptr_t)ud * 0.05f;
    s_level = v < 0 ? 0 : v > 1 ? 1 : v;
}

static void ui_create(lv_obj_t *s) {
    // the header every app has: its name, then the two Tulip squares
    sw_label(s, "EXAMPLE", SW_F_BIG, SW_TEXT, 12, 8, 0, false);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);

    sw_label(s, "NOTES IN", SW_F_SMALL, SW_DIM, 12, 92, 0, false);
    sw_btn(s, "-", 120, 80, 44, 40, ACCENT, ch_cb, (void *)-1, SW_F_MID);
    s_ch_lbl = sw_label(s, "", SW_F_LABEL, SW_TEXT, 164, 92, 100, true);
    sw_btn(s, "+", 264, 80, 44, 40, ACCENT, ch_cb, (void *)1, SW_F_MID);
    sw_label(s, "LEVEL", SW_F_SMALL, SW_DIM, 12, 142, 0, false);
    sw_btn_repeat(sw_btn(s, "-", 120, 130, 44, 40, ACCENT, level_cb, (void *)-1, SW_F_MID));
    sw_btn_repeat(sw_btn(s, "+", 264, 130, 44, 40, ACCENT, level_cb, (void *)1, SW_F_MID));
    // (a real app saves its settings: files go in /files/<name>/, see docs/APPS.md)
    s_note_lbl = sw_label(s, "", SW_F_BIG, ACCENT, 12, 200, 0, false);
    // the runtime's I/O for this role: clock in, what MIDI IN plays
    studio_io_panel(s, 680, 76, &mod_example, ACCENT);
    refresh();
}

static void ui_poll(bool visible) {
    static uint8_t shown = 255;
    if (visible && s_last_key != shown) {
        shown = s_last_key;
        lv_label_set_text_fmt(s_note_lbl, "last note %d", shown);
    }
}

static void midi_desc(char *b, int n) {
    if (s_midi_ch < 0)
        snprintf(b, n, "off (routed tracks only)");
    else if (s_midi_ch == 0)
        snprintf(b, n, "notes on every channel");
    else
        snprintf(b, n, "notes on ch %d", s_midi_ch);
}

// --- start-up, and the Studio patch -------------------------------------------------------------

// On its own: at boot. In the Studio: when the app is first switched ON.
static void init(void) {
    s_q = xQueueCreate(32, sizeof(tnote_t));
    for (int i = 0; i <= SINE_N; i++) s_sine[i] = sinf(6.2831853f * i / SINE_N);
    for (int k = 0; k < 128; k++) s_note_inc[k] = 440.0f * powf(2.0f, (k - 69) / 12.0f) / STUDIO_SR;
}

static cJSON *patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddNumberToObject(o, "level", s_level);
    return o;
}

static void patch_load(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "level");
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 1) s_level = (float)v->valuedouble;
    if (s_ch_lbl) refresh();
}

tmod_t mod_example = {
    .name = "example",   // = app.json "name" = the folder mod_example
    .title = "EXAMPLE",
    .blurb = "Template: a sine synth that plays MIDI IN and routed tracks",
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
    .patch_save = patch_save,
    .patch_load = patch_load,
};

TULIP_APP(mod_example)   // the entry point when built as an .elf; nothing otherwise
