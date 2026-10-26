// audio.c - the Studio audio engine: every running app's render() mixed
// into one stereo stream to the Tulip's I2S DAC (44.1 kHz, 16 bit).
//
// Small DMA ring (4 x 48 frames = 4.4 ms), task on core 1 below the rt
// task. Each block is rendered just before it's queued, so its first frame
// reaches the DAC about 4 ms later - and the engine knows WHEN, in clock
// microseconds: every block carries t0, the clock time its frame 0 is
// heard. That mapping is a phase-locked estimate (below), so an app can put
// a drum hit on the exact sample where the master clock says the step is.
//
// Sample clock vs clock: i2s_channel_write() returns the moment a DMA
// buffer has been played and refilled; at that instant the next block will
// start after the DESC buffers now queued. Each block measures that, and a
// slow first-order loop (1/16 per block) follows it: jitter in the task's
// wake-up is filtered out, and any rate difference between the I2S clock
// and the 1 MHz timer is tracked (it settles to a few us).
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "studio_priv.h"
#include "tulip/board.h"

static const char *TAG = "audio";

#define DESC        4
#define B           STUDIO_BLOCK
#define AUDIO_CORE  1
#define AUDIO_PRIO  (configMAX_PRIORITIES - 4)
#define RELOCK_US   2000     // an error bigger than this: re-lock at once

static i2s_chan_handle_t s_tx;
static float s_l[B], s_r[B];
static int16_t s_out[B * 2];
static volatile int s_load_pct;
static volatile uint32_t s_underruns;
static volatile float s_gain;
static volatile uint32_t s_blocks;    // blocks handed to the DAC so far
static volatile esp_err_t s_err;      // why audio couldn't start, if it couldn't
static const char *s_err_at = "";

static float vol_gain(int v) {
    if (v <= 0) return 0.0f;
    return powf(10.0f, (float)(v - 10) * 3.0f / 20.0f);   // 3 dB per step, 10 = 0 dB
}

void studio_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    g_cfg.volume = (uint8_t)v;
    s_gain = vol_gain(v);
    cfg_changed();
}

int studio_volume(void) { return g_cfg.volume; }
int studio_audio_load_pct(void) { return s_load_pct; }
uint32_t studio_audio_underruns(void) { return s_underruns; }

// Shown on screen: no sound is otherwise a silent failure. NULL = all well.
const char *studio_audio_problem(void) {
    static char b[64];
    static uint32_t last_blocks;
    static int64_t last_change;
    if (s_err != ESP_OK) {
        snprintf(b, sizeof b, "AUDIO OFF: %s %s", s_err_at, esp_err_to_name(s_err));
        return b;
    }
    int64_t now = esp_timer_get_time();
    uint32_t n = s_blocks;
    if (n != last_blocks || !last_change) {
        last_blocks = n;
        last_change = now;
        return NULL;
    }
    if (now - last_change > 500000) {   // ~450 blocks should have gone by
        snprintf(b, sizeof b, "AUDIO STALLED (%u blocks)", (unsigned)n);
        return b;
    }
    return NULL;
}

static bool IRAM_ATTR on_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx) {
    s_underruns++;
    return false;
}

// Gentle limiter: linear up to 0.8, then a soft knee into +-1.
static inline float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float z = (a - 0.8f) * 5.0f;
    float t = z >= 3.0f ? 1.0f : z * (27.0f + z * z) / (27.0f + 9.0f * z * z);
    float y = 0.8f + 0.2f * t;
    return x < 0 ? -y : y;
}

