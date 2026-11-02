// touch_aim.c - see touch_aim.h.
#include "tulip/touch_aim.h"

#include <stdlib.h>

// the rings searched: close ones first and finely (a 6 px gap between
// buttons must find the nearer one), then every 4 px
static const uint8_t RINGS[] = {2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 36, 40};
#define DIRS 12

// unit directions every 30 degrees, x 1000
static const int16_t DX[DIRS] = {1000, 866, 500, 0, -500, -866, -1000, -866, -500, 0, 500, 866};
static const int16_t DY[DIRS] = {0, 500, 866, 1000, 866, 500, 0, -500, -866, -1000, -866, -500};

void touch_aim_init(touch_aim_t *a, aim_hit_fn hit, void *ctx, int w, int h) {
    *a = (touch_aim_t){.hit = hit, .ctx = ctx, .w = w, .h = h};
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool touch_aim_snap(const touch_aim_t *a, int x, int y, int r, int *ox, int *oy) {
    if (a->hit(x, y, a->ctx)) {
        *ox = x;
        *oy = y;
        return true;
    }
    for (unsigned k = 0; k < sizeof RINGS && RINGS[k] <= r; k++)
        for (int i = 0, d = RINGS[k]; i < DIRS; i++) {
            int px = x + DX[i] * d / 1000, py = y + DY[i] * d / 1000;
            if (px < 0 || py < 0 || px >= a->w || py >= a->h) continue;
            if (a->hit(px, py, a->ctx)) {
                *ox = px;
                *oy = py;
                return true;
            }
        }
    return false;
}

static aim_ev_t ev(const touch_aim_t *a, int x, int y, bool pressed) {
    return (aim_ev_t){(int16_t)clampi(x, 0, a->w - 1), (int16_t)clampi(y, 0, a->h - 1), pressed};
}

// from WAIT: press there, the finger driving it from now on
static aim_ev_t start_pass(touch_aim_t *a, int fx, int fy, int tx, int ty) {
    a->st = AIM_PASS;
    a->dx = (int16_t)(tx - fx);
    a->dy = (int16_t)(ty - fy);
    aim_ev_t e = ev(a, tx, ty, true);
    a->lx = e.x;
    a->ly = e.y;
    return e;
}

int touch_aim_feed(touch_aim_t *a, const aim_ev_t *in, uint32_t t_ms, aim_ev_t out[2]) {
    int x = in->x, y = in->y;
    switch (a->st) {
    case AIM_IDLE: {
        if (!in->pressed) return 0;
        int tx, ty;
        if (touch_aim_snap(a, x, y, AIM_SNAP_PX, &tx, &ty)) {
            out[0] = start_pass(a, x, y, tx, ty);
            return 1;
        }
        // on nothing: hold it back, and watch
        a->st = AIM_WAIT;
        a->x0 = a->lx = (int16_t)x;
        a->y0 = a->ly = (int16_t)y;
        a->t0 = t_ms;
        a->n = 1;
        a->sx = x;
        a->sy = y;
        a->tried = false;
        return 0;
    }
    case AIM_PASS:
        if (!in->pressed) {
            a->st = AIM_IDLE;
            out[0] = ev(a, a->lx, a->ly, false);
            return 1;
        }
        out[0] = ev(a, x + a->dx, y + a->dy, true);
        a->lx = out[0].x;
        a->ly = out[0].y;
        return 1;
    case AIM_WAIT:
        if (!in->pressed) {
            // a quick tap on nothing: it still happened, where it was
            a->st = AIM_IDLE;
            out[0] = ev(a, a->lx, a->ly, true);
            out[1] = ev(a, a->lx, a->ly, false);
            return 2;
        }
        a->lx = (int16_t)x;
        a->ly = (int16_t)y;
        if (abs(x - a->x0) > AIM_MOVE_PX || abs(y - a->y0) > AIM_MOVE_PX) {
            // a drag, not an aim: from here as it is
            a->st = AIM_PASS;
            a->dx = a->dy = 0;
            out[0] = ev(a, x, y, true);
            return 1;
        }
        if (a->n < AIM_SAMPLES) {
            a->n++;
            a->sx += x;
            a->sy += y;
        }
        return touch_aim_tick(a, t_ms, out);
    }
    return 0;
}

int touch_aim_tick(touch_aim_t *a, uint32_t t_ms, aim_ev_t out[1]) {
    if (a->st != AIM_WAIT || a->tried || t_ms - a->t0 < AIM_HOLD_MS) return 0;
    a->tried = true;   // once: then it waits for a move or the lift
    int mx = (int)(a->sx / a->n), my = (int)(a->sy / a->n);
    int tx, ty;
    if (!touch_aim_snap(a, mx, my, AIM_HOLD_SNAP_PX, &tx, &ty)) return 0;
    out[0] = start_pass(a, a->lx, a->ly, tx, ty);
    return 1;
}
