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
tray_source = Path(os.environ.get("TRAY_SOURCE", root / "src/tray.c"))
tray = tray_source.read_text()


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", tray)
    assert definition, f"could not find definition for {signature}"
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
    raise AssertionError(f"unterminated function: {signature}")


window_proc = extract_function("LRESULT CALLBACK tray_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)")
destroy_case = re.search(
    r"case WM_DESTROY:\s*(.*?)\s*break;\s*case WM_POWERBROADCAST:",
    window_proc,
    re.S,
)
assert destroy_case, "could not find WM_DESTROY cleanup"

prelude = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef void *HANDLE;
typedef void *HWND;
typedef void *HMENU;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef struct { int unused; } SRWLOCK;
typedef struct { HWND hWnd; } NOTIFYICONDATA;
typedef struct NoSleepTray {
    HWND hwnd;
    HMENU hmenu;
    NOTIFYICONDATA nid;
    SRWLOCK delayed_action_lock;
    SRWLOCK tray_icon_lock;
    bool starting_nosleep;
    HANDLE stop_event;
    HANDLE sleep_timer;
    HANDLE shutdown_timer;
    HANDLE sleep_stop_event;
    HANDLE shutdown_stop_event;
    HANDLE countdown_stop_event;
} NoSleepTray;

#define DEBUG_LOG(...) ((void)0)
#define NIM_DELETE 2u
#define WM_DESTROY 0x0002u
#define WM_POWERBROADCAST 0x0218u

static NoSleepTray *active_tray;
static int destroy_window_calls;
static int shell_delete_calls;
static int post_quit_calls;
static int destroy_icons_calls;
static int destroy_menu_calls;
static int expected_window;

static void AcquireSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
static void ReleaseSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
static void tray_stop_nosleep(NoSleepTray *tray, bool expired, bool suppress) {
    (void)tray; (void)expired; (void)suppress;
}
static void tray_stop_countdown(NoSleepTray *tray) { (void)tray; }
static bool tray_wait_for_worker_threads(NoSleepTray *tray) { (void)tray; return true; }
static bool tray_wait_for_update_check(NoSleepTray *tray) { (void)tray; return true; }
static int CloseHandle(HANDLE handle) { (void)handle; return 1; }
static void tray_destroy_icons(NoSleepTray *tray) { (void)tray; ++destroy_icons_calls; }
static int DestroyMenu(HMENU menu) { (void)menu; ++destroy_menu_calls; return 1; }
static int Shell_NotifyIcon(DWORD message, NOTIFYICONDATA *nid) {
    (void)nid;
    if (message == NIM_DELETE) ++shell_delete_calls;
    return 1;
}
static void PostQuitMessage(int exit_code) { (void)exit_code; ++post_quit_calls; }

static void dispatch_wm_destroy(NoSleepTray *tray) {
    switch (WM_DESTROY) {
    case WM_DESTROY:
"""

tail = r"""
        break;
    default:
        break;
    }
}

static int DestroyWindow(HWND hwnd) {
    if (hwnd != &expected_window) return 0;
    ++destroy_window_calls;
    dispatch_wm_destroy(active_tray);
    return 1;
}
"""

main = r"""
static void reset_counts(void) {
    destroy_window_calls = 0;
    shell_delete_calls = 0;
    post_quit_calls = 0;
    destroy_icons_calls = 0;
    destroy_menu_calls = 0;
}

int main(void) {
    NoSleepTray *tray = calloc(1, sizeof(*tray));
    if (!tray) return 2;
    tray->hwnd = &expected_window;
    tray->nid.hWnd = tray->hwnd;
    active_tray = tray;

    tray_destroy(tray);
    if (destroy_window_calls != 1 || shell_delete_calls != 1 || post_quit_calls != 1) {
        fprintf(stderr, "FAIL: startup teardown destroyed window %d times, removed icon %d times, posted quit %d times\n",
                destroy_window_calls, shell_delete_calls, post_quit_calls);
        return 1;
    }

    reset_counts();
    tray = calloc(1, sizeof(*tray));
    if (!tray) return 2;
    tray->hwnd = &expected_window;
    tray->nid.hWnd = tray->hwnd;
    active_tray = tray;
    dispatch_wm_destroy(tray);
    tray_destroy(tray);
    if (destroy_window_calls != 0 || shell_delete_calls != 1 || post_quit_calls != 1) {
        fprintf(stderr, "FAIL: normal teardown repeated window/icon cleanup (%d/%d/%d)\n",
                destroy_window_calls, shell_delete_calls, post_quit_calls);
        return 1;
    }

    puts("PASS: tray_destroy removes the shell icon on startup failure without repeating normal cleanup");
    return 0;
}
"""

source_text = prelude + destroy_case.group(1) + tail
source_text += "\n" + extract_function("void tray_destroy(NoSleepTray* tray)") + "\n" + main

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_destroy_shell_icon_cleanup.c"
    binary = Path(tmp) / "test_tray_destroy_shell_icon_cleanup"
    source.write_text(source_text)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
