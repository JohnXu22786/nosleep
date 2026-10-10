#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMP_DIR/test_tray_disable_wake_events.c" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
tray = (root / "src/tray.c").read_text()
definition = re.search(
    r"\bstatic\s+void\s+trigger_system_sleep\s*\([^;]*?\)\s*\{",
    tray,
    re.S,
)
assert definition, "could not find trigger_system_sleep definition"

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
                function = tray[definition.start() : i + 1]
                break
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
else:
    raise AssertionError("unterminated trigger_system_sleep definition")

harness = r'''#include <stdbool.h>
#include <stdio.h>

typedef int BOOL;
typedef unsigned long DWORD;

#define TRUE 1
#define FALSE 0
#define ES_CONTINUOUS 0x80000000UL
#define DEBUG_LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)

typedef enum {
    NOTIFY_EVENT_ACTION_FAILED
} NotifyEventId;

typedef struct NoSleepTray {
    int unused;
} NoSleepTray;

static int suspend_calls;
static BOOL suspend_hibernate;
static BOOL suspend_force_critical;
static BOOL suspend_disable_wake_event;

static BOOL SetThreadExecutionState(DWORD state) {
    (void)state;
    return TRUE;
}

static BOOL SetSuspendState(BOOL hibernate,
                            BOOL force_critical,
                            BOOL disable_wake_event) {
    ++suspend_calls;
    suspend_hibernate = hibernate;
    suspend_force_critical = force_critical;
    suspend_disable_wake_event = disable_wake_event;
    return TRUE;
}

static DWORD GetLastError(void) {
    return 0;
}

static void tray_show_notification(NoSleepTray* tray,
                                  NotifyEventId event_type,
                                  const char* title,
                                  const char* message,
                                  BOOL urgent) {
    (void)tray;
    (void)event_type;
    (void)title;
    (void)message;
    (void)urgent;
}

'''

test = r'''
int main(void) {
    NoSleepTray tray = {0};
    trigger_system_sleep(&tray);

    if (suspend_calls != 1) {
        fprintf(stderr, "FAIL: scheduled sleep must call SetSuspendState once\n");
        return 1;
    }
    if (suspend_hibernate != FALSE || suspend_force_critical != FALSE) {
        fprintf(stderr, "FAIL: scheduled sleep must preserve its existing mode arguments\n");
        return 1;
    }
    if (suspend_disable_wake_event != TRUE) {
        fprintf(stderr, "FAIL: scheduled sleep must disable wake events\n");
        return 1;
    }

    puts("PASS: scheduled sleep disables wake events");
    return 0;
}
'''
output.write_text(harness + function + test)
PY

"${CC:-cc}" -std=c99 -Wall -Wextra "$TMP_DIR/test_tray_disable_wake_events.c" \
    -o "$TMP_DIR/test_tray_disable_wake_events"
"$TMP_DIR/test_tray_disable_wake_events"
