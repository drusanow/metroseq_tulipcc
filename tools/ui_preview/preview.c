// preview.c - render the firmware's real UI code on the host and save
// screenshots of every page, through the same RGB565 -> RGB332 conversion
// the device's flush uses (so colours are exactly what the panel shows).
//
//   make -C tools/ui_preview      -> tools/ui_preview/build/*.png
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "lvgl.h"
#include "platform/preview_audio.h"
#include "platform/storage.h"
#include "ui/ui.h"
#include "ui/ui_common.h"

#define W 1024
#define H 600

// --- stand-ins for the firmware's platform layer -------------------------------

pattern_t g_pat;
seq_status_t g_status;
void pat_lock(void) {}
void pat_unlock(void) {}
void pat_store(const pattern_t *src) { g_pat = *src; }
void pat_load(pattern_t *dst) { *dst = g_pat; }
void status_info(const char *msg) {
    snprintf(g_status.info, sizeof g_status.info, "%s", msg);
    g_status.info_seq++;
}
void status_info_get(char *buf, int len, uint32_t *seq) {
    snprintf(buf, len, "%s", g_status.info);
    *seq = g_status.info_seq;
}
bool seq_cmd(seq_cmd_t cmd) {
    if (cmd == CMD_TOGGLE_PLAY) g_status.playing = !g_status.playing;
    return true;
}
static bool s_audio = true;
void preview_set_enabled(bool on) { s_audio = on; }
bool preview_enabled(void) { return s_audio; }

static pattern_t s_presets[NUM_PRESETS];
static bool s_used[NUM_PRESETS];
bool storage_preset_used(int s) { return s_used[s]; }
bool storage_preset_get(int s, pattern_t *o) {
    if (!s_used[s]) return false;
    *o = s_presets[s];
    return true;
}
void storage_preset_save(int s, const pattern_t *p) {
    s_presets[s] = *p;
    s_used[s] = true;
}
int storage_project_count(void) { return 2; }
const char *storage_project_name(int i) { return i ? "Dark Arp" : "Live Set A"; }
bool storage_project_get(int i, pattern_t *o) {
    pattern_default(o);
    return true;
}
int storage_project_save(const char *n, const pattern_t *p) { return 0; }
bool storage_write_pending(void) { return false; }

// --- display -------------------------------------------------------------------

static uint8_t fb[W * H];   // RGB332, like the device frame buffer

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

static int touch_x, touch_y, touch_down;
static void touch_cb(lv_indev_t *i, lv_indev_data_t *d) {
    d->point.x = touch_x;
    d->point.y = touch_y;
    d->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t s_ms;
static uint32_t tick(void) { return s_ms; }

static void pump(int ms) {
    for (int i = 0; i < ms; i += 5) {
        s_ms += 5;
        lv_timer_handler();
        ui_poll();
    }
}

static void tap(int x, int y) {
    touch_x = x;
    touch_y = y;
    touch_down = 1;
    pump(60);
    touch_down = 0;
    pump(60);
}

static void save(const char *name) {
    lv_obj_invalidate(lv_screen_active());
    pump(40);
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

int main(void) {
    pattern_default(&g_pat);
    storage_preset_save(0, &g_pat);
    storage_preset_save(3, &g_pat);
    g_status.cv_enabled = true;
    g_status.dac_present[0] = true;
    g_status.cv_ready = true;
    g_status.late_count = 4096;
    g_status.late_sum_us = 4096 * 112;
    g_status.late_max_us = 231;

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

    ui_create();
    pump(50);

    // pretend the sequence is running so the playhead LEDs show
    g_status.playing = true;
    g_status.disp_stage[0] = 2;
    g_status.disp_stage[1] = 5;
    g_status.lane_disp[0] = 3;
    status_info("CV ready");
    pump(50);
    save("1_seq");

    tap(SEQ_X0 + 3 * COL_W + 50, PAGE_Y + 4 + 14 + 15);   // stage 4 number -> detail
    save("2_detail");
    tap(512 - 280 + 440 + 50, 300 - 160 + 12 + 20);         // Close

    show_page(1);
    pump(50);
    save("3_trk");
    show_page(2);
    pump(50);
    save("4_mod");
    show_page(3);
    pump(300);
    save("5_set");
    tap(110 + 60, PAGE_Y + 428 + 20);   // SAVE AS...
    save("6_save_project");
    return 0;
}
