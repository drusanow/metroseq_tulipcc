// gt911.h - minimal GT911 touch driver.
//
// The touch controller sits on the SAME I2C bus as the Mabee DACs, so it is
// polled by the sequencer task (the bus owner) in the gaps between CV
// writes - a touch read can never delay a gate. Touch events are handed to
// LVGL through a queue, so a quick tap is never lost even if the UI is busy
// redrawing when it happens, and the LVGL task is woken the moment one
// arrives instead of waiting for its next frame.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch.h"

// Boot-time only (blocking reset sequence, ~1.2 s).
bool gt911_init(i2c_master_bus_handle_t bus, uint32_t scl_hz);

// One poll of the controller; queues an event when the state changes.
// Returns false on I2C error.
bool gt911_poll(int64_t now_us);

// Worst-case bus time of one gt911_poll() at the configured clock.
int64_t gt911_poll_budget_us(void);

// Consumer side (LVGL task).
void touch_set_listener(TaskHandle_t task);   // notified on every new event
bool touch_next_event(touch_event_t *ev);
bool touch_events_pending(void);
