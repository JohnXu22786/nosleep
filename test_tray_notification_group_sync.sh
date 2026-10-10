#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR/src/tray.c" "$SCRIPT_DIR/src/tray.h" <<'PY'
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
header = Path(sys.argv[2]).read_text()


def extract_function(signature):
    definition = re.search(signature + r"\s*\{", source)
    assert definition, f"could not find definition for {signature}"
    start = definition.start()
    opening = definition.end() - 1
    depth = 0
    state = "code"
    i = opening
    while i < len(source):
        char = source[i]
        next_two = source[i : i + 2]
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
                    return source[start : i + 1]
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


assert re.search(r"SRWLOCK\s+notify_groups_lock\s*;", header), (
    "the tray needs a dedicated lock for notification group state"
)
create = extract_function(r"NoSleepTray\s*\*\s*tray_create\s*\(\s*void\s*\)")
assert "InitializeSRWLock(&tray->notify_groups_lock)" in create, (
    "the notification group lock must be initialized with the other tray locks"
)

notification = extract_function(
    r"void\s+tray_show_notification\s*\([^;]*?\)"
)
snapshot = re.search(
    r"NotifyGroupManager\s+(\w+);\s*"
    r"AcquireSRWLockShared\(&tray->notify_groups_lock\);\s*"
    r"\1\s*=\s*tray->notify_groups;\s*"
    r"ReleaseSRWLockShared\(&tray->notify_groups_lock\);",
    notification,
)
assert snapshot, "notification filtering must copy the groups under a shared lock"
assert re.search(
    r"notify_groups_should_show\(&" + re.escape(snapshot.group(1)) + r",\s*event_type\)",
    notification,
), "notification filtering must use the synchronized group snapshot"
assert "notify_groups_should_show(&tray->notify_groups, event_type)" not in notification, (
    "notification filtering must not read the shared manager after releasing its lock"
)

replace = extract_function(
    r"static\s+void\s+replace_notification_groups\s*\([^;]*?\)"
)
assert re.search(
    r"AcquireSRWLockExclusive\(lock\);\s*"
    r"\*target\s*=\s*\*updated;\s*"
    r"ReleaseSRWLockExclusive\(lock\);",
    replace,
), "runtime group replacements must hold the same lock exclusively"

runtime_writes = re.findall(r"settings_tray->notify_groups\s*=", source)
assert not runtime_writes, "settings must replace the manager through the synchronized helper"
assert len(re.findall(
    r"replace_notification_groups\(\s*&settings_tray->notify_groups_lock\s*,\s*"
    r"&settings_tray->notify_groups\s*,\s*&updated\s*\)", source
)) == 3, "restore, activation, and deletion must use the synchronized replacement"
assert "replace_notification_groups(g_notify_edit_ctx.lock, edit_mgr, &updated)" in source, (
    "editor saves must replace groups through the synchronized helper"
)
assert len(re.findall(
    r"g_notify_edit_ctx\.lock\s*=\s*&settings_tray->notify_groups_lock;\s*"
    r"show_notify_group_edit_dialog\(", source
)) == 2, (
    "group editor entry points must supply the notification group lock"
)

print("PASS: notification filtering snapshots group state under the replacement lock")
PY
