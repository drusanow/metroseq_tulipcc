// page_files.c - file browser: open folders, launch apps, new folder,
// rename, delete.
//
// Tap a row to select it; tap the selected row again to open it (folder)
// or run it (app). The toolbar does the same things explicitly.
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "core/path.h"
#include "dialog.h"
#include "fsutil.h"
#include "theme.h"
#include "tulip/fs.h"
#include "ui.h"

#define MAX_ENTRIES 128
#define ROW_H 44

static struct {
    lv_obj_t *path_lbl, *usage, *list, *hint;
    lv_obj_t *b_up, *b_new, *b_ren, *b_del, *b_run;
    char dir[300];
    fs_entry_t ents[MAX_ENTRIES];
    bool is_app[MAX_ENTRIES];
    char app_ver[MAX_ENTRIES][33];
    lv_obj_t *rows[MAX_ENTRIES];
    int n, sel;
} F = {.dir = TULIP_APPS_DIR, .sel = -1};

static bool sel_path(char *out, size_t n) {
    return path_join(F.dir, F.ents[F.sel].name, out, n);
}

static void update_buttons(void) {
    bool s = F.sel >= 0;
    th_btn_set_enabled(F.b_up, strcmp(F.dir, TULIP_FS_ROOT) != 0);
    th_btn_set_enabled(F.b_ren, s);
    th_btn_set_enabled(F.b_del, s);
    th_btn_set_enabled(F.b_run, s && (F.ents[F.sel].dir || F.is_app[F.sel]));
    th_btn_set_text(F.b_run, s && F.ents[F.sel].dir ? LV_SYMBOL_RIGHT "  OPEN" : LV_SYMBOL_PLAY "  LAUNCH");
}

static void select_row(int i) {
    if (F.sel >= 0 && F.sel < F.n) {
        lv_obj_set_style_bg_color(F.rows[F.sel], th_c(TH_BG), 0);
        lv_obj_set_style_border_color(F.rows[F.sel], th_c(TH_LINE), 0);
    }
    F.sel = i;
    if (i >= 0) {
        lv_obj_set_style_bg_color(F.rows[i], th_c(TH_SEL_BG), 0);
        lv_obj_set_style_border_color(F.rows[i], th_c(TH_ORANGE), 0);
    }
    update_buttons();
}

static void open_selected(void) {
    if (F.sel < 0) return;
    char p[300];
    if (!sel_path(p, sizeof p)) return;
    if (F.ents[F.sel].dir)
        files_open_dir(p);
    else if (F.is_app[F.sel])
        ui_launch(p);
}

static void row_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == F.sel)
        open_selected();
    else
        select_row(i);
}

