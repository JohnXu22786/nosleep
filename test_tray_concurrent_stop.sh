#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

python3 - "$SCRIPT_DIR/src/tray.c" "$TMP_DIR/test_tray_concurrent_stop.c" <<'PY'
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", source)
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


prelude = r"""
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef unsigned int DWORD;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef pthread_mutex_t SRWLOCK;
typedef pthread_cond_t CONDITION_VARIABLE;
typedef void *HMENU;
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

typedef struct TrayUpdateCheckTask {
    bool shutdown_requested;
    HANDLE thread;
    DWORD thread_id;
} TrayUpdateCheckTask;

#define TRUE 1
#define FALSE 0
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define ES_CONTINUOUS 0x80000000u
#define DEBUG_LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ATOMIC_STORE_BOOL(destination, value) \
    __atomic_store_n((destination), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD_BOOL(source) \
    __atomic_load_n((source), __ATOMIC_SEQ_CST)
#define ATOMIC_EXCHANGE_BOOL(destination, value) \
    __atomic_exchange_n((destination), (value), __ATOMIC_SEQ_CST)

typedef struct NoSleepTray {
    HWND hwnd;
    SRWLOCK delayed_action_lock;
    CONDITION_VARIABLE stop_condition;
    bool stopping;
    DWORD stopping_thread_id;
    bool starting_nosleep;
    bool delayed_countdown_starting;
    DWORD nosleep_thread_id;
    bool session_action_cancelled;
    bool core_init_failed;
    bool nosleep_run_failed;
    bool is_running;
    bool delayed_sleep_countdown_active;
    bool sleep_action_claimed;
    SessionFinishedAction shutdown_action;
    bool shutdown_action_claimed;
    HANDLE sleep_timer;
    HANDLE shutdown_timer;
    SessionFinishedAction countdown_action;
    HANDLE stop_event;
    HANDLE timer_thread;
    HANDLE nosleep_thread;
    DWORD timer_thread_id;
    TrayUpdateCheckTask *update_check_task;
    ULONGLONG start_tick64;
    bool duration_expired;
    HANDLE sleep_stop_event;
    HANDLE shutdown_stop_event;
    HANDLE countdown_timer_thread;
    HANDLE countdown_stop_event;
    HMENU hmenu;
} NoSleepTray;

static bool tray_stop_nosleep_for_session(NoSleepTray *tray,
                                          DWORD expected_thread_id,
                                          bool timer_expired,
                                          bool suppress_notification);
void tray_stop_nosleep(NoSleepTray *tray, bool timer_expired,
                       bool suppress_notification);

static NoSleepTray *active_tray;
static pthread_mutex_t harness_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t harness_changed = PTHREAD_COND_INITIALIZER;
static __thread DWORD current_thread_id;
static bool owner_in_icon_update;
static bool release_owner;
static bool reentrant_call_returned;
static bool owner_icon_update_finished;
static bool owner_completion_released;
static bool second_stop_waiting;
static bool owner_in_timer_tail;
static bool release_owner_tail;
static bool owner_timer_tail_finished;
static bool destroy_waiting_for_timer;
static bool destroy_freed_tray;
static bool freed_after_owner_completion;
static bool worker_wait_started;
static bool worker_wait_indefinitely;
static bool release_worker_wait;
static bool worker_joined;
static bool worker_handle_cleared_after_join;
static bool worker_handle_closed;
static bool worker_handle_closed_too_early;
static bool freed_after_worker_join;
static volatile ULONGLONG post_stop_tick;

static void test_free(void *pointer) {
    pthread_mutex_lock(&harness_lock);
    destroy_freed_tray = true;
    freed_after_owner_completion = owner_completion_released &&
                                   owner_timer_tail_finished;
    freed_after_worker_join = worker_wait_started &&
                              worker_wait_indefinitely && worker_joined &&
                              worker_handle_closed;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);
    (void)pointer;
}

#define free test_free

static void AcquireSRWLockExclusive(SRWLOCK *lock) {
    pthread_mutex_lock(lock);
}

static void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    bool owner_finishing = active_tray &&
        lock == &active_tray->delayed_action_lock && current_thread_id == 1 &&
        !ATOMIC_LOAD_BOOL(&active_tray->stopping);
    if (owner_finishing) {
        pthread_mutex_lock(&harness_lock);
        owner_completion_released = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
    }
    pthread_mutex_unlock(lock);
}

static BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *condition,
                                      SRWLOCK *lock,
                                      DWORD milliseconds,
                                      ULONG flags) {
    (void)milliseconds;
    (void)flags;
    if (current_thread_id == 2) {
        pthread_mutex_lock(&harness_lock);
        second_stop_waiting = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
    }
    return pthread_cond_wait(condition, lock) == 0 ? TRUE : FALSE;
}

static void WakeAllConditionVariable(CONDITION_VARIABLE *condition) {
    pthread_cond_broadcast(condition);
}

static DWORD GetCurrentThreadId(void) {
    return current_thread_id;
}

static BOOL CancelSynchronousIo(HANDLE thread) {
    (void)thread;
    return TRUE;
}

static DWORD GetThreadId(HANDLE thread) {
    (void)thread;
    return 0;
}

static BOOL SetEvent(HANDLE event) {
    (void)event;
    return TRUE;
}

static BOOL ResetEvent(HANDLE event) {
    (void)event;
    return TRUE;
}

static DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    if (active_tray && handle == active_tray->nosleep_thread) {
        pthread_mutex_lock(&harness_lock);
        worker_wait_started = true;
        worker_wait_indefinitely = milliseconds == INFINITE;
        pthread_cond_broadcast(&harness_changed);
        while (!release_worker_wait) {
            pthread_cond_wait(&harness_changed, &harness_lock);
        }
        worker_joined = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        return WAIT_OBJECT_0;
    }
    if (active_tray && handle == active_tray->timer_thread &&
        current_thread_id == 2) {
        pthread_mutex_lock(&harness_lock);
        destroy_waiting_for_timer = true;
        pthread_cond_broadcast(&harness_changed);
        while (!owner_timer_tail_finished) {
            pthread_cond_wait(&harness_changed, &harness_lock);
        }
        pthread_mutex_unlock(&harness_lock);
    }
    return WAIT_OBJECT_0;
}

static BOOL CloseHandle(HANDLE handle) {
    if (active_tray && handle == active_tray->nosleep_thread) {
        worker_handle_closed = true;
        if (!worker_joined) {
            worker_handle_closed_too_early = true;
        }
    }
    return TRUE;
}

static DWORD SetThreadExecutionState(DWORD state) {
    return state;
}

static ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    (void)start_tick64;
    return 0;
}

static void tray_stop_countdown(NoSleepTray *tray) {
    (void)tray;
}

static void tray_show_notification(NoSleepTray *tray, NotifyEventId event,
                                   const char *title, const char *message,
                                   bool critical) {
    (void)tray;
    (void)event;
    (void)title;
    (void)message;
    (void)critical;
}

static void tray_update_stop_menu_item(NoSleepTray *tray) {
    (void)tray;
}

static void tray_update_icon(NoSleepTray *tray) {
    (void)tray;
    if (current_thread_id != 1) return;

    tray_stop_nosleep(tray, false, true);
    pthread_mutex_lock(&harness_lock);
    reentrant_call_returned = true;
    owner_in_icon_update = true;
    pthread_cond_broadcast(&harness_changed);
    while (!release_owner) {
        pthread_cond_wait(&harness_changed, &harness_lock);
    }
    owner_icon_update_finished = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);
}

// Update-check UI state is outside this worker-retention fixture's scope.
static void tray_update_check_end(void) {}
static void tray_set_update_check_visible(NoSleepTray *tray, bool checking) {
    (void)tray;
    (void)checking;
}

static void tray_destroy_icons(NoSleepTray *tray) {
    (void)tray;
}

static BOOL DestroyMenu(HMENU menu) {
    (void)menu;
    return TRUE;
}

static BOOL DestroyWindow(HWND hwnd) {
    (void)hwnd;
    return TRUE;
}

static bool tray_stop_has_work(bool is_running,
                               bool countdown_active,
                               bool sleep_timer_present,
                               bool shutdown_timer_present) {
    return is_running || countdown_active || sleep_timer_present ||
        shutdown_timer_present;
}

"""

