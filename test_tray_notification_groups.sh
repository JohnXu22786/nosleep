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
header = (root / "src/tray.h").read_text()


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


notification = extract_function("tray_show_notification")
assert re.search(
    r"tray_show_notification\s*\(\s*NoSleepTray\s*\*\s*tray\s*,\s*"
    r"NotifyEventId\s+event_type",
    notification,
), "direct notification dispatch must require an explicit event type"
group_gate = notification.find("notify_groups_should_show(&tray->notify_groups, event_type)")
display = notification.find("Shell_NotifyIcon(NIM_MODIFY")
assert group_gate >= 0 and group_gate < display, (
    "direct dispatch must apply the active event group before displaying a notification"
)

assert re.search(
    r"tray_show_notification\s*\(\s*tray\s*,\s*NOTIFY_EVENT_SESSION_START\s*,\s*\"Starting\"",
    extract_function("tray_start_nosleep"),
), "session start notifications must use the session-start group setting"

initialization = extract_function("tray_init")
assert initialization.index("notify_groups_init(&tray->notify_groups, tray->notification_mode)") < initialization.index(
    "tray_show_notification(tray, NOTIFY_EVENT_APP_START"
), "the application-start notification must wait until notification groups are loaded"

typed_calls = re.findall(
    r"tray_show_notification\s*\(\s*tray\s*,\s*(NOTIFY_EVENT_[A-Z_]+)", tray
)
all_calls = re.findall(r"\btray_show_notification\s*\(", tray)
assert len(typed_calls) == len(all_calls) - 1, (
    "every direct notification call must name its group event"
)
assert "tray_show_notification_event" not in tray and "tray_show_notification_event" not in header, (
    "event filtering must be enforced by the direct notification API, not an optional wrapper"
)

windows_stub = r"""
#ifndef TEST_NOTIFICATION_WINDOWS_H
#define TEST_NOTIFICATION_WINDOWS_H
#include <stddef.h>
typedef unsigned int DWORD;
typedef long LONG;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef void *HKEY;
typedef int BOOL;
typedef void *HWND;
typedef struct {
    DWORD uFlags;
    char szInfoTitle[256];
    char szInfo[256];
    DWORD dwInfoFlags;
    DWORD uTimeout;
} NOTIFYICONDATA;

#define TRUE 1
#define FALSE 0
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define REG_OPTION_NON_VOLATILE 0
#define KEY_WRITE 0
#define KEY_READ 0
#define REG_SZ 1
#define REG_DWORD 4
#define HKEY_CURRENT_USER ((HKEY)1)

LONG RegCreateKeyEx(HKEY, const char *, DWORD, const char *, DWORD, DWORD,
                    void *, HKEY *, DWORD *);
LONG RegOpenKeyEx(HKEY, const char *, DWORD, DWORD, HKEY *);
LONG RegSetValueEx(HKEY, const char *, DWORD, DWORD, const BYTE *, DWORD);
LONG RegQueryValueEx(HKEY, const char *, DWORD *, DWORD *, BYTE *, DWORD *);
LONG RegDeleteKey(HKEY, const char *);
LONG RegCloseKey(HKEY);
LONG RegRenameKey(HKEY, const wchar_t *, const wchar_t *);
#endif
"""

