#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    import re

    definition = re.search(
        r"\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", tray, re.S
    )
    assert definition, f"could not find definition for {name}"
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
                    return tray[opening + 1 : i]
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
    raise AssertionError(f"unterminated function: {name}")


def extract_lock_region(source, starting_marker):
    marker_position = source.index(starting_marker)
    start = source.index("AcquireSRWLockExclusive(&tray->delayed_action_lock);", marker_position)
    end_marker = "ReleaseSRWLockExclusive(&tray->delayed_action_lock);"
    end = source.index(end_marker, start) + len(end_marker)
    return source[start:end]


window_proc = extract_function("tray_window_proc")
suspend_start = window_proc.index("case PBT_APMSUSPEND:")
suspend_end = window_proc.index("case PBT_APMRESUMESUSPEND:", suspend_start)
suspend_case = window_proc[suspend_start:suspend_end]

duration_timer = extract_function("tray_duration_timer")
readiness_lock = extract_lock_region(
    duration_timer,
    "// Do not honor a follow-up action until the NoSleep core is known to exist.",
)
assert "ATOMIC_STORE_BOOL(&tray->duration_expired, true)" in readiness_lock, (
    "duration expiry must be published under the final session-state lock"
)
sleep_startup = extract_lock_region(duration_timer, "HANDLE sleep_timer = NULL;")
shutdown_startup = extract_lock_region(duration_timer, "HANDLE shutdown_timer = NULL;")

