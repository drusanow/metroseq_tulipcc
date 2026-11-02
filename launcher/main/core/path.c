// path.c - see path.h.
#define _POSIX_C_SOURCE 200809L   // strtok_r, strcasecmp on strict C11 hosts
#include "path.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define MAX_SEGS 32

bool path_resolve(const char *root, const char *in, char *out, size_t n) {
    size_t rl = strlen(root);
    if (!in || rl + 1 >= n) return false;
    // copy and split
    char tmp[512];
    size_t il = strlen(in);
    if (il >= sizeof tmp) return false;
    memcpy(tmp, in, il + 1);
    // Accept paths given with the root prefix too ("/files/apps").
    char *p = tmp;
    if (strncmp(p, root, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) p += rl;
    const char *segs[MAX_SEGS];
    int ns = 0;
    char *save = NULL;
    for (char *s = strtok_r(p, "/", &save); s; s = strtok_r(NULL, "/", &save)) {
        if (strcmp(s, ".") == 0) continue;
        if (strcmp(s, "..") == 0) {
            if (ns == 0) return false;   // would leave the root
            ns--;
            continue;
        }
        for (const char *c = s; *c; c++)
            if ((unsigned char)*c < 0x20 || *c == '\\') return false;
        if (ns == MAX_SEGS) return false;
        segs[ns++] = s;
    }
    size_t len = rl;
    memcpy(out, root, rl);
    for (int i = 0; i < ns; i++) {
        size_t sl = strlen(segs[i]);
        if (len + 1 + sl + 1 > n) return false;
        out[len++] = '/';
        memcpy(out + len, segs[i], sl);
        len += sl;
    }
    out[len] = 0;
    return true;
}

bool path_join(const char *dir, const char *name, char *out, size_t n) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > n) {
        if (n) out[0] = 0;
        return false;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
    return true;
}

bool path_parent(const char *p, char *out, size_t n) {
    const char *slash = strrchr(p, '/');
    if (!slash || slash == p) return false;
    size_t len = (size_t)(slash - p);
    if (len + 1 > n) return false;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

const char *path_basename(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

bool path_valid_name(const char *name) {
    size_t l = strlen(name);
    if (l == 0 || l > 64) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (name[l - 1] == ' ' || name[l - 1] == '.') return false;
    for (const char *c = name; *c; c++) {
        if ((unsigned char)*c < 0x20) return false;
        if (strchr("/\\:*?\"<>|", *c)) return false;
    }
    return true;
}

bool path_has_ext(const char *p, const char *ext) {
    size_t pl = strlen(p), el = strlen(ext);
    return pl > el && strcasecmp(p + pl - el, ext) == 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool url_decode(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%') {
            int h = hexval(r[1]), l = h < 0 ? -1 : hexval(r[2]);
            if (h < 0 || l < 0) return false;
            int v = h * 16 + l;
            if (v == 0) return false;
            *w++ = (char)v;
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    return true;
}
