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
notify_groups_header = (root / "src/notify_groups.h").read_text()
event_count_match = re.search(r"\bNOTIFY_EVENT_COUNT\s*=\s*(\d+)", notify_groups_header)
assert event_count_match, "could not find the notification event count"
event_count = int(event_count_match.group(1))


def extract_function(name):
    definition = re.search(
        r"\bstatic\s+(?:bool|unsigned int|LRESULT\s+CALLBACK)\s+"
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


toggle = extract_function("handle_notify_group_bulk_toggle")
event_mask = extract_function("notify_group_event_mask_from_checkboxes")
edit_proc = extract_function("notify_group_edit_proc")
assert re.search(
    r"handle_notify_group_bulk_toggle\s*\(\s*hCheckboxes\s*,\s*LOWORD\(wParam\)\s*\)",
    edit_proc,
), "the editor must route button commands through the tested bulk-toggle handler"
for label, control_id in (
    ("Select All", "IDC_NOTIFY_EVENT_SELECT_ALL"),
    ("Clear All", "IDC_NOTIFY_EVENT_CLEAR_ALL"),
):
    assert re.search(
        r"CreateWindowEx\(\s*0\s*,\s*\"BUTTON\"\s*,\s*\""
        + re.escape(label)
        + r"\"\s*,[\s\S]*?\(HMENU\)"
        + re.escape(control_id),
        edit_proc,
    ), f"the editor must create a {label} button with the tested command ID"
assert re.search(
    r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*NOTIFY_EVENT_COUNT\s*;\s*i\+\+\s*\)\s*\{"
    r"[^}]*hCheckboxes\[i\]\s*=\s*CreateWindowEx\([\s\S]*?"
    r"\(HMENU\)\s*\(\s*IDC_NOTIFY_EVENT_CHECK_START\s*\+\s*i\s*\)",
    edit_proc,
), "the dialog must create one indexed checkbox for every notification event"
save_case = re.search(
    r"\bcase\s+IDC_NOTIFY_GROUP_EDIT_OK\s*:(.*?)\bcase\s+"
    r"IDC_NOTIFY_GROUP_EDIT_CANCEL\s*:",
    edit_proc,
    re.S,
)
assert save_case, "could not find the editor's OK/save command"
assert "notify_group_event_mask_from_checkboxes(hCheckboxes)" in save_case.group(1), (
    "the editor must build its saved event mask through the tested checkbox conversion"
)
assert "notify_groups_add(&updated, name, mask)" in save_case.group(1) and (
    "notify_groups_update(&updated, edit_index, name, mask)" in save_case.group(1)
), "the editor must pass the checked-event mask into custom group add and update"

def macro_value(name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(\d+)\s*$", tray, re.M)
    assert match, f"could not find numeric definition for {name}"
    return int(match.group(1))


select_all_id = macro_value("IDC_NOTIFY_EVENT_SELECT_ALL")
clear_all_id = macro_value("IDC_NOTIFY_EVENT_CLEAR_ALL")

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef void* HWND;
typedef unsigned int UINT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;

#define NOTIFY_EVENT_COUNT __EVENT_COUNT__
#define BM_GETCHECK 0x00F0u
#define BM_SETCHECK 0x00F1u
#define BST_UNCHECKED 0u
#define BST_CHECKED 1u
#define IDC_NOTIFY_EVENT_SELECT_ALL __SELECT_ALL_ID__u
#define IDC_NOTIFY_EVENT_CLEAR_ALL __CLEAR_ALL_ID__u

static unsigned int check_state[NOTIFY_EVENT_COUNT];
static int failures;

static LRESULT SendMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    uintptr_t handle = (uintptr_t)hwnd;
    if (handle == 0 || handle > NOTIFY_EVENT_COUNT) return 0;
    unsigned int index = (unsigned int)(handle - 1);
    if (message == BM_GETCHECK) return check_state[index];
    if (message == BM_SETCHECK && (wParam == BST_CHECKED || wParam == BST_UNCHECKED)) {
        check_state[index] = (unsigned int)wParam;
    }
    return 0;
}

static void expect(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

__TOGGLE_FUNCTION__
__EVENT_MASK_FUNCTION__

int main(void) {
    HWND checkboxes[NOTIFY_EVENT_COUNT];
    for (unsigned int i = 0; i < NOTIFY_EVENT_COUNT; ++i) {
        checkboxes[i] = (HWND)(uintptr_t)(i + 1);
        check_state[i] = (i % 3 == 0) ? BST_CHECKED : BST_UNCHECKED;
    }

    unsigned int seeded_mask = 0;
    for (unsigned int i = 0; i < NOTIFY_EVENT_COUNT; i += 3) seeded_mask |= 1u << i;
    expect(notify_group_event_mask_from_checkboxes(checkboxes) == seeded_mask,
           "the saved event mask preserves the checked state of each event index");

    expect(handle_notify_group_bulk_toggle(checkboxes, IDC_NOTIFY_EVENT_SELECT_ALL),
           "Select All is handled by the bulk-toggle command handler");
    for (unsigned int i = 0; i < NOTIFY_EVENT_COUNT; ++i) {
        expect(check_state[i] == BST_CHECKED, "Select All checks every event");
    }
    expect(notify_group_event_mask_from_checkboxes(checkboxes) == (1u << NOTIFY_EVENT_COUNT) - 1u,
           "Select All produces a mask containing all 11 event bits");

    expect(handle_notify_group_bulk_toggle(checkboxes, IDC_NOTIFY_EVENT_CLEAR_ALL),
           "Clear All is handled by the bulk-toggle command handler");
    for (unsigned int i = 0; i < NOTIFY_EVENT_COUNT; ++i) {
        expect(check_state[i] == BST_UNCHECKED, "Clear All unchecks every event");
    }
    expect(notify_group_event_mask_from_checkboxes(checkboxes) == 0,
           "Clear All produces an empty event mask");

    check_state[2] = BST_CHECKED;
    expect(!handle_notify_group_bulk_toggle(checkboxes, 65535u),
           "unrelated editor commands are left for the normal command handler");
    expect(notify_group_event_mask_from_checkboxes(checkboxes) == (1u << 2),
           "an unrelated command leaves event selections unchanged");

    if (failures != 0) return 1;
    puts("PASS: notification group select-all and clear-all update all event checks and masks");
    return 0;
}
'''

harness = harness.replace("__SELECT_ALL_ID__", str(select_all_id)).replace(
    "__CLEAR_ALL_ID__", str(clear_all_id)
)
harness = harness.replace("__EVENT_COUNT__", str(event_count))
harness = harness.replace("__TOGGLE_FUNCTION__", toggle).replace(
    "__EVENT_MASK_FUNCTION__", event_mask
)

with tempfile.TemporaryDirectory(prefix="nosleep-notify-bulk-toggle-") as temp_dir:
    temp = Path(temp_dir)
    source = temp / "notify_group_bulk_toggle.c"
    executable = temp / "notify_group_bulk_toggle"
    source.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
PY
