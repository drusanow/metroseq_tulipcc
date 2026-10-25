// gt911.c - GT911 polling over i2c_master (reset sequence and calibration
// from Tulip's gt911_touchscreen.c for the R11 board).
#include "gt911.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define REG_PRODUCT_ID  0x8140
#define REG_STATUS      0x814E
#define REG_POINT1      0x814F
#define I2C_TIMEOUT_MS  5
#define HELD_TIMEOUT_US 150000   // no fresh sample for this long => lifted

static i2c_master_dev_handle_t s_dev;
static uint32_t s_hz;
static QueueHandle_t s_q;
static bool s_held;
static int16_t s_x, s_y;
static int64_t s_last_sample;

static esp_err_t rd(uint16_t reg, uint8_t *buf, size_t n) {
    uint8_t r[2] = {reg >> 8, reg & 0xff};
    return i2c_master_transmit_receive(s_dev, r, 2, buf, n, I2C_TIMEOUT_MS);
}

static esp_err_t wr8(uint16_t reg, uint8_t v) {
    uint8_t b[3] = {reg >> 8, reg & 0xff, v};
    return i2c_master_transmit(s_dev, b, 3, I2C_TIMEOUT_MS);
}

static void push(bool pressed) {
    touch_event_t ev = {.x = s_x, .y = s_y, .pressed = pressed};
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        // queue full: drop the oldest so the newest state always arrives
        touch_event_t old;
        xQueueReceive(s_q, &old, 0);
        xQueueSend(s_q, &ev, 0);
    }
}

bool gt911_init(i2c_master_bus_handle_t bus, uint32_t scl_hz) {
    s_hz = scl_hz;
    if (!s_q) s_q = xQueueCreate(32, sizeof(touch_event_t));

    // R11 needs the reset pin toggled twice before the controller answers
    gpio_config_t io = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_TOUCH_RST};
    gpio_config(&io);
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(11));
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(11));
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));

    const uint8_t addrs[2] = {TOUCH_ADDR_A, TOUCH_ADDR_B};
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(bus, addrs[i], 20) != ESP_OK) continue;
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = scl_hz,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
        uint8_t id[4] = {0};
        rd(REG_PRODUCT_ID, id, 3);
        ESP_LOGI(TAG, "GT%c%c%c at 0x%02x", id[0], id[1], id[2], addrs[i]);
        wr8(REG_STATUS, 0);
        return true;
    }
    ESP_LOGE(TAG, "touchscreen not found at 0x5D or 0x14 - check the cable");
    return false;
}

int64_t gt911_poll_budget_us(void) {
    // status read + 8-byte point read + status clear is ~200 bit times;
    // allow 2x for clock stretching and driver overhead.
    return 2 * (200LL * 1000000 / (s_hz ? s_hz : 100000)) + 200;
}

bool gt911_poll(int64_t now_us) {
    if (!s_dev) return false;
    uint8_t st = 0;
    if (rd(REG_STATUS, &st, 1) != ESP_OK) return false;
    if (!(st & 0x80)) {
        // no new scan yet; guard against a lost release report
        if (s_held && now_us - s_last_sample > HELD_TIMEOUT_US) {
            s_held = false;
            push(false);
        }
        return true;
    }
    s_last_sample = now_us;
    int n = st & 0x0f;
    if (n > 0 && n <= 5) {
        uint8_t p[8];
        if (rd(REG_POINT1, p, sizeof p) != ESP_OK) {
            wr8(REG_STATUS, 0);
            return false;
        }
        int x = (p[1] | (p[2] << 8)) + TOUCH_X_DELTA;
        int y = ((p[3] | (p[4] << 8)) + TOUCH_Y_DELTA) * TOUCH_Y_SCALE_NUM / TOUCH_Y_SCALE_DEN;
        // clamp edge touches onto the screen instead of dropping them
        x = x < 0 ? 0 : (x >= LCD_H_RES ? LCD_H_RES - 1 : x);
        y = y < 0 ? 0 : (y >= LCD_V_RES ? LCD_V_RES - 1 : y);
        if (!s_held || x != s_x || y != s_y) {
            s_x = x;
            s_y = y;
            s_held = true;
            push(true);
        }
    } else if (s_held) {
        s_held = false;
        push(false);
    }
    wr8(REG_STATUS, 0);
    return true;
}

bool touch_next_event(touch_event_t *ev) {
    return s_q && xQueueReceive(s_q, ev, 0) == pdTRUE;
}

bool touch_events_pending(void) {
    return s_q && uxQueueMessagesWaiting(s_q) > 0;
}
