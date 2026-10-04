#ifndef TRAY_COUNTDOWN_ICON_H
#define TRAY_COUNTDOWN_ICON_H

#include <windows.h>
#include <stdbool.h>

#define TRAY_COUNTDOWN_NUMBERED_ICON_COUNT 61

typedef HICON (*TrayCountdownIconFactory)(int number);
typedef BOOL (WINAPI *TrayCountdownIconDestroyer)(HICON icon);

static inline HICON tray_countdown_display_icon(
    HICON numbered_icons[TRAY_COUNTDOWN_NUMBERED_ICON_COUNT],
    int countdown_seconds,
    bool blink_state,
    HICON blank_icon,
    HICON fallback_icon,
    TrayCountdownIconFactory create_numbered_icon) {
    if (!blink_state) {
        return blank_icon ? blank_icon : fallback_icon;
    }

    if (countdown_seconds < 0 ||
        countdown_seconds >= TRAY_COUNTDOWN_NUMBERED_ICON_COUNT) {
        return fallback_icon;
    }

    if (!numbered_icons[countdown_seconds]) {
        numbered_icons[countdown_seconds] = create_numbered_icon(countdown_seconds);
    }

    return numbered_icons[countdown_seconds]
        ? numbered_icons[countdown_seconds]
        : fallback_icon;
}

static inline void tray_destroy_countdown_icon_cache(
    HICON numbered_icons[TRAY_COUNTDOWN_NUMBERED_ICON_COUNT],
    TrayCountdownIconDestroyer destroy_icon) {
    for (int i = 0; i < TRAY_COUNTDOWN_NUMBERED_ICON_COUNT; ++i) {
        if (numbered_icons[i]) {
            destroy_icon(numbered_icons[i]);
            numbered_icons[i] = NULL;
        }
    }
}

#endif
