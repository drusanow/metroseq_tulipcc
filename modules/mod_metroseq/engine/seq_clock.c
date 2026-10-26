// seq_clock.c - master clock -> sequencer engine (see seq_clock.h).
#include "seq_clock.h"

static void begin(seq_clock_t *c) {
    if (c->lock) c->lock();
}

static void end(seq_clock_t *c) {
    if (c->unlock) c->unlock();
    if (c->flush) {
        c->flush(c->acts);
        seq_actions_clear(c->acts);
    }
}

static void s_start(void *ctx, int64_t t) {
    seq_clock_t *c = ctx;
    c->restart_pending = false;
    begin(c);
    seq_engine_start(c->e, t, c->acts);
    end(c);
}

static void s_cont(void *ctx, int64_t t) {
    seq_clock_t *c = ctx;
    begin(c);
    seq_engine_continue(c->e, t, c->acts);
    end(c);
}

static void s_stop(void *ctx, int64_t t) {
    seq_clock_t *c = ctx;
    c->restart_pending = false;
    begin(c);
    seq_engine_stop(c->e, t, c->acts);
    end(c);
}

static void s_locate(void *ctx, int64_t t, uint32_t pos16) {
    seq_clock_t *c = ctx;
    begin(c);
    seq_engine_locate(c->e, t, pos16);
    end(c);
}

static void s_pulse(void *ctx, int64_t t, uint32_t pos16) {
    seq_clock_t *c = ctx;
    (void)pos16;
    begin(c);
    if (c->restart_pending || !c->e->playing) {
        // RST / a new pattern, or switched on while the others play: the
        // tracks start from stage 1 on this 1/16, in step with everyone
        c->restart_pending = false;
        seq_engine_start(c->e, t, c->acts);
    } else {
        seq_engine_ext_pulse(c->e, t, c->acts);
    }
    end(c);
}

static void s_tempo(void *ctx, int64_t p16_us, int32_t bpm_x100) {
    seq_clock_t *c = ctx;
    seq_engine_ext_tempo_exact(c->e, p16_us, bpm_x100);
}

void seq_clock_sink(seq_clock_t *c, tclock_sink_t *out) {
    seq_engine_set_ext_clock(c->e, true);
    out->ctx = c;
    out->start = s_start;
    out->cont = s_cont;
    out->stop = s_stop;
    out->locate = s_locate;
    out->pulse = s_pulse;
    out->tempo = s_tempo;
}
