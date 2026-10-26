// ui.c - top bar, page switching, install overlay, boot flow.
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "dialog.h"
#include "lsettings.h"
#include "sysinfo.h"
#include "theme.h"
#include "tulip/settings.h"
#include "tulip/touchcal_ui.h"
#include "web.h"
#include "wifi.h"

static const char *const TABS[PG_COUNT] = {"HOME", "FILES", "WI-FI", "SETTINGS", "CONSOLE"};

static struct {
    lv_obj_t *tabs[PG_COUNT];
    lv_obj_t *pages[PG_COUNT];
    lv_obj_t *status;
    int page;
    uint32_t toast_until;
    // install overlay
    lv_obj_t *inst, *inst_title, *inst_bar, *inst_msg, *inst_ok;
    int inst_shown;
    // autostart countdown
    lv_obj_t *auto_ov, *auto_lbl;
    uint32_t auto_t0;
    char auto_name[33];
} U = {.page = -1};

// --- pages ---------------------------------------------------------------------

static void (*const SHOW[PG_COUNT])(void) = {home_show, files_show, wifi_page_show, settings_show,
                                            console_page_show};
static void (*const POLL[PG_COUNT])(void) = {home_poll, files_poll, wifi_page_poll, settings_poll,
                                            console_page_poll};

