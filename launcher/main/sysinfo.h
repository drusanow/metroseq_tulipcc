// sysinfo.h - system actions and facts for the SETTINGS page (keeps the UI
// code free of ESP-IDF headers so it also renders in tools/ui_preview).
#pragma once

#include <stdbool.h>
#include <stddef.h>

void sysinfo_text(char *buf, size_t n);   // multi-line "about" block
void sys_restart(void);
void sys_factory_reset(void);             // erase NVS, restart
bool sys_format_storage(void);
