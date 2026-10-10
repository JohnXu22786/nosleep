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
restore_taskbar_icon = extract_function("static bool tray_restore_taskbar_icon(NoSleepTray* tray)")
handle_taskbar_retry = extract_function("static bool tray_handle_taskbar_restore_retry(NoSleepTray* tray, WPARAM timer_id)")
show_notification = extract_function(
    "void tray_show_notification(NoSleepTray* tray, NotifyEventId event_type,\n"
    "                            const char* title, const char* message, bool critical)"
)
assert re.search(
    r'tray->uTaskbarCreatedMessage\s*=\s*RegisterWindowMessage\("TaskbarCreated"\)',
    tray_init,
), "tray_init must register the shell TaskbarCreated message"
assert re.search(
    r"bool\s+tray_icon_added\s*=\s*tray_restore_taskbar_icon\(tray\)",
    tray_init,
), "tray_init must route initial icon registration through the retry helper"
assert re.search(
    r"if\s*\(\s*!tray_icon_added\s*&&\s*!tray->taskbar_restore_timer_id\s*\)",
    tray_init,
), "tray_init must continue when a failed initial add has a retry timer"
assert re.search(
    r"tray_handle_taskbar_created\s*\(\s*tray\s*,\s*msg\s*\)", window_proc
), "tray_window_proc must dispatch the registered shell restart message"
assert re.search(
    r"tray_handle_taskbar_restore_retry\s*\(\s*tray\s*,\s*wParam\s*\)", window_proc
), "tray_window_proc must dispatch tray-icon retry timer messages"
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
typedef unsigned long long UINT_PTR;
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
    HWND hwnd;
    UINT_PTR taskbar_restore_timer_id;
    SRWLOCK tray_icon_lock;
    UINT uTaskbarCreatedMessage;
    NOTIFYICONDATA nid;
} NoSleepTray;

#define NIM_ADD 1u
#define NIM_SETVERSION 4u
#define NIF_ICON 1u
#define NIF_MESSAGE 2u
#define NIF_TIP 4u
#define TIMER_ID_TASKBAR_RESTORE 1003u
#define TASKBAR_RESTORE_RETRY_INTERVAL_MS 1000u
#define DEBUG_PRINT(...) ((void)0)

typedef struct {
    DWORD operation;
    NOTIFYICONDATA *data;
    UINT flags;
    HICON icon;
    UINT callback_message;
    UINT version;
} ShellCall;