static void refresh(void) {
    uint64_t total = 0, fr = 0;
    char b[96];
    if (tulip_fs_usage(&total, &fr))
        snprintf(b, sizeof b, "%.1f MB free of %.1f MB", fr / 1048576.0, total / 1048576.0);
    else
        snprintf(b, sizeof b, "storage not mounted");
    lv_label_set_text(F.usage, b);
    lv_label_set_text(F.path_lbl, F.dir);

    lv_obj_clean(F.list);
    F.sel = -1;
    F.n = fs_list(F.dir, F.ents, MAX_ENTRIES);
    if (F.n < 0) F.n = 0;
    for (int i = 0; i < F.n; i++) {
        const fs_entry_t *en = &F.ents[i];
        char p[300];
        app_info_t info;
        uint32_t sz;
        F.is_app[i] = !en->dir && path_has_ext(en->name, ".bin") && path_join(F.dir, en->name, p, sizeof p) &&
                      apps_read_info(p, &info, &sz);
        if (F.is_app[i]) snprintf(F.app_ver[i], sizeof F.app_ver[i], "%s", info.version);

        // rows scroll, so they act on a completed tap, not on press
        lv_obj_t *r = th_btn(F.list, "", 0, i * (ROW_H + 4), SCR_W - 32 - 12, ROW_H, NULL, NULL);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_delete(lv_obj_get_child(r, 0));
        F.rows[i] = r;
        const char *icon = en->dir ? LV_SYMBOL_DIRECTORY : F.is_app[i] ? LV_SYMBOL_PLAY : LV_SYMBOL_FILE;
        uint8_t icol = en->dir ? TH_ORANGE : F.is_app[i] ? TH_GREEN : TH_DIM;
        th_text(r, icon, 14, 12, icol, &lv_font_montserrat_16);
        lv_obj_t *nm = th_text(r, en->name, 48, 12, TH_WHITE, &lv_font_montserrat_16);
        lv_obj_set_width(nm, 640);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        char right[48];
        if (en->dir)
            snprintf(right, sizeof right, "folder");
        else if (F.is_app[i])
            snprintf(right, sizeof right, "app v%s   %lu KB", F.app_ver[i], (unsigned long)(en->size / 1024));
        else if (en->size < 10240)
            snprintf(right, sizeof right, "%lu B", (unsigned long)en->size);
        else
            snprintf(right, sizeof right, "%lu KB", (unsigned long)(en->size / 1024));
        lv_obj_t *rl = th_text(r, right, 0, 14, TH_DIM, &lv_font_montserrat_14);
        lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_label_set_text(F.hint, F.n ? "tap to select  -  tap again to open a folder or run an app"
                                  : "this folder is empty");
    update_buttons();
}

void files_open_dir(const char *abs) {
    snprintf(F.dir, sizeof F.dir, "%s", abs);
    ui_show_page(PG_FILES);
}

static void up_cb(lv_event_t *e) {
    char parent[300];
    if (path_parent(F.dir, parent, sizeof parent) && strlen(parent) >= strlen(TULIP_FS_ROOT)) {
        snprintf(F.dir, sizeof F.dir, "%s", parent);
        refresh();
    }
}

static void mkdir_ok(const char *name, void *ud) {
    char p[300];
    if (!path_valid_name(name) || !path_join(F.dir, name, p, sizeof p)) {
        ui_toast("not a valid folder name");
        return;
    }
    ui_toast(fs_mkdir(p) ? "folder created" : "could not create the folder");
    refresh();
}

static void new_cb(lv_event_t *e) {
    dlg_prompt("NEW FOLDER", "", "CREATE", false, mkdir_ok, NULL);
}

static void rename_ok(const char *name, void *ud) {
    char from[300], to[300];
    if (!sel_path(from, sizeof from) || !path_valid_name(name) || !path_join(F.dir, name, to, sizeof to)) {
        ui_toast("not a valid name");
        return;
    }
    if (strcmp(from, TULIP_APPS_DIR) == 0) {
        ui_toast("the apps folder keeps its name");
        return;
    }
    ui_toast(fs_rename(from, to) ? "renamed" : "rename failed");
    refresh();
}

static void rename_cb(lv_event_t *e) {
    if (F.sel < 0) return;
    dlg_prompt("RENAME", F.ents[F.sel].name, "RENAME", false, rename_ok, NULL);
}

static void delete_ok(void *ud) {
    char p[300];
    if (!sel_path(p, sizeof p)) return;
    if (strcmp(p, TULIP_APPS_DIR) == 0) {
        ui_toast("the apps folder can't be deleted");
        return;
    }
    ui_toast(fs_remove(p) ? "deleted" : "delete failed");
    refresh();
}

static void delete_cb(lv_event_t *e) {
    if (F.sel < 0) return;
    char body[160];
    snprintf(body, sizeof body,
             F.ents[F.sel].dir ? "Delete the folder \"%s\" and everything in it?"
                               : "Delete \"%s\"?",
             F.ents[F.sel].name);
    dlg_confirm("DELETE", body, "DELETE", true, delete_ok, NULL);
}

static void run_cb(lv_event_t *e) { open_selected(); }
static void refresh_cb(lv_event_t *e) { refresh(); }

void files_create(lv_obj_t *p) {
    th_title(p, "FILES", 16, 14, TH_ORANGE);
    F.path_lbl = th_text(p, "", 110, 18, TH_WHITE, &lv_font_montserrat_16);
    F.usage = th_text(p, "", 700, 19, TH_DIM, &lv_font_montserrat_14);
    lv_obj_set_width(F.usage, 308);
    lv_obj_set_style_text_align(F.usage, LV_TEXT_ALIGN_RIGHT, 0);
    int y = 52, x = 16, h = 42;
    F.b_up = th_btn(p, LV_SYMBOL_UP "  UP", x, y, 110, h, up_cb, NULL);
    F.b_new = th_btn(p, LV_SYMBOL_PLUS "  NEW FOLDER", x += 122, y, 170, h, new_cb, NULL);
    F.b_ren = th_btn(p, LV_SYMBOL_EDIT "  RENAME", x += 182, y, 140, h, rename_cb, NULL);
    F.b_del = th_btn(p, LV_SYMBOL_TRASH "  DELETE", x += 152, y, 140, h, delete_cb, NULL);
    F.b_run = th_btn(p, LV_SYMBOL_PLAY "  LAUNCH", x += 152, y, 150, h, run_cb, NULL);
    th_btn(p, LV_SYMBOL_REFRESH "  REFRESH", SCR_W - 16 - 150, y, 150, h, refresh_cb, NULL);
    F.list = th_box(p, 16, 108, SCR_W - 32, PAGE_H - 108 - 34);
    lv_obj_add_flag(F.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(F.list, LV_DIR_VER);
    F.hint = th_text(p, "", 16, PAGE_H - 26, TH_DIM, &lv_font_montserrat_12);
}

void files_show(void) { refresh(); }
void files_poll(void) {}
