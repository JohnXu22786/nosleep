#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\bstatic\s+LRESULT\s+CALLBACK\s+"
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


notify_proc = extract_function("notify_tab_subclass_proc")
assert re.search(
    r"SetWindowSubclass\(\s*hNotifyTab\s*,\s*notify_tab_subclass_proc\s*,",
    tray,
), "the notifications page must install the command-forwarding subclass"

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef void* HWND;
typedef unsigned int UINT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef uintptr_t UINT_PTR;
typedef uintptr_t DWORD_PTR;

#define CALLBACK
#define WM_COMMAND 0x0111u
#define WM_NOTIFY 0x004eu

static HWND settings_dialog = (HWND)(uintptr_t)1;
static HWND notifications_page = (HWND)(uintptr_t)2;
static HWND button = (HWND)(uintptr_t)3;
static HWND last_target;
static UINT last_message;
static WPARAM last_wparam;
static LPARAM last_lparam;
static int send_count;
static int default_count;
static int failures;

static void expect(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static HWND GetParent(HWND hwnd) {
    expect(hwnd == notifications_page, "the notification page receives the button command");
    return settings_dialog;
}

static LRESULT SendMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    last_target = hwnd;
    last_message = message;
    last_wparam = wParam;
    last_lparam = lParam;
    ++send_count;
    return 37;
}

static LRESULT DefSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)hwnd; (void)message; (void)wParam; (void)lParam;
    ++default_count;
    return 19;
}

'''

main = r'''
int main(void) {
    const WPARAM command_ids[] = {2202, 2204, 2203, 2205};
    for (unsigned int i = 0; i < sizeof(command_ids) / sizeof(command_ids[0]); ++i) {
        int sends_before = send_count;
        LRESULT result = notify_tab_subclass_proc(
            notifications_page, WM_COMMAND, command_ids[i], (LPARAM)button, 0, 0);
        expect(send_count == sends_before + 1,
               "each notification button command is forwarded exactly once");
        expect(last_target == settings_dialog,
               "button commands reach the settings dialog that handles them");
        expect(last_message == WM_COMMAND && last_wparam == command_ids[i] &&
               last_lparam == (LPARAM)button,
               "forwarding preserves the command ID and originating control handle");
        expect(result == 37, "the subclass returns the settings handler result");
    }

    LRESULT result = notify_tab_subclass_proc(
        notifications_page, WM_NOTIFY, 12, (LPARAM)button, 0, 0);
    expect(default_count == 1 && result == 19,
           "non-command messages continue through the original window procedure");
    expect(send_count == 4, "only WM_COMMAND messages are sent to the settings dialog");
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nosleep-notify-routing-") as temp_dir:
    temp = Path(temp_dir)
    harness_path = temp / "notify_command_routing.c"
    executable_path = temp / "notify_command_routing"
    harness_path.write_text(harness + notify_proc + main)
    subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness_path), "-o", str(executable_path)],
        check=True,
    )
    subprocess.run([str(executable_path)], check=True)

print("PASS: notification button commands reach the settings dialog")
PY
