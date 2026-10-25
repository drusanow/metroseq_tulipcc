// display.c - RGB panel + LVGL port.
//
// The panel is wired for 8-bit RGB332 (see board.h). The esp_lcd RGB driver
// scans an 8-bit frame buffer in PSRAM out through two internal-RAM bounce
// buffers (DMA from internal RAM is what keeps the panel stable while the
// CPU and PSRAM are busy). LVGL renders RGB565 bands into an internal-RAM
// buffer; the flush converts them to RGB332 straight into the frame buffer.
//
// Everything here - panel interrupt, LVGL rendering, touch-event intake -
// lives on core 0, away from the sequencer on core 1. A slow redraw delays
// the next frame, never a gate.
#include "display.h"

#include <assert.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gt911.h"
#include "lvgl.h"
#include "ui/ui.h"

static const char *TAG = "disp";

#define LVGL_CORE      0
#define LVGL_PRIO      4
#define LVGL_STACK     12288
#define BAND_ROWS      40          // 1024*40*2 = 80 KB render band
#define FRAME_MS       10          // LVGL loop period cap

static esp_lcd_panel_handle_t s_panel;
static uint8_t *s_fb;              // 1024*600 RGB332, in PSRAM

static void backlight_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 300,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .gpio_num = PIN_LCD_BK_PWM,
        .duty = 0,   // Tulip's PWM is inverted: 0 = full brightness
    };
    ledc_channel_config(&c);
}

static void panel_init(void) {
    static const int unused[] = PIN_LCD_UNUSED_LOW;
    for (unsigned i = 0; i < sizeof unused / sizeof unused[0]; i++) {
        gpio_config_t io = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << unused[i]};
        gpio_config(&io);
        gpio_set_level(unused[i], 0);
    }
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_back_porch = LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = LCD_HSYNC_FRONT_PORCH,
            .hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH,
            .vsync_back_porch = LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = LCD_VSYNC_FRONT_PORCH,
            .vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH,
        },
        .data_width = 8,
        .bits_per_pixel = 8,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_ROWS,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {PIN_LCD_B6, PIN_LCD_B7, PIN_LCD_G5, PIN_LCD_G6, PIN_LCD_G7,
                           PIN_LCD_R5, PIN_LCD_R6, PIN_LCD_R7},
        .flags.fb_in_psram = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    void *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb));
    s_fb = fb;
    backlight_init();
}

// RGB565 band -> RGB332 rows of the live frame buffer.
static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)px;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        uint8_t *dst = s_fb + y * LCD_H_RES + a->x1;
        for (int x = 0; x < w; x++) {
            uint16_t c = *src++;
            dst[x] = (uint8_t)(((c >> 13) << 5) | (((c >> 8) & 7) << 2) | ((c >> 3) & 3));
        }
    }
    lv_display_flush_ready(disp);
}

// Touch: drain queued events one at a time so a tap that happened while
// we were busy rendering is still seen as press + release.
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    static touch_event_t last;
    touch_event_t ev;
    if (touch_next_event(&ev)) last = ev;
    data->point.x = last.x;
    data->point.y = last.y;
    data->state = last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = touch_events_pending();
}

static uint32_t tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg) {
    panel_init();
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    size_t sz = LCD_H_RES * BAND_ROWS * 2;
    void *buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);
    lv_display_set_buffers(disp, buf, NULL, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_timer_set_period(lv_indev_get_read_timer(indev), 5);

    ui_create();
    ESP_LOGI(TAG, "LVGL running on core %d", xPortGetCoreID());

    for (;;) {
        uint32_t next = lv_timer_handler();
        ui_poll();
        if (next > FRAME_MS) next = FRAME_MS;
        if (next < 1) next = 1;
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

void display_start(void) {
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_STACK, NULL, LVGL_PRIO, NULL, LVGL_CORE);
}
