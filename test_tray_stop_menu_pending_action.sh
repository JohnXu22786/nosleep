#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMP_DIR/test_tray_stop_menu_pending_action.c" <<'PY'
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
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
            if (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function: {name}")


update_internal = extract_function("tray_update_stop_menu_item_internal")
update = extract_function("tray_update_stop_menu_item")
reap = extract_function("tray_reap_delayed_action_handle")

harness = r'''#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef void *HANDLE;
typedef void *HMENU;
typedef char *LPSTR;
typedef enum {
    SESSION_FINISHED_NONE = 0,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN_GRACEFUL
} SessionFinishedAction;

typedef struct {
    unsigned int cbSize;
    unsigned int fMask;
    LPSTR dwTypeData;
} MENUITEMINFO;

typedef struct NoSleepTray {
    HMENU hmenu;
    int delayed_action_lock;
    bool delayed_sleep_countdown_active;
    bool is_running;
    HANDLE sleep_timer;
    HANDLE shutdown_timer;
    bool sleep_action_claimed;
    bool shutdown_action_claimed;
    SessionFinishedAction countdown_action;
    SessionFinishedAction shutdown_action;
    SessionFinishedAction session_finished_action;
} NoSleepTray;

#define MIIM_STRING 1
#define IDM_STOP 2
#define FALSE 0
#define MF_BYCOMMAND 0
#define MF_ENABLED 0
#define MF_GRAYED 1
#define ATOMIC_LOAD_BOOL(value) (*(value))
#define DEBUG_LOG(...) ((void)0)

static char actual_menu_text[64];
static int close_handle_calls;
static int delayed_action_lock_depth;
static int menu_updates_under_lock;

static void AcquireSRWLockExclusive(int *lock) {
    (void)lock;
    ++delayed_action_lock_depth;
}
static void ReleaseSRWLockExclusive(int *lock) {
    (void)lock;
    --delayed_action_lock_depth;
}
static int CloseHandle(HANDLE handle) {
    (void)handle;
    ++close_handle_calls;
    return 1;
}

static int EnableMenuItem(HMENU menu, unsigned int item, unsigned int flags) {
    (void)menu;
    (void)item;
    (void)flags;
    return 1;
}

static int SetMenuItemInfo(HMENU menu, unsigned int item, bool by_position,
                           MENUITEMINFO *info) {
    (void)menu;
    (void)item;
    (void)by_position;
    if (delayed_action_lock_depth > 0) ++menu_updates_under_lock;
    snprintf(actual_menu_text, sizeof(actual_menu_text), "%s", info->dwTypeData);
    return 1;
}

''' + update_internal + "\n" + update + "\n" + reap + r'''

static int expect_menu_text(const char *scenario,
                            bool countdown_active,
                            bool is_running,
                            bool sleep_pending,
                            bool shutdown_pending,
                            bool sleep_claimed,
                            bool shutdown_claimed,
                            SessionFinishedAction countdown_action,
                            SessionFinishedAction shutdown_action,
                            const char *expected) {
    NoSleepTray tray = {0};
    tray.hmenu = (HMENU)1;
    tray.delayed_sleep_countdown_active = countdown_active;
    tray.is_running = is_running;
    tray.sleep_timer = sleep_pending ? (HANDLE)2 : NULL;
    tray.shutdown_timer = shutdown_pending ? (HANDLE)3 : NULL;
    tray.sleep_action_claimed = sleep_claimed;
    tray.shutdown_action_claimed = shutdown_claimed;
    tray.countdown_action = countdown_action;
    tray.shutdown_action = shutdown_action;
    actual_menu_text[0] = '\0';

    tray_update_stop_menu_item(&tray);
    if (strcmp(actual_menu_text, expected) != 0) {
        fprintf(stderr, "FAIL: %s produced '%s', expected '%s'\n",
                scenario, actual_menu_text, expected);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += expect_menu_text(
        "pending sleep after countdown thread creation failure", false, false,
        true, false, false, false, SESSION_FINISHED_SHUTDOWN,
        SESSION_FINISHED_NONE, "Cancel sleep");
    failures += expect_menu_text(
        "pending shutdown after countdown thread creation failure", false, false,
        false, true, false, false, SESSION_FINISHED_SLEEP,
        SESSION_FINISHED_SHUTDOWN_GRACEFUL, "Cancel shutdown");
    failures += expect_menu_text(
        "active sleep countdown", true, false, true, false, false, false,
        SESSION_FINISHED_SLEEP, SESSION_FINISHED_NONE, "Cancel sleep");
    failures += expect_menu_text(
        "active shutdown countdown", true, false, false, true, false, false,
        SESSION_FINISHED_SHUTDOWN, SESSION_FINISHED_SHUTDOWN,
        "Cancel shutdown");
    failures += expect_menu_text(
        "claimed sleep action is no longer cancellable", false, false,
        true, false, true, false, SESSION_FINISHED_SLEEP,
        SESSION_FINISHED_NONE, "Stop");
    failures += expect_menu_text(
        "active NoSleep session", false, true, false, false, false, false,
        SESSION_FINISHED_NONE, SESSION_FINISHED_NONE, "Stop nosleep session");
    failures += expect_menu_text(
        "idle tray", false, false, false, false, false, false,
        SESSION_FINISHED_NONE, SESSION_FINISHED_NONE, "Stop");
    if (failures) return 1;
    NoSleepTray sleep_tray = {0};
    sleep_tray.hmenu = (HMENU)1;
    sleep_tray.sleep_timer = (HANDLE)2;
    sleep_tray.countdown_action = SESSION_FINISHED_SLEEP;
    tray_update_stop_menu_item(&sleep_tray);
    if (strcmp(actual_menu_text, "Cancel sleep") != 0) {
        fprintf(stderr, "FAIL: pending sleep was not shown as cancellable before reaping\n");
        return 1;
    }
    int sleep_menu_updates_before_reap = menu_updates_under_lock;
    tray_reap_delayed_action_handle(&sleep_tray, &sleep_tray.sleep_timer);
    if (sleep_tray.sleep_timer != NULL || strcmp(actual_menu_text, "Stop") != 0 ||
        menu_updates_under_lock != sleep_menu_updates_before_reap + 1) {
        fprintf(stderr, "FAIL: sleep reap left caption '%s' or updated it outside the tray lock\n",
                actual_menu_text);
        return 1;
    }

    NoSleepTray shutdown_tray = {0};
    shutdown_tray.hmenu = (HMENU)1;
    shutdown_tray.shutdown_timer = (HANDLE)3;
    shutdown_tray.shutdown_action = SESSION_FINISHED_SHUTDOWN_GRACEFUL;
    shutdown_tray.countdown_action = SESSION_FINISHED_SHUTDOWN_GRACEFUL;
    tray_update_stop_menu_item(&shutdown_tray);
    if (strcmp(actual_menu_text, "Cancel shutdown") != 0) {
        fprintf(stderr, "FAIL: pending shutdown was not shown as cancellable before reaping\n");
        return 1;
    }
    int shutdown_menu_updates_before_reap = menu_updates_under_lock;
    tray_reap_delayed_action_handle(&shutdown_tray, &shutdown_tray.shutdown_timer);
    if (shutdown_tray.shutdown_timer != NULL || strcmp(actual_menu_text, "Stop") != 0 ||
        close_handle_calls != 2 ||
        menu_updates_under_lock != shutdown_menu_updates_before_reap + 1) {
        fprintf(stderr, "FAIL: shutdown reap left caption '%s' or updated it outside the tray lock\n",
                actual_menu_text);
        return 1;
    }
    puts("PASS: stop menu labels pending delayed actions and active countdowns");
    return 0;
}
'''

output.write_text(harness)
command = shlex.split(os.environ.get("CC", "cc")) + [
    "-std=c99", "-Wall", "-Wextra", str(output), "-o", str(output.with_suffix(""))
]
subprocess.run(command, check=True)
subprocess.run([str(output.with_suffix(""))], check=True)
PY
