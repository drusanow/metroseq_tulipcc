// rt.c - the Studio real-time task: master clock, sequencer engines, the
// I2C bus (CV DACs + touch) and MIDI OUT.
//
// TIMING DESIGN (metroseq's, see docs/INVESTIGATION.md), now shared:
//
//  * Runs on core 1 at near-top priority. Only the MIDI IN task (tiny) and
//    the audio task (lower priority) share the core; the UI, the panel
//    interrupt and file writes are on core 0.
//
//  * One clock: a 1 MHz GPTimer counting from boot. The master clock and
//    every sequencer work in absolute microseconds on it.
//
//  * The task sleeps on a GPTimer alarm set WAKE_EARLY_US before the next
//    deadline (a pulse, a module's event, a MIDI byte), then spins to it.
//
//  * The only user of the I2C bus. Touch is read only when the gap to the
//    next deadline is longer than a worst-case touch read, so a touch read
//    can never sit on the bus when a gate is due.
//
//  * Modules' engines run inside this task, one after the other; each does
//    its own (short) I/O. Nothing here ever waits on another task.
#include <string.h>

#include "driver/gptimer.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "studio/studio_hw.h"
#include "studio_priv.h"
#include "tulip/board.h"
#include "tulip/gt911.h"

static const char *TAG = "rt";

#define RT_CORE           1
#define RT_PRIORITY       (configMAX_PRIORITIES - 2)
#define RT_STACK          8192
#define WAKE_EARLY_US     40
#define TOUCH_PERIOD_US   10000
#define TOUCH_STARVE_US   30000   // a tap lasts ~50 ms: never poll slower than this
#define TOUCH_I2C_HZ      100000
#define OUT_HEAP          256   // timed MIDI OUT bytes (drum pattern out + note-offs)

typedef struct {
    uint8_t kind, mod, cmd;
    int32_t arg;
} rt_msg_t;

typedef struct {
    int64_t t;
    uint8_t type;
    uint16_t arg;
    uint8_t mmc[5];
} midi_in_msg_t;

typedef struct {
    int64_t t;
    uint8_t n;
    uint8_t b[12];
} midi_out_msg_t;

static gptimer_handle_t s_timer;
static TaskHandle_t s_task;
static QueueHandle_t s_cmdq, s_midiq, s_outq;
static i2c_master_bus_handle_t s_bus;
static tclock_t s_clk;
static volatile bool s_ready;

static bool s_touch_ok;
static int64_t s_touch_next, s_touch_last;

static midi_out_msg_t s_out[OUT_HEAP];
static int s_nout;

// published to other tasks
static volatile uint32_t s_tl_seq;
static tclock_tl_t s_tl_pub;
static uint32_t s_tl_ver_seen = UINT32_MAX;
static volatile studio_clock_status_t s_status;

// --- clock ------------------------------------------------------------------------

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

int64_t studio_now_us(void) { return s_timer ? now_us() : 0; }

bool rt_ready(void) { return s_ready; }

static bool inputs_waiting(void) {
    return uxQueueMessagesWaiting(s_cmdq) || uxQueueMessagesWaiting(s_midiq) ||
           uxQueueMessagesWaiting(s_outq);
}

static void timer_init(void) {
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 3,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &s_timer));
    gptimer_event_callbacks_t cbs = {.on_alarm = on_alarm};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));
    ESP_ERROR_CHECK(gptimer_start(s_timer));
}

static void wait_until(int64_t due) {
    if (due == INT64_MAX) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return;
    }
    int64_t wake = due - WAKE_EARLY_US;
    while (now_us() < wake) {
        gptimer_alarm_config_t a = {.alarm_count = (uint64_t)wake};
        gptimer_set_alarm_action(s_timer, &a);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (inputs_waiting()) return;
    }
    while (now_us() < due) {
        if (inputs_waiting()) return;
    }
}

// --- publishing ---------------------------------------------------------------------

