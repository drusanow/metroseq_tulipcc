// touch_cal.c - least-squares affine fit for touch calibration.
#include "touch_cal.h"

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
