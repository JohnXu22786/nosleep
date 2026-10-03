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
assert re.search(r"tray_start_countdown\(tray,\s*SESSION_FINISHED_SHUTDOWN\)", shutdown_thread), (
    "the delayed shutdown thread must identify its scheduled action"
)

update = extract_function("tray_update_icon")
countdown_thread = extract_function("countdown_thread")
countdown_display = update.split("// Handle delayed sleep countdown display", 1)[1].split(
    "\n    if (is_running)", 1
)[0]
assert re.search(
    r"tray_format_countdown_tooltip\(tip,\s*sizeof\(tip\),\s*"
    r"tray->countdown_action\s*==\s*SESSION_FINISHED_SHUTDOWN,\s*"
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
        "nosleep - System will sleep in 1 seconds");
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
PY