static void publish(void) {
    if (s_clk.tl_ver != s_tl_ver_seen) {
        s_tl_ver_seen = s_clk.tl_ver;
        s_tl_seq++;   // odd: writing
        __sync_synchronize();
        s_tl_pub = s_clk.tl;
        __sync_synchronize();
        s_tl_seq++;
    }
    s_status.playing = s_clk.running;
    s_status.armed = tclock_armed(&s_clk);
    s_status.ext = s_clk.ext;
    s_status.ext_bpm_x10 = tclock_ext_alive(&s_clk, now_us()) ? tclock_ext_bpm_x10(&s_clk) : 0;
    s_status.resyncs = s_clk.resyncs;
}

void studio_timeline(tclock_tl_t *out) {
    uint32_t a, b;
    do {
        a = s_tl_seq;
        __sync_synchronize();
        *out = s_tl_pub;
        __sync_synchronize();
        b = s_tl_seq;
    } while ((a & 1) || a != b);
}

void studio_clock_status(studio_clock_status_t *st) {
    st->playing = s_status.playing;
    st->armed = s_status.armed;
    st->ext = s_status.ext;
    st->ext_bpm_x10 = s_status.ext_bpm_x10;
    st->resyncs = s_status.resyncs;
}

// --- the master clock's sinks: every enabled module ------------------------------------

#define FOR_ON(call)                                            \
    for (int i = 0; i < g_nmods; i++) {                          \
        if (!g_enabled[i]) continue;                             \
        const tclock_sink_t *s = &g_mods[i]->sink;               \
        call;                                                    \
    }

static void h_start(void *x, int64_t t) { FOR_ON(if (s->start) s->start(s->ctx, t)); }
static void h_cont(void *x, int64_t t) { FOR_ON(if (s->cont) s->cont(s->ctx, t)); }
static void h_stop(void *x, int64_t t) { FOR_ON(if (s->stop) s->stop(s->ctx, t)); }
static void h_locate(void *x, int64_t t, uint32_t p) {
    FOR_ON(if (s->locate) s->locate(s->ctx, t, p));
}
static void h_pulse(void *x, int64_t t, uint32_t p) {
    FOR_ON(if (s->pulse) s->pulse(s->ctx, t, p));
}
static void h_tempo(void *x, int64_t p16, int32_t bpm_x100) {
    // every module keeps the tempo, enabled or not
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i]->sink.tempo) g_mods[i]->sink.tempo(g_mods[i]->sink.ctx, p16, bpm_x100);
}

static void clock_out_fn(void *ctx, const uint8_t *b, int n) { midi_io_write(b, n); }

// --- commands -------------------------------------------------------------------------

bool rt_post(uint8_t kind, uint8_t mod, uint8_t cmd, int32_t arg) {
    rt_msg_t m = {kind, mod, cmd, arg};
    if (!s_cmdq || xQueueSend(s_cmdq, &m, 0) != pdTRUE) return false;
    if (s_task) xTaskNotifyGive(s_task);
    return true;
}

void rt_post_midi_in(const mp_ev_t *ev, int64_t t) {
    midi_in_msg_t m = {.t = t, .type = ev->type, .arg = ev->arg};
    memcpy(m.mmc, ev->d, sizeof m.mmc);
    if (!s_midiq || xQueueSend(s_midiq, &m, 0) != pdTRUE) return;
    xTaskNotifyGive(s_task);
}

bool studio_midi_out_at(int64_t t, const uint8_t *b, int n) {
    if (n <= 0 || n > 12 || !s_outq) return false;
    midi_out_msg_t m = {.t = t, .n = (uint8_t)n};
    memcpy(m.b, b, n);
    if (xQueueSend(s_outq, &m, 0) != pdTRUE) return false;
    xTaskNotifyGive(s_task);
    return true;
}

bool studio_midi_out(const uint8_t *b, int n) { return studio_midi_out_at(0, b, n); }

