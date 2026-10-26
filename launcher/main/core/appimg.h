// appimg.h - recognise a Tulip app (an ESP-IDF application image) and
// choose which OTA slot to run it from. Pure C, host-tested.
//
// An app .bin is exactly what `idf.py build` produces: the image header,
// segments, and the esp_app_desc_t the build system embeds (project name,
// version, build date, IDF version and the ELF's SHA-256). The launcher
// needs nothing else - no separate manifest.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPIMG_HEADER_BYTES 288   // enough to reach the app description
#define APPIMG_CHIP_ESP32S3 9

typedef struct {
    char name[33];        // project_name
    char version[33];
    char date[17];
    char time[17];
    char idf[33];
    uint8_t elf_sha[32];  // identifies the build
} app_info_t;

// Parse the start of a file/partition. False if it isn't an ESP32-S3 app
// image with an app description.
bool appimg_parse(const uint8_t *buf, size_t len, app_info_t *out);

// --- slot cache -------------------------------------------------------------

#define APP_SLOTS 4

typedef struct {
    uint8_t used;
    uint8_t elf_sha[32];
    uint32_t size;        // image file size
    uint32_t last_used;   // launch counter value when last run
    char name[33];
    char version[33];
} app_slot_t;

// Slot already holding this exact build, or -1.
int slots_find(const app_slot_t *s, int n, const uint8_t sha[32], uint32_t size);

// Slot to overwrite: the first empty one, else the least recently used.
int slots_pick(const app_slot_t *s, int n);
