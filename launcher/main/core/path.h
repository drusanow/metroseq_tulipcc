// path.h - path handling for the file browser, web API and console.
// Pure C (host-tested): every path that comes from the web or the console
// goes through path_resolve(), which is what keeps it inside /files.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Resolve `in` (absolute like "/apps/x.bin", or relative to `root`) into an
// absolute path under `root`, collapsing "." / ".." and duplicate slashes.
// Returns false if it would escape root, is too long, or has bad chars.
bool path_resolve(const char *root, const char *in, char *out, size_t n);

// dir + "/" + name into out. False (and nothing usable) if it doesn't fit.
bool path_join(const char *dir, const char *name, char *out, size_t n);

// "/files/apps/x.bin" -> "/files/apps". False at the root.
bool path_parent(const char *p, char *out, size_t n);

// Pointer to the last component ("x.bin").
const char *path_basename(const char *p);

// A single file/folder name a user may create: 1..64 chars, no '/', '\',
// control chars or FAT-reserved characters, not "." / "..", no trailing
// space or dot.
bool path_valid_name(const char *name);

// Case-insensitive extension test, ext like ".bin".
bool path_has_ext(const char *p, const char *ext);

// In-place %XX / '+' URL decoding. Returns false on malformed escapes or
// an embedded NUL.
bool url_decode(char *s);