static void handle_cmd(const rt_msg_t *m, int64_t now) {
    switch (m->kind) {
    case HC_PLAY: tclock_play(&s_clk, now); break;
    case HC_STOP: tclock_stop(&s_clk, now); break;
    case HC_TOGGLE: tclock_toggle(&s_clk, now); break;
    case HC_RESTART: tclock_restart(&s_clk, now); break;
    case HC_BPM: tclock_set_bpm(&s_clk, m->arg, now); break;
    case HC_CLOCK_SRC: tclock_set_ext(&s_clk, m->arg == CLOCK_SRC_MIDI, now); break;
    case HC_CLOCK_OUT: tclock_set_clock_out(&s_clk, m->arg != 0, now); break;
    case HC_MMC: s_clk.mmc_out = m->arg != 0; break;
    case HC_ENABLE: {
        int i = m->mod;
        if (i >= g_nmods) break;
        bool on = m->arg != 0;
        if (g_enabled[i] == on) break;
        g_enabled[i] = on;
        if (g_mods[i]->rt_enable) g_mods[i]->rt_enable(on, now);
        break;
    }
    case HC_MODULE:
        if (m->mod < g_nmods && g_mods[m->mod]->rt_cmd)
            g_mods[m->mod]->rt_cmd(m->cmd, m->arg, now);
        break;
    }
}

// --- MIDI OUT at a given time ----------------------------------------------------------

static void out_push(const midi_out_msg_t *m) {
    if (s_nout >= OUT_HEAP) return;
    int i = s_nout++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (s_out[p].t <= m->t) break;
        s_out[i] = s_out[p];
        i = p;
    }
    s_out[i] = *m;
}

static void out_pop(void) {
    midi_out_msg_t last = s_out[--s_nout];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        int64_t mt = last.t;
        if (l < s_nout && s_out[l].t < mt) { m = l; mt = s_out[l].t; }
        if (r < s_nout && s_out[r].t < mt) m = r;
        if (m == i) break;
        s_out[i] = s_out[m];
        i = m;
    }
    if (s_nout > 0) s_out[i] = last;
}

static void out_service(int64_t now) {
    while (s_nout > 0 && s_out[0].t <= now + 20) {
        midi_io_write(s_out[0].b, s_out[0].n);
        out_pop();
    }
}

// --- touch in the gaps -------------------------------------------------------------------

static int64_t engines_next_due(void) {
    int64_t due = tclock_next_due(&s_clk);
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i]->rt_next_due) continue;
        int64_t d = g_mods[i]->rt_next_due();
        if (d < due) due = d;
    }
    if (s_nout > 0 && s_out[0].t < due) due = s_out[0].t;
    return due;
}

static int64_t touch_service(int64_t now) {
    if (!s_touch_ok) return INT64_MAX;
    if (now < s_touch_next) return s_touch_next;
    int64_t next = engines_next_due();
    int64_t gap = next - now;
    bool starving = s_clk.running && now - s_touch_last > TOUCH_STARVE_US;
    if (gap > gt911_poll_budget_us() || starving) {
        gt911_poll(now);
        s_touch_last = now;
        s_touch_next = now + TOUCH_PERIOD_US;
        return s_touch_next;
    }
    return next + 1;   // right after the next deadline
}

// --- the task --------------------------------------------------------------------------------

