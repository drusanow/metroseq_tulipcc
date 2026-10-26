// input.h - I2C bus + touch polling for apps without real-time I2C needs.
//
// Apps that must schedule the bus themselves (metroseq, which keeps CV
// writes on time by polling touch only in gaps) create the bus and call
// gt911_poll() on their own instead.
#pragma once

#include "driver/i2c_master.h"

// Creates the shared I2C bus (GPIO17/18), brings up the GT911 in the
// background and polls it every 10 ms from a task on `core`.
i2c_master_bus_handle_t tulip_input_start(int core, int priority);

// The shared bus created by tulip_input_start() (NULL before).
i2c_master_bus_handle_t tulip_i2c_bus(void);
