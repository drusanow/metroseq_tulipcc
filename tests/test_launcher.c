// test_launcher.c - host tests for the launcher's pure core: path safety
// (everything from the web/console goes through it), app image parsing and
// slot selection.   make -C tests test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appimg.h"
#include "path.h"

static int failures;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void expect_resolve(const char *in, const char *want) {
    char out[300];
    bool ok = path_resolve("/files", in, out, sizeof out);
    if (want == NULL)
        CHECK(!ok, "'%s' should be rejected, got '%s'", in, out);
    else
        CHECK(ok && strcmp(out, want) == 0, "'%s' -> '%s' (ok=%d), want '%s'", in, ok ? out : "",
              ok, want);
}

static void test_paths(void) {
    expect_resolve("/", "/files");
    expect_resolve("", "/files");
    expect_resolve("/apps", "/files/apps");
    expect_resolve("apps/x.bin", "/files/apps/x.bin");
    expect_resolve("//apps///x.bin/", "/files/apps/x.bin");
    expect_resolve("/apps/./x.bin", "/files/apps/x.bin");
    expect_resolve("/apps/sub/../x.bin", "/files/apps/x.bin");
    expect_resolve("/files/apps", "/files/apps");          // root prefix accepted
    expect_resolve("/filesystem", "/files/filesystem");     // ...but not as a bare prefix
    // escapes are refused
    expect_resolve("..", NULL);
    expect_resolve("/../etc", NULL);
    expect_resolve("/apps/../../x", NULL);
    expect_resolve("/files/../x", NULL);
    expect_resolve("a\\b", NULL);
    expect_resolve("bad\x01name", NULL);
    // too long
    char longp[400];
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = 0;
    expect_resolve(longp, NULL);

    CHECK(path_valid_name("My Folder"), "plain name");
    CHECK(!path_valid_name(""), "empty");
    CHECK(!path_valid_name("."), "dot");
    CHECK(!path_valid_name(".."), "dotdot");
    CHECK(!path_valid_name("a/b"), "slash");
    CHECK(!path_valid_name("what?"), "reserved char");
    CHECK(!path_valid_name("trailing."), "trailing dot");
    CHECK(path_has_ext("/x/APP.BIN", ".bin"), "ext case-insensitive");
    CHECK(!path_has_ext(".bin", ".bin"), "ext needs a name");

    char p[64];
    CHECK(path_parent("/files/apps/x.bin", p, sizeof p) && strcmp(p, "/files/apps") == 0, "parent");
    CHECK(!path_parent("/files", p, sizeof p) || strcmp(p, "") != 0, "parent of root");
    CHECK(path_join("/files/apps", "x.bin", p, sizeof p) && strcmp(p, "/files/apps/x.bin") == 0, "join");
    char tiny[8];
    CHECK(!path_join("/files/apps", "x.bin", tiny, sizeof tiny) && tiny[0] == 0,
          "join must fail cleanly, never truncate");

    char u[64];
    strcpy(u, "%2Fapps%2Fmy+app%20v2.bin");
    CHECK(url_decode(u) && strcmp(u, "/apps/my app v2.bin") == 0, "url_decode: %s", u);
    strcpy(u, "bad%2");
    CHECK(!url_decode(u), "truncated escape rejected");
    strcpy(u, "nul%00byte");
    CHECK(!url_decode(u), "embedded NUL rejected");
    printf("paths: resolve/escape/name/join/url-decode cases ok\n");
}

// Build a minimal ESP32-S3 app image header + app description.
static void fake_image(uint8_t *b, const char *name, const char *ver, uint8_t sha0) {
    memset(b, 0, APPIMG_HEADER_BYTES);
    b[0] = 0xE9;
    b[1] = 3;     // segments
    b[12] = 9;    // chip id ESP32-S3
    uint8_t *d = b + 32;
    d[0] = 0x32; d[1] = 0x54; d[2] = 0xCD; d[3] = 0xAB;   // ESP_APP_DESC_MAGIC_WORD
    strcpy((char *)d + 16, ver);
    strcpy((char *)d + 48, name);
    strcpy((char *)d + 96, "Oct 26 2026");
    strcpy((char *)d + 112, "v5.3.2");
    memset(d + 144, sha0, 32);
}

static void test_appimg(void) {
    uint8_t b[APPIMG_HEADER_BYTES];
    app_info_t info;
    fake_image(b, "metroseq", "1.2.0", 7);
    CHECK(appimg_parse(b, sizeof b, &info) && strcmp(info.name, "metroseq") == 0 &&
              strcmp(info.version, "1.2.0") == 0 && strcmp(info.idf, "v5.3.2") == 0 &&
              info.elf_sha[0] == 7,
          "parse fake image");
    b[12] = 0;   // ESP32, not S3
    CHECK(!appimg_parse(b, sizeof b, &info), "wrong chip rejected");
    fake_image(b, "x", "1", 1);
    b[0] = 0x7F;
    CHECK(!appimg_parse(b, sizeof b, &info), "not an image rejected");
    fake_image(b, "x", "1", 1);
    b[32] = 0;
    CHECK(!appimg_parse(b, sizeof b, &info), "no app description rejected");
    CHECK(!appimg_parse(b, 40, &info), "short buffer rejected");

    // The real thing: the metroseq app built by idf.py, if it's been built.
    const char *real = "../apps/metroseq/build/metroseq.bin";
    FILE *f = fopen(real, "rb");
    if (f) {
        uint8_t h[APPIMG_HEADER_BYTES];
        size_t n = fread(h, 1, sizeof h, f);
        fclose(f);
        CHECK(appimg_parse(h, n, &info) && strcmp(info.name, "metroseq") == 0,
              "real metroseq.bin: name '%s'", info.name);
        printf("appimg: fake images ok; real %s -> \"%s\" v%s (IDF %s)\n", real, info.name,
               info.version, info.idf);
    } else {
        printf("appimg: fake images ok (build apps/metroseq to also check a real image)\n");
    }
}

static void test_slots(void) {
    app_slot_t s[APP_SLOTS] = {0};
    uint8_t a[32], b[32];
    memset(a, 1, 32);
    memset(b, 2, 32);
    CHECK(slots_find(s, APP_SLOTS, a, 100) == -1, "empty table");
    CHECK(slots_pick(s, APP_SLOTS) == 0, "first empty");
    s[0].used = 1; memcpy(s[0].elf_sha, a, 32); s[0].size = 100; s[0].last_used = 5;
    s[1].used = 1; memcpy(s[1].elf_sha, b, 32); s[1].size = 200; s[1].last_used = 9;
    CHECK(slots_find(s, APP_SLOTS, a, 100) == 0, "found a");
    CHECK(slots_find(s, APP_SLOTS, a, 101) == -1, "same build different size = different");
    CHECK(slots_pick(s, APP_SLOTS) == 2, "next empty");
    for (int i = 2; i < APP_SLOTS; i++) { s[i].used = 1; s[i].last_used = 20 + i; }
    CHECK(slots_pick(s, APP_SLOTS) == 0, "least recently used when full");
    printf("slots: find / first-empty / LRU ok\n");
}

int main(void) {
    test_paths();
    test_appimg();
    test_slots();
    printf(failures ? "\n%d FAILURES\n" : "\nall launcher tests passed\n", failures);
    return failures ? 1 : 0;
}
