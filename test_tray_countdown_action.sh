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
tray_header = (root / "src/tray.h").read_text()


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


assert re.search(r"SessionFinishedAction\s+countdown_action\s*;", tray_header), (
    "the tray must retain the action scheduled for its active countdown"
)

start_countdown = extract_function("tray_start_countdown")
assert re.search(
    r"tray_start_countdown\(NoSleepTray\s*\*\s*tray,\s*"
    r"SessionFinishedAction\s+action,\s*ULONGLONG\s+start_tick64\)",
    start_countdown,
), "countdown startup must receive its scheduled action explicitly"
assert re.search(r"tray->countdown_action\s*=\s*action\s*;", start_countdown), (
    "countdown startup must snapshot the scheduled action before activating display"
)
assert start_countdown.index("tray->countdown_action = action;") < start_countdown.index(
    "ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, true)"
), "countdown action must be stored before the countdown becomes visible"

sleep_thread = extract_function("delayed_sleep_thread")
shutdown_thread = extract_function("delayed_shutdown_thread")
countdown_start = extract_function("tray_start_delayed_countdown")
assert re.search(
    r"tray_start_delayed_countdown\(tray,\s*SESSION_FINISHED_SLEEP,\s*"
    r"tray->sleep_stop_event,\s*start_tick64\)",
    sleep_thread,
), "the delayed sleep thread must identify its scheduled action and stop event"
assert re.search(
    r"tray_start_countdown\(tray,\s*action,\s*start_tick64\)",
    countdown_start,
), (
    "cancellation-safe countdown startup must pass through its scheduled action"
)
assert re.search(
    r"SessionFinishedAction\s+action\s*=\s*tray->shutdown_action;"
    r"[\s\S]*?tray_announce_delayed_action\(tray,\s*action\);"
    r"[\s\S]*?tray_start_delayed_countdown\(tray,\s*action,\s*"
    r"tray->shutdown_stop_event,\s*start_tick64\)"
    r"[\s\S]*?trigger_system_shutdown\(tray,\s*action\)",
    shutdown_thread,
), "the delayed shutdown thread must reuse one captured action for its notice, countdown, and dispatch"
assert shutdown_thread.count("SessionFinishedAction action = tray->shutdown_action;") == 1, (
    "the delayed shutdown thread must capture its scheduled action only once"
)

update = extract_function("tray_update_icon")
countdown_thread = extract_function("countdown_thread")
countdown_display = update.split("// Handle delayed sleep countdown display", 1)[1].split(
    "\n    if (is_running)", 1
)[0]
assert re.search(
    r"tray_format_countdown_tooltip\(tip,\s*sizeof\(tip\),\s*"
    r"tray->countdown_action,\s*"
    r"countdown_seconds\)",
    countdown_display,
), "the countdown tooltip must distinguish the action captured for this countdown"
assert "tray->session_finished_action" not in countdown_display, (
    "changing the When finished preference must not relabel an existing countdown"
)
assert re.search(
    r"tray_countdown_display_seconds\(\s*remaining_ms\s*\)",
    countdown_thread,
), "the tooltip countdown must round positive remaining milliseconds up"
assert re.search(
    r"elapsed_ms\s*>=\s*total_duration_ms[\s\S]*?ATOMIC_STORE_INT\(&tray->countdown_seconds,\s*0\)",
    countdown_thread,
), "an expired countdown must still set its displayed seconds to zero"
announce = extract_function("tray_announce_delayed_action")
assert re.search(
    r"tray_announce_delayed_action\(NoSleepTray\s*\*\s*tray,\s*"
    r"SessionFinishedAction\s+action\)",
    announce,
), "the delayed action announcement must receive its captured action"

