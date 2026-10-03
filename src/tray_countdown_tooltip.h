#ifndef TRAY_COUNTDOWN_TOOLTIP_H
#define TRAY_COUNTDOWN_TOOLTIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static inline void tray_format_countdown_tooltip(char* tip,
                                                 size_t tip_size,
                                                 bool shutdown_scheduled,
                                                 int countdown_seconds) {
    snprintf(tip, tip_size, "nosleep - System will %s in %d seconds",
             shutdown_scheduled ? "shut down" : "sleep", countdown_seconds);
}

#endif
