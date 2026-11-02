// dialog.h - modal dialogs on the top layer.
#pragma once

#include <stdbool.h>

typedef void (*dlg_ok_cb)(void *ud);
typedef void (*dlg_text_cb)(const char *text, void *ud);

// "Are you sure?" - ok runs on OK; danger paints OK red.
void dlg_confirm(const char *title, const char *body, const char *ok_label, bool danger,
                 dlg_ok_cb ok, void *ud);

// One-line text entry with the on-screen keyboard. `password` hides input.
void dlg_prompt(const char *title, const char *initial, const char *ok_label, bool password,
                dlg_text_cb ok, void *ud);

bool dlg_open(void);
void dlg_close(void);
