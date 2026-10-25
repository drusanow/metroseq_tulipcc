// seq_task.c - the real-time sequencer task.
//
// TIMING DESIGN (see docs/INVESTIGATION.md for the reasoning):
//
//  * Runs alone on core 1 at near-top priority. Core 0 has the LCD
//    bounce-buffer ISR, LVGL, audio and storage; nothing else is pinned
//    here, so this task is effectively never preempted.
//
//  * One clock: a 1 MHz GPTimer counting up from boot. The engine works in
//    absolute microseconds on that clock; nothing is ever "N ms from now"
//    relative to a callback that might itself be late, so jitter cannot
//    accumulate into drift.
//
//  * The task sleeps on a GPTimer alarm (ISR in IRAM -> task notify) set
//    WAKE_EARLY_US before the next event, then spins the last few us to
//    the exact deadline. Interrupt / context-switch latency is absorbed by
//    the spin instead of showing up as gate jitter.
//
//  * This task is the only user of the I2C bus. CV writes go out at their
//    deadline; touch polling only happens when the gap to the next event is
//    longer than a worst-case touch read, so a touch read can never sit on
//    the bus when a gate is due.
#include "seq_task.h"

#include <string.h>

#include "app.h"
#include "board.h"
#include "cv_dac.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/i2c_master.h"
#include "engine/seq_engine.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gt911.h"
#include "preview_audio.h"
#include "sdkconfig.h"

static const char *TAG = "seq";

#define SEQ_CORE            1
#define SEQ_PRIORITY        (configMAX_PRIORITIES - 2)
#define SEQ_STACK           6144
#define WAKE_EARLY_US       40          // sleep until this close, then spin
#define TOUCH_PERIOD_US     10000       // GT911 scans at ~100 Hz
#define TOUCH_STARVE_US     100000      // poll anyway after this long
#define CV_INIT_DELAY_US    1000000     // after touch bring-up (Python: 2.5 s from boot)
#define CV_RAMP_STEPS       16          // soft start on VOUT0 (Python CV_RAMP_*)
#define CV_RAMP_STEP_US     40000

static gptimer_handle_t s_timer;
static TaskHandle_t s_task;
static QueueHandle_t s_cmdq;
static i2c_master_bus_handle_t s_bus;

static seq_engine_t s_eng;
static seq_actions_t s_acts;

static bool s_touch_ok;
static int64_t s_touch_next, s_touch_last;
static bool s_touch_was_deferred;
static int64_t s_cv_init_at = INT64_MAX;
static int s_ramp_i = -1;               // -1 = no ramp running
static int64_t s_ramp_next = INT64_MAX;

// --- clock -----------------------------------------------------------------

static bool IRAM_ATTR on_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *d, void *ctx) {
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &hp);
    return hp == pdTRUE;
}

static inline int64_t now_us(void) {
    uint64_t c = 0;
    gptimer_get_raw_count(s_timer, &c);
    return (int64_t)c;
}

static void clock_init(void) {
    // Created from this task so the alarm interrupt lands on core 1.
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 3,   // highest level available to C handlers
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &s_timer));
    gptimer_event_callbacks_t cbs = {.on_alarm = on_alarm};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));
    ESP_ERROR_CHECK(gptimer_start(s_timer));
}

// Sleep until `due` (absolute us). Returns early if a UI command arrives.
static void wait_until(int64_t due) {
    if (due == INT64_MAX) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return;
    }
    int64_t wake = due - WAKE_EARLY_US;
    if (wake > now_us()) {
        gptimer_alarm_config_t a = {.alarm_count = (uint64_t)wake};
        gptimer_set_alarm_action(s_timer, &a);   // fires at once if already past
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (uxQueueMessagesWaiting(s_cmdq)) return;
    }
    while (now_us() < due) {
        if (uxQueueMessagesWaiting(s_cmdq)) return;
    }
}

// --- output execution ------------------------------------------------------

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

