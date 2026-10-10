#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:void|bool|DWORD\s+WINAPI|ULONGLONG|LRESULT\s+CALLBACK)\s+"
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


menu = extract_function("tray_create_menu")
update = extract_function("tray_update_session_finished_menu")
command_handler = extract_function("tray_window_proc")
duration_timer = extract_function("tray_duration_timer")
stop_menu_update = extract_function("tray_update_stop_menu_item_internal")

finished_item = re.search(
    r"AppendMenu\(tray->hmenu,\s*MF_STRING\s*\|\s*MF_POPUP,\s*"
    r"\(UINT_PTR\)hSessionFinishedMenu,\s*finished_text\)",
    menu,
)
assert finished_item, "tray_create_menu must add the When finished submenu to the main menu"
first_root_item = re.search(r"AppendMenu\(tray->hmenu,", menu)
assert first_root_item and first_root_item.start() == finished_item.start(), (
    "When finished must remain at main-menu position 0"
)

assert re.search(r"GetSubMenu\(tray->hmenu,\s*0\)", update), (
    "session-finished checkmarks must use the submenu at main-menu position 0"
)
assert len(re.findall(r"CheckMenuItem\(hSubMenu,\s*IDM_SESSION_FINISHED_", update)) == 4, (
    "the update must refresh all four session-finished checkmarks"
)
assert re.search(r"SetMenuItemInfo\(tray->hmenu,\s*0,\s*TRUE,\s*&mii\)", update), (
    "the selected action caption must update the When finished item at position 0"
)

for command, action in (
    ("IDM_SESSION_FINISHED_NONE", "SESSION_FINISHED_NONE"),
    ("IDM_SESSION_FINISHED_SHUTDOWN", "SESSION_FINISHED_SHUTDOWN"),
    ("IDM_SESSION_FINISHED_SLEEP", "SESSION_FINISHED_SLEEP"),
):
    case = re.search(
        rf"case {command}:(?P<body>.*?)(?=^\s*break;)",
        command_handler,
        re.S | re.M,
    )
    assert case, f"could not find command handler for {command}"
    assignment = re.search(
        rf"tray->session_finished_action\s*=\s*{action}\s*;", case["body"]
    )
    save = re.search(r"tray_save_settings_with_warning\(hwnd,\s*tray\)\s*;", case["body"])
    assert assignment, f"{command} must select {action}"
    assert save and assignment.start() < save.start(), (
        f"{command} must persist the selected session-finished action"
    )

snapshot = re.search(
    r"SessionFinishedAction\s+finished_action\s*=\s*"
    r"tray->session_finished_action\s*;",
    duration_timer,
)
assert snapshot, "the duration timer must snapshot the selected session-finished action"
assert len(re.findall(r"tray->session_finished_action", duration_timer)) == 1, (
    "the duration timer must not read the selected action outside its snapshot"
)
snapshot_lock = duration_timer.rfind(
    "AcquireSRWLockExclusive(&tray->delayed_action_lock)", 0, snapshot.start()
)
snapshot_unlock = duration_timer.find(
    "ReleaseSRWLockExclusive(&tray->delayed_action_lock)", snapshot_lock
)
assert snapshot_lock >= 0 and snapshot_unlock > snapshot.end(), (
    "the duration timer must snapshot the selected action under delayed_action_lock"
)

menu_snapshot = re.search(
    r"SessionFinishedAction\s+session_action\s*=\s*"
    r"tray->session_finished_action\s*;",
    stop_menu_update,
)
assert menu_snapshot, "the stop-menu updater must snapshot the selected action"
assert len(re.findall(r"tray->session_finished_action", stop_menu_update)) == 1, (
    "the stop-menu updater must not read the selected action outside its snapshot"
)
menu_lock = stop_menu_update.rfind(
    "if (!lock_held) AcquireSRWLockExclusive(&tray->delayed_action_lock)",
    0,
    menu_snapshot.start(),
)
menu_unlock = stop_menu_update.find(
    "if (!lock_held) ReleaseSRWLockExclusive(&tray->delayed_action_lock)",
    menu_lock,
)
assert menu_lock >= 0 and menu_unlock > menu_snapshot.end(), (
    "the stop-menu updater must capture the selected action inside its lock section"
)

for command in (
    "IDM_TOGGLE_SLEEP_AFTER_TIMEOUT",
    "IDM_SESSION_FINISHED_NONE",
    "IDM_SESSION_FINISHED_SHUTDOWN",
    "IDM_SESSION_FINISHED_SHUTDOWN_GRACEFUL",
    "IDM_SESSION_FINISHED_SLEEP",
):
    case = re.search(
        rf"case {command}:(?P<body>.*?)(?=^\s*break;)",
        command_handler,
        re.S | re.M,
    )
    assert case, f"could not find command handler for {command}"
    action_accesses = list(re.finditer(r"tray->session_finished_action", case["body"]))
    assert action_accesses, f"{command} must access the session-finished action"
    action_lock = case["body"].rfind(
        "AcquireSRWLockExclusive(&tray->delayed_action_lock)", 0, action_accesses[0].start()
    )
    action_unlock = case["body"].find(
        "ReleaseSRWLockExclusive(&tray->delayed_action_lock)", action_lock
    )
    assert action_lock >= 0 and action_unlock > action_accesses[-1].end(), (
        f"{command} must read or write the selected action under delayed_action_lock"
    )

print("PASS: When finished menu updates and synchronizes session-finished action access")
PY
