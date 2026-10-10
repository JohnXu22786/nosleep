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
    while True:
        brace = tray.index("{", start)
        declaration_end = tray.find(";", start, brace)
        if declaration_end == -1:
            break
        start = tray.index(signature, brace)
    depth = 0
    for index in range(brace, len(tray)):
        if tray[index] == "{":
            depth += 1
        elif tray[index] == "}":
            depth -= 1
            if depth == 0:
                return tray[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


icon_rect_helper = extract_function("static BOOL tray_get_icon_rect(")
icon_helper = extract_function("static HMONITOR tray_get_icon_monitor(")
helper = extract_function("static void center_about_dialog_on_invoking_monitor(")

harness = r"""
#include <stdio.h>
#include <string.h>

#define WINAPI

typedef void* HWND;
typedef void* HMONITOR;
typedef void* HMODULE;
typedef void* FARPROC;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef long HRESULT;
typedef struct { int x, y; } POINT;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct {
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    struct { unsigned int Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } guidItem;
} TrayNotifyIconIdentifier;
typedef HRESULT (WINAPI *ShellNotifyIconGetRectFn)(
    const TrayNotifyIconIdentifier* identifier, RECT* icon_rect);

#define TRUE 1
#define FALSE 0
#define MONITOR_DEFAULTTONEAREST 2
#define SPI_GETWORKAREA 48
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SWP_NOSIZE 1
#define SWP_NOZORDER 4
#define FAILED(result) ((result) < 0)

static POINT cursor_point = { 100, 100 };
static BOOL cursor_available = TRUE;
static BOOL shell_api_available = TRUE;
static HMONITOR icon_monitor = (HMONITOR)3;
static HMONITOR cursor_monitor = (HMONITOR)2;
static HMONITOR owner_monitor = (HMONITOR)1;
static BOOL icon_rect_available = TRUE;
static BOOL cursor_monitor_available = TRUE;
static BOOL owner_monitor_available = TRUE;
static BOOL system_work_area_available = TRUE;
static RECT icon_work_area = { -1600, 100, 0, 1040 };
static RECT icon_bounds = { -40, 500, -20, 520 };
static RECT cursor_work_area = { 0, 0, 1920, 1040 };
static RECT owner_work_area = { 1920, 0, 3840, 1040 };
static RECT system_work_area = { 50, 50, 1870, 1030 };
static RECT dialog_bounds = { 100, 100, 500, 340 };
static int set_x;
static int set_y;
static UINT set_flags;
static int icon_rect_queries;
static int icon_api_lookups;
static int icon_monitor_queries;
static int cursor_queries;
static int cursor_monitor_queries;
static int owner_monitor_queries;

static HRESULT mock_shell_notify_icon_get_rect(
    const TrayNotifyIconIdentifier* identifier, RECT* bounds) {
    icon_rect_queries++;
    if (identifier->cbSize != sizeof(*identifier) ||
        identifier->hWnd != (HWND)11 || identifier->uID != 42 ||
        !icon_rect_available) {
        return -1;
    }
    *bounds = icon_bounds;
    return 0;
}

static HMODULE GetModuleHandle(const char* name) {
    return name && strcmp(name, "shell32.dll") == 0 && shell_api_available
        ? (HMODULE)1 : NULL;
}

static FARPROC GetProcAddress(HMODULE module, const char* name) {
    icon_api_lookups++;
    if (!module || !name || !shell_api_available ||
        strcmp(name, "Shell_NotifyIconGetRect") != 0) return NULL;
    return (FARPROC)mock_shell_notify_icon_get_rect;
}

static HMONITOR MonitorFromRect(const RECT* bounds, DWORD flags) {
    (void)flags;
    icon_monitor_queries++;
    if (bounds->left == icon_bounds.left && bounds->right == icon_bounds.right) {
        return icon_monitor;
    }
    return NULL;
}

static BOOL GetCursorPos(POINT* point) {
    cursor_queries++;
    if (!cursor_available) return FALSE;
    *point = cursor_point;
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

static BOOL GetMonitorInfo(HMONITOR monitor, MONITORINFO* info) {
    if (monitor == icon_monitor && icon_rect_available) {
        info->rcWork = icon_work_area;
        return TRUE;
    }
    if (monitor == cursor_monitor && cursor_monitor_available) {
        info->rcWork = cursor_work_area;
        return TRUE;
    }
    if (monitor == owner_monitor && owner_monitor_available) {
        info->rcWork = owner_work_area;
        return TRUE;
    }
    return FALSE;
}

static BOOL SystemParametersInfo(UINT action, UINT parameter, void* output, UINT flags) {
    (void)action;
    (void)parameter;
    (void)flags;
    if (!system_work_area_available) return FALSE;
    *(RECT*)output = system_work_area;
    return TRUE;
}

static int GetSystemMetrics(int index) {
    return index == SM_CXSCREEN ? 1920 : 1080;
}

static BOOL GetWindowRect(HWND hwnd, RECT* bounds) {
    (void)hwnd;
    *bounds = dialog_bounds;
    return TRUE;
}

static BOOL SetWindowPos(HWND hwnd, HWND insert_after, int x, int y,
                         int width, int height, UINT flags) {
    (void)hwnd;
    (void)insert_after;
    (void)width;
    (void)height;
    set_x = x;
    set_y = y;
    set_flags = flags;
    return TRUE;
}

__ICON_RECT_HELPER__

__ICON_HELPER__

__HELPER__

static int check_position(const char* label, BOOL keyboard_invoked,
                         int expected_x, int expected_y) {
    center_about_dialog_on_invoking_monitor((HWND)10, (HWND)11, 42,
                                             keyboard_invoked);
    if (set_x != expected_x || set_y != expected_y) {
        fprintf(stderr, "FAIL: %s positioned at (%d, %d), expected (%d, %d)\n",
                label, set_x, set_y, expected_x, expected_y);
        return 1;
    }
    if ((set_flags & (SWP_NOSIZE | SWP_NOZORDER)) !=
        (SWP_NOSIZE | SWP_NOZORDER)) {
        fprintf(stderr, "FAIL: %s changed dialog size or z-order\n", label);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}

int main(void) {
    int failed = 0;

    failed |= check_position("tray icon monitor takes priority over cursor", FALSE,
                             -1000, 450);
    if (icon_rect_queries != 1 || icon_monitor_queries != 1 ||
        icon_api_lookups != 1 || cursor_queries != 0 ||
        owner_monitor_queries != 0) {
        fprintf(stderr, "FAIL: tray icon monitor was not preferred\n");
        failed = 1;
    }

    cursor_queries = 0;
    failed |= check_position("keyboard invocation uses tray icon monitor", TRUE,
                             -1000, 450);
    if (cursor_queries != 0) {
        fprintf(stderr, "FAIL: keyboard invocation used the stale cursor position\n");
        failed = 1;
    }

    icon_rect_available = FALSE;
    cursor_queries = 0;
    owner_monitor_queries = 0;
    failed |= check_position("keyboard lookup failure uses owner monitor", TRUE,
                             2680, 400);
    if (cursor_queries != 0 || owner_monitor_queries != 1) {
        fprintf(stderr, "FAIL: keyboard invocation did not avoid stale cursor fallback\n");
        failed = 1;
    }

    cursor_queries = 0;
    owner_monitor_queries = 0;
    failed |= check_position("mouse invocation falls back to cursor monitor", FALSE,
                             760, 400);
    if (cursor_queries != 1 || cursor_monitor_queries != 1 ||
        owner_monitor_queries != 0) {
        fprintf(stderr, "FAIL: cursor monitor was not used after icon lookup failed\n");
        failed = 1;
    }

    shell_api_available = FALSE;
    cursor_queries = 0;
    cursor_monitor_queries = 0;
    failed |= check_position("missing optional Shell API falls back to cursor monitor",
                             FALSE, 760, 400);
    if (cursor_queries != 1 || cursor_monitor_queries != 1) {
        fprintf(stderr, "FAIL: cursor fallback did not cover the unavailable Shell API\n");
        failed = 1;
    }

    cursor_available = FALSE;
    shell_api_available = TRUE;
    owner_monitor_queries = 0;
    failed |= check_position("unavailable cursor falls back to owner monitor", FALSE,
                             2680, 400);
    if (owner_monitor_queries != 1) {
        fprintf(stderr, "FAIL: owner monitor fallback was not used\n");
        failed = 1;
    }

    owner_monitor_available = FALSE;
    failed |= check_position("system work area fallback", FALSE, 760, 420);

    system_work_area_available = FALSE;
    failed |= check_position("primary screen fallback", FALSE, 760, 420);

    icon_rect_available = TRUE;
    dialog_bounds = (RECT){ 100, 100, 1900, 1300 };
    failed |= check_position("oversized dialog stays anchored in work area", FALSE,
                             -1600, 100);

    return failed;
}
""".replace("__ICON_RECT_HELPER__", icon_rect_helper).replace(
    "__ICON_HELPER__", icon_helper
).replace("__HELPER__", helper)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "tray_about_dialog_monitor.c"
    binary = Path(tmp) / "tray_about_dialog_monitor"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PYTEST
