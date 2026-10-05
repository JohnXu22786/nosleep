#ifndef TRAY_INDEFINITE_ICON_H
#define TRAY_INDEFINITE_ICON_H

#include <windows.h>

typedef HICON (*TrayNumberedIconFactory)(int number);
typedef BOOL (WINAPI *TrayIconDestroyer)(HICON icon);

static inline void tray_ensure_indefinite_icon(
    HICON *current_icon,
    int *current_number,
    HICON active_icon,
    TrayNumberedIconFactory create_numbered_icon,
    TrayIconDestroyer destroy_icon) {
    // Keep the infinity icon on repeated updates; release only a prior number icon.
    if (*current_icon && (*current_number != -1 || *current_icon == active_icon)) {
        if (*current_icon != active_icon &&
            (*current_number < 0 || *current_number >= 60)) {
            destroy_icon(*current_icon);
        }
        *current_icon = NULL;
        *current_number = -1;
    }

    if (!*current_icon) {
        *current_icon = create_numbered_icon(-1);
        *current_number = -1;
    }
}

#endif
