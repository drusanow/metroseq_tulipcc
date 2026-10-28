// harness.c - see harness.h. Also provides the touch-driver stubs the
// shared calibration screen needs (tulip/touch.h, tulip/gt911.h).
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "tulip/touch.h"
#include "tulip/touch_cal.h"

#define W 1024
#define H 600

static float s_cal[6] = TOUCH_CAL_DEFAULT;
void gt911_set_cal(const float m[6]) { memcpy(s_cal, m, sizeof s_cal); }
void gt911_get_cal(float m[6]) { memcpy(m, s_cal, sizeof s_cal); }
static touch_map_t s_map;
void gt911_set_map(const touch_map_t *m) { s_map = *m; }
void gt911_get_map(touch_map_t *m) { *m = s_map; }
void gt911_raw_max(int *x, int *y) { *x = 1024; *y = 750; }
bool gt911_ready(void) { return true; }
static touch_raw_hook_t s_hook;
void display_set_touch_hook(touch_raw_hook_t h) { s_hook = h; }

static uint8_t fb[W * H];   // RGB332, like the device frame buffer
static int tx, ty, tdown;
static uint32_t s_ms;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)px;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        uint8_t *dst = fb + y * W + a->x1;
        for (int x = 0; x < w; x++) {
            uint16_t c = *src++;
            dst[x] = (uint8_t)(((c >> 13) << 5) | (((c >> 8) & 7) << 2) | ((c >> 3) & 3));
        }
    }
    lv_display_flush_ready(d);
}

static void touch_cb(lv_indev_t *i, lv_indev_data_t *d) {
    d->point.x = tx;
    d->point.y = ty;
    d->state = tdown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick(void) { return s_ms; }

void (*hx_poll)(void);
uint32_t hx_now_ms(void) { return s_ms; }

void hx_pump(int ms) {
    for (int i = 0; i < ms; i += 5) {
        s_ms += 5;
        lv_timer_handler();
        if (hx_poll) hx_poll();
    }
}

// As the GT911 would report it with Tulip's default map, offered to the
// calibration hook first (display.c does the same on the device).
void hx_touch(int x, int y, int pressed) {
    touch_event_t ev = {.x = x, .y = y, .rx = x + 2, .ry = (int16_t)((y + 9.6f) / 0.8f),
                        .pressed = pressed};
    if (s_hook && s_hook(&ev)) {
        tdown = 0;
    } else {
        tx = x;
        ty = y;
        tdown = pressed;
    }
    hx_pump(30);
}

void hx_tap(int x, int y) {
    hx_touch(x, y, 1);
    hx_touch(x + 1, y, 1);
    hx_touch(x + 1, y, 0);
    hx_pump(60);
}

void hx_save(const char *name) {
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    hx_pump(40);
    char path[256];
    snprintf(path, sizeof path, "%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint8_t c = fb[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 5) * 255 / 7), (uint8_t)(((c >> 2) & 7) * 255 / 7),
                          (uint8_t)((c & 3) * 255 / 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

void hx_init(void) {
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    static uint8_t buf[W * 40 * 2];
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_cb);
}
