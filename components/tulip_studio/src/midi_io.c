// midi_io.c - MIDI IN (timestamped per byte) and MIDI OUT on the Tulip's
// MIDI ports: UART1, 31250 baud, IN on GPIO12, OUT on GPIO11.
//
// IN asks the driver for ONE byte with no timeout, so the task wakes as
// soon as each byte lands (tens of microseconds, not AMY's 1 ms poll).
// Clock / transport / MMC go to the rt task with their arrival times;
// channel messages go straight to every running app's midi() hook.
// OUT is written only by the rt task, so bytes leave in time order.
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "studio_priv.h"
#include "tulip/board.h"

static const char *TAG = "midi";

#define MIDI_UART   UART_NUM_1
#define MIDI_CORE   1
#define MIDI_PRIO   (configMAX_PRIORITIES - 3)   // just below the rt task
#define BYTE_US     320                          // 10 bits at 31250 baud

static volatile bool s_up;
static portMUX_TYPE s_mon_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_mon[40] = "--";
static volatile uint32_t s_mon_seq;

void midi_monitor_set(const char *txt) {
    taskENTER_CRITICAL(&s_mon_mux);
    strlcpy(s_mon, txt, sizeof s_mon);
    s_mon_seq++;
    taskEXIT_CRITICAL(&s_mon_mux);
}

uint32_t studio_midi_monitor(char *buf, int len) {
    taskENTER_CRITICAL(&s_mon_mux);
    strlcpy(buf, s_mon, len);
    uint32_t s = s_mon_seq;
    taskEXIT_CRITICAL(&s_mon_mux);
    return s;
}

static void monitor(const mp_ev_t *ev) {
    char t[40];
    switch (ev->type) {
    case MP_START: midi_monitor_set("MIDI Start"); return;
    case MP_CONTINUE: midi_monitor_set("MIDI Continue"); return;
    case MP_STOP: midi_monitor_set("MIDI Stop"); return;
    case MP_SPP:
        snprintf(t, sizeof t, "Song Position %u", ev->arg);
        break;
    case MP_MMC:
        snprintf(t, sizeof t, "MMC %s", ev->d[0] == MMC_PLAY ? "Play"
                                      : ev->d[0] == MMC_STOP ? "Stop"
                                      : ev->d[0] == MMC_LOCATE ? "Locate" : "cmd");
        break;
    case MP_CHANNEL: {
        int ch = (ev->b[0] & 15) + 1;
        switch (ev->b[0] & 0xF0) {
        case 0x90:
            if (ev->b[2])
                snprintf(t, sizeof t, "ch%d note %d v%d", ch, ev->b[1], ev->b[2]);
            else   // velocity 0 = note off
                snprintf(t, sizeof t, "ch%d note off %d", ch, ev->b[1]);
            break;
        case 0x80: snprintf(t, sizeof t, "ch%d note off %d", ch, ev->b[1]); break;
        case 0xB0: snprintf(t, sizeof t, "ch%d CC%d = %d", ch, ev->b[1], ev->b[2]); break;
        case 0xC0: snprintf(t, sizeof t, "ch%d PC %d", ch, ev->b[1]); break;
        default: return;
        }
        break;
    }
    default:
        return;   // clocks: too many to show
    }
    midi_monitor_set(t);
}

void midi_io_write(const uint8_t *b, int n) {
    if (s_up) uart_write_bytes(MIDI_UART, (const char *)b, n);
}

static void midi_task(void *arg) {
    uart_config_t cfg = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // installed from this task so the UART interrupt is on core 1 too
    if (uart_driver_install(MIDI_UART, 512, 512, 0, NULL, 0) != ESP_OK ||
        uart_param_config(MIDI_UART, &cfg) != ESP_OK ||
        uart_set_pin(MIDI_UART, PIN_MIDI_OUT, PIN_MIDI_IN, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "MIDI init failed");
        vTaskDelete(NULL);
    }
    uart_set_rx_full_threshold(MIDI_UART, 1);   // interrupt on every byte
    uart_set_rx_timeout(MIDI_UART, 1);
    s_up = true;
    ESP_LOGI(TAG, "MIDI IN on GPIO%d, OUT on GPIO%d", PIN_MIDI_IN, PIN_MIDI_OUT);

    midi_parser_t parser;
    mp_init(&parser);
    uint8_t buf[32];
    for (;;) {
        // Block for the first byte, then take whatever else is buffered.
        // Bytes that arrived together are back-dated one byte time apart.
        int n = uart_read_bytes(MIDI_UART, buf, 1, portMAX_DELAY);
        if (n <= 0) continue;
        int64_t t = studio_now_us();
        int more = uart_read_bytes(MIDI_UART, buf + 1, sizeof buf - 1, 0);
        if (more > 0) n += more;
        for (int i = 0; i < n; i++) {
            mp_ev_t ev = mp_byte(&parser, buf[i]);
            if (ev.type == MP_NONE) continue;
            int64_t tb = t - (int64_t)(n - 1 - i) * BYTE_US;
            if (ev.type == MP_CHANNEL) {
                for (int m = 0; m < g_nmods; m++)
                    if (g_enabled[m] && g_mods[m]->midi) g_mods[m]->midi(ev.b, ev.len, tb);
            } else {
                rt_post_midi_in(&ev, tb);
            }
            monitor(&ev);
        }
    }
}

void midi_io_start(void) {
    xTaskCreatePinnedToCore(midi_task, "midi", 4096, NULL, MIDI_PRIO, NULL, MIDI_CORE);
}
