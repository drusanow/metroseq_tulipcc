// midi_in.h - Tulip CC MIDI IN (5-pin DIN -> opto -> GPIO12, UART1).
//
// A small task on core 1 reads each byte the moment the UART interrupt
// delivers it, timestamps it on the sequencer clock, parses it, and posts
// only clock/transport events to the sequencer task.
#pragma once

void midi_in_start(void);