static void rt_task(void *arg) {
    timer_init();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   // board has external pull-ups
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    tclock_init(&s_clk, g_cfg.bpm_x100);
    tclock_sink_t hs = {NULL, h_start, h_cont, h_stop, h_locate, h_pulse, h_tempo};
    tclock_add_sink(&s_clk, &hs);
    tclock_set_out(&s_clk, clock_out_fn, NULL);
    tclock_set_ext(&s_clk, g_cfg.clock_src == CLOCK_SRC_MIDI, now_us());
    tclock_set_clock_out(&s_clk, g_cfg.clock_out, now_us());
    s_clk.mmc_out = g_cfg.mmc;
    publish();
    s_ready = true;   // the time base runs: the rest of the system can start

    // GT911 reset sequence (~1.2 s); commands queue up meanwhile
    s_touch_ok = gt911_init(s_bus, TOUCH_I2C_HZ);
    s_touch_next = now_us();
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i]->rt_init) g_mods[i]->rt_init();
    midi_io_start();   // once the loop is about to run: no backlog of stale clocks
    ESP_LOGI(TAG, "real-time task on core %d, prio %d", xPortGetCoreID(), RT_PRIORITY);

    for (;;) {
        rt_msg_t cm;
        while (xQueueReceive(s_cmdq, &cm, 0) == pdTRUE) handle_cmd(&cm, now_us());

        // engines' own events first (gate lows before a pulse at the same
        // time), then the master clock
        int64_t now = now_us();
        for (int i = 0; i < g_nmods; i++) {
            const tmod_t *m = g_mods[i];
            if (m->rt_advance && m->rt_next_due && m->rt_next_due() <= now + TC_LOOKAHEAD_US)
                m->rt_advance(now_us());
        }
        tclock_advance(&s_clk, now_us());

        // MIDI clock: everything due before it has been done above, so each
        // clock lands in order at its own arrival time
        midi_in_msg_t mm;
        while (xQueueReceive(s_midiq, &mm, 0) == pdTRUE)
            tclock_midi(&s_clk, mm.type, mm.arg, mm.mmc, mm.t);

        midi_out_msg_t om;
        while (xQueueReceive(s_outq, &om, 0) == pdTRUE) {
            if (om.t == 0) om.t = now_us();
            out_push(&om);
        }
        out_service(now_us());

        int64_t touch_due = touch_service(now_us());
        now = now_us();
        for (int i = 0; i < g_nmods; i++)
            if (g_mods[i]->rt_poll) g_mods[i]->rt_poll(now);
        publish();

        int64_t due = engines_next_due();
        if (touch_due < due) due = touch_due;
        wait_until(due);
    }
}

void rt_start(void) {
    s_cmdq = xQueueCreate(32, sizeof(rt_msg_t));
    s_midiq = xQueueCreate(64, sizeof(midi_in_msg_t));
    s_outq = xQueueCreate(64, sizeof(midi_out_msg_t));
    xTaskCreatePinnedToCore(rt_task, "rt", RT_STACK, NULL, RT_PRIORITY, &s_task, RT_CORE);
}

i2c_master_bus_handle_t studio_i2c_bus(void) { return s_bus; }

// --- public transport API ---------------------------------------------------------------------

void studio_play(void) { rt_post(HC_PLAY, 0, 0, 0); }
void studio_stop(void) { rt_post(HC_STOP, 0, 0, 0); }
void studio_toggle_play(void) { rt_post(HC_TOGGLE, 0, 0, 0); }
void studio_restart(void) { rt_post(HC_RESTART, 0, 0, 0); }

void studio_set_bpm_x100(int32_t v) {
    if (v < TC_MIN_BPM_X100) v = TC_MIN_BPM_X100;
    if (v > TC_MAX_BPM_X100) v = TC_MAX_BPM_X100;
    if (v == g_cfg.bpm_x100) return;
    g_cfg.bpm_x100 = v;
    rt_post(HC_BPM, 0, 0, v);
    cfg_changed();
}
int32_t studio_bpm_x100(void) { return g_cfg.bpm_x100; }

void studio_set_clock_src(int src) {
    g_cfg.clock_src = (uint8_t)src;
    rt_post(HC_CLOCK_SRC, 0, 0, src);
    cfg_changed();
}
int studio_clock_src(void) { return g_cfg.clock_src; }

void studio_set_clock_out(bool on) {
    g_cfg.clock_out = on;
    rt_post(HC_CLOCK_OUT, 0, 0, on);
    cfg_changed();
}
bool studio_clock_out(void) { return g_cfg.clock_out; }

void studio_set_mmc(bool on) {
    g_cfg.mmc = on;
    rt_post(HC_MMC, 0, 0, on);
    cfg_changed();
}
bool studio_mmc(void) { return g_cfg.mmc; }

bool studio_cmd(const tmod_t *m, uint8_t cmd, int32_t arg) {
    int i = studio_module_index(m);
    if (i < 0) return false;
    return rt_post(HC_MODULE, (uint8_t)i, cmd, arg);
}
