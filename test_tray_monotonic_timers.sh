#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()
tray_header = (root / "src/tray.h").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:DWORD\s+WINAPI|ULONGLONG|void|bool)\s+"
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


elapsed_helper = extract_function("get_elapsed_milliseconds")
assert "GetTickCount64()" in elapsed_helper, (
    "elapsed session and action durations must come from the monotonic Windows tick count"
)
assert "GetSystemTime" not in elapsed_helper, (
    "wall-clock time must not be used to measure elapsed duration"
)
assert re.search(r"ULONGLONG\s+start_tick64\s*;", tray_header), (
    "tray sessions must retain their monotonic start tick"
)
assert "SYSTEMTIME start_time" not in tray_header, (
    "tray sessions must not retain wall-clock time as their elapsed-time origin"
)

start = extract_function("tray_start_nosleep")
assert re.search(r"tray->start_tick64\s*=\s*GetTickCount64\(\)", start), (
    "a tray session must capture its monotonic start tick"
)

for name in (
    "tray_stop_nosleep_for_session",
    "tray_duration_timer",
    "delayed_sleep_thread",
    "delayed_shutdown_thread",
    "countdown_thread",
    "tray_update_icon",
):
    body = extract_function(name)
    assert "get_elapsed_milliseconds" in body, (
        f"{name} must calculate elapsed time from monotonic ticks"
    )
    assert "GetSystemTime" not in body and "SystemTimeToFileTime" not in body, (
        f"{name} must not use wall-clock subtraction for elapsed time"
    )

print("PASS: tray session and delayed-action durations use monotonic ticks")
PY