void ui_show_page(int p) {
    U.page = p;
    for (int i = 0; i < PG_COUNT; i++) {
        th_btn_set_active(U.tabs[i], i == p);
        if (i == p)
            lv_obj_remove_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    SHOW[p]();
}

static void tab_cb(lv_event_t *e) { ui_show_page((int)(intptr_t)lv_event_get_user_data(e)); }

void ui_toast(const char *msg) {
    lv_label_set_text(U.status, msg);
    lv_obj_set_style_text_color(U.status, th_c(TH_WHITE), 0);
    U.toast_until = lv_tick_get() + 3000;
}

static void draw_status(void) {
    if (lv_tick_get() < U.toast_until) return;
    char b[64];
    switch (g_wifi.state) {
    case WIFI_ST_CONNECTED:
        snprintf(b, sizeof b, LV_SYMBOL_WIFI "  %s", g_wifi.ip);
        break;
    case WIFI_ST_CONNECTING: snprintf(b, sizeof b, LV_SYMBOL_WIFI "  connecting"); break;
    case WIFI_ST_FAILED: snprintf(b, sizeof b, LV_SYMBOL_WIFI "  retrying"); break;
    case WIFI_ST_NO_NETWORK: snprintf(b, sizeof b, LV_SYMBOL_WIFI "  not set up"); break;
    default: snprintf(b, sizeof b, "WI-FI OFF"); break;
    }
    lv_label_set_text(U.status, b);
    lv_obj_set_style_text_color(U.status, th_c(TH_ORANGE), 0);
}

static void restart_ok(void *ud) { sys_restart(); }
static void power_cb(lv_event_t *e) {
    dlg_confirm("RESTART", "Restart the Tulip?", "RESTART", true, restart_ok, NULL);
}
static void wifi_cb(lv_event_t *e) { ui_show_page(PG_WIFI); }

static void top_bar(lv_obj_t *scr) {
    lv_obj_t *t = th_text(scr, "TULIP", 18, 12, TH_FG, &lv_font_montserrat_28);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    for (int i = 0; i < PG_COUNT; i++)
        U.tabs[i] = th_btn(scr, TABS[i], 150 + i * 106, 10, 100, 36, tab_cb, (void *)(intptr_t)i);
    U.status = th_text(scr, "", 692, 20, TH_ORANGE, &lv_font_montserrat_14);
    lv_obj_set_width(U.status, 210);
    lv_label_set_long_mode(U.status, LV_LABEL_LONG_CLIP);
    th_square(scr, LV_SYMBOL_WIFI, 912, 10, 48, 36, TH_BLUE, wifi_cb, NULL);
    th_square(scr, LV_SYMBOL_POWER, 964, 10, 48, 36, TH_RED, power_cb, NULL);
    th_hline(scr, 0, TOP_H - 1, SCR_W);
}

// --- launching / install overlay ---------------------------------------------

void ui_launch(const char *path) {
    if (!apps_launch(path)) ui_toast("not an app, or busy");
}

static void inst_ok_cb(lv_event_t *e) {
    g_install.state = INSTALL_IDLE;
}

static void install_overlay(void) {
    int st = g_install.state;
    if (st == INSTALL_IDLE) {
        if (U.inst) lv_obj_add_flag(U.inst, LV_OBJ_FLAG_HIDDEN);
        U.inst_shown = 0;
        return;
    }
    if (!U.inst) {
        U.inst = th_box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
        lv_obj_set_style_bg_opa(U.inst, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(U.inst, th_c(TH_BG), 0);
        lv_obj_add_flag(U.inst, LV_OBJ_FLAG_CLICKABLE);
        U.inst_title = th_title(U.inst, "", 212, 200, TH_ORANGE);
        U.inst_bar = lv_bar_create(U.inst);
        lv_obj_set_pos(U.inst_bar, 212, 250);
        lv_obj_set_size(U.inst_bar, 600, 14);
        lv_obj_set_style_radius(U.inst_bar, 0, 0);
        lv_obj_set_style_radius(U.inst_bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(U.inst_bar, th_c(TH_LINE), 0);
        lv_obj_set_style_bg_opa(U.inst_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(U.inst_bar, th_c(TH_ORANGE), LV_PART_INDICATOR);
        U.inst_msg = th_text(U.inst, "", 212, 280, TH_FG, NULL);
        U.inst_ok = th_btn(U.inst, "OK", 212, 330, 140, 44, inst_ok_cb, NULL);
    }
    lv_obj_remove_flag(U.inst, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.inst);
    char t[64];
    snprintf(t, sizeof t, st == INSTALL_FAILED ? "COULD NOT START %s" : "STARTING %s",
             g_install.name);
    for (char *c = t; *c; c++) *c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
    lv_label_set_text(U.inst_title, t);
    lv_bar_set_value(U.inst_bar, st == INSTALL_WRITING ? g_install.pct : st == INSTALL_STARTING ? 100 : 0,
                     LV_ANIM_OFF);
    char m[120];
    if (st == INSTALL_WRITING)
        snprintf(m, sizeof m, "installing into an app slot  %d%%", g_install.pct);
    else
        snprintf(m, sizeof m, "%s", g_install.msg);
    lv_label_set_text(U.inst_msg, m);
    if (st == INSTALL_FAILED)
        lv_obj_remove_flag(U.inst_ok, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(U.inst_ok, LV_OBJ_FLAG_HIDDEN);
}

// --- boot flow: touch calibration prompt, then (optionally) auto-launch --------

#define AUTO_MS 3000

static void auto_cancel(lv_event_t *e) {
    if (U.auto_ov) lv_obj_delete_async(U.auto_ov);
    U.auto_ov = NULL;
    ui_toast("staying in the launcher");
}

static void after_cal(void) {
    app_info_t info;
    uint32_t size;
    if (!g_ls.autostart || !g_ls.last_app[0] || !apps_read_info(g_ls.last_app, &info, &size)) return;
    U.auto_ov = th_box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(U.auto_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(U.auto_ov, th_c(TH_BG), 0);
    lv_obj_add_flag(U.auto_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.auto_ov, auto_cancel, LV_EVENT_PRESSED, NULL);
    U.auto_lbl = th_title(U.auto_ov, "", 0, 260, TH_ORANGE);
    lv_obj_set_width(U.auto_lbl, SCR_W);
    lv_obj_set_style_text_align(U.auto_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *h = th_text(U.auto_ov, "tap anywhere to stay in the launcher", 0, 300, TH_DIM, NULL);
    lv_obj_set_width(h, SCR_W);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    U.auto_t0 = lv_tick_get();
    snprintf(U.auto_name, sizeof U.auto_name, "%s", info.name);
}

static void autostart_poll(void) {
    if (!U.auto_ov) return;
    uint32_t el = lv_tick_get() - U.auto_t0;
    if (el >= AUTO_MS) {
        lv_obj_delete_async(U.auto_ov);
        U.auto_ov = NULL;
        ui_launch(g_ls.last_app);
        return;
    }
    char b[96];
    snprintf(b, sizeof b, "LAUNCHING %s IN %d", U.auto_name, (int)((AUTO_MS - el + 999) / 1000));
    lv_label_set_text(U.auto_lbl, b);
}

// --- entry points --------------------------------------------------------------

void ui_create(void) {
    th_init();
    lv_obj_t *scr = lv_screen_active();
    top_bar(scr);
    for (int i = 0; i < PG_COUNT; i++) {
        U.pages[i] = th_box(scr, 0, PAGE_Y, SCR_W, PAGE_H);
        lv_obj_add_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    home_create(U.pages[PG_HOME]);
    files_create(U.pages[PG_FILES]);
    wifi_page_create(U.pages[PG_WIFI]);
    settings_create(U.pages[PG_SETTINGS]);
    console_page_create(U.pages[PG_CONSOLE]);
    ui_show_page(PG_HOME);
    tulip_touchcal_set_notify(ui_toast);
    if (g_tulip.cal_prompt)
        tulip_touchcal_boot_prompt(after_cal);
    else
        after_cal();
}

void ui_poll(void) {
    static uint32_t last;
    tulip_touchcal_poll();
    install_overlay();
    autostart_poll();
    uint32_t now = lv_tick_get();
    if (now - last < 100) return;   // pages refresh at 10 Hz
    last = now;
    draw_status();
    if (U.page >= 0) POLL[U.page]();
}
