#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

python3 - "$SCRIPT_DIR/src/tray.c" "$TMP_DIR/test_tray_manual_stop_expiry_race.c" <<'PY'
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])
def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*[^;]*?\{", source)
    assert definition, f"could not find definition for {signature}"
    start = definition.start()
    opening = definition.end() - 1
    depth = 0
    state = "code"
    i = opening
    while i < len(source):
        char = source[i]
        next_two = source[i : i + 2]
        if state == "code":
            if next_two == "//":
                state = "line_comment"
                i += 2
                continue
            if next_two == "/*":
                state = "block_comment"
                i += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start : i + 1]
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if next_two == "*/":
                state = "code"
                i += 2
                continue
        elif state in ("string", "char"):
            if char == "\\":
                i += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function: {signature}")


def extract_lock_region(function, marker):
    marker_position = function.index(marker)
    start = function.index(
        "AcquireSRWLockExclusive(&tray->delayed_action_lock);", marker_position
    )
    end_marker = "ReleaseSRWLockExclusive(&tray->delayed_action_lock);"
    end = function.index(end_marker, start) + len(end_marker)
    return function[start:end]


stop_function = extract_function(
    "static bool tray_stop_nosleep_for_session(NoSleepTray* tray,"
)
manual_notice_function = extract_function(
    "static void tray_show_manual_stop_notification(NoSleepTray* tray,"
)
reap_function = extract_function(
    "static void tray_reap_delayed_action_handle(NoSleepTray* tray,"
)
duration_timer = extract_function(
    "static DWORD WINAPI tray_duration_timer(LPVOID lpParam)"
)
sleep_startup = extract_lock_region(duration_timer, "HANDLE sleep_timer = NULL;")

prelude = r'''#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "tray_stop_guard.h"

typedef unsigned int DWORD;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef void *HANDLE;
typedef pthread_mutex_t SRWLOCK;
typedef pthread_cond_t CONDITION_VARIABLE;
typedef enum {
    SESSION_FINISHED_NONE,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN_GRACEFUL
} SessionFinishedAction;
typedef enum {
    NOTIFY_EVENT_COUNTDOWN_CANCEL,
    NOTIFY_EVENT_TIMER_EXPIRED,
    NOTIFY_EVENT_SESSION_STOP,
    NOTIFY_EVENT_ERROR
} NotifyEventId;

#define TRUE 1
#define FALSE 0
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0
#define ES_CONTINUOUS 0x80000000u
#define CreateThread(...) NULL
#define DEBUG_LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ATOMIC_STORE_BOOL(destination, value) \
    __atomic_store_n((destination), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD_BOOL(source) \
    __atomic_load_n((source), __ATOMIC_SEQ_CST)
#define ATOMIC_EXCHANGE_BOOL(destination, value) \
    __atomic_exchange_n((destination), (value), __ATOMIC_SEQ_CST)

typedef struct NoSleepTray {
    SRWLOCK delayed_action_lock;
    CONDITION_VARIABLE stop_condition;
    DWORD stopping_thread_id;
    bool stopping;
    bool stopping_expiry_notification_suppressed;
    bool is_running;
    bool duration_expired;
    bool core_init_failed;
    bool core_init_succeeded;
    bool nosleep_run_failed;
    bool starting_nosleep;
    bool delayed_countdown_starting;
    bool delayed_sleep_countdown_active;
    bool session_action_cancelled;
    bool session_action_pending;
    bool sleep_action_claimed;
    bool shutdown_action_claimed;
    HANDLE stop_event;
    HANDLE timer_thread;
    HANDLE nosleep_thread;
    DWORD timer_thread_id;
    DWORD nosleep_thread_id;
    HANDLE sleep_timer;
    HANDLE sleep_stop_event;
    HANDLE shutdown_timer;
    HANDLE shutdown_stop_event;
    SessionFinishedAction shutdown_action;
    SessionFinishedAction countdown_action;
    ULONGLONG start_tick64;
} NoSleepTray;

static void tray_update_stop_menu_item_internal(NoSleepTray *tray,
                                                bool lock_held) {
    (void)tray;
    (void)lock_held;
}

static NoSleepTray *active_tray;
static __thread DWORD current_thread_id;
static pthread_mutex_t harness_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t harness_changed = PTHREAD_COND_INITIALIZER;
static bool expiry_waiting_for_worker;
static bool manual_stop_waiting_for_cleanup;
static bool defer_stop_condition_wake;
static bool release_expiry_worker_wait;
static DWORD blocked_wait_thread_id;
static HANDLE blocked_wait_handle;
static int notification_count;
static int stop_menu_update_count;
static NotifyEventId notification_event;
static char notification_title[128];
static char notification_message[256];

static void AcquireSRWLockExclusive(SRWLOCK *lock) {
    pthread_mutex_lock(lock);
}

static void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    pthread_mutex_unlock(lock);
}

static bool SleepConditionVariableSRW(CONDITION_VARIABLE *condition,
                                      SRWLOCK *lock,
                                      DWORD milliseconds,
                                      ULONG flags) {
    (void)milliseconds;
    (void)flags;
    if (current_thread_id == 2) {
        pthread_mutex_lock(&harness_lock);
        manual_stop_waiting_for_cleanup = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
    }
    return pthread_cond_wait(condition, lock) == 0 ? TRUE : FALSE;
}

static void WakeAllConditionVariable(CONDITION_VARIABLE *condition) {
    if (!defer_stop_condition_wake) {
        pthread_cond_broadcast(condition);
    }
}

static DWORD GetCurrentThreadId(void) {
    return current_thread_id;
}

static DWORD GetThreadId(HANDLE thread) {
    (void)thread;
    return 0;
}

static DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    if (current_thread_id == blocked_wait_thread_id &&
        handle == blocked_wait_handle) {
        pthread_mutex_lock(&harness_lock);
        expiry_waiting_for_worker = true;
        pthread_cond_broadcast(&harness_changed);
        while (!release_expiry_worker_wait) {
            pthread_cond_wait(&harness_changed, &harness_lock);
        }
        pthread_mutex_unlock(&harness_lock);
    }
    (void)milliseconds;
    return WAIT_OBJECT_0;
}

static BOOL SetEvent(HANDLE event) {
    (void)event;
    return TRUE;
}

static BOOL ResetEvent(HANDLE event) {
    (void)event;
    return TRUE;
}

static BOOL CloseHandle(HANDLE handle) {
    (void)handle;
    return TRUE;
}

static DWORD SetThreadExecutionState(DWORD state) {
    return state;
}

static ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    (void)start_tick64;
    return 0;
}

static void tray_wait_for_delayed_countdown_start(NoSleepTray *tray) {
    (void)tray;
}

static void tray_stop_countdown(NoSleepTray *tray) {
    (void)tray;
}

static void tray_show_notification(NoSleepTray *tray,
                                   NotifyEventId event,
                                   const char *title,
                                   const char *message,
                                   bool critical) {
    (void)tray;
    (void)critical;
    pthread_mutex_lock(&harness_lock);
    ++notification_count;
    notification_event = event;
    snprintf(notification_title, sizeof(notification_title), "%s", title);
    snprintf(notification_message, sizeof(notification_message), "%s", message);
    pthread_mutex_unlock(&harness_lock);
}

static void tray_update_stop_menu_item(NoSleepTray *tray) {
    (void)tray;
    ++stop_menu_update_count;
}

static void tray_update_icon(NoSleepTray *tray) {
    (void)tray;
}
'''

