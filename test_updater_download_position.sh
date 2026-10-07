#!/bin/bash
# Exercise updater download-dialog positioning with mocked monitor/work-area APIs.
set -euo pipefail

python3 - "${1:-src/updater.c}" <<'PY'
import re
import subprocess
import sys
import tempfile
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(
    r"\bstatic\s+void\s+position_download_dialog\s*\([^)]*\)\s*\{",
    source,
)
if not match:
    raise SystemExit("FAIL: position_download_dialog definition was not found")

start = match.start()
opening_brace = source.index("{", match.start())
depth = 0
end = None
for index in range(opening_brace, len(source)):
    if source[index] == "{":
        depth += 1
    elif source[index] == "}":
        depth -= 1
        if depth == 0:
            end = index + 1
            break
if end is None:
    raise SystemExit("FAIL: position_download_dialog definition is incomplete")

function = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef void *HWND;
typedef void *HMONITOR;
typedef int BOOL;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef struct { long left, top, right, bottom; } RECT;
typedef struct { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; } MONITORINFO;

#define TRUE 1
#define FALSE 0
#define MONITOR_DEFAULTTONEAREST 2
#define SPI_GETWORKAREA 48
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SWP_NOSIZE 1
#define HWND_TOPMOST ((HWND)(intptr_t)-1)

static HWND parent = (HWND)(uintptr_t)1;
static HWND dialog = (HWND)(uintptr_t)2;
static HMONITOR owner_monitor = (HMONITOR)(uintptr_t)3;
static BOOL parent_is_valid;
static BOOL monitor_info_succeeds;
static BOOL work_area_query_succeeds;
static RECT owner_work_area;
static RECT fallback_work_area;
static RECT window_rect;
static int monitor_calls;
static HWND monitor_window_argument;
static DWORD monitor_flags_argument;
static int monitor_info_calls;
static int work_area_calls;
static int set_window_pos_calls;
static int position_x;
static int position_y;
static int position_width;
static int position_height;
static UINT position_flags;

static BOOL IsWindow(HWND hwnd) { return hwnd == parent && parent_is_valid; }
static HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    monitor_calls++;
    monitor_window_argument = hwnd;
    monitor_flags_argument = flags;
    return owner_monitor;
}
static BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO *info) {
    monitor_info_calls++;
    assert(monitor == owner_monitor);
    assert(info->cbSize == sizeof(*info));
    if (!monitor_info_succeeds) return FALSE;
    info->rcWork = owner_work_area;
    return TRUE;
}
static BOOL SystemParametersInfoA(UINT action, UINT parameter, void *output, UINT flags) {
    work_area_calls++;
    assert(action == SPI_GETWORKAREA);
    assert(parameter == 0);
    assert(flags == 0);
    if (!work_area_query_succeeds) return FALSE;
    *(RECT *)output = fallback_work_area;
    return TRUE;
}
static BOOL GetWindowRect(HWND hwnd, RECT *rect) {
    assert(hwnd == dialog);
    *rect = window_rect;
    return TRUE;
}
static int GetSystemMetrics(int metric) {
    if (metric == SM_CXSCREEN) return 1920;
    assert(metric == SM_CYSCREEN);
    return 1080;
}
static BOOL SetWindowPos(HWND hwnd, HWND insert_after, int x, int y,
                         int width, int height, UINT flags) {
    assert(hwnd == dialog);
    assert(insert_after == HWND_TOPMOST);
    set_window_pos_calls++;
    position_x = x;
    position_y = y;
    position_width = width;
    position_height = height;
    position_flags = flags;
    return TRUE;
}
'''
checks = r'''
static void reset_state(void) {
    parent_is_valid = TRUE;
    monitor_info_succeeds = TRUE;
    work_area_query_succeeds = TRUE;
    owner_work_area = (RECT){0, 0, 1920, 1080};
    fallback_work_area = (RECT){40, 50, 1960, 1050};
    window_rect = (RECT){100, 100, 530, 260};
    monitor_calls = 0;
    monitor_window_argument = NULL;
    monitor_flags_argument = 0;
    monitor_info_calls = 0;
    work_area_calls = 0;
    set_window_pos_calls = 0;
    position_x = position_y = position_width = position_height = 0;
    position_flags = 0;
}

static void assert_position(RECT work_area) {
    int width = (int)(window_rect.right - window_rect.left);
    int height = (int)(window_rect.bottom - window_rect.top);
    int expected_x = (int)(work_area.left +
        ((work_area.right - work_area.left) - width) / 2);
    int expected_y = (int)(work_area.top +
        ((work_area.bottom - work_area.top) - height) / 2);
    assert(set_window_pos_calls == 1);
    assert(position_x == expected_x);
    assert(position_y == expected_y);
    assert(position_width == 0 && position_height == 0);
    assert(position_flags == SWP_NOSIZE);
}

int main(void) {
    reset_state();
    owner_work_area = (RECT){-1600, 40, 0, 1000};
    position_download_dialog(dialog, parent);
    assert(monitor_calls == 1);
    assert(monitor_window_argument == parent);
    assert(monitor_flags_argument == MONITOR_DEFAULTTONEAREST);
    assert(monitor_info_calls == 1);
    assert(work_area_calls == 0);
    assert_position(owner_work_area);

    reset_state();
    position_download_dialog(dialog, NULL);
    assert(monitor_calls == 0);
    assert(work_area_calls == 1);
    assert_position(fallback_work_area);

    reset_state();
    parent_is_valid = FALSE;
    position_download_dialog(dialog, parent);
    assert(monitor_calls == 0);
    assert(work_area_calls == 1);
    assert_position(fallback_work_area);

    reset_state();
    monitor_info_succeeds = FALSE;
    position_download_dialog(dialog, parent);
    assert(monitor_calls == 1);
    assert(monitor_info_calls == 1);
    assert(work_area_calls == 1);
    assert_position(fallback_work_area);

    reset_state();
    work_area_query_succeeds = FALSE;
    position_download_dialog(dialog, NULL);
    assert(work_area_calls == 1);
    assert_position((RECT){0, 0, 1920, 1080});
    puts("PASS: updater dialog uses owner work area and safe fallbacks");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / "position.c"
    test.write_text(harness + function + checks, encoding="utf-8")
    binary = Path(directory) / "position"
    subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
