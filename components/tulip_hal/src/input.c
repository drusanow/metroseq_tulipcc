// input.c - shared I2C bus and a simple touch polling task.
#include "tulip/input.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tulip/board.h"
#include "tulip/gt911.h"

#define TOUCH_I2C_HZ 100000   // Tulip's touch bus speed on R11
#define POLL_MS 10            // GT911 scans at ~100 Hz

static i2c_master_bus_handle_t s_bus;

static void touch_task(void *arg) {
    gt911_init(s_bus, TOUCH_I2C_HZ);   // ~1.2 s reset sequence, off the UI's back
    for (;;) {
        if (gt911_ready()) gt911_poll(esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

i2c_master_bus_handle_t tulip_input_start(int core, int priority) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, priority, NULL, core);
    return s_bus;
}

i2c_master_bus_handle_t tulip_i2c_bus(void) { return s_bus; }
