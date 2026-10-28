// touch_cal.c - least-squares affine fit for touch calibration.
#include "tulip/touch_cal.h"

#include <math.h>

// Solve the 3x3 system A*x = b by Cramer's rule; false if singular.
static bool solve3(const double A[3][3], const double b[3], double x[3]) {
    double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                 A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                 A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (fabs(det) < 1e-9) return false;
    for (int c = 0; c < 3; c++) {
        double M[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) M[i][j] = (j == c) ? b[i] : A[i][j];
        x[c] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) /
               det;
    }
    return true;
}

bool touch_cal_solve(const float *rx, const float *ry, const float *sx, const float *sy, int n,
                     float m[6], touch_cal_fit_t *fit) {
    if (n < 3 || n > TOUCH_CAL_MAX_POINTS) return false;
    // Normal equations: [Σx² Σxy Σx; Σxy Σy² Σy; Σx Σy n] [a b c]' = [Σx·X Σy·X ΣX]'
    // (computed about the mean for numerical stability)
    double mx = 0, my = 0;
    for (int i = 0; i < n; i++) {
        mx += rx[i];
        my += ry[i];
    }
    mx /= n;
    my /= n;
    double A[3][3] = {{0}}, bx[3] = {0}, by[3] = {0};
    for (int i = 0; i < n; i++) {
        double x = rx[i] - mx, y = ry[i] - my;
        double v[3] = {x, y, 1.0};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) A[r][c] += v[r] * v[c];
            bx[r] += v[r] * sx[i];
            by[r] += v[r] * sy[i];
        }
    }
    double px[3], py[3];
    if (!solve3(A, bx, px) || !solve3(A, by, py)) return false;
    // undo the centring: c' = c - a*mx - b*my
    m[0] = (float)px[0];
    m[1] = (float)px[1];
    m[2] = (float)(px[2] - px[0] * mx - px[1] * my);
    m[3] = (float)py[0];
    m[4] = (float)py[1];
    m[5] = (float)(py[2] - py[0] * mx - py[1] * my);

    // A real panel is close to axis-aligned with scale near 1: reject
    // anything wildly off (mirrored, collapsed, or a tap in the wrong place).
    double sxx = fabs(m[0]), syy = fabs(m[4]);
    if (sxx < 0.3 || sxx > 3.0 || syy < 0.3 || syy > 3.0) return false;
    if (fabs(m[1]) > 0.3 * sxx || fabs(m[3]) > 0.3 * syy) return false;

    double se = 0, worst = 0;
    for (int i = 0; i < n; i++) {
        float x, y;
        touch_cal_apply(m, rx[i], ry[i], &x, &y);
        double d = hypot(x - sx[i], y - sy[i]);
        se += d * d;
        if (d > worst) worst = d;
    }
    fit->rms_px = (float)sqrt(se / n);
    fit->max_px = (float)worst;
    return fit->rms_px <= TOUCH_CAL_MAX_RMS_PX && fit->max_px <= TOUCH_CAL_MAX_ERR_PX;
}

// --- the edge correction (see touch_cal.h) -------------------------------------------

bool touch_cal_solve_grid(const float *rx, const float *ry, int w, int h, touch_map_t *map,
                          touch_cal_fit_t *fit) {
    enum { N = TOUCH_GRID_N * TOUCH_GRID_N };
    float sx[N], sy[N];
    for (int r = 0; r < TOUCH_GRID_N; r++)
        for (int c = 0; c < TOUCH_GRID_N; c++) {
            sx[r * TOUCH_GRID_N + c] = (float)TOUCH_GRID_X(c, w);
            sy[r * TOUCH_GRID_N + c] = (float)TOUCH_GRID_Y(r, h);
        }
    *fit = (touch_cal_fit_t){0};
    map->grid = 0;
    // the affine part: as touch_cal_solve(), with the looser limits a
    // grid calibration allows
    if (!touch_cal_solve(rx, ry, sx, sy, N, map->m, fit) &&
        !(fit->rms_px > 0 && fit->rms_px <= TOUCH_GRID_MAX_RMS_PX && fit->max_px <= TOUCH_GRID_MAX_ERR_PX))
        return false;
    for (int i = 0; i < N; i++) {
        float x, y;
        touch_cal_apply(map->m, rx[i], ry[i], &x, &y);
        map->dx[i] = sx[i] - x;
        map->dy[i] = sy[i] - y;
        if (fabsf(map->dx[i]) > TOUCH_GRID_MAX_PX || fabsf(map->dy[i]) > TOUCH_GRID_MAX_PX) return false;
    }
    map->grid = 1;
    return true;
}

// where v falls between the grid lines a[0..2]: the cell and the fraction
// across it. Past the outer lines (the last 40 px to the edge) the outer
// cell's slope carries on a little way, as a panel's bend keeps growing.
static void grid_pos(float v, float a0, float a1, float a2, int *cell, float *t) {
    if (v <= a1) {
        *cell = 0;
        *t = (v - a0) / (a1 - a0);
        if (*t < -0.25f) *t = -0.25f;
        if (*t > 1) *t = 1;
    } else {
        *cell = 1;
        *t = (v - a1) / (a2 - a1);
        if (*t < 0) *t = 0;
        if (*t > 1.25f) *t = 1.25f;
    }
}

void touch_map_apply(const touch_map_t *map, int w, int h, float rx, float ry, float *x, float *y) {
    touch_cal_apply(map->m, rx, ry, x, y);
    if (!map->grid) return;
    int cx, cy;
    float tx, ty;
    grid_pos(*x, TOUCH_GRID_X(0, w), TOUCH_GRID_X(1, w), TOUCH_GRID_X(2, w), &cx, &tx);
    grid_pos(*y, TOUCH_GRID_Y(0, h), TOUCH_GRID_Y(1, h), TOUCH_GRID_Y(2, h), &cy, &ty);
    const int n = TOUCH_GRID_N;
    int i00 = cy * n + cx, i01 = i00 + 1, i10 = i00 + n, i11 = i10 + 1;
    float w00 = (1 - tx) * (1 - ty), w01 = tx * (1 - ty), w10 = (1 - tx) * ty, w11 = tx * ty;
    *x += w00 * map->dx[i00] + w01 * map->dx[i01] + w10 * map->dx[i10] + w11 * map->dx[i11];
    *y += w00 * map->dy[i00] + w01 * map->dy[i01] + w10 * map->dy[i10] + w11 * map->dy[i11];
}
