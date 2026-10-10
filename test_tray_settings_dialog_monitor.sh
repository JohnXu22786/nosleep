#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PYTEST'
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(signature):
    start = tray.index(signature)
    brace = tray.index("{", start)
    depth = 0
    for index in range(brace, len(tray)):
        if tray[index] == "{":
            depth += 1
        elif tray[index] == "}":
            depth -= 1
            if depth == 0:
                return tray[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


settings_bounds = extract_function("static void settings_dialog_initial_bounds(")
settings_dialog = extract_function("void tray_show_settings_dialog(")
if "settings_dialog_initial_bounds(tray, &initial_bounds);" not in settings_dialog:
    raise AssertionError("Settings dialog does not use its monitor-selection helper")

harness = r"""
#include <stdio.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* HWND;
typedef void* HMONITOR;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct {
    HWND hwnd;
    struct { UINT uID; } nid;
} NoSleepTray;

#define TRUE 1
#define FALSE 0

static HWND expected_owner = (HWND)10;
static UINT expected_icon_id = 42;
static HMONITOR icon_monitor = (HMONITOR)3;
static BOOL icon_monitor_available = TRUE;
static BOOL monitor_info_available = TRUE;
static RECT owner_bounds = {1920, 10, 2200, 100};
static RECT icon_work_area = {-1600, 100, 0, 1040};
static int owner_rect_queries;
static int icon_monitor_queries;
static int monitor_info_queries;

static BOOL GetWindowRect(HWND hwnd, RECT* bounds) {
    owner_rect_queries++;
    if (hwnd != expected_owner) return FALSE;
    *bounds = owner_bounds;
    return TRUE;
}

static HMONITOR tray_get_icon_monitor(HWND hwnd, UINT icon_id) {
    icon_monitor_queries++;
    if (hwnd != expected_owner || icon_id != expected_icon_id ||
        !icon_monitor_available) return NULL;
    return icon_monitor;
}

static BOOL GetMonitorInfo(HMONITOR monitor, MONITORINFO* info) {
    monitor_info_queries++;
    if (monitor != icon_monitor || !monitor_info_available ||
        info->cbSize != sizeof(*info)) return FALSE;
    info->rcWork = icon_work_area;
    return TRUE;
}

__SETTINGS_BOUNDS__

static int same_bounds(RECT actual, RECT expected) {
    return actual.left == expected.left && actual.top == expected.top &&
        actual.right == expected.right && actual.bottom == expected.bottom;
}

static int check_case(const char* label, BOOL have_icon_monitor,
                      BOOL have_monitor_info, RECT expected_bounds,
                      int expected_info_queries) {
    NoSleepTray tray = {expected_owner, {expected_icon_id}};
    icon_monitor_available = have_icon_monitor;
    monitor_info_available = have_monitor_info;
    owner_rect_queries = 0;
    icon_monitor_queries = 0;
    monitor_info_queries = 0;

    RECT bounds = {0};
    settings_dialog_initial_bounds(&tray, &bounds);
    if (!same_bounds(bounds, expected_bounds) || owner_rect_queries != 1 ||
        icon_monitor_queries != 1 || monitor_info_queries != expected_info_queries) {
        fprintf(stderr,
                "FAIL: %s bounds=(%d,%d,%d,%d), calls owner=%d icon=%d info=%d\n",
                label, bounds.left, bounds.top, bounds.right, bounds.bottom,
                owner_rect_queries, icon_monitor_queries, monitor_info_queries);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= check_case("tray icon work area anchors Settings on its monitor",
                         TRUE, TRUE, (RECT){-1600, 100, 2200, 100}, 1);
    failed |= check_case("missing tray icon monitor keeps the owner placement",
                         FALSE, TRUE, owner_bounds, 0);
    failed |= check_case("unavailable monitor information keeps the owner placement",
                         TRUE, FALSE, owner_bounds, 1);
    return failed;
}
""".replace("__SETTINGS_BOUNDS__", settings_bounds)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "settings_dialog_monitor.c"
    binary = Path(tmp) / "settings_dialog_monitor"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PYTEST
