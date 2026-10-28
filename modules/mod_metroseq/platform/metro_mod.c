// metro_mod.c - metroseq as a Studio module.
//
// The engine and its timing rules are unchanged (see docs/INVESTIGATION.md);
// what used to be metroseq's own sequencer task now runs as hooks inside
// the Studio real-time task on core 1:
//
//  * the master clock calls the engine at every 1/16 (seq_clock.c), for the
//    internal tempo as well as MIDI clock - so metroseq shares its grid with
//    every other app, to the microsecond;
//  * rt_advance runs the engine's own events (gate lows, swung / ratcheted
//    gates, glide steps) at their deadlines;
//  * actions are executed after the pattern lock is released: CV through
//    the Mabee DACs, and each track's notes either to the preview voice or,
//    when routed on the Studio home page, to a synth / MIDI OUT.
#include <string.h>

#include "app.h"
#include "cv_dac.h"
#include "driver/gpio.h"
#include "engine/seq_clock.h"
#include "engine/seq_engine.h"
#include "engine/seq_json.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mod_metroseq.h"
#include "preview_audio.h"
#include "sdkconfig.h"
#include "storage.h"
#include "studio/studio_hw.h"
#include "ui/ui.h"
#include "ui/ui_common.h"

static const char *TAG = "metro";

#define CV_INIT_DELAY_US    1000000     // after touch bring-up (Python: 2.5 s from boot)
#define CV_RAMP_STEPS       16          // soft start on VOUT0 (Python CV_RAMP_*)
#define CV_RAMP_STEP_US     40000

enum {
    MC_RESET = 1,      // RST: tracks back to stage 1 (on the next 1/16 when playing)
    MC_RESTART,        // pattern replaced: start over on the next 1/16 if playing
    MC_OUTPUTS,        // merge mode toggled
    MC_CV_TOGGLE,
    MC_CV_RESCAN,
    MC_STATS_RESET,
};

pattern_t g_pat;
seq_status_t g_status;

static portMUX_TYPE s_pat_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;

static seq_engine_t s_eng;
static seq_actions_t s_acts;
static seq_clock_t s_sc;

static int64_t s_cv_init_at = INT64_MAX;
static int s_ramp_i = -1;               // -1 = no ramp running
static int64_t s_ramp_next = INT64_MAX;

// --- shared state (any task) ---------------------------------------------------------

void pat_lock(void) { taskENTER_CRITICAL(&s_pat_mux); }
void pat_unlock(void) { taskEXIT_CRITICAL(&s_pat_mux); }

void pat_store(const pattern_t *src) {
    pat_lock();
    memcpy(&g_pat, src, sizeof g_pat);
    pat_unlock();
}

void pat_load(pattern_t *dst) {
    pat_lock();
    memcpy(dst, &g_pat, sizeof g_pat);
    pat_unlock();
}

// --- a new pattern at its switch point (the next bar, by default) -----------------------------

static pattern_t s_next;
static volatile uint32_t s_swaps;

// in place, keeping the tempo (lock held)
static void put_next(void) {
    uint16_t bpm = g_pat.bpm;
    memcpy(&g_pat, &s_next, sizeof g_pat);
    g_pat.bpm = bpm;
    s_swaps++;
}

// rt task, at the switch point (seq_clock.c; lock held)
static void rt_swap(void) {
    bool merge = g_pat.merge_cv;
    put_next();
    if (merge != g_pat.merge_cv) seq_engine_outputs_changed(&s_eng, studio_now_us(), &s_acts);
}

void pat_swap(const pattern_t *p) {
    pat_lock();
    memcpy(&s_next, p, sizeof s_next);
    pattern_sanitise(&s_next);
    bool now = !g_status.playing;
    if (now) {
        s_sc.swap_pending = false;
        put_next();
    } else {
        s_sc.swap_pending = true;
    }
    pat_unlock();
    if (now) seq_cmd(CMD_OUTPUTS_CHANGED);   // nothing left high across the swap
}

bool pat_swap_pending(void) { return s_sc.swap_pending; }

int pat_swap_at(void) { return s_sc.swap_at; }