harness = r'''#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef void *LPVOID;
typedef void *HANDLE;
#define WINAPI
#define PBT_APMSUSPEND 4
#define DEBUG_LOG(...) ((void)0)
#define ATOMIC_LOAD_BOOL(value) (*(value))
#define ATOMIC_STORE_BOOL(value, new_value) (*(value) = (new_value))

typedef enum {
    SESSION_FINISHED_NONE = 0,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN_GRACEFUL
} SessionFinishedAction;

typedef enum {
    NOTIFY_EVENT_SLEEP_DETECTED = 1
} NotifyEventId;

typedef struct NoSleepTray {
    int delayed_action_lock;
    bool duration_expired;
    bool is_running;
    bool session_action_cancelled;
    bool starting_nosleep;
    bool core_init_failed;
    bool core_init_succeeded;
    bool nosleep_run_failed;
    bool delayed_sleep_countdown_active;
    bool sleep_action_claimed;
    bool shutdown_action_claimed;
    DWORD timer_thread_id;
    int duration_minutes;
    ULONGLONG start_tick64;
    HANDLE sleep_stop_event;
    HANDLE shutdown_stop_event;
    HANDLE sleep_timer;
    HANDLE shutdown_timer;
    SessionFinishedAction shutdown_action;
} NoSleepTray;

static ULONGLONG current_tick64;
ULONGLONG GetTickCount64(void) { return current_tick64; }

static int create_thread_count;
static int reset_event_count;
static bool suspend_on_next_unlock;
static NoSleepTray *suspend_target;
static bool publish_expiry_on_next_acquire;
static NoSleepTray *expiry_target;
static bool start_actions_before_suspend_lock;
static bool actions_started_before_suspend_lock;

static void simulate_suspend(NoSleepTray *tray);
static bool try_start_sleep_action(NoSleepTray *tray, DWORD timer_thread_id);
static bool try_start_shutdown_action(NoSleepTray *tray, DWORD timer_thread_id,
                                      SessionFinishedAction finished_action);

void AcquireSRWLockExclusive(void *lock) {
    (void)lock;
    if (publish_expiry_on_next_acquire) {
        publish_expiry_on_next_acquire = false;
        current_tick64 = expiry_target->start_tick64 +
            (ULONGLONG)expiry_target->duration_minutes * 60 * 1000;
        expiry_target->duration_expired = true;
        if (start_actions_before_suspend_lock) {
            start_actions_before_suspend_lock = false;
            actions_started_before_suspend_lock =
                try_start_sleep_action(expiry_target, expiry_target->timer_thread_id) &&
                try_start_shutdown_action(expiry_target, expiry_target->timer_thread_id,
                                          SESSION_FINISHED_SHUTDOWN);
        }
    }
}
void ReleaseSRWLockExclusive(void *lock) {
    (void)lock;
    if (suspend_on_next_unlock) {
        suspend_on_next_unlock = false;
        simulate_suspend(suspend_target);
    }
}
void tray_wait_for_delayed_countdown_start(NoSleepTray *tray) { (void)tray; }
void tray_stop_countdown(NoSleepTray *tray) { (void)tray; }
void tray_show_notification(NoSleepTray *tray, NotifyEventId event,
                            const char *title, const char *message,
                            bool critical) {
    (void)tray;
    (void)event;
    (void)title;
    (void)message;
    (void)critical;
}

static int SetEvent(HANDLE handle) {
    *(bool *)handle = true;
    return 1;
}

static int ResetEvent(HANDLE handle) {
    ++reset_event_count;
    *(bool *)handle = false;
    return 1;
}

static DWORD WINAPI delayed_sleep_thread(LPVOID context) {
    (void)context;
    return 0;
}

static DWORD WINAPI delayed_shutdown_thread(LPVOID context) {
    (void)context;
    return 0;
}

static HANDLE CreateThread(void *attributes, size_t stack_size,
                           DWORD (WINAPI *start)(LPVOID), LPVOID parameter,
                           DWORD flags, DWORD *thread_id) {
    (void)attributes;
    (void)stack_size;
    (void)start;
    (void)parameter;
    (void)flags;
    (void)thread_id;
    ++create_thread_count;
    return (HANDLE)1;
}

static void simulate_suspend(NoSleepTray *tray) {
    switch (PBT_APMSUSPEND) {
''' + suspend_case + r'''
    }
}

static bool try_start_sleep_action(NoSleepTray *tray, DWORD timer_thread_id) {
    HANDLE sleep_timer = NULL;
    bool core_init_failed;
    bool core_init_succeeded;
    bool nosleep_run_failed;
    bool stale_session;
    bool session_starting;
''' + sleep_startup + r'''
    return sleep_timer != NULL;
}

static bool try_start_shutdown_action(NoSleepTray *tray, DWORD timer_thread_id,
                                      SessionFinishedAction finished_action) {
    HANDLE shutdown_timer = NULL;
    bool shutdown_core_init_failed;
    bool shutdown_core_init_succeeded;
    bool shutdown_nosleep_run_failed;
    bool shutdown_stale_session;
    bool shutdown_session_starting;
''' + shutdown_startup + r'''
    return shutdown_timer != NULL;
}

static bool timer_session_ready_for_action(NoSleepTray *tray,
                                           DWORD timer_thread_id) {
''' + readiness_lock + r'''
    return !should_abandon && core_init_succeeded;
}

static int expect_suspend_after_expiry_publication(void) {
    bool sleep_event = false;
    bool shutdown_event = false;
    NoSleepTray tray = {0};
    tray.is_running = true;
    tray.core_init_succeeded = true;
    tray.duration_minutes = 1;
    tray.start_tick64 = 1000;
    tray.timer_thread_id = 27;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    current_tick64 = 61000;

    create_thread_count = 0;
    reset_event_count = 0;
    suspend_target = &tray;
    suspend_on_next_unlock = true;
    if (!timer_session_ready_for_action(&tray, tray.timer_thread_id) ||
        suspend_on_next_unlock || !tray.duration_expired ||
        !tray.session_action_cancelled || !sleep_event || !shutdown_event) {
        fprintf(stderr, "FAIL: suspend missed expiry at the readiness-lock boundary\n");
        return 1;
    }

    bool sleep_started = try_start_sleep_action(&tray, tray.timer_thread_id);
    bool shutdown_started = try_start_shutdown_action(
        &tray, tray.timer_thread_id, SESSION_FINISHED_SHUTDOWN);
    if (sleep_started || shutdown_started || create_thread_count != 0 || reset_event_count != 0 ||
        !sleep_event || !shutdown_event) {
        fprintf(stderr, "FAIL: delayed-action startup erased boundary suspend cancellation\n");
        return 1;
    }
    return 0;
}

static int expect_suspend_first_after_duration_elapsed(void) {
    bool sleep_event = false;
    bool shutdown_event = false;
    NoSleepTray tray = {0};
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.start_tick64 = 1000;
    tray.timer_thread_id = 27;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    current_tick64 = 61000;

    create_thread_count = 0;
    reset_event_count = 0;
    simulate_suspend(&tray);
    if (!tray.session_action_cancelled || !sleep_event || !shutdown_event) {
        fprintf(stderr, "FAIL: suspend missed an elapsed duration before timer publication\n");
        return 1;
    }

    tray.core_init_succeeded = true;
    if (!timer_session_ready_for_action(&tray, tray.timer_thread_id) ||
        !tray.duration_expired) {
        fprintf(stderr, "FAIL: timer did not finish the expired session after resume\n");
        return 1;
    }

    bool sleep_started = try_start_sleep_action(&tray, tray.timer_thread_id);
    bool shutdown_started = try_start_shutdown_action(
        &tray, tray.timer_thread_id, SESSION_FINISHED_SHUTDOWN);
    if (sleep_started || shutdown_started || create_thread_count != 0 ||
        reset_event_count != 0 || !sleep_event || !shutdown_event) {
        fprintf(stderr, "FAIL: suspend-first cancellation was erased by delayed-action startup\n");
        return 1;
    }
    return 0;
}

static int expect_pre_expiry_suspend_does_not_cancel_later_actions(void) {
    bool sleep_event = false;
    bool shutdown_event = false;
    NoSleepTray tray = {0};
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.start_tick64 = 1000;
    tray.core_init_succeeded = true;
    tray.timer_thread_id = 27;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    current_tick64 = 60999;
    expiry_target = &tray;
    publish_expiry_on_next_acquire = true;
    start_actions_before_suspend_lock = true;
    actions_started_before_suspend_lock = false;

    create_thread_count = 0;
    reset_event_count = 0;
    simulate_suspend(&tray);
    if (publish_expiry_on_next_acquire || start_actions_before_suspend_lock ||
        !actions_started_before_suspend_lock || !tray.duration_expired ||
        sleep_event || shutdown_event || tray.session_action_cancelled) {
        fprintf(stderr, "FAIL: pre-expiry suspend canceled an action created before lock acquisition\n");
        return 1;
    }

    if (tray.sleep_timer == NULL || tray.shutdown_timer == NULL ||
        create_thread_count != 2 || reset_event_count != 2) {
        fprintf(stderr, "FAIL: actions did not start before the delayed suspend handler acquired the lock\n");
        return 1;
    }
    return 0;
}

static int expect_suspend_cancels_delayed_start(SessionFinishedAction action) {
    bool sleep_event = false;
    bool shutdown_event = false;
    NoSleepTray tray = {0};
    tray.is_running = true;
    tray.duration_expired = true;
    tray.core_init_succeeded = true;
    tray.duration_minutes = 1;
    tray.start_tick64 = 1000;
    tray.timer_thread_id = 27;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    current_tick64 = 61000;

    create_thread_count = 0;
    reset_event_count = 0;
    simulate_suspend(&tray);
    if (!sleep_event || !shutdown_event) {
        fprintf(stderr, "FAIL: suspend did not signal both delayed-action stop events\n");
        return 1;
    }

    bool started = action == SESSION_FINISHED_SLEEP
        ? try_start_sleep_action(&tray, tray.timer_thread_id)
        : try_start_shutdown_action(&tray, tray.timer_thread_id, action);
    if (started || create_thread_count != 0) {
        fprintf(stderr, "FAIL: suspend allowed delayed action %d to start\n", action);
        return 1;
    }
    if (reset_event_count != 0 || !sleep_event || !shutdown_event) {
        fprintf(stderr, "FAIL: delayed-action startup erased suspend cancellation\n");
        return 1;
    }
    return 0;
}

static int expect_active_session_keeps_future_action(SessionFinishedAction action) {
    bool sleep_event = false;
    bool shutdown_event = false;
    NoSleepTray tray = {0};
    tray.is_running = true;
    tray.duration_minutes = 1;
    tray.start_tick64 = 1000;
    tray.core_init_succeeded = true;
    tray.timer_thread_id = 27;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    current_tick64 = 60999;

    create_thread_count = 0;
    reset_event_count = 0;
    simulate_suspend(&tray);
    bool started = action == SESSION_FINISHED_SLEEP
        ? try_start_sleep_action(&tray, tray.timer_thread_id)
        : try_start_shutdown_action(&tray, tray.timer_thread_id, action);
    if (!started || create_thread_count != 1 || reset_event_count != 1 ||
        tray.session_action_cancelled) {
        fprintf(stderr, "FAIL: suspend canceled a future action for an active session\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_suspend_after_expiry_publication();
    failures += expect_suspend_first_after_duration_elapsed();
    failures += expect_pre_expiry_suspend_does_not_cancel_later_actions();
    failures += expect_suspend_cancels_delayed_start(SESSION_FINISHED_SLEEP);
    failures += expect_suspend_cancels_delayed_start(SESSION_FINISHED_SHUTDOWN);
    failures += expect_active_session_keeps_future_action(SESSION_FINISHED_SLEEP);
    failures += expect_active_session_keeps_future_action(SESSION_FINISHED_SHUTDOWN);
    if (failures) return 1;
    puts("PASS: suspend cancellation persists until delayed workers are considered");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_suspend_delayed_action_race.c"
    binary = Path(tmp) / "test_tray_suspend_delayed_action_race"
    source.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
