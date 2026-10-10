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


window_proc = extract_function("LRESULT CALLBACK tray_window_proc(")
assert "tray_show_tray_icon_context_menu(tray, hwnd, lParam);" in window_proc
assert "tray_show_window_context_menu(tray, hwnd, lParam);" in window_proc

functions = "\n\n".join(
    extract_function(signature)
    for signature in (
        "static BOOL tray_get_icon_rect(",
        "static void tray_show_context_menu(",
        "static void tray_show_tray_icon_context_menu(",
        "static void tray_show_window_context_menu(",
    )
)
fallback_signature = "static void tray_get_context_menu_fallback_position("
if fallback_signature in tray:
    functions = extract_function(fallback_signature) + "\n\n" + functions

harness = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WINAPI
typedef void* HWND;
typedef void* HMENU;
typedef void* HMONITOR;
typedef void* HMODULE;
typedef void* FARPROC;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef long HRESULT;
typedef intptr_t LPARAM;
typedef unsigned short WORD;
typedef struct { int x, y; } POINT;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
} MONITORINFO;
typedef struct {
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    struct {
        unsigned int Data1;
        unsigned short Data2, Data3;
        unsigned char Data4[8];
    } guidItem;
} TrayNotifyIconIdentifier;
typedef HRESULT (WINAPI *ShellNotifyIconGetRectFn)(
    const TrayNotifyIconIdentifier* identifier, RECT* icon_rect);
typedef struct {
    HWND hwnd;
    HMENU hmenu;
    BOOL menu_invoked_by_keyboard;
    struct { UINT uID; } nid;
} NoSleepTray;

#define TRUE 1
#define FALSE 0
#define NIN_KEYSELECT 0x0401
#define WM_RBUTTONUP 0x0205
#define WM_NULL 0x0000
#define IDM_STOP 1005
#define MONITOR_DEFAULTTONEAREST 2
#define MF_BYCOMMAND 0x00000000
#define MF_ENABLED 0x00000000
#define MF_GRAYED 0x00000001
#define TPM_RIGHTBUTTON 0x00000002
#define FAILED(result) ((result) < 0)
#define SUCCEEDED(result) ((result) >= 0)
#define LOWORD(value) ((WORD)((uintptr_t)(value) & 0xffff))
#define HIWORD(value) ((WORD)(((uintptr_t)(value) >> 16) & 0xffff))

static BOOL shell_api_available = TRUE;
static int cursor_queries;
static int icon_rect_queries;
static int owner_monitor_queries;
static int monitor_info_queries;
static int foreground_queries;
static int menu_enable_queries;
static int menu_calls;
static int post_queries;
static int menu_x;
static int menu_y;
static BOOL menu_keyboard_invocation;
static NoSleepTray* current_tray;
static POINT cursor_point = { 1800, 900 };
static RECT icon_rect = { -40, 500, -20, 520 };
static RECT owner_work_area = { -1600, 0, 0, 1040 };
static RECT owner_window_rect = { 40, 50, 640, 430 };
static BOOL icon_rect_available = TRUE;
static BOOL monitor_info_available = TRUE;
static BOOL cursor_available = TRUE;

static HRESULT mock_shell_notify_icon_get_rect(
    const TrayNotifyIconIdentifier* identifier, RECT* bounds) {
    icon_rect_queries++;
    if (identifier->cbSize != sizeof(*identifier) ||
        identifier->hWnd != (HWND)10 || identifier->uID != 42 ||
        !icon_rect_available) {
        return -1;
    }
    *bounds = icon_rect;
    return 0;
}

static HMODULE GetModuleHandle(const char* name) {
    return name && strcmp(name, "shell32.dll") == 0 && shell_api_available
        ? (HMODULE)1 : NULL;
}

static FARPROC GetProcAddress(HMODULE module, const char* name) {
    if (!module || !name || strcmp(name, "Shell_NotifyIconGetRect") != 0) {
        return NULL;
    }
    return (FARPROC)mock_shell_notify_icon_get_rect;
}

static BOOL GetCursorPos(POINT* point) {
    cursor_queries++;
    if (!cursor_available) {
        point->x = 1234;
        point->y = 5678;
        return FALSE;
    }
    *point = cursor_point;
    return TRUE;
}

static HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    (void)hwnd;
    (void)flags;
    owner_monitor_queries++;
    return (HMONITOR)30;
}

static BOOL GetMonitorInfo(HMONITOR monitor, MONITORINFO* info) {
    monitor_info_queries++;
    if (monitor != (HMONITOR)30 || !monitor_info_available) return FALSE;
    info->rcWork = owner_work_area;
    return TRUE;
}

static BOOL GetWindowRect(HWND hwnd, RECT* bounds) {
    (void)hwnd;
    *bounds = owner_window_rect;
    return TRUE;
}

static BOOL SetForegroundWindow(HWND hwnd) {
    (void)hwnd;
    foreground_queries++;
    return TRUE;
}

static UINT EnableMenuItem(HMENU menu, UINT item, UINT flags) {
    (void)menu;
    (void)item;
    (void)flags;
    menu_enable_queries++;
    return 0;
}