void pat_set_swap_at(int v) {
    if (v < 0 || v >= SWAP_AT_COUNT) return;
    s_sc.swap_at = (uint8_t)v;   // one byte: the rt task reads it on its next pulse
    storage_set_swap_at(v);
}

const char *pat_swap_when(void) {
    static const char *const W[SWAP_AT_COUNT] = {"next bar", "next 2 bars", "next 4 bars", "TRK 1 end"};
    return s_sc.swap_at < SWAP_AT_COUNT ? W[s_sc.swap_at] : "";
}

const char *pat_swap_at_name(int v) {
    static const char *const N[SWAP_AT_COUNT] = {"BAR", "2 BARS", "4 BARS", "TRK 1 END"};
    return v >= 0 && v < SWAP_AT_COUNT ? N[v] : "?";
}

bool pat_swap_cancel(void) {
    pat_lock();
    bool was = s_sc.swap_pending;
    s_sc.swap_pending = false;
    pat_unlock();
    return was;
}

uint32_t pat_snapshot(pattern_t *dst) {
    pat_lock();
    memcpy(dst, &g_pat, sizeof g_pat);
    uint32_t n = s_swaps;
    pat_unlock();
    return n;
}
uint32_t pat_swap_count(void) { return s_swaps; }

// stopped with one still waiting: it goes in now (the UI task)
void pat_swap_poll(void) {
    if (!s_sc.swap_pending || g_status.playing) return;
    bool done = false;
    pat_lock();
    if (s_sc.swap_pending && !g_status.playing) {
        s_sc.swap_pending = false;
        put_next();
        done = true;
    }
    pat_unlock();
    if (done) seq_cmd(CMD_OUTPUTS_CHANGED);
}

void status_info(const char *msg) {
    taskENTER_CRITICAL(&s_info_mux);
    strlcpy(g_status.info, msg, sizeof g_status.info);
    g_status.info_seq++;
    taskEXIT_CRITICAL(&s_info_mux);
}

void status_info_get(char *buf, int len, uint32_t *seq) {
    taskENTER_CRITICAL(&s_info_mux);
    strlcpy(buf, g_status.info, len);
    *seq = g_status.info_seq;
    taskEXIT_CRITICAL(&s_info_mux);
}

// UI -> sequencer. Transport and tempo belong to the master clock now.
bool seq_cmd(seq_cmd_t cmd) {
    switch (cmd) {
    case CMD_TOGGLE_PLAY:
        studio_toggle_play();
        return true;
    case CMD_TEMPO:
        studio_set_bpm_x100(g_pat.bpm * 100);
        return true;
    case CMD_RESET:
        return studio_cmd(&mod_metroseq, MC_RESET, 0);
    case CMD_RESTART_IF_PLAYING:
        studio_set_bpm_x100(g_pat.bpm * 100);   // a preset carries its tempo
        return studio_cmd(&mod_metroseq, MC_RESTART, 0);
    case CMD_OUTPUTS_CHANGED:
        return studio_cmd(&mod_metroseq, MC_OUTPUTS, 0);
    case CMD_CV_TOGGLE:
        return studio_cmd(&mod_metroseq, MC_CV_TOGGLE, 0);
    case CMD_CV_RESCAN:
        return studio_cmd(&mod_metroseq, MC_CV_RESCAN, 0);
    case CMD_STATS_RESET:
        return studio_cmd(&mod_metroseq, MC_STATS_RESET, 0);
    }
    return false;
}

// --- output execution (rt task) ---------------------------------------------------------

static void record_late(int64_t late) {
    if (late < 0) late = 0;
    static const int64_t edges[LATE_BUCKETS - 1] = {50, 100, 250, 500, 1000, 2000};
    int b = 0;
    while (b < LATE_BUCKETS - 1 && late >= edges[b]) b++;
    g_status.late_hist[b]++;
    g_status.late_count++;
    g_status.late_sum_us += (uint64_t)late;
    if ((uint32_t)late > g_status.late_max_us) g_status.late_max_us = (uint32_t)late;
}

