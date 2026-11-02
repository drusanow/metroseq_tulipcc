// fsutil.c - see fsutil.h.
#include "fsutil.h"

#include "core/path.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int cmp_entry(const void *a, const void *b) {
    const fs_entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int fs_list(const char *dir, fs_entry_t *out, int max) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    char p[320];
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        fs_entry_t *f = &out[n];
        size_t nl = strlen(e->d_name);
        if (nl >= sizeof f->name) continue;   // longer than we can show or address
        memcpy(f->name, e->d_name, nl + 1);
        struct stat st;
        if (!path_join(dir, e->d_name, p, sizeof p)) continue;
        f->dir = e->d_type == DT_DIR;
        f->size = 0;
        if (!f->dir && stat(p, &st) == 0) f->size = (uint32_t)st.st_size;
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof *out, cmp_entry);
    return n;
}

bool fs_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool fs_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool fs_mkdir(const char *path) {
    return mkdir(path, 0775) == 0;
}

static bool rm_tree(const char *path, int depth) {
    if (!fs_is_dir(path)) return unlink(path) == 0;
    if (depth > 16) return false;
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    bool ok = true;
    char *child = malloc(320);
    while (ok && child && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        // never act on a truncated path: that could name a different file
        ok = path_join(path, e->d_name, child, 320) && rm_tree(child, depth + 1);
    }
    free(child);
    closedir(d);
    return ok && rmdir(path) == 0;
}

bool fs_remove(const char *path) {
    return rm_tree(path, 0);
}

bool fs_rename(const char *from, const char *to) {
    if (fs_exists(to)) {
        if (fs_is_dir(to)) return false;   // never silently replace a folder
        unlink(to);                         // FAT rename won't overwrite
    }
    return rename(from, to) == 0;
}
