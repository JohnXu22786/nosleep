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
        r"\bstatic\s+" + re.escape(return_type) + r"\s+" +
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
assert "tray_apply_auto_check_interval(settings_tray, sel);" in settings_handler, (
    "saving the selected update interval must apply it to the live timer"
)

apply_interval = extract_function("tray_apply_auto_check_interval")
setup_timer = extract_function("tray_setup_update_timer")
begin_update_check = extract_function("tray_update_check_begin", "bool")
end_update_check = extract_function("tray_update_check_end")

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void* HWND;
typedef unsigned int UINT;
typedef uintptr_t UINT_PTR;
typedef struct {
    HWND hwnd;
    int auto_check_interval;
    UINT_PTR update_timer_id;
} NoSleepTray;

#define CB_ERR (-1)

static bool update_check_in_progress;

static int kill_count;
static int set_count;
static UINT last_interval_ms;

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
    return timer_id ? timer_id : 2000;
}

static void tray_setup_update_timer(NoSleepTray* tray);

__APPLY_INTERVAL__
__SETUP_TIMER__
__BEGIN_UPDATE_CHECK__
__END_UPDATE_CHECK__

static int fail(const char* scenario) {
    fprintf(stderr, "FAIL: %s\n", scenario);
    return 1;
}

int main(void) {
    NoSleepTray tray = { (HWND)1, 0, 0 };

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

    tray_apply_auto_check_interval(&tray, 1);
    if (tray.auto_check_interval != 1 || tray.update_timer_id == 0 ||
        set_count != 1 || kill_count != 0 || last_interval_ms != 86400000U) {
        return fail("Never to Daily must create a daily timer");
    }

    tray_apply_auto_check_interval(&tray, 2);
    if (tray.auto_check_interval != 2 || set_count != 2 || kill_count != 1 ||
        last_interval_ms != 604800000U) {
        return fail("Daily to Weekly must replace the daily timer with a weekly timer");
    }

    tray_apply_auto_check_interval(&tray, 0);
    if (tray.auto_check_interval != 0 || tray.update_timer_id != 0 ||
        set_count != 2 || kill_count != 2) {
        return fail("Weekly to Never must cancel the live timer");
    }

    tray_apply_auto_check_interval(&tray, 0);
    if (set_count != 2 || kill_count != 2) {
        return fail("saving an unchanged interval must not reset its timer");
    }

    tray_apply_auto_check_interval(&tray, CB_ERR);
    if (set_count != 2 || kill_count != 2) {
        return fail("an invalid combo selection must leave the timer unchanged");
    }

    puts("PASS: update checks are non-reentrant and interval changes reconfigure the timer");
    return 0;
}
'''

harness = harness.replace("__APPLY_INTERVAL__", apply_interval).replace(
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
