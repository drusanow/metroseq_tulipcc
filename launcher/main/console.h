// console.h - the debug REPL.
//
// One set of commands, reachable two ways: over the USB serial port (a
// normal esp_console REPL, 115200 baud) and from the CONSOLE page on the
// screen. Everything printed - command output and the system log - also
// goes into a ring buffer the CONSOLE page shows.
#pragma once

#include <stddef.h>
#include <stdint.h>

void console_init(void);   // registers commands, starts the UART REPL

// Queue a command line typed on the screen (runs in the console task).
void console_submit(const char *line);

// Read console text written since *cursor (advanced). Returns bytes copied.
size_t console_text_read(uint32_t *cursor, char *out, size_t max);

// printf into the console (UART + screen).
void con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