static void execute(const seq_actions_t *acts) {
    for (int i = 0; i < acts->n; i++) {
        const seq_action_t *a = &acts->a[i];
        bool ok = true;
        switch (a->type) {
        case ACT_GATE:
            record_late(now_us() - a->due_us);
#if CONFIG_METRO_TIMING_PROBE_GPIO >= 0
            if (a->out == 0) gpio_set_level(CONFIG_METRO_TIMING_PROBE_GPIO, a->value);
#endif
            if (cv_dac_present(a->out)) ok = cv_dac_gate(a->out, a->value);
            break;
        case ACT_PITCH:
            if (cv_dac_present(a->out)) ok = cv_dac_pitch_cents(a->out, a->value);
            break;
        case ACT_AUDIO_ON:
            preview_note_on(a->out, a->value);
            break;
        case ACT_AUDIO_OFF:
            preview_note_off(a->out);
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
}

// --- CV bring-up (Python CVOut.init / soft start) --------------------------

static void cv_init_now(void) {
    int n = g_status.cv_enabled ? cv_dac_init() : 0;
    if (n > 0 && !s_eng.playing) {
        s_ramp_i = 0;   // soft start: ease VOUT0 up and back down once
        s_ramp_next = now_us();
        g_status.cv_ready = false;
    } else {
        g_status.cv_ready = n > 0;
    }
}

static void ramp_step(void) {
    if (s_eng.playing) {   // never fight the sequencer for VOUT0
        s_ramp_i = 2 * CV_RAMP_STEPS + 1;
    }
    if (s_ramp_i > 2 * CV_RAMP_STEPS) {
        cv_dac_all_low();
        s_ramp_i = -1;
        s_ramp_next = INT64_MAX;
        g_status.cv_ready = true;
        ESP_LOGI(TAG, "cv soft start done");
        return;
    }
    int i = s_ramp_i;
    float frac = (float)(i <= CV_RAMP_STEPS ? i : 2 * CV_RAMP_STEPS - i) / CV_RAMP_STEPS;
    for (int t = 0; t < CV_NUM_DACS; t++)
        if (cv_dac_present(t)) cv_dac_gate_volts(t, CV_HIGH_V * frac);
    s_ramp_i++;
    s_ramp_next += CV_RAMP_STEP_US;
}

// --- commands from the UI --------------------------------------------------

static void handle_cmd(uint8_t cmd, int64_t now) {
    seq_actions_clear(&s_acts);
    pat_lock();
    switch (cmd) {
    case CMD_TOGGLE_PLAY:
        if (s_eng.playing)
            seq_engine_stop(&s_eng, now, &s_acts);
        else
            seq_engine_start(&s_eng, now, &s_acts);
        break;
    case CMD_RESET:
        seq_engine_reset(&s_eng, now, &s_acts);
        break;
    case CMD_RESTART_IF_PLAYING:
        if (s_eng.playing) seq_engine_start(&s_eng, now, &s_acts);
        break;
    case CMD_OUTPUTS_CHANGED:
        seq_engine_outputs_changed(&s_eng, now, &s_acts);
        break;
    case CMD_TEMPO:   // picked up by seq_engine_advance()'s re-anchor
    default:
        break;
    }
    pat_unlock();
    execute(&s_acts);

    switch (cmd) {
    case CMD_TOGGLE_PLAY:
        if (s_ramp_i >= 0) ramp_step();   // PLAY during soft start ends it now
        break;
    case CMD_CV_TOGGLE:
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
    case CMD_CV_RESCAN:
        cv_init_now();
        break;
    case CMD_STATS_RESET:
        memset((void *)g_status.late_hist, 0, sizeof g_status.late_hist);
        g_status.late_max_us = 0;
        g_status.late_count = 0;
        g_status.late_sum_us = 0;
        g_status.i2c_errors = 0;
        g_status.touch_deferred = 0;
        g_status.touch_forced = 0;
        break;
    }
}

bool seq_cmd(seq_cmd_t cmd) {
    uint8_t c = (uint8_t)cmd;
    if (!s_cmdq || xQueueSend(s_cmdq, &c, 0) != pdTRUE) return false;
    xTaskNotifyGive(s_task);
    return true;
}

// --- touch polling in the gaps ---------------------------------------------

// Returns when touch next needs attention.
static int64_t touch_service(int64_t now) {
    if (!s_touch_ok) return INT64_MAX;
    if (now < s_touch_next) return s_touch_next;
    int64_t eng_next = seq_engine_next_due(&s_eng);
    int64_t gap = eng_next - now;
    bool starving = s_eng.playing && now - s_touch_last > TOUCH_STARVE_US;
    if (gap > gt911_poll_budget_us() || starving) {
        if (gap <= gt911_poll_budget_us()) g_status.touch_forced++;
        if (s_touch_was_deferred) g_status.touch_deferred++;
        s_touch_was_deferred = false;
        if (!gt911_poll(now)) {
            g_status.i2c_errors++;
        }
        g_status.touch_polls++;
        s_touch_last = now;
        s_touch_next = now + TOUCH_PERIOD_US;
        return s_touch_next;
    }
    // Not enough room before the next CV write: try again right after it.
    s_touch_was_deferred = true;
    return eng_next + 1;
}

// --- status ------------------------------------------------------------------

static void publish(void) {
    g_status.playing = s_eng.playing;
    for (int t = 0; t < NUM_TRACKS; t++) {
        g_status.disp_stage[t] = s_eng.tr[t].disp_stage;
        g_status.dac_present[t] = cv_dac_present(t);
    }
    for (int l = 0; l < NUM_MOD_LANES; l++) g_status.lane_disp[l] = s_eng.ln[l].disp_stage;
    g_status.resyncs = s_eng.stats.resyncs;
}

// --- the task ----------------------------------------------------------------

static void seq_task(void *arg) {
    clock_init();

#if CONFIG_METRO_TIMING_PROBE_GPIO >= 0
    gpio_config_t io = {.mode = GPIO_MODE_OUTPUT,
                        .pin_bit_mask = 1ULL << CONFIG_METRO_TIMING_PROBE_GPIO};
    gpio_config(&io);
    gpio_set_level(CONFIG_METRO_TIMING_PROBE_GPIO, 0);
#endif

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   // board has external pull-ups
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));
    cv_dac_attach_bus(s_bus, CONFIG_METRO_DAC_I2C_HZ);

    s_touch_ok = gt911_init(s_bus, CONFIG_METRO_TOUCH_I2C_HZ);
    s_touch_next = now_us();

    seq_engine_init(&s_eng, &g_pat, esp_random());
    g_status.cv_enabled = true;
    s_cv_init_at = now_us() + CV_INIT_DELAY_US;
    ESP_LOGI(TAG, "sequencer running on core %d, prio %d", xPortGetCoreID(), SEQ_PRIORITY);

    for (;;) {
        int64_t now = now_us();
        uint8_t cmd;
        while (xQueueReceive(s_cmdq, &cmd, 0) == pdTRUE) handle_cmd(cmd, now_us());

        run_engine(now_us());

        now = now_us();
        if (now >= s_cv_init_at) {
            s_cv_init_at = INT64_MAX;
            cv_init_now();
            status_info(cv_dac_present(0) || cv_dac_present(1) ? "CV ready" : "no DAC found");
        }
        if (s_ramp_i >= 0 && now >= s_ramp_next) ramp_step();

        int64_t touch_due = touch_service(now_us());
        publish();

        int64_t due = seq_engine_next_due(&s_eng);
        if (touch_due < due) due = touch_due;
        if (s_cv_init_at < due) due = s_cv_init_at;
        if (s_ramp_next < due) due = s_ramp_next;
        wait_until(due);
    }
}

void seq_task_start(void) {
    s_cmdq = xQueueCreate(16, sizeof(uint8_t));
    xTaskCreatePinnedToCore(seq_task, "seq", SEQ_STACK, NULL, SEQ_PRIORITY, &s_task, SEQ_CORE);
}