static void note_out(int track, uint8_t type, int midi, int32_t glide, int64_t t) {
    tnote_t n = {.type = type, .vel = 100, .cents = midi * 100, .glide_us = glide, .t = t};
    if (!studio_note_out(&mod_metroseq, track, &n)) {
        if (type == TN_ON)
            preview_note_on(track, midi);
    }
    if (type == TN_OFF) preview_note_off(track);
}

static void execute(const seq_actions_t *acts) {
    for (int i = 0; i < acts->n; i++) {
        const seq_action_t *a = &acts->a[i];
        bool ok = true;
        switch (a->type) {
        case ACT_GATE:
            record_late(studio_now_us() - a->due_us);
#if CONFIG_METRO_TIMING_PROBE_GPIO >= 0
            if (a->out == 0) gpio_set_level(CONFIG_METRO_TIMING_PROBE_GPIO, a->value);
#endif
            if (cv_dac_present(a->out)) ok = cv_dac_gate(a->out, a->value);
            break;
        case ACT_PITCH:
            if (cv_dac_present(a->out)) ok = cv_dac_pitch_cents(a->out, a->value);
            break;
        case ACT_AUDIO_ON:
            note_out(a->out, TN_ON, a->value, a->arg, a->due_us);
            break;
        case ACT_AUDIO_OFF:
            note_out(a->out, TN_OFF, 0, 0, a->due_us);
            break;
        case ACT_CC: {
            // a MOD lane's value as a MIDI CC (MOD page)
            uint8_t cc = g_pat.lane_cc[a->out & 7], ch = g_pat.lane_cc_ch[a->out & 7];
            if (cc) {
                uint8_t m[3] = {(uint8_t)(0xB0 | ((ch - 1) & 15)), (uint8_t)(cc & 127),
                                (uint8_t)(a->value & 127)};
                studio_midi_out(m, 3);
            }
            break;
        }
        }
        if (!ok) g_status.i2c_errors++;
    }
    int lost = cv_dac_take_lost();
    if (lost >= 0) {
        char msg[24];
        snprintf(msg, sizeof msg, "CV TRK%d lost", lost + 1);
        status_info(msg);
    }
}

static void run_engine(int64_t now) {
    seq_actions_clear(&s_acts);
    pat_lock();
    seq_engine_advance(&s_eng, now, &s_acts);
    pat_unlock();
    execute(&s_acts);
    seq_actions_clear(&s_acts);
}

// --- CV bring-up (Python CVOut.init / soft start) --------------------------------

static void cv_init_now(void) {
    int n = g_status.cv_enabled ? cv_dac_init() : 0;
    if (n > 0 && !s_eng.playing) {
        s_ramp_i = 0;   // soft start: ease VOUT0 up and back down once
        s_ramp_next = studio_now_us();
        g_status.cv_ready = false;
    } else {
        g_status.cv_ready = n > 0;
    }
}

static void ramp_end(void) {
    cv_dac_all_low();
    s_ramp_i = -1;
    s_ramp_next = INT64_MAX;
    g_status.cv_ready = true;
    ESP_LOGI(TAG, "cv soft start done");
}

static void ramp_step(void) {
    if (s_ramp_i > 2 * CV_RAMP_STEPS) {
        ramp_end();
        return;
    }
    int i = s_ramp_i;
    float frac = (float)(i <= CV_RAMP_STEPS ? i : 2 * CV_RAMP_STEPS - i) / CV_RAMP_STEPS;
    for (int t = 0; t < CV_NUM_DACS; t++)
        if (cv_dac_present(t)) cv_dac_gate_volts(t, CV_HIGH_V * frac);
    s_ramp_i++;
    s_ramp_next += CV_RAMP_STEP_US;
}

// --- module hooks (rt task) ------------------------------------------------------------

static void flush(const seq_actions_t *acts) { execute(acts); }