behavior_harness = r"""
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "notify_groups.h"

#define NOTIFY_ALL 0
#define NOTIFY_CRITICAL_ONLY 1
#define NOTIFY_NONE 2
#define NIF_MESSAGE 0x01u
#define NIF_ICON 0x02u
#define NIF_TIP 0x04u
#define NIF_INFO 0x10u
#define NIIF_INFO 0x01u
#define NIM_MODIFY 1u
#define DEBUG_LOG(...) ((void)0)

typedef struct {
    HWND hwnd;
    NOTIFYICONDATA nid;
    int notification_mode;
    NotifyGroupManager notify_groups;
} NoSleepTray;

static unsigned int shell_notify_calls;

LONG RegCreateKeyEx(HKEY root, const char *path, DWORD reserved,
                    const char *class_name, DWORD options, DWORD access,
                    void *security, HKEY *key, DWORD *disposition) {
    (void)root; (void)path; (void)reserved; (void)class_name; (void)options;
    (void)access; (void)security; (void)disposition;
    *key = (HKEY)1;
    return ERROR_SUCCESS;
}
LONG RegOpenKeyEx(HKEY root, const char *path, DWORD reserved, DWORD access, HKEY *key) {
    (void)root; (void)path; (void)reserved; (void)access; (void)key;
    return ERROR_FILE_NOT_FOUND;
}
LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size) {
    (void)key; (void)name; (void)reserved; (void)type; (void)value; (void)size;
    return ERROR_SUCCESS;
}
LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved, DWORD *type,
                     BYTE *value, DWORD *size) {
    (void)key; (void)name; (void)reserved; (void)type; (void)value; (void)size;
    return ERROR_FILE_NOT_FOUND;
}
LONG RegDeleteKey(HKEY root, const char *path) {
    (void)root; (void)path;
    return ERROR_SUCCESS;
}
LONG RegCloseKey(HKEY key) {
    (void)key;
    return ERROR_SUCCESS;
}
LONG RegRenameKey(HKEY key, const wchar_t *subkey_name, const wchar_t *new_name) {
    (void)key; (void)subkey_name; (void)new_name;
    return ERROR_SUCCESS;
}
BOOL Shell_NotifyIcon(DWORD operation, NOTIFYICONDATA *data) {
    (void)operation; (void)data;
    ++shell_notify_calls;
    return TRUE;
}

static int failures;
static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

"""

behavior_main = r"""
int main(void) {
    NoSleepTray tray = {0};
    tray.hwnd = (HWND)1;
    tray.notification_mode = NOTIFY_ALL;
    tray.notify_groups.count = 1;
    tray.notify_groups.active_index = 0;

    // Regression: the active None group suppresses session-start notifications
    // even though the legacy mode still says to show all notifications.
    tray.notify_groups.groups[0].event_mask = 0;
    tray_show_notification(&tray, NOTIFY_EVENT_SESSION_START, "Starting", "test", false);
    expect(shell_notify_calls == 0,
           "None group must suppress a direct session-start notification with legacy all mode");

    tray.notify_groups.groups[0].event_mask = 1u << NOTIFY_EVENT_SESSION_START;
    tray_show_notification(&tray, NOTIFY_EVENT_SESSION_START, "Starting", "test", false);
    expect(shell_notify_calls == 1,
           "an enabled event must reach the tray when legacy mode is all");

    tray.notify_groups.groups[0].event_mask = 0;
    tray_show_notification(&tray, NOTIFY_EVENT_SESSION_START, "Starting", "test", false);
    expect(shell_notify_calls == 1,
           "a disabled event in a custom group must remain suppressed");

    tray.notify_groups.groups[0].event_mask = 1u << NOTIFY_EVENT_UPDATE_AVAILABLE;
    tray.notification_mode = NOTIFY_CRITICAL_ONLY;
    tray_show_notification(&tray, NOTIFY_EVENT_UPDATE_AVAILABLE, "Update Available", "test", false);
    expect(shell_notify_calls == 2,
           "critical-only legacy mode must not suppress an update enabled by the active group");

    tray.notification_mode = NOTIFY_NONE;
    tray_show_notification(&tray, NOTIFY_EVENT_UPDATE_AVAILABLE, "Update Available", "test", false);
    expect(shell_notify_calls == 3,
           "none legacy mode must not suppress an update enabled by the active group");

    tray.notification_mode = NOTIFY_ALL;
    tray.notify_groups.groups[0].event_mask = 1u << NOTIFY_EVENT_SESSION_START;
    tray_show_notification(&tray, NOTIFY_EVENT_SESSION_START, "Starting", "test", true);
    expect(shell_notify_calls == 4,
           "enabled critical notifications must reach the tray when legacy mode is all");

    if (failures != 0) return 1;
    puts("PASS: direct notifications honor active event groups");
    return 0;
}
"""

with tempfile.TemporaryDirectory() as temp_dir:
    temp = Path(temp_dir)
    (temp / "windows.h").write_text(windows_stub)
    source = temp / "test_tray_notification_groups.c"
    source.write_text(behavior_harness + "\n" + notification + "\n" + behavior_main)
    binary = temp / "test_tray_notification_groups"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", f"-I{temp}", f"-I{root / 'src'}",
        str(source), str(root / "src/notify_groups.c"), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

print("PASS: direct notifications use their active event-group setting")
PY
