// midi_clock.c - MIDI real-time parser and clock follower (see header).
#include "midi_clock.h"

#include <string.h>

// --- parser ------------------------------------------------------------------

void midi_parser_init(midi_parser_t *p) {
    memset(p, 0, sizeof *p);
}

midi_ev_t midi_parse_byte(midi_parser_t *p, uint8_t b) {
    midi_ev_t ev = {MIDI_EV_NONE, 0};
    if (b >= 0xF8) {   // real-time: single byte, may interrupt anything
        switch (b) {
        case 0xF8: ev.type = MIDI_EV_CLOCK; break;
        case 0xFA: ev.type = MIDI_EV_START; break;
        case 0xFB: ev.type = MIDI_EV_CONTINUE; break;
        case 0xFC: ev.type = MIDI_EV_STOP; break;
        }
        return ev;
    }
    if (b & 0x80) {   // any other status byte ends what came before
        p->in_sysex = (b == 0xF0);
        p->status = 0;
        p->need = 0;
        p->have = 0;
        if (b == 0xF2) {   // song position pointer: 2 data bytes
            p->status = b;
            p->need = 2;
        }
        // channel messages and the other system-common messages are not
        // needed by the sequencer; their data bytes are skipped below
        return ev;
    }
    // data byte
    if (p->in_sysex || !p->status) return ev;
    p->data[p->have++] = b;
    if (p->have == p->need) {
        if (p->status == 0xF2) {
            ev.type = MIDI_EV_SPP;
            ev.arg = (uint16_t)(p->data[0] | (p->data[1] << 7));
        }
        p->status = 0;
        p->need = 0;
        p->have = 0;
    }
    return ev;
}

// --- tempo estimate ----------------------------------------------------------

static void tempo_reset(midi_clock_t *m) {
    m->n = 0;
    m->idx = 0;
    m->sum = 0;
    m->outliers = 0;
    m->last_clock_us = 0;
}

static void tempo_add(midi_clock_t *m, int32_t iv) {
    if (m->n == MIDI_TEMPO_WINDOW) {
        m->sum -= m->iv[m->idx];
    } else {
        m->n++;
    }
    m->iv[m->idx] = iv;
    m->sum += iv;
    m->idx = (m->idx + 1) % MIDI_TEMPO_WINDOW;
}

static void tempo_clock(midi_clock_t *m, int64_t t) {
    if (m->last_clock_us) {
        int64_t iv = t - m->last_clock_us;
        // 2 ms .. 250 ms per clock = 1250 .. 10 BPM
        if (iv >= 2000 && iv <= 250000) {
            int64_t avg = m->n ? m->sum / m->n : iv;
            if (m->n >= 4 && (iv > 2 * avg || 2 * iv < avg)) {
                // a lone hiccup is ignored; three in a row is a new tempo
                if (++m->outliers >= 3) {
                    tempo_reset(m);
                    tempo_add(m, (int32_t)iv);
                }
            } else {
                m->outliers = 0;
                tempo_add(m, (int32_t)iv);
            }
        }
    }
    m->last_clock_us = t;
}

int64_t midi_clock_p16_us(const midi_clock_t *m) {
    return m->n ? (m->sum * MIDI_CLOCKS_PER_16TH) / m->n : 0;
}

int midi_clock_bpm_x10(const midi_clock_t *m) {
    if (!m->n) return 0;
    // bpm = 60e6 / (avg clock * 24)
    return (int)((600000000LL * m->n) / (m->sum * MIDI_PPQN));
}

bool midi_clock_alive(midi_clock_t *m, int64_t now) {
    if (m->last_clock_us && now - m->last_clock_us > MIDI_CLOCK_TIMEOUT_US) tempo_reset(m);
    return m->last_clock_us != 0;
}

// --- transport -----------------------------------------------------------------

void midi_clock_init(midi_clock_t *m) {
    memset(m, 0, sizeof *m);
    m->at_start = true;
}

void midi_clock_arm(midi_clock_t *m) {
    m->armed = true;
}

void midi_clock_disarm(midi_clock_t *m) {
    m->armed = false;
}

void midi_clock_event(midi_clock_t *m, seq_engine_t *e, const midi_ev_t *ev, int64_t t,
                      seq_actions_t *out) {
    switch (ev->type) {
    case MIDI_EV_CLOCK:
        tempo_clock(m, t);
        seq_engine_ext_tempo(e, midi_clock_p16_us(m));
        if (m->armed) {
            // the first clock after START is the downbeat
            m->armed = false;
            m->at_start = false;
            m->clocks = 0;
            seq_engine_start(e, t, out);
        } else if (e->playing) {
            if (++m->clocks % MIDI_CLOCKS_PER_16TH == 0) seq_engine_ext_pulse(e, t, out);
        }
        break;
    case MIDI_EV_START:
        if (e->playing) seq_engine_stop(e, t, out);
        m->armed = true;
        break;
    case MIDI_EV_CONTINUE:
        if (e->playing) break;
        if (m->at_start) {
            m->armed = true;   // continuing from the top is a start
        } else {
            seq_engine_continue(e, t, out);
        }
        break;
    case MIDI_EV_STOP:
        m->armed = false;
        if (e->playing) seq_engine_stop(e, t, out);
        break;
    case MIDI_EV_SPP:
        if (e->playing) break;   // the spec only allows SPP while stopped
        if (ev->arg == 0) {
            m->at_start = true;
        } else {
            // the next clock after CONTINUE starts 1/16 number `arg`
            seq_engine_locate(e, t, ev->arg);
            m->clocks = (uint32_t)ev->arg * MIDI_CLOCKS_PER_16TH - 1;
            m->at_start = false;
        }
        break;
    }
}