static void m_rt_init(void) {
#if CONFIG_METRO_TIMING_PROBE_GPIO >= 0
    gpio_config_t io = {.mode = GPIO_MODE_OUTPUT,
                        .pin_bit_mask = 1ULL << CONFIG_METRO_TIMING_PROBE_GPIO};
    gpio_config(&io);
    gpio_set_level(CONFIG_METRO_TIMING_PROBE_GPIO, 0);
#endif
    cv_dac_attach_bus(studio_i2c_bus(), CONFIG_METRO_DAC_I2C_HZ);
    g_status.touch_ready = true;
    g_status.cv_enabled = true;
    s_cv_init_at = studio_now_us() + CV_INIT_DELAY_US;
}

static int64_t m_next_due(void) {
    int64_t due = seq_engine_next_due(&s_eng);
    if (s_cv_init_at < due) due = s_cv_init_at;
    if (s_ramp_next < due) due = s_ramp_next;
    return due;
}

static void m_advance(int64_t now) {
    run_engine(now);
    if (now >= s_cv_init_at) {
        s_cv_init_at = INT64_MAX;
        cv_init_now();
        status_info(cv_dac_present(0) || cv_dac_present(1) ? "CV ready" : "no DAC found");
    }
    if (s_ramp_i >= 0 && now >= s_ramp_next) ramp_step();
}

static void m_cmd(uint8_t cmd, int32_t arg, int64_t now) {
    (void)arg;
    seq_actions_clear(&s_acts);
    switch (cmd) {
    case MC_RESET:
        if (s_eng.playing) {
            seq_clock_restart(&s_sc);   // in step: stage 1 on the next 1/16
        } else {
            pat_lock();
            seq_engine_reset(&s_eng, now, &s_acts);
            pat_unlock();
        }
        break;
    case MC_RESTART:
        if (s_eng.playing) seq_clock_restart(&s_sc);
        break;
    case MC_OUTPUTS:
        pat_lock();
        seq_engine_outputs_changed(&s_eng, now, &s_acts);
        pat_unlock();
        break;
    case MC_CV_TOGGLE:
        if (!g_status.cv_enabled) {
            g_status.cv_enabled = true;
            cv_init_now();
            if (cv_dac_present(0) || cv_dac_present(1)) {
                status_info("CV on");
            } else {
                g_status.cv_enabled = false;
                status_info("no DAC found");
            }
        } else {
            cv_dac_disable();
            g_status.cv_enabled = false;
            status_info("CV off");
        }
        break;
    case MC_CV_RESCAN:
        cv_init_now();
        break;
    case MC_STATS_RESET:
        memset((void *)g_status.late_hist, 0, sizeof g_status.late_hist);
        g_status.late_max_us = 0;
        g_status.late_count = 0;
        g_status.late_sum_us = 0;
        g_status.i2c_errors = 0;
        break;
    }
    execute(&s_acts);
    seq_actions_clear(&s_acts);
}

static void m_poll(int64_t now) {
    (void)now;
    g_status.playing = s_eng.playing;
    for (int t = 0; t < NUM_TRACKS; t++) {
        g_status.disp_stage[t] = s_eng.tr[t].disp_stage;
        g_status.dac_present[t] = cv_dac_present(t);
    }
    for (int l = 0; l < NUM_MOD_LANES; l++) g_status.lane_disp[l] = s_eng.ln[l].disp_stage;
    g_status.late_gates_dropped = s_eng.stats.late_gates_dropped;
}

static void m_enable(bool on, int64_t now) {
    if (on) return;   // the next 1/16 starts the tracks (seq_clock.c)
    // closed in Studio: everything low, silent
    seq_actions_clear(&s_acts);
    pat_lock();
    seq_engine_stop(&s_eng, now, &s_acts);
    pat_unlock();
    execute(&s_acts);
    seq_actions_clear(&s_acts);
}

// PLAY during the CV soft start: finish it (gates low) BEFORE the downbeat.
static tclock_sink_t s_inner;

static void m_start(void *ctx, int64_t t) {
    if (s_ramp_i >= 0) ramp_end();
    s_inner.start(ctx, t);
}

// --- boot ------------------------------------------------------------------------------

