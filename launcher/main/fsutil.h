// fsutil.h - file operations for the browser, the web API and the console.
// All paths are absolute and already resolved under /files (core/path.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char name[129];
    bool dir;
    uint32_t size;
} fs_entry_t;

// List a directory: folders first, then files, each alphabetical
// (case-insensitive). Hidden entries (starting with '.') are skipped.
// Returns the count, or -1 if the directory can't be opened.
int fs_list(const char *dir, fs_entry_t *out, int max);

bool fs_is_dir(const char *path);
bool fs_exists(const char *path);
bool fs_mkdir(const char *path);
bool fs_remove(const char *path);   // files, or folders recursively
bool fs_rename(const char *from, const char *to);
