// appimg.c - see appimg.h.
#include "appimg.h"

#include <string.h>

#define IMAGE_MAGIC 0xE9
#define APP_DESC_MAGIC 0xABCD5432u
#define DESC_OFF 32   // 24-byte image header + 8-byte first segment header

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void cpy_str(char *dst, const uint8_t *src, size_t field) {
    size_t i = 0;
    for (; i < field && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

bool appimg_parse(const uint8_t *buf, size_t len, app_info_t *out) {
    if (len < DESC_OFF + 176) return false;
    if (buf[0] != IMAGE_MAGIC) return false;
    if (buf[1] == 0 || buf[1] > 16) return false;   // segment count
    uint16_t chip = (uint16_t)(buf[12] | (buf[13] << 8));
    if (chip != APPIMG_CHIP_ESP32S3) return false;
    const uint8_t *d = buf + DESC_OFF;
    if (rd32(d) != APP_DESC_MAGIC) return false;
    memset(out, 0, sizeof *out);
    cpy_str(out->version, d + 16, 32);
    cpy_str(out->name, d + 48, 32);
    cpy_str(out->time, d + 80, 16);
    cpy_str(out->date, d + 96, 16);
    cpy_str(out->idf, d + 112, 32);
    memcpy(out->elf_sha, d + 144, 32);
    return out->name[0] != 0;
}

bool appimg_manifest(const uint8_t *buf, size_t len, app_manifest_t *out) {
    if (len < (size_t)APP_MANIFEST_END) return false;
    app_info_t info;
    if (!appimg_parse(buf, len, &info)) return false;
    memcpy(out, buf + APP_MANIFEST_OFFSET, sizeof *out);   // byte copy: buf may be unaligned
    if (out->magic != APP_MANIFEST_MAGIC || out->version != APP_MANIFEST_VERSION || out->count == 0 ||
        out->count > APP_MANIFEST_MAX)
        return false;
    for (int i = 0; i < out->count; i++) {
        app_manifest_entry_t *e = &out->app[i];
        e->name[sizeof e->name - 1] = 0;
        e->title[sizeof e->title - 1] = 0;
        e->blurb[sizeof e->blurb - 1] = 0;
        if (!e->name[0]) return false;
        if (!e->title[0]) memcpy(e->title, e->name, sizeof e->title);
        if (e->role > APP_ROLE_SYNTH) e->role = APP_ROLE_SYNTH;
    }
    return true;
}

// the string value of "key", or false
static bool json_str(const char *txt, const char *key, char *out, size_t len) {
    char pat[24];
    size_t kl = strlen(key);
    if (kl + 3 > sizeof pat) return false;
    pat[0] = '"';
    memcpy(pat + 1, key, kl);
    pat[kl + 1] = '"';
    pat[kl + 2] = 0;
    const char *p = strstr(txt, pat);
    if (!p) return false;
    p += kl + 2;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (n + 1 < len) out[n++] = *p;
    }
    out[n] = 0;
    return *p == '"';
}

bool appimg_app_json(const char *txt, app_manifest_entry_t *out) {
    memset(out, 0, sizeof *out);
    char role[16];
    if (!json_str(txt, "name", out->name, sizeof out->name) || !out->name[0]) return false;
    if (!json_str(txt, "role", role, sizeof role)) return false;
    if (!strcmp(role, "sequencer"))
        out->role = APP_ROLE_SEQUENCER;
    else if (!strcmp(role, "drums"))
        out->role = APP_ROLE_DRUMS;
    else if (!strcmp(role, "synth"))
        out->role = APP_ROLE_SYNTH;
    else
        return false;
    if (!json_str(txt, "title", out->title, sizeof out->title) || !out->title[0])
        memcpy(out->title, out->name, sizeof out->title);
    json_str(txt, "blurb", out->blurb, sizeof out->blurb);
    return true;
}

int slots_find(const app_slot_t *s, int n, const uint8_t sha[32], uint32_t size) {
    for (int i = 0; i < n; i++)
        if (s[i].used && s[i].size == size && memcmp(s[i].elf_sha, sha, 32) == 0) return i;
    return -1;
}

int slots_pick(const app_slot_t *s, int n) {
    int best = 0;
    for (int i = 0; i < n; i++) {
        if (!s[i].used) return i;
        if (s[i].last_used < s[best].last_used) best = i;
    }
    return best;
}
