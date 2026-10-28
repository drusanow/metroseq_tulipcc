#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
enum { I2C_ADDR_BIT_LEN_7 = 0 };
typedef struct {
    int dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;
static inline esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c,
                                                  i2c_master_dev_handle_t *d) { return ESP_FAIL; }
static inline esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *b, int n, int t) {
    return ESP_FAIL;
}
