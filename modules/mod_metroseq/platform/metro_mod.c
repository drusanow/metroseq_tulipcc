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
    preview_init();
    seq_engine_init(&s_eng, &g_pat, esp_random());
    s_sc = (seq_clock_t){.e = &s_eng, .acts = &s_acts, .lock = pat_lock, .unlock = pat_unlock,
                         .flush = flush};
    seq_clock_sink(&s_sc, &s_inner);
    mod_metroseq.sink = s_inner;
    mod_metroseq.sink.start = m_start;
}

static bool m_busy(void) { return g_status.playing || storage_write_pending(); }

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
};