wait_function = extract_function(
    "static bool tray_wait_for_worker_threads(NoSleepTray* tray)"
)
update_check_wait_function = extract_function(
    "static bool tray_wait_for_update_check(NoSleepTray* tray)"
)
countdown_start_wait_function = extract_function(
    "static void tray_wait_for_delayed_countdown_start(NoSleepTray* tray)"
)
destroy_function = extract_function("void tray_destroy(NoSleepTray* tray)")
stop_function = extract_function(
    "static bool tray_stop_nosleep_for_session(NoSleepTray* tray,\n"
    "                                          DWORD expected_thread_id,\n"
    "                                          bool timer_expired,\n"
    "                                          bool suppress_notification)"
)
wrapper = r"""
void tray_stop_nosleep(NoSleepTray *tray, bool timer_expired,
                       bool suppress_notification) {
    (void)tray_stop_nosleep_for_session(tray, 0, timer_expired,
                                       suppress_notification);
}

static void *owner_stop(void *argument) {
    current_thread_id = 1;
    NoSleepTray *tray = (NoSleepTray *)argument;
    tray_stop_nosleep(tray, false, true);
    worker_handle_cleared_after_join = tray->nosleep_thread == NULL;
    pthread_mutex_lock(&harness_lock);
    owner_in_timer_tail = true;
    pthread_cond_broadcast(&harness_changed);
    while (!release_owner_tail) {
        pthread_cond_wait(&harness_changed, &harness_lock);
    }
    pthread_mutex_unlock(&harness_lock);
    post_stop_tick = tray->start_tick64;
    pthread_mutex_lock(&harness_lock);
    owner_timer_tail_finished = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);
    return NULL;
}

static void *destroy_tray(void *argument) {
    current_thread_id = 2;
    tray_destroy((NoSleepTray *)argument);
    return NULL;
}

static int wait_for_flag_or_free(bool *flag) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;

    pthread_mutex_lock(&harness_lock);
    int result = 0;
    while (!*flag && !destroy_freed_tray && result == 0) {
        result = pthread_cond_timedwait(&harness_changed, &harness_lock,
                                       &deadline);
    }
    pthread_mutex_unlock(&harness_lock);
    return result;
}

int main(void) {
    NoSleepTray tray = {0};
    pthread_t owner_thread;
    pthread_t destroy_thread;
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    pthread_cond_init(&tray.stop_condition, NULL);
    tray.is_running = true;
    tray.stop_event = (HANDLE)(uintptr_t)1;
    tray.timer_thread = (HANDLE)(uintptr_t)2;
    tray.timer_thread_id = 1;
    tray.nosleep_thread = (HANDLE)(uintptr_t)3;
    tray.nosleep_thread_id = 3;
    tray.start_tick64 = 12345;
    active_tray = &tray;

    if (pthread_create(&owner_thread, NULL, owner_stop, &tray) != 0) {
        fprintf(stderr, "FAIL: could not start first stop\n");
        return 1;
    }

    int worker_wait_result = wait_for_flag_or_free(&worker_wait_started);
    pthread_mutex_lock(&harness_lock);
    release_worker_wait = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);
    if (worker_wait_result != 0) {
        fprintf(stderr, "FAIL: stop did not wait for the core worker\n");
        pthread_join(owner_thread, NULL);
        return 1;
    }

    pthread_mutex_lock(&harness_lock);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    int wait_result = 0;
    while (!owner_in_icon_update && wait_result == 0) {
        wait_result = pthread_cond_timedwait(&harness_changed, &harness_lock,
                                            &deadline);
    }
    pthread_mutex_unlock(&harness_lock);
    if (wait_result != 0) {
        fprintf(stderr, "FAIL: first stop did not reach controlled cleanup\n");
        pthread_mutex_lock(&harness_lock);
        release_owner = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        pthread_join(owner_thread, NULL);
        return 1;
    }

    if (pthread_create(&destroy_thread, NULL, destroy_tray, &tray) != 0) {
        fprintf(stderr, "FAIL: could not start concurrent teardown\n");
        pthread_mutex_lock(&harness_lock);
        release_owner = true;
        pthread_cond_broadcast(&harness_changed);
        pthread_mutex_unlock(&harness_lock);
        pthread_join(owner_thread, NULL);
        return 1;
    }

    int stop_wait_result = wait_for_flag_or_free(&second_stop_waiting);
    pthread_mutex_lock(&harness_lock);
    bool freed_early = destroy_freed_tray;
    release_owner = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);

    int timer_wait_result = wait_for_flag_or_free(&destroy_waiting_for_timer);
    pthread_mutex_lock(&harness_lock);
    bool freed_during_timer_tail = destroy_freed_tray;
    release_owner_tail = true;
    pthread_cond_broadcast(&harness_changed);
    pthread_mutex_unlock(&harness_lock);

    pthread_join(owner_thread, NULL);
    pthread_join(destroy_thread, NULL);

    if (stop_wait_result != 0 || timer_wait_result != 0) {
        fprintf(stderr, "FAIL: concurrent teardown did not reach both waits\n");
        return 1;
    }
    if (freed_early || freed_during_timer_tail ||
        !freed_after_owner_completion || !owner_in_timer_tail ||
        !owner_icon_update_finished || !reentrant_call_returned ||
        !destroy_freed_tray || post_stop_tick != 12345) {
        fprintf(stderr,
                "FAIL: teardown freed tray before the stop owner finished using it\n");
        return 1;
    }
    if (!worker_wait_indefinitely || !worker_joined ||
        !worker_handle_cleared_after_join || worker_handle_closed_too_early ||
        !worker_handle_closed || !freed_after_worker_join) {
        fprintf(stderr,
                "FAIL: stop did not join the core worker before tray teardown\n");
        return 1;
    }

    puts("PASS: tray stop joins the core worker before concurrent teardown");
    return 0;
}
"""

output.write_text(prelude + update_check_wait_function + "\n" + wait_function + "\n" +
                  countdown_start_wait_function + "\n" + destroy_function + "\n" +
                  stop_function + "\n" + wrapper)
PY

"${CC:-cc}" -std=c99 -Wall -Wextra -pthread "$TMP_DIR/test_tray_concurrent_stop.c" \
    -o "$TMP_DIR/test_tray_concurrent_stop"
"$TMP_DIR/test_tray_concurrent_stop"
