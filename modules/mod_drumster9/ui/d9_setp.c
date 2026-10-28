// d9_setp.c - the SET page: projects, and the setup (the runtime's I/O panel:
// clock, MIDI clock out, the CV clock; then MIDI notes in).
//
// Projects are the Python's: /files/drumster9/tulipdrums_projects.json, so
// a library copied from the old Tulip's /user loads as it is. A project is
// everything - the grid, the A-J slots, kits and their samples, the buses,
// the mix and the tempo.
#include <stdio.h>
#include <string.h>

#include "d9_ui_int.h"
#include "mod_drumster9.h"

#define NLIST 16
#define LIST_Y 40
#define LIST_DY 46

static struct {
    lv_obj_t *item[NLIST], *del, *msg, *empty;
    lv_obj_t *ch, *samples;
    int sel;
    bool del_armed;
} P = {.sel = -1};

static void say(const char *t) { lv_label_set_text(P.msg, t); }

static void disarm(void) {
    P.del_armed = false;
    sw_btn_text(P.del, "DELETE");
    sw_btn_set(P.del, false);
}

static void refresh_list(void) {
    int n = d9_proj_count();
    if (P.sel >= n) P.sel = n - 1;
    if (n)
        lv_obj_add_flag(P.empty, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(P.empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < NLIST; i++) {
        if (i >= n) {
            lv_obj_add_flag(P.item[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(P.item[i], LV_OBJ_FLAG_HIDDEN);
        sw_btn_text(P.item[i], d9_proj_name(i));
        sw_btn_set(P.item[i], i == P.sel);
    }
}

static void refresh_setup(void) {
    char t[48];
    if (g_d9set.midi_ch < 0)
        snprintf(t, sizeof t, "OFF");
    else if (g_d9set.midi_ch == 0)
        snprintf(t, sizeof t, "OMNI");
    else
        snprintf(t, sizeof t, "CH %d", g_d9set.midi_ch);
    lv_label_set_text(P.ch, t);
    if (g_d9s.samples_ok)
        snprintf(t, sizeof t, "%d samples in %d banks", g_d9ix.nsamples, g_d9ix.nbanks);
    else
        snprintf(t, sizeof t, "NO SAMPLES: flash tulip-full.bin");
    lv_label_set_text(P.samples, t);
    lv_obj_set_style_text_color(P.samples, sw_c(g_d9s.samples_ok ? C_DIM : SW_RED), 0);
}

void d9_set_page_refresh(void) {
    if (P.sel < 0 && g_d9_proj_cur[0]) P.sel = d9_proj_find(g_d9_proj_cur);
    refresh_list();
    refresh_setup();
}

// --- projects ------------------------------------------------------------------------------

static void item_cb(void *ud) {
    disarm();
    P.sel = (int)(intptr_t)ud;
    refresh_list();
}

static void load_cb(void *ud) {
    disarm();
    if (P.sel < 0) return say("pick a project first");
    char t[48];
    snprintf(t, sizeof t, d9_proj_load(P.sel) ? "loaded %s" : "couldn't load %s", d9_proj_name(P.sel));
    say(t);
}

static void saved(const char *name, void *ud) {
    char t[48];
    bool ok = d9_proj_save(name);
    P.sel = d9_proj_find(g_d9_proj_cur);
    refresh_list();
    snprintf(t, sizeof t, ok ? "saved %s" : "couldn't write the file", g_d9_proj_cur);
    say(t);
}

static void save_as_cb(void *ud) {
    disarm();
    char def[D9_PROJECT_NAME + 1];
    if (g_d9_proj_cur[0])
        snprintf(def, sizeof def, "%s", g_d9_proj_cur);
    else
        snprintf(def, sizeof def, "Project %d", d9_proj_count() + 1);
    sw_text_entry("SAVE DRUMSTER9 PROJECT", def, D9_PROJECT_NAME, ACC, saved, NULL);
}

// SAVE: over the one picked (the Python's UPDATE); none picked: a new name
static void save_cb(void *ud) {
    disarm();
    if (P.sel >= 0) {
        char nm[D9_PROJECT_NAME + 1];
        snprintf(nm, sizeof nm, "%s", d9_proj_name(P.sel));
        saved(nm, NULL);
    } else {
        save_as_cb(NULL);
    }
}

static void del_cb(void *ud) {
    if (P.sel < 0) return say("pick a project first");
    if (!P.del_armed) {   // tap twice
        P.del_armed = true;
        sw_btn_text(P.del, "SURE?");
        sw_btn_set(P.del, true);
        return;
    }
    disarm();
    char t[48];
    snprintf(t, sizeof t, "deleted %s", d9_proj_name(P.sel));
    if (!d9_proj_delete(P.sel)) snprintf(t, sizeof t, "couldn't write the file");
    P.sel = -1;
    refresh_list();
    say(t);
}

// --- setup ---------------------------------------------------------------------------------------

static void ch_cb(void *ud) {
    int c = g_d9set.midi_ch + ((intptr_t)ud & 1 ? 1 : -1);
    g_d9set.midi_ch = (int8_t)(c < -1 ? 16 : (c > 16 ? -1 : c));
    d9_settings_save();
    refresh_setup();
}

void d9_set_page_create(lv_obj_t *p) {
    sw_label(p, "PROJECTS", SW_F_LABEL, SW_ORANGE, 12, 12, 0, false);
    sw_label(p, "/files/drumster9/tulipdrums_projects.json - the Python's file loads as it is", SW_F_SMALL,
             C_DIM, 110, 14, 0, false);
    for (int i = 0; i < NLIST; i++)
        P.item[i] = sw_btn(p, "", 12 + (i / 8) * 316, LIST_Y + (i % 8) * LIST_DY, 310, 40, ACC, item_cb,
                           (void *)(intptr_t)i, NULL);
    P.empty = sw_label(p, "No projects yet: SAVE AS... keeps everything - the grid, A-J, the kits\n"
                          "and their samples, the buses, the mix and the tempo - under a name.\n\n"
                          "Your projects from the Python Drumster9: copy /user/tulipdrums_projects.json\n"
                          "to /files/drumster9/ with the launcher's Wi-Fi page.",
                       SW_F_LABEL, C_DIM, 12, LIST_Y + 10, 640, false);
    int y = LIST_Y + 8 * LIST_DY + 12;
    sw_btn(p, "LOAD", 12, y, 110, 44, ACC, load_cb, NULL, NULL);
    sw_btn(p, "SAVE", 128, y, 110, 44, ACC, save_cb, NULL, NULL);
    sw_btn(p, "SAVE AS...", 244, y, 150, 44, ACC, save_as_cb, NULL, NULL);
    P.del = sw_btn(p, "DELETE", 400, y, 110, 44, SW_RED, del_cb, NULL, NULL);
    P.msg = sw_label(p, "", SW_F_LABEL, SW_TEXT, 12, y + 58, 620, false);
    lv_label_set_long_mode(P.msg, LV_LABEL_LONG_CLIP);

    const int x = 676;
    sw_line(p, x - 16, 8, 1, PAGE_H - 24);
    sw_label(p, "SETUP", SW_F_LABEL, SW_ORANGE, x, 12, 0, false);
    studio_io_panel(p, x, 44, &mod_drumster9, ACC);
    const int my = 44 + 6 * 46 + 6;
    sw_label(p, "NOTES IN", SW_F_SMALL, C_DIM, x, my + 12, 0, false);
    sw_btn_repeat(sw_btn(p, "-", x + 120, my, 44, 40, ACC, ch_cb, (void *)0, SW_F_MID));
    P.ch = sw_label(p, "", SW_F_LABEL, SW_TEXT, x + 164, my + 12, 118, true);
    sw_btn_repeat(sw_btn(p, "+", x + 282, my, 44, 40, ACC, ch_cb, (void *)1, SW_F_MID));
    sw_label(p, "GM notes play the lanes: 36 K, 38 SN, 39 CL,\n42 CH, 46 OH, 49 CY, 45 TM", SW_F_SMALL, C_DIM,
             x, my + 48, 0, false);
    sw_label(p, "SAMPLES", SW_F_LABEL, SW_ORANGE, x, 436, 0, false);
    P.samples = sw_label(p, "", SW_F_SMALL, C_DIM, x, 460, 330, false);
    sw_label(p, "Gamma 9001, from AMY (MIT)", SW_F_SMALL, C_DIM, x, 480, 0, false);
    d9_set_page_refresh();
}
