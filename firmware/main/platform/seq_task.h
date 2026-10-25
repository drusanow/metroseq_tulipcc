// seq_task.h - the real-time sequencer task (pinned to core 1).
#pragma once

// Creates the sequencer task. It owns: the engine runtime, the hardware
// timer, the shared I2C bus, the Mabee DACs and the GT911 touch controller.
void seq_task_start(void);
