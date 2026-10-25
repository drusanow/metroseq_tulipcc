// cv_dac.h - Makerfabs Mabee DAC (GP8413) driver.
//
// Called ONLY from the sequencer task, which owns the shared I2C bus.
//
//   physical out 0 (TRK 1) -> DAC 0x59 : VOUT0 gate, VOUT1 pitch (1V/oct)
//   physical out 1 (TRK 2) -> DAC 0x58 : VOUT0 gate, VOUT1 pitch (optional;
//                                        a second Mabee with A0 link cut)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#define CV_NUM_DACS    2
#define CV_HIGH_V      5.0f    // gate high level (eurorack standard)
#define CV_MAX_V       10.0f   // DAC full scale
#define CV_MAX_FAILS   5       // consecutive I2C failures before a DAC is dropped

// Per-channel calibration: volts_out = volts * gain + offset. Measure with a
// meter (see docs) and edit cv_dac.c's table; defaults are ideal.
typedef struct {
    float gain;
    float offset_v;
} cv_cal_t;

void cv_dac_attach_bus(i2c_master_bus_handle_t bus, uint32_t scl_hz);

// Probe both addresses; returns number of DACs found. Writes 0V to both
// channels of each one found (the write is the probe, as in the Python).
int cv_dac_init(void);
void cv_dac_disable(void);

bool cv_dac_present(int out);

// Returns false on an I2C error (counted; the DAC is dropped after
// CV_MAX_FAILS consecutive errors).
bool cv_dac_gate_volts(int out, float volts);
bool cv_dac_gate(int out, bool high);
bool cv_dac_pitch_cents(int out, int32_t cents);   // midi * 100, 1V/oct

void cv_dac_all_low(void);

// Set to the out index when a DAC was just dropped (-1 otherwise); the
// sequencer task turns this into a header message.
int cv_dac_take_lost(void);