harness = r'''#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "tray_countdown_tooltip.h"

static int expect_tooltip_after_preference_change(const char* scenario,
                                                   SessionFinishedAction scheduled_action,
                                                   SessionFinishedAction new_preference_action,
                                                   const char* expected) {
    SessionFinishedAction countdown_action = scheduled_action;
    SessionFinishedAction session_finished_action = scheduled_action;
    session_finished_action = new_preference_action;
    if (session_finished_action == countdown_action) {
        fprintf(stderr, "FAIL: %s did not change the current preference\n", scenario);
        return 1;
    }

    char tip[128];
    tray_format_countdown_tooltip(tip, sizeof(tip), countdown_action, 27);
    if (strcmp(tip, expected) != 0) {
        fprintf(stderr, "FAIL: %s produced '%s'\n", scenario, tip);
        return 1;
    }
    return 0;
}

static int expect_tooltip_for_remaining_time(const char* scenario,
                                             unsigned long long remaining_ms,
                                             const char* expected) {
    int countdown_seconds = tray_countdown_display_seconds(remaining_ms);
    char tip[128];
    tray_format_countdown_tooltip(tip, sizeof(tip), SESSION_FINISHED_SLEEP,
                                  countdown_seconds);
    if (strcmp(tip, expected) != 0) {
        fprintf(stderr, "FAIL: %s produced '%s'\n", scenario, tip);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_tooltip_after_preference_change(
        "sleep countdown changed to forced shutdown",
        SESSION_FINISHED_SLEEP, SESSION_FINISHED_SHUTDOWN,
        "nosleep - System will sleep in 27 seconds");
    failures += expect_tooltip_after_preference_change(
        "forced shutdown countdown changed to sleep",
        SESSION_FINISHED_SHUTDOWN, SESSION_FINISHED_SLEEP,
        "nosleep - System will forcibly shut down in 27 seconds");
    failures += expect_tooltip_after_preference_change(
        "graceful shutdown countdown changed to forced shutdown",
        SESSION_FINISHED_SHUTDOWN_GRACEFUL, SESSION_FINISHED_SHUTDOWN,
        "nosleep - System will gracefully shut down in 27 seconds");
    failures += expect_tooltip_for_remaining_time(
        "positive sub-second sleep countdown", 1,
        "nosleep - System will sleep in 1 second");
    failures += expect_tooltip_for_remaining_time(
        "zero remaining sleep countdown", 0,
        "nosleep - System will sleep in 0 seconds");
    if (failures) return 1;
    puts("PASS: countdown tooltip distinguishes scheduled actions and rounds positive time up");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_countdown_action.c"
    binary = Path(tmp) / "test_tray_countdown_action"
    source.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", f"-I{root / 'src'}",
        str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

print("PASS: countdown startup and tray update use the scheduled action snapshot")

announcement_harness = r'''#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "tray_countdown_tooltip.h"

typedef unsigned long long ULONGLONG;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef struct { int unused; } SRWLOCK;
typedef struct { int unused; } CONDITION_VARIABLE;
typedef enum { NOTIFY_EVENT_TIMER_EXPIRED = 1 } NotifyEventId;
typedef struct NoSleepTray {
    SRWLOCK delayed_action_lock;
    CONDITION_VARIABLE stop_condition;
    bool session_action_cancelled;
    bool starting_nosleep;
    bool stopping;
    bool delayed_countdown_starting;
    HANDLE sleep_stop_event;
    HANDLE shutdown_stop_event;
    ULONGLONG start_tick64;
} NoSleepTray;

#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define ATOMIC_LOAD_BOOL(value) (*(value))
#define DEBUG_LOG(...) ((void)0)
static int notification_count;
static NotifyEventId notification_event;
static char notification_title[128];
static char notification_message[512];
static HANDLE signaled_stop_event;

void AcquireSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
void ReleaseSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
void WakeAllConditionVariable(CONDITION_VARIABLE *condition) { (void)condition; }
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    (void)milliseconds;
    return signaled_stop_event && handle == signaled_stop_event
        ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    (void)start_tick64;
    return 0;
}
void tray_show_notification(NoSleepTray *tray, NotifyEventId event,
                            const char *title, const char *message,
                            bool critical) {
    (void)tray;
    (void)critical;
    ++notification_count;
    notification_event = event;
    snprintf(notification_title, sizeof(notification_title), "%s", title);
    snprintf(notification_message, sizeof(notification_message), "%s", message);
}

