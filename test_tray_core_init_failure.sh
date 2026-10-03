#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray_source = Path(os.environ.get("TRAY_SOURCE", root / "src/tray.c"))
tray = tray_source.read_text()


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", tray)
    assert definition, f"could not find definition for {signature}"
    start = definition.start()
    opening = definition.end() - 1

    depth = 0
    state = "code"
    i = opening
    while i < len(tray):
        char = tray[i]
        next_two = tray[i : i + 2]
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
                    return tray[start : i + 1]
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "tray_stop_guard.h"

#define WINAPI
typedef unsigned int DWORD;
typedef unsigned long long ULONGLONG;
typedef void* HANDLE;
typedef void* LPVOID;
typedef pthread_mutex_t SRWLOCK;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
typedef struct { unsigned short wHour, wMinute, wSecond; } SYSTEMTIME;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef union {
    struct { DWORD LowPart, HighPart; };
    uint64_t QuadPart;
} ULARGE_INTEGER;
typedef struct { DWORD thread_id; bool signaled, closed; pthread_t native_thread; } MockHandle;
typedef struct NoSleep { int unused; } NoSleep;
static NoSleep mock_nosleep;
typedef enum {
    SESSION_FINISHED_NONE,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN
} SessionFinishedAction;
typedef enum {
    NOTIFY_EVENT_SESSION_START = 1,
    NOTIFY_EVENT_SESSION_STOP = 2,
    NOTIFY_EVENT_TIMER_EXPIRED = 3,
    NOTIFY_EVENT_ERROR = 4,
    NOTIFY_EVENT_COUNTDOWN_CANCEL = 5
} NotifyEventId;

#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define INFINITE 0xffffffffu
#define ES_CONTINUOUS 0x80000000u
#define ATOMIC_STORE_BOOL(dest, value) __atomic_store_n((dest), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD_BOOL(src) __atomic_load_n((src), __ATOMIC_SEQ_CST)
#define ATOMIC_EXCHANGE_BOOL(dest, value) __atomic_exchange_n((dest), (value), __ATOMIC_SEQ_CST)
#define DEBUG_LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)

typedef struct NoSleepTray {
    bool is_running, duration_expired, stopping, core_init_failed;
    bool core_init_succeeded, starting_nosleep;
    SRWLOCK delayed_action_lock;
    int duration_minutes;
    ULONGLONG start_tick64;
    HANDLE stop_event, timer_thread, nosleep_thread;
    DWORD timer_thread_id, nosleep_thread_id;
    bool prevent_display, away_mode, verbose;
    int refresh_interval_seconds;
    SessionFinishedAction session_finished_action;
    HANDLE sleep_timer, sleep_stop_event;
    HANDLE shutdown_timer, shutdown_stop_event;
    bool delayed_sleep_countdown_active;
} NoSleepTray;

static DWORD WINAPI tray_duration_timer(LPVOID lpParam);
static DWORD WINAPI tray_nosleep_thread(LPVOID lpParam);

static DWORD WINAPI delayed_sleep_thread(LPVOID context) {
    (void)context;
    return 0;
}
static DWORD WINAPI delayed_shutdown_thread(LPVOID context) {
    (void)context;
    return 0;
}

static __thread DWORD mock_thread_id;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static bool start_test_active, release_create_failure;
static bool create_nosleep_succeeds;
static bool worker_lock_observed, worker_lock_blocked, start_worker_completed;
static bool pause_core_resolution, timer_waiting_for_init;
static bool action_attempted_before_init, release_core_resolution;
static unsigned int action_threads_started, error_notifications;
static unsigned int action_stop_signals;
static ULONGLONG mock_tick64;
static MockHandle action_handle, replacement_worker_handle;
static MockHandle* action_stop_event_to_watch;
static MockHandle start_worker_handle, start_timer_handle;
static pthread_t start_worker_thread;

static void* run_start_failure_worker(void* context) {
    mock_thread_id = 42;
    tray_nosleep_thread(context);
    pthread_mutex_lock(&gate_lock);
    start_worker_completed = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_lock);
    return NULL;
}