main = r'''
static void *expiry_cleanup(void *argument) {
    current_thread_id = 1;
    NoSleepTray *tray = (NoSleepTray *)argument;
    (void)tray_stop_nosleep_for_session(tray, 1, true, true);
    try_start_cancelled_sleep_action(tray, tray->timer_thread_id);
    if (defer_stop_condition_wake) {
        defer_stop_condition_wake = false;
        WakeAllConditionVariable(&tray->stop_condition);
    }
    return NULL;
}

static void *manual_stop(void *argument) {
    current_thread_id = 2;
    NoSleepTray *tray = (NoSleepTray *)argument;
    (void)tray_stop_nosleep_for_session(tray, 0, false, false);
    return NULL;
}

static void *worker_failure_cleanup(void *argument) {
    current_thread_id = 3;
    NoSleepTray *tray = (NoSleepTray *)argument;
    bool session_stopped = tray_stop_nosleep_for_session(tray, 3, false, true);
    if (session_stopped) {
        tray_show_notification(tray, NOTIFY_EVENT_ERROR,
            "Sleep prevention failed",
            "Sleep prevention stopped after repeated refresh failures.", true);
    }
    return NULL;
}

static bool wait_for_flag(bool *flag) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;

    pthread_mutex_lock(&harness_lock);
    int result = 0;
    while (!*flag && result == 0) {
        result = pthread_cond_timedwait(&harness_changed, &harness_lock,
                                        &deadline);
    }
    pthread_mutex_unlock(&harness_lock);
    return result == 0;
}

int main(void) {
    NoSleepTray tray = {0};
    pthread_t expiry_thread;
    pthread_t manual_thread;
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    pthread_cond_init(&tray.stop_condition, NULL);
    tray.is_running = true;
    tray.duration_expired = true;
    tray.session_action_pending = true;
    tray.timer_thread = (HANDLE)(uintptr_t)1;
    tray.timer_thread_id = 1;
    tray.nosleep_thread = (HANDLE)(uintptr_t)2;
    tray.nosleep_thread_id = 3;
    tray.stop_event = (HANDLE)(uintptr_t)4;
    blocked_wait_thread_id = 1;
    blocked_wait_handle = tray.nosleep_thread;
    defer_stop_condition_wake = true;
    active_tray = &tray;

    if (pthread_create(&expiry_thread, NULL, expiry_cleanup, &tray) != 0) {
        fputs("FAIL: could not start expiry cleanup thread\n", stderr);
        return 1;
    }
    if (!wait_for_flag(&expiry_waiting_for_worker)) {
        fputs("FAIL: expiry cleanup did not reach its controlled wait\n", stderr);
        pthread_mutex_lock(&harness_lock);
        release_expiry_worker_wait = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        pthread_join(expiry_thread, NULL);
        return 1;
    }

    if (pthread_create(&manual_thread, NULL, manual_stop, &tray) != 0) {
        fputs("FAIL: could not start concurrent manual Stop\n", stderr);
        pthread_mutex_lock(&harness_lock);
        release_expiry_worker_wait = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        pthread_join(expiry_thread, NULL);
        return 1;
    }
    if (!wait_for_flag(&manual_stop_waiting_for_cleanup)) {
        fputs("FAIL: manual Stop did not wait for expiry cleanup\n", stderr);
        pthread_mutex_lock(&harness_lock);
        release_expiry_worker_wait = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        pthread_join(expiry_thread, NULL);
        pthread_join(manual_thread, NULL);
        return 1;
    }

    pthread_mutex_lock(&harness_lock);
    release_expiry_worker_wait = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);
    pthread_join(expiry_thread, NULL);
    pthread_join(manual_thread, NULL);

    if (!tray.session_action_cancelled ||
        tray.stopping_expiry_notification_suppressed) {
        fputs("FAIL: manual Stop did not cancel the expiry action\n", stderr);
        return 1;
    }
    if (notification_count != 1 ||
        notification_event != NOTIFY_EVENT_SESSION_STOP ||
        strcmp(notification_title, "Stopped") != 0 ||
        strstr(notification_message, "manually stopped") == NULL) {
        fprintf(stderr,
                "FAIL: manual Stop during suppressed expiry cleanup should show the Stopped notice; got %d notifications, title '%s'\n",
                notification_count, notification_title);
        return 1;
    }

    NoSleepTray gap_tray = {0};
    pthread_mutex_init(&gap_tray.delayed_action_lock, NULL);
    pthread_cond_init(&gap_tray.stop_condition, NULL);
    gap_tray.is_running = true;
    gap_tray.duration_expired = true;
    gap_tray.session_action_pending = true;
    gap_tray.timer_thread = (HANDLE)(uintptr_t)5;
    gap_tray.timer_thread_id = 1;
    gap_tray.nosleep_thread = (HANDLE)(uintptr_t)6;
    gap_tray.nosleep_thread_id = 3;
    gap_tray.stop_event = (HANDLE)(uintptr_t)7;
    active_tray = &gap_tray;
    notification_count = 0;
    notification_title[0] = '\0';
    notification_message[0] = '\0';
    current_thread_id = 1;
    (void)tray_stop_nosleep_for_session(&gap_tray, 1, true, true);
    if (notification_count != 0) {
        fputs("FAIL: expiry cleanup should suppress its configured action notice\n", stderr);
        return 1;
    }
    int menu_updates_after_expiry_cleanup = stop_menu_update_count;

    current_thread_id = 4;
    (void)tray_stop_nosleep_for_session(&gap_tray, 0, false, false);
    if (!gap_tray.session_action_cancelled ||
        gap_tray.stopping_expiry_notification_suppressed ||
        notification_count != 1 ||
        notification_event != NOTIFY_EVENT_SESSION_STOP ||
        strcmp(notification_title, "Stopped") != 0 ||
        strstr(notification_message, "manually stopped") == NULL ||
        stop_menu_update_count != menu_updates_after_expiry_cleanup + 1) {
        fprintf(stderr,
                "FAIL: manual Stop between expiry cleanup and action startup should refresh the menu and show Stopped; got %d notifications, title '%s'\n",
                notification_count, notification_title);
        return 1;
    }

    NoSleepTray failed_start_tray = {0};
    pthread_mutex_init(&failed_start_tray.delayed_action_lock, NULL);
    pthread_cond_init(&failed_start_tray.stop_condition, NULL);
    failed_start_tray.duration_expired = true;
    failed_start_tray.stopping_expiry_notification_suppressed = true;
    active_tray = &failed_start_tray;
    notification_count = 0;
    notification_title[0] = '\0';
    notification_message[0] = '\0';
    current_thread_id = 5;
    (void)tray_stop_nosleep_for_session(&failed_start_tray, 0, false, false);
    if (notification_count != 0 ||
        failed_start_tray.stopping_expiry_notification_suppressed) {
        fprintf(stderr,
                "FAIL: stale Stop after failed action startup showed a false notice or retained suppression state; got %d notifications, title '%s'\n",
                notification_count, notification_title);
        return 1;
    }

    NoSleepTray completed_tray = {0};
    pthread_mutex_init(&completed_tray.delayed_action_lock, NULL);
    pthread_cond_init(&completed_tray.stop_condition, NULL);
    completed_tray.duration_expired = true;
    completed_tray.session_action_pending = true;
    completed_tray.stopping_expiry_notification_suppressed = true;
    completed_tray.sleep_timer = (HANDLE)(uintptr_t)12;
    tray_reap_delayed_action_handle(&completed_tray,
                                   &completed_tray.sleep_timer);
    if (completed_tray.session_action_pending ||
        completed_tray.stopping_expiry_notification_suppressed ||
        completed_tray.sleep_timer != NULL) {
        fputs("FAIL: reaping a completed delayed action retained stale expiry suppression\n",
              stderr);
        return 1;
    }
    active_tray = &completed_tray;
    notification_count = 0;
    notification_title[0] = '\0';
    notification_message[0] = '\0';
    current_thread_id = 6;
    (void)tray_stop_nosleep_for_session(&completed_tray, 0, false, false);
    if (notification_count != 0) {
        fprintf(stderr,
                "FAIL: queued Stop after delayed action completion showed a false notice; got %d notifications, title '%s'\n",
                notification_count, notification_title);
        return 1;
    }

    NoSleepTray failure_tray = {0};
    pthread_mutex_init(&failure_tray.delayed_action_lock, NULL);
    pthread_cond_init(&failure_tray.stop_condition, NULL);
    failure_tray.is_running = true;
    failure_tray.nosleep_run_failed = true;
    failure_tray.timer_thread = (HANDLE)(uintptr_t)8;
    failure_tray.timer_thread_id = 9;
    failure_tray.nosleep_thread = (HANDLE)(uintptr_t)10;
    failure_tray.nosleep_thread_id = 3;
    failure_tray.stop_event = (HANDLE)(uintptr_t)11;
    active_tray = &failure_tray;
    notification_count = 0;
    notification_title[0] = '\0';
    notification_message[0] = '\0';
    expiry_waiting_for_worker = false;
    manual_stop_waiting_for_cleanup = false;
    release_expiry_worker_wait = false;
    blocked_wait_thread_id = 3;
    blocked_wait_handle = failure_tray.timer_thread;

    pthread_t failure_thread;
    pthread_t failure_manual_thread;
    if (pthread_create(&failure_thread, NULL, worker_failure_cleanup,
                       &failure_tray) != 0 ||
        !wait_for_flag(&expiry_waiting_for_worker)) {
        fputs("FAIL: worker-failure cleanup did not reach its controlled wait\n", stderr);
        return 1;
    }
    if (pthread_create(&failure_manual_thread, NULL, manual_stop,
                       &failure_tray) != 0 ||
        !wait_for_flag(&manual_stop_waiting_for_cleanup)) {
        fputs("FAIL: manual Stop did not wait for worker-failure cleanup\n", stderr);
        pthread_mutex_lock(&harness_lock);
        release_expiry_worker_wait = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        pthread_join(failure_thread, NULL);
        return 1;
    }
    pthread_mutex_lock(&harness_lock);
    release_expiry_worker_wait = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);
    pthread_join(failure_thread, NULL);
    pthread_join(failure_manual_thread, NULL);

    if (notification_count != 1 ||
        notification_event != NOTIFY_EVENT_ERROR ||
        strcmp(notification_title, "Sleep prevention failed") != 0) {
        fprintf(stderr,
                "FAIL: manual Stop during worker-failure cleanup must not add a Stopped notice; got %d notifications, title '%s'\n",
                notification_count, notification_title);
        return 1;
    }

    puts("PASS: manual Stop shows Stopped for expiry cleanup without masking worker failure");
    return 0;
}
'''

output.write_text(prelude + "\n" + manual_notice_function + "\n" +
                  stop_function + "\n" + reap_function + "\n" +
                  "static void try_start_cancelled_sleep_action(NoSleepTray *tray, DWORD timer_thread_id) {\n"
                  "    HANDLE sleep_timer = NULL;\n"
                  "    bool core_init_failed;\n"
                  "    bool core_init_succeeded;\n"
                  "    bool nosleep_run_failed;\n"
                  "    bool stale_session;\n"
                  "    bool session_starting;\n" + sleep_startup + "\n}\n" + main)
PY

"${CC:-cc}" -std=c99 -Wall -Wextra -pthread -I"$SCRIPT_DIR/src" \
    "$TMP_DIR/test_tray_manual_stop_expiry_race.c" \
    -o "$TMP_DIR/test_tray_manual_stop_expiry_race"
"$TMP_DIR/test_tray_manual_stop_expiry_race"