static void audio_task(void *arg) {
    // frame-accurate clock mapping: frame f plays at base_t + (f - base_f) / SR
    uint64_t frame = 0, base_f = 0;
    int64_t base_t = studio_now_us() + DESC * B * 1000000LL / STUDIO_SR;
    float base_frac = 0;
    float load = 0;
    for (;;) {
        studio_block_t blk = {
            .n = B,
            .t0 = base_t + (int64_t)((frame - base_f) * 1000000ULL / STUDIO_SR),
            .frame = frame,
        };
        int64_t c0 = studio_now_us();
        memset(s_l, 0, sizeof s_l);
        memset(s_r, 0, sizeof s_r);
        for (int i = 0; i < g_nmods; i++) {
            const tmod_t *m = g_mods[i];
            if (g_enabled[i] && m->render) m->render(s_l, s_r, &blk);
        }
        float g = s_gain;
        for (int i = 0; i < B; i++) {
            s_out[2 * i] = (int16_t)(soft_clip(s_l[i] * g) * 32000.0f);
            s_out[2 * i + 1] = (int16_t)(soft_clip(s_r[i] * g) * 32000.0f);
        }
        int64_t c1 = studio_now_us();
        load += ((float)(c1 - c0) * (100.0f * STUDIO_SR / 1e6f / B) - load) * 0.02f;
        s_load_pct = (int)(load + 0.5f);

        size_t written;
        esp_err_t e = i2s_channel_write(s_tx, s_out, sizeof s_out, &written, pdMS_TO_TICKS(100));
        if (e != ESP_OK) {
            // the DAC isn't taking data: say so, and don't spin on it
            static uint32_t nerr;
            if (nerr++ % 50 == 0) ESP_LOGE(TAG, "I2S write: %s", esp_err_to_name(e));
            vTaskDelay(1);
            continue;
        }
        int64_t tw = studio_now_us();
        frame += B;
        s_blocks++;

        // when the next block (frame) will be heard, measured vs predicted
        int64_t measured = tw + DESC * B * 1000000LL / STUDIO_SR;
        int64_t predicted = base_t + (int64_t)((frame - base_f) * 1000000ULL / STUDIO_SR);
        int64_t err = measured - predicted;
        if (err > RELOCK_US || err < -RELOCK_US) {
            base_t = measured;   // start-up, or after an underrun
            base_f = frame;
            base_frac = 0;
        } else {
            base_frac += (float)err / 16.0f;
            int64_t whole = (int64_t)base_frac;
            base_t += whole;
            base_frac -= (float)whole;
        }
        // keep the base recent so the multiply never overflows
        if (frame - base_f > (uint64_t)STUDIO_SR * 600) {
            base_t += (int64_t)((frame - base_f) * 1000000ULL / STUDIO_SR);
            base_f = frame;
        }
    }
}

static esp_err_t i2s_init(void) {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DESC;
    cc.dma_frame_num = B;
    cc.auto_clear = true;   // an underrun plays silence, not a stuck buffer
    esp_err_t e;
    s_err_at = "channel";
    if ((e = i2s_new_channel(&cc, &s_tx, NULL)) != ESP_OK) return e;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(STUDIO_SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    s_err_at = "std mode";
    if ((e = i2s_channel_init_std_mode(s_tx, &sc)) != ESP_OK) return e;
    i2s_event_callbacks_t cbs = {.on_send_q_ovf = on_ovf};
    i2s_channel_register_event_callback(s_tx, &cbs, NULL);
    s_err_at = "enable";
    return i2s_channel_enable(s_tx);
}

// I2S is set up from the audio task itself, so its interrupt is on core 1
// with the rest of the audio, away from the panel's on core 0.
static void audio_boot(void *arg) {
    esp_err_t e = i2s_init();
    if (e != ESP_OK) {
        s_err = e;
        ESP_LOGE(TAG, "I2S %s failed: %s - no audio", s_err_at, esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "%d Hz, %d x %d frames, BCLK %d WS %d DOUT %d, volume %d", STUDIO_SR, DESC, B,
             PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT, g_cfg.volume);
    audio_task(arg);
}

void audio_start(void) {
    s_gain = vol_gain(g_cfg.volume);
    if (xTaskCreatePinnedToCore(audio_boot, "audio", 8192, NULL, AUDIO_PRIO, NULL, AUDIO_CORE) !=
        pdPASS) {
        s_err = ESP_ERR_NO_MEM;
        s_err_at = "task";
        ESP_LOGE(TAG, "no memory for the audio task - no audio");
    }
}