static void m_init(void) {
    pattern_default(&g_pat);
    g_pat.bpm = (uint16_t)((studio_bpm_x100() + 50) / 100);
    storage_init();
    preset_init();   // the preset played last, with every change kept
    preview_init();
    seq_engine_init(&s_eng, &g_pat, esp_random());
    s_sc = (seq_clock_t){.e = &s_eng, .acts = &s_acts, .lock = pat_lock, .unlock = pat_unlock,
                         .flush = flush, .swap = rt_swap, .swap_at = (uint8_t)storage_swap_at()};
    seq_clock_sink(&s_sc, &s_inner);
    mod_metroseq.sink = s_inner;
    mod_metroseq.sink.start = m_start;
}

static bool m_busy(void) { return g_status.playing || storage_write_pending(); }

// --- Studio patches: the pattern, all 10 presets, which one is playing -----------------------

static cJSON *m_patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    static pattern_t p;
    pat_load(&p);
    cJSON_AddItemToObject(o, "pattern", pattern_to_json(&p));
    cJSON_AddNumberToObject(o, "preset", preset_current());
    cJSON *a = cJSON_AddArrayToObject(o, "presets");
    for (int i = 0; i < NUM_PRESETS; i++)
        cJSON_AddItemToArray(a, storage_preset_get(i, &p) ? pattern_to_json(&p) : cJSON_CreateNull());
    cJSON_AddBoolToObject(o, "preview", preview_enabled());
    static const char *const SW[SWAP_AT_COUNT] = {"bar", "2bars", "4bars", "trk1"};
    cJSON_AddStringToObject(o, "preset_switch", SW[pat_swap_at()]);
    return o;
}

static void m_patch_load(const cJSON *o) {
    static pattern_t p;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "presets");
    if (cJSON_IsArray(a)) {
        for (int i = 0; i < NUM_PRESETS; i++) {
            const cJSON *x = cJSON_GetArrayItem(a, i);
            if (cJSON_IsObject(x)) {
                pattern_from_json(x, &p);
                storage_preset_save(i, &p);
            } else {
                storage_preset_clear(i);
            }
        }
    }
    const cJSON *pat = cJSON_GetObjectItemCaseSensitive(o, "pattern");
    if (cJSON_IsObject(pat)) {
        pattern_from_json(pat, &p);
        p.bpm = (uint16_t)clampi((studio_bpm_x100() + 50) / 100, MIN_BPM, MAX_BPM);
        pat_store(&p);
        // in step with the others: the tracks start over on the next 1/16
        seq_cmd(CMD_OUTPUTS_CHANGED);
        seq_cmd(CMD_RESET);
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "preset");
    preset_set_current(cJSON_IsNumber(v) ? v->valueint : 0);
    v = cJSON_GetObjectItemCaseSensitive(o, "preview");
    if (cJSON_IsBool(v)) preview_set_enabled(cJSON_IsTrue(v));
    v = cJSON_GetObjectItemCaseSensitive(o, "preset_switch");
    if (cJSON_IsString(v)) {
        static const char *const SW[SWAP_AT_COUNT] = {"bar", "2bars", "4bars", "trk1"};
        for (int i = 0; i < SWAP_AT_COUNT; i++)
            if (!strcmp(v->valuestring, SW[i])) pat_set_swap_at(i);
    }
    if (ui_built()) ui_refresh_all();
}

static const char *const OUTS[NUM_TRACKS] = {"TRK 1", "TRK 2"};

tmod_t mod_metroseq = {
    .name = "metroseq",
    .title = "METROSEQ",
    .blurb = "Metropolix-style 2-track sequencer: gate + 1 V/oct on the Mabee DACs",
    .role = TMOD_SEQUENCER,
    .flags = TMOD_F_CV | TMOD_F_AUDIO | TMOD_F_CLOCKED,
    .n_note_outs = NUM_TRACKS,
    .note_out_names = OUTS,
    .init = m_init,
    .rt_init = m_rt_init,
    .rt_next_due = m_next_due,
    .rt_advance = m_advance,
    .rt_cmd = m_cmd,
    .rt_poll = m_poll,
    .rt_enable = m_enable,
    .render = preview_render,
    .ui_create = ui_create,
    .ui_poll = ui_poll,
    .busy = m_busy,
    .patch_save = m_patch_save,
    .patch_load = m_patch_load,
};
