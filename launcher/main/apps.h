// apps.h - finding, installing and launching apps.
//
// An app is an ESP-IDF application image (.bin) in /files/apps (or a
// folder inside it). Launching copies the image into one of four OTA slots
// - unless a slot already holds that exact build, in which case it boots
// immediately - then reboots into it. The slot table lives in NVS.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/appimg.h"
#include "tulip/fs.h"

#define MAX_APPS 48

typedef struct {
    char path[192];
    app_info_t info;
    uint32_t size;
    int slot;   // OTA slot already holding this build, or -1
} app_entry_t;

typedef enum {
    INSTALL_IDLE = 0,
    INSTALL_ERASING,
    INSTALL_WRITING,
    INSTALL_STARTING,   // about to reboot into the app
    INSTALL_FAILED,
} install_state_t;

typedef struct {
    volatile int state;   // install_state_t
    volatile int pct;
    char name[33];
    char msg[96];
} install_status_t;

extern install_status_t g_install;

// Start-up progress, shown by the UI while the launcher gets ready (the
// first start after flashing formats /files and copies the preloaded apps
// in, which takes a while).
typedef struct {
    volatile bool done;
    volatile int pct;      // -1: no bar
    char title[48];
    char msg[96];
} boot_status_t;

extern boot_status_t g_boot;
void boot_status(const char *title, const char *msg, int pct);

// Load the slot table and reconcile it with what the slots really hold.
// If a slot holds an app that isn't in /files/apps (e.g. preloaded with
// the first flash), copy it there so it shows up.
void apps_init(void);

// Scan /files/apps (one level of sub-folders too) for valid app images.
int apps_scan(app_entry_t *out, int max);

// Read an app's description from its file. False if it isn't an app.
bool apps_read_info(const char *path, app_info_t *info, uint32_t *size);

// The apps inside a Tulip Studio image (its manifest). False if it isn't one.
bool apps_read_manifest(const char *path, app_manifest_t *m);

// Apps Tulip Studio loads at start-up (an experiment): each <name>.elf in
// /files/studio/apps with its <name>.json beside it. Returns how many.
#define STUDIO_APPS_DIR TULIP_FS_ROOT "/studio/apps"
int apps_loaded(app_manifest_entry_t *out, int max);

// Start launching (in the background; watch g_install). False if busy or
// not an app. mode: how a Tulip Studio image starts - "" the Studio, or an
// app's name to run it on its own (tulip_boot_mode_set); title: what the
// progress screen calls it (NULL: the image's name).
bool apps_launch(const char *path);
bool apps_launch_as(const char *path, const char *mode, const char *title);

int apps_slots(app_slot_t out[APP_SLOTS]);
void apps_clear_slots(void);
