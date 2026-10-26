// web.h - file transfer over Wi-Fi: a small web app at http://tulip.local
// plus a JSON API (used by the web app and tools/tulip.py).
//
//   GET  /                 the web app
//   GET  /api/info         device, storage and app list
//   GET  /api/ls?path=     list a folder
//   GET  /api/file?path=   download
//   PUT  /api/file?path=   upload (raw body), replaces an existing file
//   POST /api/mkdir?path=  POST /api/rm?path=  POST /api/mv?from=&to=
//   POST /api/launch?path= install + run an app (the Tulip reboots into it)
//
// Every /api call needs the header  X-Tulip-Pin: <the PIN on the Tulip's
// Wi-Fi page>. Paths are relative to /files and can never leave it.
#pragma once

#include <stdbool.h>

bool web_start(void);
void web_stop(void);
bool web_running(void);
