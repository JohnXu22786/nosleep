#ifndef TRAY_NUMBERED_ICON_H
#define TRAY_NUMBERED_ICON_H

#include <windows.h>

typedef BOOL (WINAPI *TrayNumberedIconDestroyer)(HICON icon);

static inline void tray_release_numbered_icon(
    HICON *current_icon,
    int current_number,
    HICON active_icon,
    TrayNumberedIconDestroyer destroy_icon) {
    if (*current_icon) {
        if (*current_icon != active_icon &&
            (current_number < 0 || current_number >= 60)) {
            destroy_icon(*current_icon);
        }
        *current_icon = NULL;
    }
}

#endif
