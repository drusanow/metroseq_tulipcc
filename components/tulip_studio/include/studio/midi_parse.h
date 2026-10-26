// midi_parse.h - MIDI IN byte parser for the Studio runtime.
//
// Recognises everything the apps need: real-time (clock, start, continue,
// stop) anywhere in the stream, song position, channel voice messages with
// running status, and MIDI Machine Control (an MPC's transport) inside
// SysEx. Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_NONE = 0,
    MP_CLOCK,      // 0xF8
    MP_START,      // 0xFA
    MP_CONTINUE,   // 0xFB
    MP_STOP,       // 0xFC
    MP_SPP,        // 0xF2: arg = position in 1/16ths
    MP_CHANNEL,    // b[0..len-1] = status + data (running status expanded)
    MP_MMC,        // d[0] = command; LOCATE: d[1..4] = hh mm ss ff
} mp_type_t;

// MMC commands
#define MMC_STOP          0x01
#define MMC_PLAY          0x02
#define MMC_DEFERRED_PLAY 0x03
#define MMC_REWIND        0x05
#define MMC_PAUSE         0x09
#define MMC_LOCATE        0x44

typedef struct {
    uint8_t type;      // mp_type_t
    uint8_t len;       // MP_CHANNEL: 2 or 3
    uint16_t arg;      // MP_SPP
    uint8_t b[3];      // MP_CHANNEL
    uint8_t d[5];      // MP_MMC
} mp_ev_t;

#define MP_SYSEX_MAX 16   // longer SysEx is skipped (MMC needs 13 bytes)

typedef struct {
    uint8_t running;   // running status (channel message), 0 = none
    uint8_t status;    // message being collected
    uint8_t need, have;
    uint8_t data[2];
    bool in_sysex;
    uint8_t sx[MP_SYSEX_MAX];
    uint8_t sx_n;
    bool sx_over;
} midi_parser_t;

void mp_init(midi_parser_t *p);
// Feed one byte; returns an event (MP_NONE while a message is incomplete).
mp_ev_t mp_byte(midi_parser_t *p, uint8_t b);

#ifdef __cplusplus
}
#endif