''' + announce + r'''

static int expect_notice(SessionFinishedAction action, const char *expected) {
    NoSleepTray tray = {0};
    static int sleep_event;
    static int shutdown_event;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    signaled_stop_event = NULL;
    notification_count = 0;
    tray_announce_delayed_action(&tray, action);
    if (notification_count != 1 || notification_event != NOTIFY_EVENT_TIMER_EXPIRED ||
        strcmp(notification_title, "Time's up!") != 0 ||
        strcmp(notification_message, expected) != 0) {
        fprintf(stderr, "FAIL: action %d produced '%s': '%s' (%d notices)\n",
                action, notification_title, notification_message, notification_count);
        return 1;
    }
    return 0;
}

static int expect_suspended_event_suppresses_notice(SessionFinishedAction action,
                                                     bool sleep_event_signaled) {
    NoSleepTray tray = {0};
    static int sleep_event;
    static int shutdown_event;
    tray.sleep_stop_event = &sleep_event;
    tray.shutdown_stop_event = &shutdown_event;
    signaled_stop_event = sleep_event_signaled
        ? tray.sleep_stop_event : tray.shutdown_stop_event;
    notification_count = 0;
    notification_title[0] = '\0';
    notification_message[0] = '\0';

    tray_announce_delayed_action(&tray, action);

    if (notification_count != 0) {
        fprintf(stderr, "FAIL: signaled suspend stop event published '%s: %s'\n",
                notification_title, notification_message);
        return 1;
    }
    if (tray.delayed_countdown_starting) {
        fprintf(stderr, "FAIL: canceled action reserved countdown startup\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_notice(
        SESSION_FINISHED_SHUTDOWN,
        "Sleep prevention stopped\nDuration: 0m 0s\n"
        "System will forcibly shut down in 60 seconds...\n"
        "Unsaved work may be lost because applications will be closed forcibly.\n"
        "Use Cancel shutdown in the tray menu.");
    failures += expect_notice(
        SESSION_FINISHED_SHUTDOWN_GRACEFUL,
        "Sleep prevention stopped\nDuration: 0m 0s\n"
        "System will gracefully shut down in 60 seconds...\n"
        "Use Cancel shutdown in the tray menu.");
    failures += expect_notice(
        SESSION_FINISHED_SLEEP,
        "Sleep prevention stopped\nDuration: 0m 0s\n"
        "System will sleep in 60 seconds...\n"
        "Use Cancel sleep in the tray menu.");
    failures += expect_suspended_event_suppresses_notice(
        SESSION_FINISHED_SLEEP, true);
    failures += expect_suspended_event_suppresses_notice(
        SESSION_FINISHED_SHUTDOWN, false);
    failures += expect_suspended_event_suppresses_notice(
        SESSION_FINISHED_SHUTDOWN_GRACEFUL, false);
    if (failures) return 1;
    puts("PASS: delayed action notices distinguish shutdown modes and preserve sleep wording");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_delayed_action_notice.c"
    binary = Path(tmp) / "test_tray_delayed_action_notice"
    source.write_text(announcement_harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", f"-I{root / 'src'}",
        str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

startup_failure_harness = r'''#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef void *LPVOID;
typedef void *HANDLE;
#define WINAPI
#define DEBUG_LOG(...) ((void)0)
#define ATOMIC_STORE_BOOL(dest, value) (*(dest) = (value))
#define ATOMIC_STORE_INT(dest, value) (*(dest) = (value))

typedef enum {
    SESSION_FINISHED_NONE = 0,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN_GRACEFUL
} SessionFinishedAction;

typedef enum {
    NOTIFY_EVENT_ERROR = 4
} NotifyEventId;

typedef struct NoSleepTray {
    SessionFinishedAction countdown_action;
    ULONGLONG countdown_start_tick64;
    bool delayed_sleep_countdown_active;
    int countdown_seconds;
    bool countdown_blink_state;
    bool countdown_stopping;
    HANDLE countdown_stop_event;
    HANDLE countdown_timer_thread;
} NoSleepTray;

static ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    (void)start_tick64;
    return 0;
}

static int tray_countdown_display_seconds(ULONGLONG remaining_ms) {
    return (int)((remaining_ms + 999) / 1000);
}

static bool create_thread_success;
static int notification_count;
static int icon_update_count;
static int menu_update_count;
static NotifyEventId notification_event;
static char notification_title[128];
static char notification_message[256];

static DWORD WINAPI countdown_thread(LPVOID lp_param) {
    (void)lp_param;
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
    return create_thread_success ? (HANDLE)1 : NULL;
}

static int ResetEvent(HANDLE event) {
    (void)event;
    return 1;
}

static void tray_stop_countdown(NoSleepTray *tray) {
    (void)tray;
}

static void tray_update_icon(NoSleepTray *tray) {
    (void)tray;
    ++icon_update_count;
}

static void tray_update_stop_menu_item(NoSleepTray *tray) {
    (void)tray;
    ++menu_update_count;
}

static void tray_show_notification(NoSleepTray *tray, NotifyEventId event,
                                   const char *title, const char *message,
                                   bool critical) {
    (void)tray;
    (void)critical;
    ++notification_count;
    notification_event = event;
    snprintf(notification_title, sizeof(notification_title), "%s", title);
    snprintf(notification_message, sizeof(notification_message), "%s", message);
}

''' + start_countdown + r'''

static int expect_failure_notice(SessionFinishedAction action,
                                 const char *expected_message) {
    NoSleepTray tray = {0};
    tray.countdown_stop_event = (HANDLE)1;
    create_thread_success = false;
    notification_count = 0;
    icon_update_count = 0;
    menu_update_count = 0;

    tray_start_countdown(&tray, action, 0);

    if (notification_count != 1 || notification_event != NOTIFY_EVENT_ERROR ||
        strcmp(notification_title, "Countdown Display Unavailable") != 0 ||
        strcmp(notification_message, expected_message) != 0) {
        fprintf(stderr,
                "FAIL: thread creation failure notice was '%s': '%s' (%d notices)\n",
                notification_title, notification_message, notification_count);
        return 1;
    }
    if (icon_update_count != 1 || menu_update_count != 1) {
        fprintf(stderr, "FAIL: startup failure skipped tray state refresh\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_failure_notice(
        SESSION_FINISHED_SLEEP,
        "System will sleep in 60 seconds, but the countdown display could not be started.");
    failures += expect_failure_notice(
        SESSION_FINISHED_SHUTDOWN,
        "System will shut down in 60 seconds, but the countdown display could not be started.");
    if (failures) return 1;
    puts("PASS: countdown thread startup failure reports the pending action");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_countdown_start_failure.c"
    binary = Path(tmp) / "test_tray_countdown_start_failure"
    source.write_text(startup_failure_harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

wait_for_countdown_start = extract_function("tray_wait_for_delayed_countdown_start")
race_harness = r'''#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "tray_countdown_tooltip.h"

typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef void *HANDLE;
typedef pthread_mutex_t SRWLOCK;
typedef pthread_cond_t CONDITION_VARIABLE;
typedef struct NoSleepTray {
    SRWLOCK delayed_action_lock;
    CONDITION_VARIABLE stop_condition;
    bool session_action_cancelled;
    bool starting_nosleep;
    bool stopping;
    bool delayed_countdown_starting;
    HANDLE sleep_stop_event;
    HANDLE shutdown_stop_event;
    ULONGLONG start_tick64;
} NoSleepTray;

enum { NOTIFY_EVENT_TIMER_EXPIRED = 1 };
#define INFINITE 0xffffffffUL
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define ATOMIC_LOAD_BOOL(value) (*(value))
#define DEBUG_LOG(...) ((void)0)

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_condition = PTHREAD_COND_INITIALIZER;
static NoSleepTray *test_tray;
static pthread_t stop_thread;
static bool stop_waiting;
static bool stop_finished;
static bool cancelled_when_published;
static bool stop_thread_start_failed;
static int notification_count;
static void *request_stop(void *parameter);

void AcquireSRWLockExclusive(SRWLOCK *lock) {
    pthread_mutex_lock(lock);
}
void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    pthread_mutex_unlock(lock);
}
bool SleepConditionVariableSRW(CONDITION_VARIABLE *condition, SRWLOCK *lock,
                               DWORD milliseconds, ULONG flags) {
    (void)milliseconds;
    (void)flags;
    pthread_mutex_lock(&test_lock);
    stop_waiting = true;
    pthread_cond_broadcast(&test_condition);
    pthread_mutex_unlock(&test_lock);
    return pthread_cond_wait(condition, lock) == 0;
}
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    (void)handle;
    (void)milliseconds;
    return WAIT_TIMEOUT;
}
void WakeAllConditionVariable(CONDITION_VARIABLE *condition) {
    pthread_cond_broadcast(condition);
}
ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    (void)start_tick64;
    if (pthread_create(&stop_thread, NULL, request_stop, test_tray) != 0) {
        stop_thread_start_failed = true;
        return 0;
    }
    pthread_mutex_lock(&test_lock);
    while (!stop_waiting && !stop_finished) {
        pthread_cond_wait(&test_condition, &test_lock);
    }
    pthread_mutex_unlock(&test_lock);
    return 0;
}

