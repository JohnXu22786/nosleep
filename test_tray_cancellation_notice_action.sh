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
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:void|bool|DWORD\s+WINAPI|ULONGLONG)\s+"
        + re.escape(name)
        + r"\s*\([^;]*?\)\s*\{",
        tray,
        re.S,
    )
    assert definition, f"could not find definition for {name}"
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
    raise AssertionError(f"unterminated function: {name}")


prelude = r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "tray_stop_guard.h"

typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef void *HANDLE;
typedef struct { int unused; } SRWLOCK;
typedef struct { int unused; } CONDITION_VARIABLE;
typedef struct { int unused; } MockHandle;
typedef enum {
    SESSION_FINISHED_NONE,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP
} SessionFinishedAction;

#define INFINITE 0xffffffffUL
#define WAIT_OBJECT_0 0
#define ES_CONTINUOUS 0x80000000UL
#define NOTIFY_EVENT_COUNTDOWN_CANCEL 1
#define NOTIFY_EVENT_TIMER_EXPIRED 2
#define NOTIFY_EVENT_SESSION_STOP 3
#define NOTIFY_EVENT_ERROR 4
#define DEBUG_LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ATOMIC_LOAD_BOOL(src) (*(src))
#define ATOMIC_STORE_BOOL(dest, value) (*(dest) = (value))

static bool atomic_exchange_bool(bool *value, bool replacement) {
    bool previous = *value;
    *value = replacement;
    return previous;
}
#define ATOMIC_EXCHANGE_BOOL(dest, value) atomic_exchange_bool((dest), (value))

typedef struct NoSleepTray {
    SRWLOCK delayed_action_lock;
    CONDITION_VARIABLE stop_condition;
    DWORD stopping_thread_id;
    bool is_running;
    bool duration_expired;
    bool stopping;
    bool core_init_failed;
    bool nosleep_run_failed;
    HANDLE stop_event;
    HANDLE timer_thread;
    HANDLE nosleep_thread;
    DWORD timer_thread_id;
    DWORD nosleep_thread_id;
    bool sleep_action_claimed;
    bool shutdown_action_claimed;
    HANDLE sleep_timer;
    HANDLE sleep_stop_event;
    HANDLE shutdown_timer;
    HANDLE shutdown_stop_event;
    bool delayed_sleep_countdown_active;
    SessionFinishedAction session_finished_action;
    SessionFinishedAction countdown_action;
    ULONGLONG start_tick64;
} NoSleepTray;

static MockHandle delayed_action_handle;
static NoSleepTray *active_tray;
static int notification_event;
static char notification_title[128];
static char notification_message[128];
static bool countdown_stop_called;

