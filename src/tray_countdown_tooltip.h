#ifndef TRAY_COUNTDOWN_TOOLTIP_H
#define TRAY_COUNTDOWN_TOOLTIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static inline int tray_countdown_display_seconds(unsigned long long remaining_ms) {
    return (int)(remaining_ms / 1000ULL + (remaining_ms % 1000ULL != 0));
}

static inline void tray_format_countdown_tooltip(char* tip,
                                                 size_t tip_size,
                                                 bool shutdown_scheduled,
                                                 int countdown_seconds) {
    snprintf(tip, tip_size, "nosleep - System will %s in %d seconds",
             shutdown_scheduled ? "shut down" : "sleep", countdown_seconds);
}

#endif