static void AcquireSRWLockExclusive(SRWLOCK* lock) {
    if (start_test_active && mock_thread_id == 42 && !worker_lock_observed) {
        int result = pthread_mutex_trylock(lock);
        pthread_mutex_lock(&gate_lock);
        worker_lock_observed = true;
        worker_lock_blocked = result != 0;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_lock);
        if (result == 0) return;
    }
    pthread_mutex_lock(lock);
}
static void ReleaseSRWLockExclusive(SRWLOCK* lock) { pthread_mutex_unlock(lock); }
static DWORD GetCurrentThreadId(void) { return mock_thread_id; }
static ULONGLONG GetTickCount64(void) { return mock_tick64; }
static DWORD GetThreadId(HANDLE handle) { return ((MockHandle*)handle)->thread_id; }
static DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    (void)handle;
    (void)milliseconds;
    return WAIT_OBJECT_0;
}
static bool SetEvent(HANDLE handle) {
    ((MockHandle*)handle)->signaled = true;
    if (handle == action_stop_event_to_watch) ++action_stop_signals;
    return true;
}
static bool ResetEvent(HANDLE handle) {
    ((MockHandle*)handle)->signaled = false;
    return true;
}
static bool CloseHandle(HANDLE handle) {
    ((MockHandle*)handle)->closed = true;
    return true;
}
static HANDLE CreateThread(void* attributes, size_t stack_size,
                           LPTHREAD_START_ROUTINE start, LPVOID parameter,
                           DWORD flags, DWORD* thread_id) {
    (void)attributes;
    (void)stack_size;
    (void)flags;
    if (start_test_active && start == tray_nosleep_thread) {
        start_worker_handle.thread_id = 42;
        start_worker_handle.closed = false;
        if (pthread_create(&start_worker_thread, NULL,
                           run_start_failure_worker, parameter) != 0) {
            return NULL;
        }
        if (thread_id) *thread_id = start_worker_handle.thread_id;
        return &start_worker_handle;
    }
    if (start_test_active && start == tray_duration_timer) {
        pthread_mutex_lock(&gate_lock);
        release_create_failure = true;
        pthread_cond_broadcast(&gate_changed);
        while (!worker_lock_observed) pthread_cond_wait(&gate_changed, &gate_lock);
        if (!worker_lock_blocked) {
            while (!start_worker_completed) pthread_cond_wait(&gate_changed, &gate_lock);
        }
        pthread_mutex_unlock(&gate_lock);
        start_timer_handle.thread_id = 77;
        start_timer_handle.closed = false;
        if (thread_id) *thread_id = start_timer_handle.thread_id;
        return &start_timer_handle;
    }

    if (pause_core_resolution && mock_thread_id == 42 &&
        (start == delayed_sleep_thread || start == delayed_shutdown_thread)) {
        pthread_mutex_lock(&gate_lock);
        action_attempted_before_init = true;
        pthread_cond_broadcast(&gate_changed);
        while (!release_core_resolution) pthread_cond_wait(&gate_changed, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
    }

    if (start == tray_nosleep_thread) {
        replacement_worker_handle.thread_id = 77;
        replacement_worker_handle.closed = false;
        if (thread_id) *thread_id = replacement_worker_handle.thread_id;
        return &replacement_worker_handle;
    }

    ++action_threads_started;
    action_handle.thread_id = 77;
    if (thread_id) *thread_id = action_handle.thread_id;
    return &action_handle;
}
static DWORD SetThreadExecutionState(DWORD state) { return state; }
static void Sleep(DWORD milliseconds) {
    (void)milliseconds;
    if (!pause_core_resolution || mock_thread_id != 42) return;
    pthread_mutex_lock(&gate_lock);
    timer_waiting_for_init = true;
    pthread_cond_broadcast(&gate_changed);
    while (!release_core_resolution) pthread_cond_wait(&gate_changed, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
}
static unsigned long GetLastError(void) { return 123; }
static void tray_stop_countdown(NoSleepTray* tray) {
    tray->delayed_sleep_countdown_active = false;
}
static void tray_update_stop_menu_item(NoSleepTray* tray) { (void)tray; }
static void tray_update_icon(NoSleepTray* tray) {
    (void)tray;
}
static void tray_show_notification(NoSleepTray* tray, NotifyEventId event_type,
                                   const char* title, const char* message,
                                   bool critical) {
    (void)tray;
    (void)event_type;
    (void)message;
    (void)critical;
    if (strcmp(title, "Error") == 0) ++error_notifications;
}
static NoSleep* nosleep_create(void) {
    if (start_test_active && mock_thread_id == 42) {
        pthread_mutex_lock(&gate_lock);
        while (!release_create_failure) pthread_cond_wait(&gate_changed, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
    }
    return create_nosleep_succeeds ? &mock_nosleep : NULL;
}
static int nosleep_run(NoSleep* ns, int duration, int interval, bool display,
                       bool away_mode, bool verbose, HANDLE stop_event) {
    (void)ns; (void)duration; (void)interval; (void)display;
    (void)away_mode; (void)verbose; (void)stop_event;
    return 0;
}
static void nosleep_destroy(NoSleep* ns) { (void)ns; }

void tray_stop_nosleep(NoSleepTray*, bool, bool);
void tray_start_nosleep(NoSleepTray*, int);
"""

main = r"""
static void* run_duration_timer(void* context) {
    mock_thread_id = 42;
    tray_duration_timer(context);
    return NULL;
}

static int check_direct_failure(SessionFinishedAction action, const char* label) {
    NoSleepTray tray;
    MockHandle events[5] = {{0}};
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.duration_minutes = 30;
    tray.start_tick64 = 0;
    tray.session_finished_action = action;
    tray.stop_event = &events[0];
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 42;
    tray.nosleep_thread = &events[2];
    tray.nosleep_thread_id = 43;
    tray.sleep_stop_event = &events[3];
    tray.shutdown_stop_event = &events[4];
    mock_tick64 = 60000;
    mock_thread_id = 43;
    action_threads_started = 0;
    error_notifications = 0;

    int failures = 0;
    if (tray_nosleep_thread(&tray) != 1) {
        fprintf(stderr, "FAIL: %s did not report core initialization failure\n", label);
        ++failures;
    }
    if (tray.is_running || tray.timer_thread != NULL || !events[1].closed ||
        !events[0].signaled) {
        fprintf(stderr, "FAIL: %s did not reset the session and stop its duration timer\n", label);
        ++failures;
    }
    if (tray.nosleep_thread != NULL || !events[2].closed ||
        !tray.core_init_failed || error_notifications != 1) {
        fprintf(stderr, "FAIL: %s did not clean up and report the failed session\n", label);
        ++failures;
    }
    if (action_threads_started != 0) {
        fprintf(stderr, "FAIL: %s started a delayed action\n", label);
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

static int check_successful_initialization_marks_ready(void) {
    NoSleepTray tray;
    MockHandle events[1] = {{0}};
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.nosleep_thread_id = 43;
    tray.stop_event = &events[0];
    mock_thread_id = 43;
    create_nosleep_succeeds = true;

    int failures = 0;
    if (tray_nosleep_thread(&tray) != 0 || !tray.core_init_succeeded || tray.is_running) {
        fprintf(stderr, "FAIL: successful NoSleep initialization did not release the session action gate\n");
        ++failures;
    }
    create_nosleep_succeeds = false;
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

static int check_stale_worker_failure(void) {
    NoSleepTray tray;
    MockHandle events[3] = {{0}};
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.nosleep_thread = &events[0];
    tray.nosleep_thread_id = 43;
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 44;
    tray.stop_event = &events[2];
    mock_thread_id = 42; // This worker belongs to the session that already ended.
    action_threads_started = 0;
    error_notifications = 0;

    int failures = 0;
    if (tray_nosleep_thread(&tray) != 1) {
        fprintf(stderr, "FAIL: stale worker did not return its initialization error\n");
        ++failures;
    }
    if (!tray.is_running || tray.timer_thread != &events[1] || events[0].closed ||
        events[1].closed || tray.core_init_failed || error_notifications != 0) {
        fprintf(stderr, "FAIL: stale worker failure changed the current session\n");
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

static int check_failure_race(SessionFinishedAction action, const char* label) {
    NoSleepTray tray;
    MockHandle events[3] = {{0}};
    pthread_t timer_thread;
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.core_init_succeeded = false;
    tray.start_tick64 = 0;
    tray.session_finished_action = action;
    tray.stop_event = &events[0];
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 42;
    tray.nosleep_thread_id = 43;
    tray.sleep_stop_event = &events[2];
    tray.shutdown_stop_event = &events[2];
    mock_tick64 = 120000;
    mock_thread_id = 43;
    action_threads_started = 0;
    error_notifications = 0;
    pause_core_resolution = true;
    timer_waiting_for_init = false;
    release_core_resolution = false;

    if (pthread_create(&timer_thread, NULL, run_duration_timer, &tray) != 0) {
        fprintf(stderr, "FAIL: %s could not start duration worker\n", label);
        return 1;
    }
    pthread_mutex_lock(&gate_lock);
    while (!timer_waiting_for_init) pthread_cond_wait(&gate_changed, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    // Fail core initialization while the expired duration waits for it to resolve.
    if (tray_nosleep_thread(&tray) != 1) {
        fprintf(stderr, "FAIL: %s did not report core initialization failure\n", label);
        return 1;
    }
    pthread_mutex_lock(&gate_lock);
    pause_core_resolution = false;
    release_core_resolution = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_lock);
    pthread_join(timer_thread, NULL);

    int failures = 0;
    if (tray.is_running || tray.timer_thread != NULL || !events[1].closed) {
        fprintf(stderr, "FAIL: %s did not reset session and duration timer state\n", label);
        ++failures;
    }
    if (!tray.core_init_failed || error_notifications != 1) {
        fprintf(stderr, "FAIL: %s did not retain/report the initialization failure\n", label);
        ++failures;
    }
    if (action_threads_started != 0) {
        fprintf(stderr, "FAIL: %s started a delayed action after initialization failed\n", label);
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

static int check_restart_during_initialization_wait(SessionFinishedAction action,
                                                     const char* label) {
    NoSleepTray tray;
    MockHandle events[2] = {{0}};
    pthread_t timer_thread;
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.core_init_succeeded = false;
    tray.start_tick64 = 0;
    tray.session_finished_action = action;
    tray.stop_event = &events[0];
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 42;
    tray.nosleep_thread_id = 43;
    tray.sleep_stop_event = &events[0];
    tray.shutdown_stop_event = &events[0];
    mock_tick64 = 120000;
    mock_thread_id = 43;
    action_threads_started = 0;
    error_notifications = 0;
    pause_core_resolution = true;
    timer_waiting_for_init = false;
    release_core_resolution = false;

    if (pthread_create(&timer_thread, NULL, run_duration_timer, &tray) != 0) {
        fprintf(stderr, "FAIL: %s could not start duration worker\n", label);
        return 1;
    }
    pthread_mutex_lock(&gate_lock);
    while (!timer_waiting_for_init) pthread_cond_wait(&gate_changed, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    // Fail the old session, then start a replacement before its timer leaves the init wait.
    mock_thread_id = 43;
    if (tray_nosleep_thread(&tray) != 1 || !tray.core_init_failed) {
        fprintf(stderr, "FAIL: %s did not record the old session's initialization failure\n", label);
        return 1;
    }
    tray_start_nosleep(&tray, 0);
    if (!tray.is_running || tray.core_init_failed) {
        fprintf(stderr, "FAIL: %s did not start a clean replacement session\n", label);
        return 1;
    }

    pthread_mutex_lock(&gate_lock);
    pause_core_resolution = false;
    release_core_resolution = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_lock);
    pthread_join(timer_thread, NULL);

    int failures = 0;
    if (action_threads_started != 0) {
        fprintf(stderr, "FAIL: %s dispatched the old session's action after restart\n", label);
        ++failures;
    }
    if (!tray.is_running || tray.nosleep_thread_id != 77 || tray.core_init_failed) {
        fprintf(stderr, "FAIL: %s allowed its old timer to stop the replacement session\n", label);
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

static int check_restart_cancels_pending_action(SessionFinishedAction action, const char* label) {
    NoSleepTray tray;
    MockHandle events[3] = {{0}};
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.core_init_succeeded = true;
    tray.start_tick64 = 0;
    tray.session_finished_action = action;
    tray.stop_event = &events[0];
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 42;
    tray.nosleep_thread_id = 43;
    tray.sleep_stop_event = &events[2];
    tray.shutdown_stop_event = &events[2];
    mock_tick64 = 120000;
    action_threads_started = 0;
    error_notifications = 0;
    action_stop_signals = 0;
    action_stop_event_to_watch = &events[2];

    run_duration_timer(&tray);
    if (action_threads_started != 1 ||
        (action == SESSION_FINISHED_SLEEP && tray.sleep_timer == NULL) ||
        (action == SESSION_FINISHED_SHUTDOWN && tray.shutdown_timer == NULL)) {
        fprintf(stderr, "FAIL: %s did not create its old session's delayed action\n", label);
        return 1;
    }

    // A replacement session cancels the already pending action from the expired session.
    mock_thread_id = 44;
    tray_start_nosleep(&tray, 0);

    int failures = 0;
    if (action_stop_signals != 1 || !action_handle.closed ||
        tray.sleep_timer != NULL || tray.shutdown_timer != NULL) {
        fprintf(stderr, "FAIL: %s left the old delayed action pending after restart\n", label);
        ++failures;
    }
    if (!tray.is_running || tray.nosleep_thread_id != 77 || tray.core_init_failed) {
        fprintf(stderr, "FAIL: %s changed the replacement session after old action cancellation\n", label);
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    action_stop_event_to_watch = NULL;
    return failures;
}

static int check_startup_failure_race(void) {
    NoSleepTray tray;
    MockHandle events[3] = {{0}};
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.stop_event = &events[0];
    tray.sleep_stop_event = &events[1];
    tray.shutdown_stop_event = &events[2];
    tray.session_finished_action = SESSION_FINISHED_SLEEP;
    mock_tick64 = 0;
    mock_thread_id = 43;
    action_threads_started = 0;
    error_notifications = 0;
    start_test_active = true;
    release_create_failure = false;
    worker_lock_observed = false;
    worker_lock_blocked = false;
    start_worker_completed = false;

    tray_start_nosleep(&tray, 30);
    pthread_join(start_worker_thread, NULL);

    int failures = 0;
    if (tray.is_running || tray.timer_thread != NULL || !start_timer_handle.closed) {
        fprintf(stderr, "FAIL: startup published a duration timer after core failure cleanup\n");
        ++failures;
    }
    if (tray.nosleep_thread != NULL || !start_worker_handle.closed ||
        !tray.core_init_failed || error_notifications != 1) {
        fprintf(stderr, "FAIL: startup failure did not clean its published worker/session\n");
        ++failures;
    }
    if (action_threads_started != 0) {
        fprintf(stderr, "FAIL: startup failure created an unexpected delayed action\n");
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    start_test_active = false;
    return failures;
}

static int check_normal_expiry(SessionFinishedAction action, const char* label) {
    NoSleepTray tray;
    MockHandle events[3] = {{0}};
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.core_init_succeeded = true;
    tray.start_tick64 = 0;
    tray.session_finished_action = action;
    tray.stop_event = &events[0];
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 42;
    tray.sleep_stop_event = &events[2];
    tray.shutdown_stop_event = &events[2];
    mock_tick64 = 120000;
    action_threads_started = 0;

    run_duration_timer(&tray);
    int failures = 0;
    if (action_threads_started != 1) {
        fprintf(stderr, "FAIL: %s no longer starts the configured action\n", label);
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

static int check_expiry_waits_for_core_initialization(SessionFinishedAction action,
                                                       const char* label) {
    NoSleepTray tray;
    MockHandle events[3] = {{0}};
    pthread_t timer_thread;
    memset(&tray, 0, sizeof(tray));
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.start_tick64 = 0;
    tray.session_finished_action = action;
    tray.stop_event = &events[0];
    tray.timer_thread = &events[1];
    tray.timer_thread_id = 42;
    tray.sleep_stop_event = &events[2];
    tray.shutdown_stop_event = &events[2];
    mock_tick64 = 120000;
    action_threads_started = 0;
    pause_core_resolution = true;
    timer_waiting_for_init = false;
    action_attempted_before_init = false;
    release_core_resolution = false;

    if (pthread_create(&timer_thread, NULL, run_duration_timer, &tray) != 0) {
        fprintf(stderr, "FAIL: %s could not start duration worker\n", label);
        return 1;
    }
    pthread_mutex_lock(&gate_lock);
    while (!timer_waiting_for_init && !action_attempted_before_init) {
        pthread_cond_wait(&gate_changed, &gate_lock);
    }
    bool waited_for_init = timer_waiting_for_init;
    bool action_started_early = action_attempted_before_init;
    pthread_mutex_unlock(&gate_lock);

    // Resolve initialization while the duration worker is paused at its gate.
    ATOMIC_STORE_BOOL(&tray.core_init_succeeded, true);
    pthread_mutex_lock(&gate_lock);
    pause_core_resolution = false;
    release_core_resolution = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_lock);
    pthread_join(timer_thread, NULL);

    int failures = 0;
    if (!waited_for_init || action_started_early) {
        fprintf(stderr, "FAIL: %s did not defer its action until core initialization completed\n", label);
        ++failures;
    }
    if (action_threads_started != 1) {
        fprintf(stderr, "FAIL: %s did not run its configured action after initialization succeeded\n", label);
        ++failures;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += check_direct_failure(SESSION_FINISHED_SLEEP, "sleep core failure");
    failures += check_direct_failure(SESSION_FINISHED_SHUTDOWN, "shutdown core failure");
    failures += check_successful_initialization_marks_ready();
    failures += check_stale_worker_failure();
    failures += check_failure_race(SESSION_FINISHED_SLEEP, "sleep expiry race");
    failures += check_failure_race(SESSION_FINISHED_SHUTDOWN, "shutdown expiry race");
    failures += check_restart_during_initialization_wait(SESSION_FINISHED_SLEEP, "sleep restart during init wait");
    failures += check_restart_during_initialization_wait(SESSION_FINISHED_SHUTDOWN, "shutdown restart during init wait");
    failures += check_restart_cancels_pending_action(SESSION_FINISHED_SLEEP, "sleep pending action restart race");
    failures += check_restart_cancels_pending_action(SESSION_FINISHED_SHUTDOWN, "shutdown pending action restart race");
    failures += check_startup_failure_race();
    failures += check_expiry_waits_for_core_initialization(SESSION_FINISHED_SLEEP, "sleep initialization wait");
    failures += check_expiry_waits_for_core_initialization(SESSION_FINISHED_SHUTDOWN, "shutdown initialization wait");
    failures += check_normal_expiry(SESSION_FINISHED_SLEEP, "normal sleep expiry");
    failures += check_normal_expiry(SESSION_FINISHED_SHUTDOWN, "normal shutdown expiry");
    if (failures) return 1;
    puts("PASS: failed core initialization resets tray state and suppresses raced actions");
    return 0;
}
"""

functions = "\n\n".join(
    extract_function(signature)
    for signature in (
        "static ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64)",
        "static bool tray_stop_nosleep_for_session(NoSleepTray* tray,\n                                          DWORD expected_thread_id,\n                                          bool timer_expired,\n                                          bool suppress_notification)",
        "void tray_stop_nosleep(NoSleepTray* tray, bool timer_expired, bool suppress_notification)",
        "void tray_start_nosleep(NoSleepTray* tray, int duration_minutes)",
        "static DWORD WINAPI tray_duration_timer(LPVOID lpParam)",
        "static DWORD WINAPI tray_nosleep_thread(LPVOID lpParam)",
    )
)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_core_init_failure.c"
    binary = Path(tmp) / "test_tray_core_init_failure"
    source.write_text(prelude + "\n" + functions + "\n" + main)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-pthread",
        f"-I{root / 'src'}", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
