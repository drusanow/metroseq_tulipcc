// app_manifest.h - the list of apps inside a Tulip Studio image, for the
// launcher to read without running it.
//
// The Studio build generates it from every modules/*/app.json and places it
// in the image's custom app description section (.rodata_custom_desc), which
// the linker puts right after the esp_app_desc_t: at byte 288 of the .bin
// (24-byte image header + 8-byte segment header + the 256-byte description).
// The launcher reads the first APP_MANIFEST_END bytes of studio.bin and
// shows a tile per app. Plain C: host-tested (tests/test_launcher.c).
#pragma once

#include <stdint.h>

#define APP_MANIFEST_MAGIC 0x50415354u   // "TSAP"
#define APP_MANIFEST_VERSION 1
#define APP_MANIFEST_MAX 16
#define APP_MANIFEST_OFFSET 288

enum { APP_ROLE_SEQUENCER = 0, APP_ROLE_DRUMS, APP_ROLE_SYNTH };   // = tmod_role_t

typedef struct {
    char name[16];    // the module's tmod_t.name: the boot mode that runs it alone
    char title[16];   // "METROSEQ"
    uint8_t role;     // APP_ROLE_*
    uint8_t reserved[3];
    char blurb[80];
} app_manifest_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t count;
    uint8_t reserved;
    app_manifest_entry_t app[APP_MANIFEST_MAX];
} app_manifest_t;

#define APP_MANIFEST_END (APP_MANIFEST_OFFSET + (int)sizeof(app_manifest_t))
