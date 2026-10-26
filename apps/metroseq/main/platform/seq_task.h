// seq_task.h - the real-time sequencer task (pinned to core 1).
#pragma once

// Creates the sequencer task. It owns: the engine runtime, the hardware
// timer, the shared I2C bus, the Mabee DACs and the GT911 touch controller.
void seq_task_start(void);

#include <stdint.h>

// The sequencer's clock (us since boot, 1 MHz GPTimer). Any task.
int64_t seq_now_us(void);

// Hand a parsed MIDI clock/transport event (midi_ev_type_t) that arrived
// at time t to the sequencer. Called by the MIDI IN task.
void seq_midi_post(uint8_t type, uint16_t arg, int64_t t);
