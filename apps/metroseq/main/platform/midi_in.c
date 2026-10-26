// midi_in.c - MIDI IN receiver with per-byte timestamps.
//
// UART settings are the same as AMY's MIDI input on the Tulip: UART1,
// 31250 baud 8N1, not inverted, RX interrupt on every byte. Unlike AMY's
// poll loop (uart_read_bytes(..., 1 ms timeout), which waits for a full
// buffer or the timeout), this asks for ONE byte with no timeout, so the
// task wakes as soon as each byte lands - tens of microseconds, not up to
// a millisecond.
#include "midi_in.h"

#include "tulip/board.h"
#include "driver/uart.h"
#include "engine/midi_clock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "seq_task.h"

static const char *TAG = "midi";

#define MIDI_UART       UART_NUM_1
#define MIDI_CORE       1
#define MIDI_PRIO       (configMAX_PRIORITIES - 3)   // just below the sequencer
#define BYTE_US         320                          // 10 bits at 31250 baud

static void midi_task(void *arg) {
    // Installed from this task so the UART interrupt is on core 1 too.
    uart_config_t cfg = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(MIDI_UART, 512, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(MIDI_UART, &cfg) != ESP_OK ||
        uart_set_pin(MIDI_UART, UART_PIN_NO_CHANGE, PIN_MIDI_IN, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "MIDI IN init failed");
        vTaskDelete(NULL);
    }
    uart_set_rx_full_threshold(MIDI_UART, 1);   // interrupt on every byte
    uart_set_rx_timeout(MIDI_UART, 1);
    ESP_LOGI(TAG, "MIDI IN on GPIO%d", PIN_MIDI_IN);

    midi_parser_t parser;
    midi_parser_init(&parser);
    uint8_t buf[32];
    for (;;) {
        // Block for the first byte, then take whatever else is already
        // buffered. Bytes that arrived together are back-dated one byte
        // time apart, so each keeps its own arrival time.
        int n = uart_read_bytes(MIDI_UART, buf, 1, portMAX_DELAY);
        if (n <= 0) continue;
        int64_t t = seq_now_us();
        int more = uart_read_bytes(MIDI_UART, buf + 1, sizeof buf - 1, 0);
        if (more > 0) n += more;
        for (int i = 0; i < n; i++) {
            midi_ev_t ev = midi_parse_byte(&parser, buf[i]);
            if (ev.type != MIDI_EV_NONE)
                seq_midi_post(ev.type, ev.arg, t - (int64_t)(n - 1 - i) * BYTE_US);
        }
    }
}

void midi_in_start(void) {
    xTaskCreatePinnedToCore(midi_task, "midi_in", 3072, NULL, MIDI_PRIO, NULL, MIDI_CORE);
}
