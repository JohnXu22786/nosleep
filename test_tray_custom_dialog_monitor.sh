#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()
start = tray.index("    // Use the tray interaction's monitor, including its taskbar work-area inset.")
end = tray.index("    MONITORINFO monitor_info", start)
selection = tray[start:end]

harness = r"""
#include <stdio.h>
#include <stdbool.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* HWND;
typedef void* HMONITOR;
typedef struct { int x, y; } POINT;
typedef struct {
    HWND hwnd;
    struct { UINT uID; } nid;
    BOOL menu_invoked_by_keyboard;
} NoSleepTray;

#define TRUE 1
#define FALSE 0
#define MONITOR_DEFAULTTONEAREST 2

static HMONITOR icon_monitor = (HMONITOR)3;
static HMONITOR cursor_monitor = (HMONITOR)2;
static HMONITOR owner_monitor = (HMONITOR)1;
static BOOL icon_monitor_available = TRUE;
static BOOL cursor_available = TRUE;
static int icon_queries;
static int cursor_queries;
static int cursor_monitor_queries;
static int owner_monitor_queries;

static HMONITOR tray_get_icon_monitor(HWND hwnd, UINT icon_id) {
    icon_queries++;
    if (hwnd != (HWND)10 || icon_id != 42 || !icon_monitor_available) return NULL;
    return icon_monitor;
}

static BOOL GetCursorPos(POINT* point) {
    cursor_queries++;
    if (!cursor_available) return FALSE;
    point->x = 1920;
    point->y = 100;
    return TRUE;
}

static HMONITOR MonitorFromPoint(POINT point, DWORD flags) {
    (void)point;
    (void)flags;
    cursor_monitor_queries++;
    return cursor_monitor;
}

static HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    (void)hwnd;
    (void)flags;
    owner_monitor_queries++;
    return owner_monitor;
}

static HMONITOR custom_dialog_monitor(NoSleepTray* tray) {
__SELECTION__
    return monitor;
}

static int check_case(const char* label, BOOL keyboard_invoked,
                      BOOL have_icon_monitor, BOOL have_cursor,
                      HMONITOR expected_monitor, int expected_icon_queries,
                      int expected_cursor_queries, int expected_point_queries,
                      int expected_owner_queries) {
    NoSleepTray tray = { (HWND)10, {42}, keyboard_invoked };
    icon_monitor_available = have_icon_monitor;
    cursor_available = have_cursor;
    icon_queries = 0;
    cursor_queries = 0;
    cursor_monitor_queries = 0;
    owner_monitor_queries = 0;

    HMONITOR monitor = custom_dialog_monitor(&tray);
    if (monitor != expected_monitor || icon_queries != expected_icon_queries ||
        cursor_queries != expected_cursor_queries ||
        cursor_monitor_queries != expected_point_queries ||
        owner_monitor_queries != expected_owner_queries ||
        tray.menu_invoked_by_keyboard) {
        fprintf(stderr,
                "FAIL: %s selected %p (icon=%d cursor=%d point=%d owner=%d flag=%d)\n",
                label, monitor, icon_queries, cursor_queries,
                cursor_monitor_queries, owner_monitor_queries,
                tray.menu_invoked_by_keyboard);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= check_case("keyboard activation uses the tray icon monitor",
                         TRUE, TRUE, TRUE, icon_monitor, 1, 0, 0, 0);
    failed |= check_case("keyboard activation avoids the cursor on icon lookup failure",
                         TRUE, FALSE, TRUE, owner_monitor, 1, 0, 0, 1);
    failed |= check_case("mouse activation continues to use the cursor monitor",
                         FALSE, TRUE, TRUE, cursor_monitor, 0, 1, 1, 0);
    return failed;
}
""".replace("__SELECTION__", selection)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "tray_custom_dialog_monitor.c"
    binary = Path(tmp) / "tray_custom_dialog_monitor"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