''' + wait_for_countdown_start + r'''

static void *request_stop(void *parameter) {
    NoSleepTray *tray = parameter;
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    tray_wait_for_delayed_countdown_start(tray);
    tray->session_action_cancelled = true;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    pthread_mutex_lock(&test_lock);
    stop_finished = true;
    pthread_cond_broadcast(&test_condition);
    pthread_mutex_unlock(&test_lock);
    return NULL;
}

void tray_show_notification(NoSleepTray *tray, int event,
                            const char *title, const char *message,
                            bool critical) {
    (void)event;
    (void)title;
    (void)message;
    (void)critical;
    ++notification_count;
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    cancelled_when_published = tray->session_action_cancelled;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
}

''' + announce + r'''

int main(void) {
    NoSleepTray tray = {0};
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    pthread_cond_init(&tray.stop_condition, NULL);
    test_tray = &tray;
    tray_announce_delayed_action(&tray, SESSION_FINISHED_SLEEP);
    if (stop_thread_start_failed) {
        fprintf(stderr, "FAIL: could not start concurrent Stop request\n");
        return 1;
    }
    pthread_join(stop_thread, NULL);
    if (notification_count != 1) {
        fprintf(stderr, "FAIL: expected one delayed-action notice, got %d\n",
                notification_count);
        return 1;
    }
    if (cancelled_when_published) {
        fprintf(stderr, "FAIL: Stop cancelled the action before its stale notice was published\n");
        return 1;
    }
    if (!tray.session_action_cancelled || tray.delayed_countdown_starting) {
        fprintf(stderr, "FAIL: concurrent Stop did not complete after notice publication\n");
        return 1;
    }
    pthread_cond_destroy(&tray.stop_condition);
    pthread_mutex_destroy(&tray.delayed_action_lock);
    puts("PASS: Stop waits for a delayed-action notice already being published");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_announcement_cancellation_race.c"
    binary = Path(tmp) / "test_tray_announcement_cancellation_race"
    source.write_text(race_harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", "-pthread", f"-I{root / 'src'}",
        str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
