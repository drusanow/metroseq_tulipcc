// cv_dac.c - GP8413 over the new ESP-IDF i2c_master driver.
//
// Same protocol as the Python _Mabee class and Tulip's mabeedac.py: a
// 16-bit value, low byte first, register 0x02 = VOUT0, 0x04 = VOUT1. The
// GP8413 is 15-bit; the top 15 bits of the value are used.
#include "cv_dac.h"

#include "engine/seq_model.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "cv";

#define GATE_REG      0x02   // VOUT0
#define PITCH_REG     0x04   // VOUT1
#define RANGE_REG     0x01
#define RANGE_10V     0x11
#define I2C_TIMEOUT_MS 5

static const uint8_t ADDR[CV_NUM_DACS] = {0x59, 0x58};

// [dac][channel 0 = gate, 1 = pitch]
static const cv_cal_t CAL[CV_NUM_DACS][2] = {
    {{1.0f, 0.0f}, {1.0f, 0.0f}},
    {{1.0f, 0.0f}, {1.0f, 0.0f}},
};

typedef struct {
    i2c_master_dev_handle_t dev;
    bool present;
    uint8_t fails;
    int32_t last_pitch_code;   // dedupe repeated pitch writes
} dac_t;

static i2c_master_bus_handle_t s_bus;
static uint32_t s_hz;
static dac_t s_dac[CV_NUM_DACS];
static int s_lost = -1;

void cv_dac_attach_bus(i2c_master_bus_handle_t bus, uint32_t scl_hz) {
    s_bus = bus;
    s_hz = scl_hz;
}

static uint16_t volts_to_code(int out, int ch, float volts) {
    const cv_cal_t *c = &CAL[out][ch];
    float v = volts * c->gain + c->offset_v;
    float x = (v / CV_MAX_V) * 65535.0f + 0.5f;
    if (x < 0.0f) x = 0.0f;
    if (x > 65535.0f) x = 65535.0f;
    return (uint16_t)x;
}

static bool write_reg(int out, uint8_t reg, uint16_t code) {
    dac_t *d = &s_dac[out];
    if (!d->present) return false;
    uint8_t buf[3] = {reg, code & 0xff, code >> 8};
    esp_err_t err = i2c_master_transmit(d->dev, buf, sizeof buf, I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        d->fails = 0;
        return true;
    }
    if (++d->fails >= CV_MAX_FAILS) {
        ESP_LOGW(TAG, "TRK %d DAC lost (%s) - output disabled", out + 1, esp_err_to_name(err));
        d->present = false;
        s_lost = out;
    }
    return false;
}

int cv_dac_init(void) {
    int found = 0;
    for (int i = 0; i < CV_NUM_DACS; i++) {
        dac_t *d = &s_dac[i];
        if (!d->dev) {
            i2c_device_config_t cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = ADDR[i],
                .scl_speed_hz = s_hz,
            };
            if (i2c_master_bus_add_device(s_bus, &cfg, &d->dev) != ESP_OK) continue;
        }
        d->present = true;
        d->fails = 0;
        d->last_pitch_code = -1;
#if CONFIG_METRO_CV_SET_RANGE_10V
        uint8_t rb[2] = {RANGE_REG, RANGE_10V};
        i2c_master_transmit(d->dev, rb, sizeof rb, I2C_TIMEOUT_MS);
#endif
        // a real write is the probe
        if (write_reg(i, GATE_REG, 0) && write_reg(i, PITCH_REG, 0)) {
            found++;
            ESP_LOGI(TAG, "DAC 0x%02x ready -> TRK %d (VOUT0 gate, VOUT1 pitch)", ADDR[i], i + 1);
        } else {
            d->present = false;
            if (i == 0) ESP_LOGW(TAG, "no DAC at 0x%02x (TRK1)", ADDR[i]);
        }
    }
    s_lost = -1;
    return found;
}

void cv_dac_disable(void) {
    cv_dac_all_low();
    for (int i = 0; i < CV_NUM_DACS; i++) s_dac[i].present = false;
}

bool cv_dac_present(int out) {
    return out >= 0 && out < CV_NUM_DACS && s_dac[out].present;
}

bool cv_dac_gate_volts(int out, float volts) {
    return write_reg(out, GATE_REG, volts_to_code(out, 0, volts));
}

bool cv_dac_gate(int out, bool high) {
    return cv_dac_gate_volts(out, high ? CV_HIGH_V : 0.0f);
}

bool cv_dac_pitch_cents(int out, int32_t cents) {
    if (!cv_dac_present(out)) return false;
    // 1V/oct referenced to CV_V0_MIDI (0V); the GP8413 is unipolar
    float volts = (float)(cents - CV_V0_MIDI * 100) / 1200.0f;
    if (volts < 0.0f) volts = 0.0f;
    if (volts > CV_MAX_V) volts = CV_MAX_V;
    uint16_t code = volts_to_code(out, 1, volts);
    if (code == s_dac[out].last_pitch_code) return true;   // no bus time for no change
    bool ok = write_reg(out, PITCH_REG, code);
    s_dac[out].last_pitch_code = ok ? code : -1;
    return ok;
}

void cv_dac_all_low(void) {
    for (int i = 0; i < CV_NUM_DACS; i++)
        if (s_dac[i].present) write_reg(i, GATE_REG, 0);
}

int cv_dac_take_lost(void) {
    int l = s_lost;
    s_lost = -1;
    return l;
}