static void AcquireSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
static void ReleaseSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
static bool SleepConditionVariableSRW(CONDITION_VARIABLE *condition,
                                      SRWLOCK *lock, DWORD milliseconds,
                                      ULONG flags) {
    (void)condition;
    (void)lock;
    (void)milliseconds;
    (void)flags;
    return true;
}
static void WakeAllConditionVariable(CONDITION_VARIABLE *condition) { (void)condition; }
static void SetEvent(HANDLE event) { (void)event; }
static void ResetEvent(HANDLE event) { (void)event; }
static DWORD GetCurrentThreadId(void) { return 1; }
static DWORD GetThreadId(HANDLE thread) { (void)thread; return 2; }
static DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    if (active_tray && milliseconds == INFINITE &&
        (handle == active_tray->sleep_timer || handle == active_tray->shutdown_timer) &&
        active_tray->delayed_sleep_countdown_active) {
        active_tray->delayed_sleep_countdown_active = false;
        countdown_stop_called = true;
    }
    return 0;
}
static bool CloseHandle(HANDLE handle) { (void)handle; return true; }
static unsigned long SetThreadExecutionState(unsigned long state) { return state; }
static ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    (void)start_tick64;
    return 0;
}
static void tray_stop_countdown(NoSleepTray *tray) {
    tray->delayed_sleep_countdown_active = false;
    countdown_stop_called = true;
}
static void tray_show_notification(NoSleepTray *tray,
                                   int event_type,
                                   const char *title,
                                   const char *message,
                                   bool critical) {
    (void)tray;
    (void)critical;
    notification_event = event_type;
    snprintf(notification_title, sizeof(notification_title), "%s", title);
    snprintf(notification_message, sizeof(notification_message), "%s", message);
}
static void tray_update_stop_menu_item(NoSleepTray *tray) { (void)tray; }
static void tray_update_icon(NoSleepTray *tray) { (void)tray; }
'''

main = r'''
static int expect_cancellation_notice(const char *scenario,
                                      SessionFinishedAction scheduled_action,
                                      SessionFinishedAction changed_preference,
                                      SessionFinishedAction saved_countdown_action,
                                      bool countdown_display_active,
                                      bool duration_expired,
                                      const char *expected_title,
                                      const char *expected_message) {
    NoSleepTray tray = {0};
    tray.session_finished_action = scheduled_action;
    tray.countdown_action = saved_countdown_action;
    tray.delayed_sleep_countdown_active = countdown_display_active;
    tray.duration_expired = duration_expired;
    tray.session_finished_action = changed_preference;
    if (scheduled_action == SESSION_FINISHED_SHUTDOWN) {
        tray.shutdown_timer = &delayed_action_handle;
    } else {
        tray.sleep_timer = &delayed_action_handle;
    }
    notification_event = 0;
    notification_title[0] = '\0';
    notification_message[0] = '\0';
    countdown_stop_called = false;
    active_tray = &tray;

    tray_stop_nosleep(&tray, false, false);
    active_tray = NULL;

    if (notification_event != NOTIFY_EVENT_COUNTDOWN_CANCEL ||
        strcmp(notification_title, expected_title) != 0 ||
        strcmp(notification_message, expected_message) != 0) {
        fprintf(stderr, "FAIL: %s reported '%s: %s'\n",
                scenario, notification_title, notification_message);
        return 1;
    }
    if (tray.session_finished_action != changed_preference) {
        fprintf(stderr, "FAIL: %s changed the future-session preference\n", scenario);
        return 1;
    }
    if (!countdown_stop_called || tray.delayed_sleep_countdown_active) {
        fprintf(stderr, "FAIL: %s did not stop the active countdown\n", scenario);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_cancellation_notice(
        "sleep countdown changed to shutdown preference",
        SESSION_FINISHED_SLEEP, SESSION_FINISHED_SHUTDOWN,
        SESSION_FINISHED_SLEEP,
        true, false,
        "Sleep cancelled", "System sleep has been cancelled");
    failures += expect_cancellation_notice(
        "shutdown countdown changed to sleep preference",
        SESSION_FINISHED_SHUTDOWN, SESSION_FINISHED_SLEEP,
        SESSION_FINISHED_SHUTDOWN,
        true, false,
        "Shutdown cancelled", "System shutdown has been cancelled");
    failures += expect_cancellation_notice(
        "pending sleep action with failed countdown display after duration expiry",
        SESSION_FINISHED_SLEEP, SESSION_FINISHED_SHUTDOWN,
        SESSION_FINISHED_SLEEP,
        false, true,
        "Sleep cancelled", "System sleep has been cancelled");
    failures += expect_cancellation_notice(
        "pending shutdown action before countdown worker records its type",
        SESSION_FINISHED_SHUTDOWN, SESSION_FINISHED_SLEEP,
        SESSION_FINISHED_NONE,
        false, true,
        "Shutdown cancelled", "System shutdown has been cancelled");
    failures += expect_cancellation_notice(
        "pending shutdown action after a sleep countdown left a stale type",
        SESSION_FINISHED_SHUTDOWN, SESSION_FINISHED_SLEEP,
        SESSION_FINISHED_SLEEP,
        false, true,
        "Shutdown cancelled", "System shutdown has been cancelled");
    if (failures) return 1;
    puts("PASS: cancellation notices use the scheduled action across countdown states");
    return 0;
}
'''

functions = extract_function("tray_stop_nosleep_for_session") + "\n\n" + extract_function("tray_stop_nosleep")
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_cancellation_notice_action.c"
    binary = Path(tmp) / "test_tray_cancellation_notice_action"
    source.write_text(prelude + "\n" + functions + "\n" + main)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", f"-I{root / 'src'}",
        str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
