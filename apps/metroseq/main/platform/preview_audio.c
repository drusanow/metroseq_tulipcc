// preview_audio.c - two-voice preview synth on the Tulip's I2S DAC.
//
// Mirrors the Python audio_init() voice: downward saw, low-pass at 2.5 kHz
// (resonance 1.2), envelope 1.0 -> 0.4 over 30 ms, 120 ms release, amp 0.4
// at velocity 0.5.
#include "preview_audio.h"

#include <math.h>
#include <string.h>

#include "tulip/board.h"
#include "driver/i2s_std.h"
#include "engine/seq_model.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SR          44100
#define BLOCK       256
#define AUDIO_CORE  0
#define AUDIO_PRIO  6

typedef struct {
    volatile int32_t note;    // midi, or -1 for off
    volatile uint32_t gen;    // bumps on every note-on (retrigger)
    // render-side state
    uint32_t seen_gen;
    bool on;
    float phase, inc, env, lp, bp;
} voice_t;

static voice_t s_v[NUM_TRACKS];
static volatile bool s_enabled = true;
static i2s_chan_handle_t s_tx;

void preview_note_on(int track, int midi) {
    if (!s_enabled || track < 0 || track >= NUM_TRACKS) return;
    s_v[track].note = midi;
    s_v[track].gen++;
}

void preview_note_off(int track) {
    if (track >= 0 && track < NUM_TRACKS) s_v[track].note = -1;
}

void preview_set_enabled(bool on) {
    s_enabled = on;
    if (!on)
        for (int t = 0; t < NUM_TRACKS; t++) s_v[t].note = -1;
}

bool preview_enabled(void) { return s_enabled; }

static void audio_task(void *arg) {
    static int16_t out[BLOCK * 2];
    const float att = 1.0f / (0.030f * SR);          // 1.0 -> 0.4 in 30 ms
    const float rel = 0.4f / (0.120f * SR);          // 0.4 -> 0 in 120 ms
    const float f = 2.0f * sinf((float)M_PI * 2500.0f / SR);   // SVF cutoff
    const float q = 1.0f / 1.2f;
    for (;;) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            voice_t *v = &s_v[t];
            int32_t note = v->note;
            uint32_t gen = v->gen;
            if (note >= 0 && gen != v->seen_gen) {
                v->seen_gen = gen;
                v->on = true;
                v->env = 1.0f;
                v->inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / SR;
            } else if (note < 0) {
                v->on = false;
            }
        }
        for (int i = 0; i < BLOCK; i++) {
            float mix = 0.0f;
            for (int t = 0; t < NUM_TRACKS; t++) {
                voice_t *v = &s_v[t];
                if (v->on) {
                    if (v->env > 0.4f) v->env -= att * 0.6f;
                } else if (v->env > 0.0f) {
                    v->env -= rel;
                    if (v->env < 0.0f) v->env = 0.0f;
                }
                if (v->env <= 0.0f && !v->on) continue;
                v->phase += v->inc;
                if (v->phase >= 1.0f) v->phase -= 1.0f;
                float saw = 1.0f - 2.0f * v->phase;   // SAW_DOWN
                // Chamberlin state-variable low-pass
                v->lp += f * v->bp;
                float hp = saw - v->lp - q * v->bp;
                v->bp += f * hp;
                mix += v->lp * v->env * 0.2f;   // amp 0.4 * vel 0.5
            }
            if (mix > 1.0f) mix = 1.0f;
            if (mix < -1.0f) mix = -1.0f;
            int16_t s = (int16_t)(mix * 32000.0f);
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
        size_t written;
        i2s_channel_write(s_tx, out, sizeof out, &written, portMAX_DELAY);
    }
}

void preview_audio_start(void) {
    for (int t = 0; t < NUM_TRACKS; t++) s_v[t].note = -1;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = BLOCK;
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) return;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) return;
    i2s_channel_enable(s_tx);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, AUDIO_PRIO, NULL, AUDIO_CORE);
}
