#include "tray_stop_guard.h"

#include <stdio.h>

static int failures;

static void expect_work(const char* scenario,
                        bool is_running,
                        bool countdown_active,
                        bool sleep_timer_present,
                        bool shutdown_timer_present) {
    if (!tray_stop_has_work(is_running, countdown_active,
                            sleep_timer_present, shutdown_timer_present)) {
        fprintf(stderr, "FAIL: %s must enter stop cleanup\n", scenario);
        ++failures;
    }
}

static void expect_idle(const char* scenario,
                        bool is_running,
                        bool countdown_active,
                        bool sleep_timer_present,
                        bool shutdown_timer_present) {
    if (tray_stop_has_work(is_running, countdown_active,
                           sleep_timer_present, shutdown_timer_present)) {
        fprintf(stderr, "FAIL: %s must remain eligible for idle return\n", scenario);
        ++failures;
    }
}

static void expect_countdown_work(const char* scenario,
                                  bool countdown_active,
                                  bool countdown_thread_present) {
    if (!tray_countdown_has_work(countdown_active, countdown_thread_present)) {
        fprintf(stderr, "FAIL: %s must join countdown cleanup\n", scenario);
        ++failures;
    }
}

static void expect_countdown_idle(const char* scenario,
                                  bool countdown_active,
                                  bool countdown_thread_present) {
    if (tray_countdown_has_work(countdown_active, countdown_thread_present)) {
        fprintf(stderr, "FAIL: %s must remain eligible for countdown idle return\n", scenario);
        ++failures;
    }
}

int main(void) {
    expect_work("pending sleep action", false, false, true, false);
    expect_work("pending shutdown action", false, false, false, true);
    expect_work("running session", true, false, false, false);
    expect_work("active countdown", false, true, false, false);
    expect_idle("no active work", false, false, false, false);
    expect_countdown_work("active countdown", true, false);
    expect_countdown_work("finishing countdown thread", false, true);
    expect_countdown_idle("no countdown work", false, false);

    if (failures != 0) return 1;
    puts("PASS: stop guards include delayed actions and countdown thread handles");
    return 0;
}
