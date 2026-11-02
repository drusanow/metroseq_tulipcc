// midi_parse.c - MIDI IN byte parser (see midi_parse.h).
#include "studio/midi_parse.h"

#include <string.h>

void mp_init(midi_parser_t *p) { memset(p, 0, sizeof *p); }

static int data_len(uint8_t status) {
    switch (status & 0xF0) {
    case 0xC0:
    case 0xD0:
        return 1;
    default:
        return 2;
    }
}

// F0 7F <device> 06 <command> [...] F7
static mp_ev_t sysex_end(midi_parser_t *p) {
    mp_ev_t ev = {0};
    const uint8_t *s = p->sx;
    if (p->sx_over || p->sx_n < 5 || s[0] != 0xF0 || s[1] != 0x7F || s[3] != 0x06) return ev;
    ev.type = MP_MMC;
    ev.d[0] = s[4];
    // LOCATE: F0 7F dev 06 44 06 01 hh mm ss ff sf F7
    if (s[4] == MMC_LOCATE) {
        if (p->sx_n >= 11 && s[5] == 0x06 && s[6] == 0x01) {
            ev.d[1] = s[7] & 0x1F;   // the top bits of hh are the frame rate
            ev.d[2] = s[8];
            ev.d[3] = s[9];
            ev.d[4] = s[10];
        } else {
            ev.type = MP_NONE;
        }
    }
    return ev;
}

mp_ev_t mp_byte(midi_parser_t *p, uint8_t b) {
    mp_ev_t ev = {0};
    if (b >= 0xF8) {   // real-time: single byte, may interrupt anything
        switch (b) {
        case 0xF8: ev.type = MP_CLOCK; break;
        case 0xFA: ev.type = MP_START; break;
        case 0xFB: ev.type = MP_CONTINUE; break;
        case 0xFC: ev.type = MP_STOP; break;
        }
        return ev;
    }
    if (b & 0x80) {   // a status byte ends whatever came before it
        bool was_sysex = p->in_sysex;
        p->in_sysex = false;
        p->status = 0;
        p->need = p->have = 0;
        if (b == 0xF7) {
            if (was_sysex && !p->sx_over && p->sx_n < MP_SYSEX_MAX) p->sx[p->sx_n++] = b;
            return was_sysex ? sysex_end(p) : ev;
        }
        if (b == 0xF0) {
            p->in_sysex = true;
            p->sx_n = 0;
            p->sx_over = false;
            p->sx[p->sx_n++] = b;
            p->running = 0;
            return ev;
        }
        if (b >= 0xF0) {   // other system common: clears running status
            p->running = 0;
            if (b == 0xF2) {
                p->status = b;
                p->need = 2;
            } else if (b == 0xF1 || b == 0xF3) {
                p->status = b;   // data skipped
                p->need = 1;
            }
            return ev;
        }
        p->running = b;
        p->status = b;
        p->need = (uint8_t)data_len(b);
        return ev;
    }
    // data byte
    if (p->in_sysex) {
        if (p->sx_n < MP_SYSEX_MAX)
            p->sx[p->sx_n++] = b;
        else
            p->sx_over = true;
        return ev;
    }
    if (!p->status) {
        if (!p->running) return ev;   // stray data
        p->status = p->running;       // running status
        p->need = (uint8_t)data_len(p->running);
        p->have = 0;
    }
    p->data[p->have++] = b;
    if (p->have < p->need) return ev;
    uint8_t st = p->status;
    if (st == 0xF2) {
        ev.type = MP_SPP;
        ev.arg = (uint16_t)(p->data[0] | (p->data[1] << 7));
    } else if (st < 0xF0) {
        ev.type = MP_CHANNEL;
        ev.len = (uint8_t)(1 + p->need);
        ev.b[0] = st;
        ev.b[1] = p->data[0];
        ev.b[2] = p->need > 1 ? p->data[1] : 0;
    }
    p->status = 0;   // next data byte uses running status
    p->need = p->have = 0;
    return ev;
}
