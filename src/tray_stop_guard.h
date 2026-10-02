#ifndef TRAY_STOP_GUARD_H
#define TRAY_STOP_GUARD_H

#include <stdbool.h>

static inline bool tray_stop_has_work(bool is_running,
                                      bool countdown_active,
                                      bool sleep_timer_present,
                                      bool shutdown_timer_present) {
    return is_running || countdown_active || sleep_timer_present || shutdown_timer_present;
}

static inline bool tray_countdown_has_work(bool countdown_active,
                                           bool countdown_thread_present) {
    return countdown_active || countdown_thread_present;
}

#endif
