// page_settings.c - display, touch calibration, startup, app slots, system.
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "core/path.h"
#include "dialog.h"
#include "lsettings.h"
#include "sysinfo.h"
#include "theme.h"
#include "tulip/display.h"
#include "tulip/settings.h"
#include "tulip/touchcal_ui.h"
#include "ui.h"

static struct {
    lv_obj_t *bright, *b_prompt, *b_auto, *auto_lbl, *slots, *about;
} S;

static void set_bright(int v) {
    if (v < 1) v = 1;
    if (v > 9) v = 9;
    g_tulip.brightness = (uint8_t)v;
    tulip_display_set_brightness(v);
    tulip_settings_save();
    lv_label_set_text_fmt(S.bright, "%d", v);
}
static void bright_dn(lv_event_t *e) { set_bright(g_tulip.brightness - 1); }
static void bright_up(lv_event_t *e) { set_bright(g_tulip.brightness + 1); }

static void cal_cb(lv_event_t *e) { tulip_touchcal_start(NULL); }

static void prompt_cb(lv_event_t *e) {
    g_tulip.cal_prompt = !g_tulip.cal_prompt;
    tulip_settings_save();
    settings_show();
}

static void auto_cb(lv_event_t *e) {
    g_ls.autostart = !g_ls.autostart;
    lsettings_save();
    settings_show();
}

static void clear_ok(void *ud) {
    apps_clear_slots();
    ui_toast("app slots cleared");
    settings_show();
}
static void clear_cb(lv_event_t *e) {
    dlg_confirm("CLEAR APP SLOTS",
                "Forget the installed copies of apps? Your .bin files stay; each app re-installs "
                "the next time it is launched.",
                "CLEAR", true, clear_ok, NULL);
}

static void restart_ok(void *ud) { sys_restart(); }
static void restart_cb(lv_event_t *e) {
    dlg_confirm("RESTART", "Restart the Tulip?", "RESTART", false, restart_ok, NULL);
}

static void format_ok2(void *ud) {
    ui_toast(sys_format_storage() ? "storage formatted" : "format failed");
}
static void format_ok(void *ud) {
    dlg_confirm("REALLY FORMAT?", "Last chance: every file and app in /files will be gone.",
                "FORMAT", true, format_ok2, NULL);
}
static void format_cb(lv_event_t *e) {
    dlg_confirm("FORMAT STORAGE", "Erase ALL files and apps in /files?", "ERASE", true, format_ok,
                NULL);
}

static void reset_ok(void *ud) { sys_factory_reset(); }
static void reset_cb(lv_event_t *e) {
    dlg_confirm("FACTORY RESET",
                "Erase all settings - Wi-Fi, PIN, touch calibration and every app's saved "
                "settings - and restart? Files in /files are kept.",
                "RESET", true, reset_ok, NULL);
}

void settings_create(lv_obj_t *p) {
    int y = 14;
    th_title(p, "DISPLAY", 16, y, TH_ORANGE);
    th_text(p, "BRIGHTNESS", 16, y + 48, TH_FG, NULL);
    th_btn(p, "-", 150, y + 38, 44, 40, bright_dn, NULL);
    S.bright = th_text(p, "9", 208, y + 46, TH_WHITE, &lv_font_montserrat_20);
    th_btn(p, "+", 236, y + 38, 44, 40, bright_up, NULL);
    th_btn(p, "CALIBRATE TOUCH", 300, y + 38, 190, 40, cal_cb, NULL);
    S.b_prompt = th_btn(p, "", 502, y + 38, 260, 40, prompt_cb, NULL);

    y = 110;
    th_hline(p, 16, y - 8, SCR_W - 32);
    th_title(p, "STARTUP", 16, y, TH_ORANGE);
    S.b_auto = th_btn(p, "", 16, y + 38, 260, 40, auto_cb, NULL);
    S.auto_lbl = th_text(p, "", 292, y + 49, TH_DIM, &lv_font_montserrat_14);

    y = 206;
    th_hline(p, 16, y - 8, SCR_W - 32);
    th_title(p, "APP SLOTS", 16, y, TH_CYAN);
    th_text(p, "installed copies - an app in a slot starts instantly", 160, y + 5, TH_DIM,
            &lv_font_montserrat_14);
    S.slots = th_text(p, "", 16, y + 36, TH_FG, &lv_font_montserrat_14);
    th_btn(p, "CLEAR SLOTS", SCR_W - 16 - 160, y + 34, 160, 40, clear_cb, NULL);

    y = 344;
    th_hline(p, 16, y - 8, SCR_W - 32);
    th_title(p, "SYSTEM", 16, y, TH_ORANGE);
    th_btn(p, "RESTART", 16, y + 38, 150, 40, restart_cb, NULL);
    th_btn(p, "FORMAT STORAGE", 178, y + 38, 190, 40, format_cb, NULL);
    th_btn(p, "FACTORY RESET", 380, y + 38, 190, 40, reset_cb, NULL);
    S.about = th_text(p, "", 16, y + 92, TH_DIM, &lv_font_montserrat_14);
}

void settings_show(void) {
    lv_label_set_text_fmt(S.bright, "%d", g_tulip.brightness);
    th_btn_set_text(S.b_prompt, g_tulip.cal_prompt ? "CAL PROMPT AT START: ON" : "CAL PROMPT AT START: OFF");
    th_btn_set_active(S.b_prompt, g_tulip.cal_prompt);
    th_btn_set_text(S.b_auto, g_ls.autostart ? "AUTO-LAUNCH LAST APP: ON" : "AUTO-LAUNCH LAST APP: OFF");
    th_btn_set_active(S.b_auto, g_ls.autostart);
    char b[240];
    snprintf(b, sizeof b, "last app:  %s", g_ls.last_app[0] ? path_basename(g_ls.last_app) : "none yet");
    lv_label_set_text(S.auto_lbl, b);

    app_slot_t s[APP_SLOTS];
    apps_slots(s);
    char t[300];
    size_t w = 0;
    for (int i = 0; i < APP_SLOTS; i++) {
        if (s[i].used)
            w += snprintf(t + w, sizeof t - w, "slot %d:  %s  v%s\n", i + 1, s[i].name, s[i].version);
        else
            w += snprintf(t + w, sizeof t - w, "slot %d:  empty\n", i + 1);
        if (w >= sizeof t) break;
    }
    lv_label_set_text(S.slots, t);
    settings_poll();
}

void settings_poll(void) {
    static uint32_t last;
    if (lv_tick_get() - last < 1000) return;
    last = lv_tick_get();
    char b[400];
    sysinfo_text(b, sizeof b);
    lv_label_set_text(S.about, b);
}
