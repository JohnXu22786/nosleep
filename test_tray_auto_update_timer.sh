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


def extract_function(name, return_type="void"):
    definition = re.search(
        r"\b(?:static\s+)?" + re.escape(return_type) + r"\s+" +
        re.escape(name) + r"\s*\([^;]*?\)\s*\{",
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


settings_handler = tray.split("case IDC_SETTINGS_OK:", 1)[1].split(
    "case IDC_SETTINGS_CANCEL:", 1
)[0]
assert "tray_apply_auto_check_interval(settings_tray, proposed.auto_check_interval);" in settings_handler, (
    "saving the selected update interval must apply it to the live timer"
)

startup_check = extract_function("should_check_for_updates", "bool")
assert "interval = normalize_auto_check_interval(interval);" in startup_check, (
    "startup update checks must normalize the stored interval"
)

normalize_interval = extract_function("normalize_auto_check_interval", "DWORD")
read_setting = extract_function("settings_read_dword", "bool")
load_settings = extract_function("tray_load_settings")
apply_interval = extract_function("tray_apply_auto_check_interval")
setup_timer = extract_function("tray_setup_update_timer")
begin_update_check = extract_function("tray_update_check_begin", "bool")
end_update_check = extract_function("tray_update_check_end")

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* HWND;
typedef void* HKEY;
typedef unsigned int UINT;
typedef unsigned int DWORD;
typedef int LONG;
typedef uintptr_t UINT_PTR;
typedef unsigned long long ULONGLONG;
typedef unsigned char* LPBYTE;
typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;
typedef union {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;
typedef enum {
    SESSION_FINISHED_NONE = 0,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN_GRACEFUL
} SessionFinishedAction;

typedef struct {
    HWND hwnd;
    int auto_check_interval;
    UINT_PTR update_timer_id;
    bool prevent_display;
    bool away_mode;
    bool verbose;
    bool check_updates_on_startup;
    int notification_mode;
    bool add_to_path_preference_set;
    bool add_to_path;
    SessionFinishedAction session_finished_action;
} NoSleepTray;

#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)1)
#define REG_DWORD 4
#define REG_QWORD 11
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define REG_OPTION_NON_VOLATILE 0
#define KEY_READ 0
#define SETTINGS_REG_KEY "Software\\nosleep\\settings"
#define NOTIFY_ALL 0
#define CB_ERR (-1)
#define NOTIFY_EVENT_UPDATE_CHECK_FAILED 7

typedef int NotifyEventId;

static bool update_check_in_progress;

static int kill_count;
static int set_count;
static int fail_next_set;
static UINT last_interval_ms;
static int notification_count;
static NotifyEventId last_notification_event;
static char last_notification_title[128];
static char last_notification_message[256];
static DWORD stored_auto_check_interval = 3;
static ULONGLONG stored_last_update_check;
static ULONGLONG fake_now_100ns;
static bool has_last_update_check;

static LONG RegOpenKeyEx(HKEY root, const char* subkey, DWORD options,
                         DWORD access, HKEY* result) {
    (void)root;
    (void)subkey;
    (void)options;
    (void)access;
    *result = (HKEY)(uintptr_t)1;
    return ERROR_SUCCESS;
}

static LONG RegCreateKeyEx(HKEY root, const char* subkey, DWORD reserved,
                           char* class_name, DWORD options, DWORD access,
                           void* security, HKEY* result, DWORD* disposition) {
    (void)root;
    (void)subkey;
    (void)reserved;
    (void)class_name;
    (void)options;
    (void)access;
    (void)security;
    (void)disposition;
    *result = (HKEY)(uintptr_t)1;
    return ERROR_SUCCESS;
}

static LONG RegQueryValueEx(HKEY key, const char* name, void* reserved,
                            DWORD* type, LPBYTE data, DWORD* size) {
    (void)key;
    (void)reserved;
    if (strcmp(name, "auto_check_interval") == 0) {
        if (*size < sizeof(stored_auto_check_interval)) return 234;
        if (type) *type = REG_DWORD;
        memcpy(data, &stored_auto_check_interval, sizeof(stored_auto_check_interval));
        *size = sizeof(stored_auto_check_interval);
        return ERROR_SUCCESS;
    }
    if (strcmp(name, "last_update_check") == 0 && has_last_update_check) {
        if (*size < sizeof(stored_last_update_check)) return 234;
        if (type) *type = REG_QWORD;
        memcpy(data, &stored_last_update_check, sizeof(stored_last_update_check));
        *size = sizeof(stored_last_update_check);
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

static void GetSystemTimeAsFileTime(FILETIME* file_time) {
    file_time->dwLowDateTime = (DWORD)fake_now_100ns;
    file_time->dwHighDateTime = (DWORD)(fake_now_100ns >> 32);
}

static LONG RegCloseKey(HKEY key) {
    (void)key;
    return ERROR_SUCCESS;
}

static void tray_show_notification(NoSleepTray* tray, NotifyEventId event_type,
                                   const char* title, const char* message, bool critical) {
    (void)tray;
    (void)critical;
    ++notification_count;
    last_notification_event = event_type;
    snprintf(last_notification_title, sizeof(last_notification_title), "%s", title);
    snprintf(last_notification_message, sizeof(last_notification_message), "%s", message);
}

static int KillTimer(HWND hwnd, UINT_PTR timer_id) {
    (void)hwnd;
    if (!timer_id) return 0;
    ++kill_count;
    return 1;
}

static UINT_PTR SetTimer(HWND hwnd, UINT_PTR timer_id, UINT interval_ms, void* callback) {
    (void)hwnd;
    (void)callback;
    ++set_count;
    last_interval_ms = interval_ms;
    if (fail_next_set) {
        fail_next_set = 0;
        return 0;
    }
    return timer_id ? timer_id : 2000;
}

static void tray_setup_update_timer(NoSleepTray* tray, bool use_remaining_interval);

__NORMALIZE_INTERVAL__
__READ_SETTING__
__SHOULD_CHECK__
__LOAD_SETTINGS__
__APPLY_INTERVAL__
__SETUP_TIMER__
__BEGIN_UPDATE_CHECK__
__END_UPDATE_CHECK__

static int fail(const char* scenario) {
    fprintf(stderr, "FAIL: %s\n", scenario);
    return 1;
}

int main(void) {
    NoSleepTray tray = { .hwnd = (HWND)1 };
    NoSleepTray startup_tray = { .hwnd = (HWND)1, .auto_check_interval = 1 };
    NoSleepTray loaded_tray = { .hwnd = (HWND)1 };

    tray_load_settings(&loaded_tray);
    if (loaded_tray.auto_check_interval != 1) {
        return fail("an invalid stored update interval must be normalized to Daily");
    }
    tray_setup_update_timer(&loaded_tray, true);
    if (loaded_tray.update_timer_id == 0 || set_count != 1 ||
        last_interval_ms != 86400000U) {
        return fail("an invalid stored interval must schedule the normalized daily timer");
    }

    set_count = 0;
    kill_count = 0;
    notification_count = 0;

    if (!tray_update_check_begin()) {
        return fail("the first update check must start");
    }
    if (tray_update_check_begin()) {
        return fail("a nested update check must be suppressed");
    }
    tray_update_check_end();
    if (!tray_update_check_begin()) {
        return fail("update checks must be allowed after the active check ends");
    }
    tray_update_check_end();

    fail_next_set = 1;
    tray_setup_update_timer(&startup_tray, true);
    if (startup_tray.auto_check_interval != 1 || startup_tray.update_timer_id != 0 ||
        set_count != 1 || kill_count != 0 || last_interval_ms != 86400000U ||
        notification_count != 1 ||
        last_notification_event != NOTIFY_EVENT_UPDATE_CHECK_FAILED ||
        strcmp(last_notification_title, "Automatic Updates Not Scheduled") != 0 ||
        strcmp(last_notification_message,
               "Could not schedule automatic update checks. Open Settings and use Retry applied schedule to retry.") != 0) {
        return fail("a startup SetTimer failure must preserve the saved interval and notify the user");
    }

    tray_apply_auto_check_interval(&startup_tray, 1);
    if (startup_tray.auto_check_interval != 1 || startup_tray.update_timer_id == 0 ||
        set_count != 2 || notification_count != 1) {
        return fail("the saved interval must remain available for a later settings retry");
    }

    set_count = 0;
    kill_count = 0;
    notification_count = 0;

    fail_next_set = 1;
    tray_apply_auto_check_interval(&tray, 1);
    if (tray.auto_check_interval != 1 || tray.update_timer_id != 0 ||
        set_count != 1 || kill_count != 0 || last_interval_ms != 86400000U) {
        return fail("a failed timer creation must preserve the selected interval for retry");
    }

    tray_apply_auto_check_interval(&tray, 1);
    if (tray.auto_check_interval != 1 || tray.update_timer_id == 0 ||
        set_count != 2 || kill_count != 0 || last_interval_ms != 86400000U) {
        return fail("reselecting Daily must retry and create a daily timer");
    }

    tray_apply_auto_check_interval(&tray, 2);
    if (tray.auto_check_interval != 2 || set_count != 3 || kill_count != 1 ||
        last_interval_ms != 604800000U) {
        return fail("Daily to Weekly must replace the daily timer with a weekly timer");
    }

    tray_apply_auto_check_interval(&tray, 0);
    if (tray.auto_check_interval != 0 || tray.update_timer_id != 0 ||
        set_count != 3 || kill_count != 2) {
        return fail("Weekly to Never must cancel the live timer");
    }

    tray_apply_auto_check_interval(&tray, 0);
    if (set_count != 3 || kill_count != 2) {
        return fail("saving an unchanged interval must not reset its timer");
    }

    tray_apply_auto_check_interval(&tray, CB_ERR);
    if (set_count != 3 || kill_count != 2) {
        return fail("an invalid combo selection must leave the timer unchanged");
    }

    const ULONGLONG ticks_per_millisecond = 10000ULL;
    const ULONGLONG daily_ticks = 24ULL * 60ULL * 60ULL * 10000000ULL;
    const ULONGLONG weekly_ticks = 7ULL * daily_ticks;
    fake_now_100ns = 20ULL * daily_ticks;
    stored_auto_check_interval = 1;
    has_last_update_check = true;
    stored_last_update_check = fake_now_100ns - daily_ticks + 100ULL * ticks_per_millisecond;
    if (should_check_for_updates()) {
        return fail("a daily startup check must wait until its saved interval is due");
    }
    NoSleepTray daily_tray = { .hwnd = (HWND)1, .auto_check_interval = 1 };
    set_count = 0;
    tray_setup_update_timer(&daily_tray, true);
    if (last_interval_ms != 100 || set_count != 1) {
        return fail("a daily timer must wait only the time remaining since its last check");
    }
    tray_setup_update_timer(&daily_tray, false);
    if (last_interval_ms != 86400000U) {
        return fail("a daily timer must return to its full interval after the first partial period");
    }

    stored_auto_check_interval = 2;
    stored_last_update_check = fake_now_100ns - weekly_ticks + 250ULL * ticks_per_millisecond;
    NoSleepTray weekly_tray = { .hwnd = (HWND)1, .auto_check_interval = 2 };
    set_count = 0;
    tray_setup_update_timer(&weekly_tray, true);
    if (last_interval_ms != 250 || set_count != 1) {
        return fail("a weekly timer must wait only the time remaining since its last check");
    }

    stored_auto_check_interval = 1;
    stored_last_update_check = fake_now_100ns + 60ULL * 60ULL * 10000000ULL;
    if (!should_check_for_updates()) {
        return fail("a future saved timestamp after clock rollback must allow a startup check");
    }
    NoSleepTray rollback_tray = { .hwnd = (HWND)1, .auto_check_interval = 1 };
    tray_setup_update_timer(&rollback_tray, true);
    if (last_interval_ms != 86400000U) {
        return fail("a clock rollback must keep the next periodic timer from firing repeatedly");
    }

    puts("PASS: update checks are non-reentrant and interval changes reconfigure the timer");
    return 0;
}
'''

harness = harness.replace("__NORMALIZE_INTERVAL__", normalize_interval).replace(
    "__READ_SETTING__", read_setting
).replace(
    "__SHOULD_CHECK__", startup_check
).replace(
    "__LOAD_SETTINGS__", load_settings
).replace("__APPLY_INTERVAL__", apply_interval).replace(
    "__SETUP_TIMER__", setup_timer
).replace("__BEGIN_UPDATE_CHECK__", begin_update_check).replace(
    "__END_UPDATE_CHECK__", end_update_check
)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_auto_update_timer.c"
    binary = Path(tmp) / "test_tray_auto_update_timer"
    source.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
