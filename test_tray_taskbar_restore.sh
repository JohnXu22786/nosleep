#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

python3 - "$SCRIPT_DIR/src/tray.c" "$TMPDIR_PATH/test.c" <<'PYCODE'
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", source)
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


tray_init = extract_function("bool tray_init(NoSleepTray* tray)")
window_proc = extract_function("LRESULT CALLBACK tray_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)")
update_icon = extract_function("void tray_update_icon(NoSleepTray* tray)")
show_notification = extract_function(
    "void tray_show_notification(NoSleepTray* tray, NotifyEventId event_type,\n"
    "                            const char* title, const char* message, bool critical)"
)
assert re.search(
    r'tray->uTaskbarCreatedMessage\s*=\s*RegisterWindowMessage\("TaskbarCreated"\)',
    tray_init,
), "tray_init must register the shell TaskbarCreated message"
assert "tray_add_icon_to_shell(tray)" in tray_init, (
    "initialization and Explorer recovery must use the same tray-icon add path"
)
assert re.search(
    r"tray_handle_taskbar_created\s*\(\s*tray\s*,\s*msg\s*\)", window_proc
), "tray_window_proc must dispatch the registered shell restart message"
assert update_icon.count("AcquireSRWLockExclusive(&tray->tray_icon_lock)") == 1
assert update_icon.count("ReleaseSRWLockExclusive(&tray->tray_icon_lock)") == 2, (
    "tray_update_icon must release the icon lock on both countdown and normal paths"
)
assert "AcquireSRWLockExclusive(&tray->tray_icon_lock)" in show_notification
assert "ReleaseSRWLockExclusive(&tray->tray_icon_lock)" in show_notification

prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef int BOOL;
typedef int SRWLOCK;
typedef void *HWND;
typedef void *HICON;
typedef struct {
    UINT cbSize;
    HWND hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    HICON hIcon;
    char szTip[128];
    UINT uVersion;
} NOTIFYICONDATA;
typedef struct {
    SRWLOCK tray_icon_lock;
    UINT uTaskbarCreatedMessage;
    NOTIFYICONDATA nid;
} NoSleepTray;

#define NIM_ADD 1u
#define NIM_SETVERSION 4u
#define NIF_ICON 1u
#define NIF_MESSAGE 2u
#define NIF_TIP 4u
#define DEBUG_PRINT(...) ((void)0)

typedef struct {
    DWORD operation;
    NOTIFYICONDATA *data;
    UINT flags;
    HICON icon;
    UINT callback_message;
    UINT version;
} ShellCall;

static ShellCall shell_calls[2];
static unsigned int shell_call_count;
static unsigned int lock_depth;
static NoSleepTray *active_tray;
static BOOL fail_add;
static BOOL fail_set_version;

static DWORD GetLastError(void) { return 5; }
static void AcquireSRWLockExclusive(SRWLOCK *lock) {
    assert(active_tray && lock == &active_tray->tray_icon_lock && lock_depth == 0);
    lock_depth = 1;
}
static void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    assert(active_tray && lock == &active_tray->tray_icon_lock && lock_depth == 1);
    lock_depth = 0;
}
static BOOL Shell_NotifyIcon(DWORD operation, NOTIFYICONDATA *data) {
    assert(lock_depth == 1);
    assert(shell_call_count < 2);
    shell_calls[shell_call_count++] = (ShellCall){
        operation, data, data->uFlags, data->hIcon, data->uCallbackMessage, data->uVersion
    };
    if (operation == NIM_ADD) return !fail_add;
    if (operation == NIM_SETVERSION) return !fail_set_version;
    return 1;
}
"""

tail = r"""
int main(void) {
    NoSleepTray tray = {0};
    active_tray = &tray;
    tray.uTaskbarCreatedMessage = 0xc123;
    tray.nid.uFlags = NIF_ICON | NIF_TIP;
    tray.nid.hIcon = (HICON)1;
    tray.nid.uCallbackMessage = 0xc456;
    strcpy(tray.nid.szTip, "nosleep current icon");

    assert(!tray_handle_taskbar_created(NULL, tray.uTaskbarCreatedMessage));
    assert(!tray_handle_taskbar_created(&tray, tray.uTaskbarCreatedMessage + 1));
    assert(shell_call_count == 0);

    tray.uTaskbarCreatedMessage = 0;
    assert(!tray_handle_taskbar_created(&tray, 0));
    assert(shell_call_count == 0);
    tray.uTaskbarCreatedMessage = 0xc123;

    assert(tray_handle_taskbar_created(&tray, tray.uTaskbarCreatedMessage));
    assert(shell_call_count == 2);
    assert(shell_calls[0].operation == NIM_ADD);
    assert(shell_calls[0].data == &tray.nid);
    assert(shell_calls[0].flags == (NIF_ICON | NIF_MESSAGE | NIF_TIP));
    assert(shell_calls[0].icon == tray.nid.hIcon);
    assert(shell_calls[0].callback_message == tray.nid.uCallbackMessage);
    assert(shell_calls[1].operation == NIM_SETVERSION);
    assert(shell_calls[1].data == &tray.nid);
    assert(shell_calls[1].version == 3);
    assert(lock_depth == 0);

    shell_call_count = 0;
    fail_add = 1;
    assert(tray_handle_taskbar_created(&tray, tray.uTaskbarCreatedMessage));
    assert(shell_call_count == 1 && shell_calls[0].operation == NIM_ADD);
    assert(lock_depth == 0);
    puts("PASS: TaskbarCreated restores the current tray icon and callback version");
    return 0;
}
"""

output.write_text(
    prelude
    + extract_function("static bool tray_add_icon_to_shell(NoSleepTray* tray)")
    + extract_function("static bool tray_handle_taskbar_created(NoSleepTray* tray, UINT msg)")
    + tail
)
PYCODE

"${CC:-cc}" -std=c99 -Wall -Wextra "$TMPDIR_PATH/test.c" -o "$TMPDIR_PATH/test"
"$TMPDIR_PATH/test"