static BOOL TrackPopupMenu(HMENU menu, UINT flags, int x, int y,
                           int reserved, HWND hwnd, const RECT* rect) {
    (void)menu;
    (void)flags;
    (void)reserved;
    (void)hwnd;
    (void)rect;
    menu_calls++;
    menu_x = x;
    menu_y = y;
    menu_keyboard_invocation = current_tray->menu_invoked_by_keyboard;
    return TRUE;
}

static BOOL PostMessage(HWND hwnd, UINT message, uintptr_t wparam, intptr_t lparam) {
    (void)hwnd;
    (void)message;
    (void)wparam;
    (void)lparam;
    post_queries++;
    return TRUE;
}

static BOOL tray_has_stop_work(NoSleepTray* tray) {
    (void)tray;
    return FALSE;
}

__FUNCTIONS__

static void reset_harness(void) {
    cursor_queries = 0;
    icon_rect_queries = 0;
    owner_monitor_queries = 0;
    monitor_info_queries = 0;
    foreground_queries = 0;
    menu_enable_queries = 0;
    menu_calls = 0;
    post_queries = 0;
    menu_x = 0;
    menu_y = 0;
    menu_keyboard_invocation = FALSE;
}

static int check_result(const char* label, NoSleepTray* tray,
                        int expected_x, int expected_y,
                        BOOL expected_keyboard, int expected_cursor_queries,
                        int expected_icon_queries,
                        int expected_owner_monitor_queries) {
    if (menu_calls != 1 || menu_x != expected_x || menu_y != expected_y ||
        menu_keyboard_invocation != expected_keyboard ||
        tray->menu_invoked_by_keyboard != expected_keyboard ||
        cursor_queries != expected_cursor_queries ||
        icon_rect_queries != expected_icon_queries ||
        owner_monitor_queries != expected_owner_monitor_queries ||
        foreground_queries != 1 || menu_enable_queries != 1 || post_queries != 1) {
        fprintf(stderr,
                "FAIL: %s placed menu at (%d, %d), keyboard=%d, cursor=%d, icon=%d\n",
                label, menu_x, menu_y, menu_keyboard_invocation,
                cursor_queries, icon_rect_queries);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}

int main(void) {
    int failed = 0;
    NoSleepTray tray = { (HWND)10, (HMENU)20, FALSE, {42} };
    current_tray = &tray;

    reset_harness();
    tray_show_tray_icon_context_menu(&tray, (HWND)10, NIN_KEYSELECT);
    failed |= check_result("NIN_KEYSELECT anchors to the tray icon despite a distant cursor",
                           &tray, -40, 520, TRUE, 0, 1, 0);

    reset_harness();
    tray.menu_invoked_by_keyboard = FALSE;
    tray_show_window_context_menu(&tray, (HWND)10, (LPARAM)-1);
    failed |= check_result("keyboard WM_CONTEXTMENU coordinates anchor to the tray icon",
                           &tray, -40, 520, TRUE, 0, 1, 0);

    reset_harness();
    shell_api_available = FALSE;
    tray.menu_invoked_by_keyboard = FALSE;
    tray_show_tray_icon_context_menu(&tray, (HWND)10, NIN_KEYSELECT);
    failed |= check_result("NIN_KEYSELECT uses the tray monitor fallback when icon lookup is unavailable",
                           &tray, -1, 1039, TRUE, 0, 0, 1);

    reset_harness();
    shell_api_available = TRUE;
    icon_rect_available = FALSE;
    monitor_info_available = FALSE;
    tray.menu_invoked_by_keyboard = FALSE;
    tray_show_window_context_menu(&tray, (HWND)10, (LPARAM)-1);
    failed |= check_result("keyboard WM_CONTEXTMENU uses the tray window fallback when icon lookup fails",
                           &tray, 40, 50, TRUE, 0, 1, 1);

    shell_api_available = TRUE;
    icon_rect_available = TRUE;
    monitor_info_available = TRUE;
    cursor_available = TRUE;

    reset_harness();
    tray.menu_invoked_by_keyboard = TRUE;
    tray_show_tray_icon_context_menu(&tray, (HWND)10, WM_RBUTTONUP);
    failed |= check_result("mouse tray activation keeps cursor placement",
                           &tray, cursor_point.x, cursor_point.y, FALSE, 1, 0, 0);

    reset_harness();
    tray.menu_invoked_by_keyboard = TRUE;
    tray_show_window_context_menu(&tray, (HWND)10, (LPARAM)0x00100020);
    failed |= check_result("mouse WM_CONTEXTMENU keeps cursor placement",
                           &tray, cursor_point.x, cursor_point.y, FALSE, 1, 0, 0);

    reset_harness();
    cursor_available = FALSE;
    tray.menu_invoked_by_keyboard = TRUE;
    tray_show_tray_icon_context_menu(&tray, (HWND)10, WM_RBUTTONUP);
    failed |= check_result("mouse tray activation falls back to the tray monitor when cursor lookup fails",
                           &tray, -1, 1039, FALSE, 1, 0, 1);

    return failed;
}
""".replace("__FUNCTIONS__", functions)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "tray_keyboard_menu_anchor.c"
    binary = Path(tmp) / "tray_keyboard_menu_anchor"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
