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
    r"SessionFinishedAction\s+action\)",
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
assert re.search(r"tray_start_countdown\(tray,\s*SESSION_FINISHED_SLEEP\)", sleep_thread), (
    "the delayed sleep thread must identify its scheduled action"
)
assert re.search(r"SessionFinishedAction\s+action\s*=\s*tray->shutdown_action;[\s\S]*?tray_start_countdown\(tray,\s*action\)", shutdown_thread), (
    "the delayed shutdown thread must identify its scheduled action"
)

update = extract_function("tray_update_icon")
countdown_thread = extract_function("countdown_thread")
countdown_display = update.split("// Handle delayed sleep countdown display", 1)[1].split(
    "\n    if (is_running)", 1
)[0]
assert re.search(
    r"tray_format_countdown_tooltip\(tip,\s*sizeof\(tip\),\s*"
    r"\(tray->countdown_action\s*==\s*SESSION_FINISHED_SHUTDOWN\s*\|\|\s*"
    r"tray->countdown_action\s*==\s*SESSION_FINISHED_SHUTDOWN_GRACEFUL\),\s*"
    r"countdown_seconds\)",
    countdown_display,
), "the countdown tooltip must use the action captured for this countdown"
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

harness = r'''#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "tray_countdown_tooltip.h"

static int expect_tooltip_after_preference_change(const char* scenario,
                                                   bool scheduled_shutdown,
                                                   bool new_preference_shutdown,
                                                   const char* expected) {
    bool countdown_action_shutdown = scheduled_shutdown;
    bool session_finished_action_shutdown = scheduled_shutdown;
    session_finished_action_shutdown = new_preference_shutdown;
    if (session_finished_action_shutdown == countdown_action_shutdown) {
        fprintf(stderr, "FAIL: %s did not change the current preference\n", scenario);
        return 1;
    }

    char tip[128];
    tray_format_countdown_tooltip(tip, sizeof(tip), countdown_action_shutdown, 27);
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
    tray_format_countdown_tooltip(tip, sizeof(tip), false, countdown_seconds);
    if (strcmp(tip, expected) != 0) {
        fprintf(stderr, "FAIL: %s produced '%s'\n", scenario, tip);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_tooltip_after_preference_change(
        "sleep countdown changed to shutdown", false, true,
        "nosleep - System will sleep in 27 seconds");
    failures += expect_tooltip_after_preference_change(
        "shutdown countdown changed to sleep", true, false,
        "nosleep - System will shut down in 27 seconds");
    failures += expect_tooltip_for_remaining_time(
        "positive sub-second sleep countdown", 1,
        "nosleep - System will sleep in 1 second");
    failures += expect_tooltip_for_remaining_time(
        "zero remaining sleep countdown", 0,
        "nosleep - System will sleep in 0 seconds");
    if (failures) return 1;
    puts("PASS: countdown tooltip preserves scheduled action and rounds positive time up");
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

startup_failure_harness = r'''#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long DWORD;
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
    bool delayed_sleep_countdown_active;
    int countdown_seconds;
    bool countdown_blink_state;
    bool countdown_stopping;
    HANDLE countdown_stop_event;
    HANDLE countdown_timer_thread;
} NoSleepTray;

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

    tray_start_countdown(&tray, action);

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
PY
