#ifndef TRAY_COUNTDOWN_TOOLTIP_H
#define TRAY_COUNTDOWN_TOOLTIP_H

#include <stddef.h>
#include <stdio.h>
#include "tray_session_finished_action.h"

static inline int tray_countdown_display_seconds(unsigned long long remaining_ms) {
    return (int)(remaining_ms / 1000ULL + (remaining_ms % 1000ULL != 0));
}

static inline const char* tray_countdown_action_phrase(SessionFinishedAction action) {
    switch (action) {
        case SESSION_FINISHED_SHUTDOWN:
            return "forcibly shut down";
        case SESSION_FINISHED_SHUTDOWN_GRACEFUL:
            return "gracefully shut down";
        case SESSION_FINISHED_SLEEP:
        case SESSION_FINISHED_NONE:
        default:
            return "sleep";
    }
}

static inline void tray_format_countdown_tooltip(char* tip,
                                                 size_t tip_size,
                                                 SessionFinishedAction action,
                                                 int countdown_seconds) {
    snprintf(tip, tip_size, "nosleep - System will %s in %d %s",
             tray_countdown_action_phrase(action), countdown_seconds,
             countdown_seconds == 1 ? "second" : "seconds");
}

#endif
