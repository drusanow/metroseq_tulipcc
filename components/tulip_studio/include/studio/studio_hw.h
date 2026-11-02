// studio_hw.h - hardware handles a module may need (rt task only).
#pragma once

#include "driver/i2c_master.h"

// The shared I2C bus (touch + CV DACs). Use only from rt_* hooks: the rt
// task is the bus's only user, which is what keeps CV writes on time.
i2c_master_bus_handle_t studio_i2c_bus(void);
