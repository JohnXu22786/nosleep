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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *HANDLE;
typedef struct { int unused; } SRWLOCK;
typedef struct { int unused; } CONDITION_VARIABLE;
typedef struct { unsigned int close_count; } MockHandle;
typedef enum { SESSION_FINISHED_NONE, SESSION_FINISHED_SHUTDOWN, SESSION_FINISHED_SLEEP } SessionFinishedAction;
#define TRUE 1
#define FALSE 0
#define NOTIFY_ALL 0
typedef struct NoSleepTray {
    SRWLOCK delayed_action_lock;
    SRWLOCK countdown_icon_cache_lock;
    CONDITION_VARIABLE stop_condition;
    int duration_minutes;
    int current_number;
    bool prevent_display;
    bool away_mode;
    bool verbose;
    int refresh_interval_seconds;
    SessionFinishedAction session_finished_action;
    bool sleep_after_timeout;
    SessionFinishedAction countdown_action;
    bool countdown_stopping;
    bool start_on_startup;
    int notification_mode;
    bool add_to_path;
    HANDLE sleep_timer;
    HANDLE shutdown_timer;
    HANDLE stop_event;
    HANDLE sleep_stop_event;
    HANDLE shutdown_stop_event;
    HANDLE countdown_stop_event;
} NoSleepTray;

#define DEBUG_PRINT(...) ((void)0)

static MockHandle created_handles[4];
static int fail_create_call;
static int create_calls;
static int close_calls;
static int unexpected_close_calls;

static void InitializeSRWLock(SRWLOCK *lock) { (void)lock; }
static void InitializeConditionVariable(CONDITION_VARIABLE *condition) { (void)condition; }

static HANDLE CreateEvent(void *attributes, int manual_reset, int initial_state, const char *name) {
    (void)attributes;
    (void)manual_reset;
    (void)initial_state;
    (void)name;
    int call = ++create_calls;
    if (call == fail_create_call) return NULL;
    return &created_handles[call - 1];
}

static int CloseHandle(HANDLE handle) {
    ++close_calls;
    for (size_t i = 0; i < sizeof(created_handles) / sizeof(created_handles[0]); ++i) {
        if (handle == &created_handles[i]) {
            ++created_handles[i].close_count;
            return 1;
        }
    }
    ++unexpected_close_calls;
    return 0;
}
"""

main = r"""
int main(void) {
    int failures = 0;
    for (int fail_at = 1; fail_at <= 4; ++fail_at) {
        memset(created_handles, 0, sizeof(created_handles));
        fail_create_call = fail_at;
        create_calls = 0;
        close_calls = 0;
        unexpected_close_calls = 0;

        NoSleepTray *tray = tray_create();
        if (tray != NULL) {
            fprintf(stderr, "FAIL: CreateEvent call %d did not fail tray_create\n", fail_at);
            free(tray);
            ++failures;
        }

        int expected_closes = fail_at - 1;
        if (close_calls != expected_closes || unexpected_close_calls != 0) {
            fprintf(stderr, "FAIL: failure at CreateEvent call %d closed %d handles; expected %d\n",
                    fail_at, close_calls, expected_closes);
            ++failures;
        }
        for (int i = 0; i < expected_closes; ++i) {
            if (created_handles[i].close_count != 1) {
                fprintf(stderr, "FAIL: failure at CreateEvent call %d closed prior handle %d %u times\n",
                        fail_at, i + 1, created_handles[i].close_count);
                ++failures;
            }
        }
    }

    if (failures != 0) return 1;
    puts("PASS: tray_create closes every event created before initialization failure");
    return 0;
}
"""

function = extract_function("NoSleepTray* tray_create(void)")
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_create_event_cleanup.c"
    binary = Path(tmp) / "test_tray_create_event_cleanup"
    source.write_text(prelude + "\n" + function + "\n" + main)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