static ShellCall shell_calls[16];
static unsigned int shell_call_count;
static BOOL add_results[8];
static unsigned int add_result_count;
static unsigned int add_attempt_count;
static unsigned int lock_depth;
static NoSleepTray *active_tray;
static BOOL fail_set_version;
static unsigned int set_timer_calls;
static unsigned int kill_timer_calls;
static BOOL fail_set_timer;
static BOOL fail_first_set_timer;

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
    assert(shell_call_count < 16);
    shell_calls[shell_call_count++] = (ShellCall){
        operation, data, data->uFlags, data->hIcon, data->uCallbackMessage, data->uVersion
    };
    if (operation == NIM_ADD) {
        unsigned int attempt = add_attempt_count++;
        return attempt < add_result_count ? add_results[attempt] : 1;
    }
    if (operation == NIM_SETVERSION) return !fail_set_version;
    return 1;
}
static UINT_PTR SetTimer(HWND hwnd, UINT_PTR timer_id, UINT interval, void *callback) {
    assert(active_tray && hwnd == active_tray->hwnd);
    assert(timer_id == TIMER_ID_TASKBAR_RESTORE);
    assert(interval == 1000u);
    assert(callback == NULL);
    ++set_timer_calls;
    return (fail_set_timer || (fail_first_set_timer && set_timer_calls == 1))
        ? 0 : timer_id;
}
static BOOL KillTimer(HWND hwnd, UINT_PTR timer_id) {
    assert(active_tray && hwnd == active_tray->hwnd);
    assert(timer_id == TIMER_ID_TASKBAR_RESTORE);
    ++kill_timer_calls;
    return 1;
}
"""

tail = r"""
int main(void) {
    NoSleepTray tray = {0};
    active_tray = &tray;
    tray.hwnd = (HWND)1;
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
    add_attempt_count = 0;
    add_result_count = 3;
    add_results[0] = 0;
    add_results[1] = 0;
    add_results[2] = 1;
    set_timer_calls = 0;
    kill_timer_calls = 0;
    assert(!tray_restore_taskbar_icon(&tray));
    assert(shell_call_count == 1 && shell_calls[0].operation == NIM_ADD);
    assert(lock_depth == 0);
    assert(set_timer_calls == 1);
    assert(tray.taskbar_restore_timer_id == TIMER_ID_TASKBAR_RESTORE);
    assert(!tray_handle_taskbar_restore_retry(&tray, TIMER_ID_TASKBAR_RESTORE + 1));
    assert(shell_call_count == 1);
    assert(tray_handle_taskbar_restore_retry(&tray, TIMER_ID_TASKBAR_RESTORE));
    assert(shell_call_count == 2 && shell_calls[1].operation == NIM_ADD);
    assert(kill_timer_calls == 0);
    assert(tray_handle_taskbar_restore_retry(&tray, TIMER_ID_TASKBAR_RESTORE));
    assert(shell_call_count == 4);
    assert(shell_calls[2].operation == NIM_ADD);
    assert(shell_calls[3].operation == NIM_SETVERSION);
    assert(kill_timer_calls == 1);
    assert(tray.taskbar_restore_timer_id == 0);
    assert(!tray_handle_taskbar_restore_retry(&tray, TIMER_ID_TASKBAR_RESTORE));

    shell_call_count = 0;
    add_attempt_count = 0;
    add_result_count = 2;
    add_results[0] = 0;
    add_results[1] = 1;
    set_timer_calls = 0;
    kill_timer_calls = 0;
    fail_set_timer = 0;
    assert(tray_handle_taskbar_created(&tray, tray.uTaskbarCreatedMessage));
    assert(shell_call_count == 1 && shell_calls[0].operation == NIM_ADD);
    assert(set_timer_calls == 1);
    assert(tray.taskbar_restore_timer_id == TIMER_ID_TASKBAR_RESTORE);
    assert(tray_handle_taskbar_restore_retry(&tray, TIMER_ID_TASKBAR_RESTORE));
    assert(shell_calls[1].operation == NIM_ADD);
    assert(shell_calls[2].operation == NIM_SETVERSION);
    assert(kill_timer_calls == 1);
    assert(tray.taskbar_restore_timer_id == 0);

    shell_call_count = 0;
    add_attempt_count = 0;
    add_result_count = 2;
    add_results[0] = 0;
    add_results[1] = 1;
    set_timer_calls = 0;
    kill_timer_calls = 0;
    fail_set_timer = 1;
    assert(tray_handle_taskbar_created(&tray, tray.uTaskbarCreatedMessage));
    assert(shell_call_count == 3);
    assert(shell_calls[0].operation == NIM_ADD);
    assert(shell_calls[1].operation == NIM_ADD);
    assert(shell_calls[2].operation == NIM_SETVERSION);
    assert(set_timer_calls == 1);
    assert(kill_timer_calls == 0);
    assert(tray.taskbar_restore_timer_id == 0);

    shell_call_count = 0;
    add_attempt_count = 0;
    add_result_count = 3;
    add_results[0] = 0;
    add_results[1] = 0;
    add_results[2] = 1;
    set_timer_calls = 0;
    kill_timer_calls = 0;
    fail_set_timer = 0;
    fail_first_set_timer = 1;
    assert(tray_handle_taskbar_created(&tray, tray.uTaskbarCreatedMessage));
    assert(shell_call_count == 2);
    assert(shell_calls[0].operation == NIM_ADD);
    assert(shell_calls[1].operation == NIM_ADD);
    assert(set_timer_calls == 2);
    assert(tray.taskbar_restore_timer_id == TIMER_ID_TASKBAR_RESTORE);
    assert(tray_handle_taskbar_restore_retry(&tray, TIMER_ID_TASKBAR_RESTORE));
    assert(shell_call_count == 4);
    assert(shell_calls[2].operation == NIM_ADD);
    assert(shell_calls[3].operation == NIM_SETVERSION);
    assert(kill_timer_calls == 1);
    assert(tray.taskbar_restore_timer_id == 0);

    puts("PASS: TaskbarCreated retries failed icon adds until the shell accepts the icon");
    return 0;
}
"""

output.write_text(
    prelude
    + extract_function("static bool tray_add_icon_to_shell(NoSleepTray* tray)")
    + restore_taskbar_icon
    + handle_taskbar_retry
    + extract_function("static bool tray_handle_taskbar_created(NoSleepTray* tray, UINT msg)")
    + tail
)
PYCODE

"${CC:-cc}" -std=c99 -Wall -Wextra "$TMPDIR_PATH/test.c" -o "$TMPDIR_PATH/test"
"$TMPDIR_PATH/test"
