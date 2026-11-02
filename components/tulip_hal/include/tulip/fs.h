// fs.h - the shared file store: FAT (wear-levelled) on the "storage"
// partition, mounted at /files. Apps live in /files/apps; any app can keep
// its own data in /files/<something>.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef TULIP_FS_ROOT   // (the host UI preview points it at a local folder)
#define TULIP_FS_ROOT "/files"
#endif
#define TULIP_APPS_DIR TULIP_FS_ROOT "/apps"

// Mount /files. The launcher passes format_if_needed=true (first boot, or a
// damaged volume); apps should pass false.
bool tulip_fs_mount(bool format_if_needed);
bool tulip_fs_mounted(void);

// Total and free bytes of the volume.
bool tulip_fs_usage(uint64_t *total, uint64_t *free_bytes);

// Erase everything and start with an empty volume (launcher only).
bool tulip_fs_format(void);
