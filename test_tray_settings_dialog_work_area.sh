#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PYTEST'
from pathlib import Path
import os
import re
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


geometry_helpers = "\n\n".join(
    extract_function(signature)
    for signature in (
        "static BOOL dialog_window_bounds_for_dpi(",
        "static UINT fit_dialog_dpi(",
        "static BOOL fit_dialog_layout_to_work_area(",
    )
)
move_helper = extract_function("static BOOL settings_dialog_refit_on_move(")

settings_dialog = extract_function("static LRESULT CALLBACK settings_dialog_proc(")
move_handler = re.search(
    r"case WM_MOVE\s*:(.*?)(?=\n\s*case\s+WM_|\n\s*default\s*:)",
    settings_dialog,
    re.S,
)
if not move_handler:
    raise AssertionError("Settings dialog does not refit its layout when moved")
if "settings_dialog_refit_on_move(hwnd, &settings_monitor," not in move_handler.group(1):
    raise AssertionError("Settings WM_MOVE does not invoke its monitor refit handler")
if "scale_dialog_layout(hwnd, custom_dialog_dpi(hwnd), NULL);" not in move_helper:
    raise AssertionError("Settings move handler does not refit at the current DPI")

harness = r"""
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* HWND;
typedef void* HMONITOR;
typedef struct { int left, top, right, bottom; } RECT;
typedef RECT* LPRECT;

#define WINAPI
#define TRUE 1
#define FALSE 0
#define MONITOR_DEFAULTTONEAREST 2

static HMONITOR window_monitor = (HMONITOR)1;
static HWND expected_window = (HWND)10;

static HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    (void)flags;
    return hwnd == expected_window ? window_monitor : NULL;
}

static int MulDiv(int value, int numerator, int denominator) {
    long long product = (long long)value * numerator;
    return (int)((product + denominator / 2) / denominator);
}

static void* GetModuleHandle(const char* name) {
    (void)name;
    return NULL;
}

static void* GetProcAddress(void* module, const char* name) {
    (void)module;
    (void)name;
    return NULL;
}

static BOOL AdjustWindowRectEx(RECT* rect, DWORD style, BOOL menu, DWORD ex_style) {
    (void)style;
    (void)menu;
    (void)ex_style;
    rect->left -= 8;
    rect->right += 8;
    rect->top -= 31;
    rect->bottom += 8;
    return TRUE;
}

__GEOMETRY_HELPERS__

static RECT simulated_work_area;
static RECT simulated_window_bounds;
static HMONITOR* active_monitor_state;
static BOOL* active_reflow_state;
static int scale_calls;
static UINT scale_dpi;
static UINT fitted_layout_dpi;
static BOOL nested_move_refit;
static BOOL scale_failed;

static UINT custom_dialog_dpi(HWND hwnd) {
    (void)hwnd;
    return 192;
}

static BOOL settings_dialog_refit_on_move(HWND hwnd, HMONITOR* previous_monitor,
                                           BOOL* reflowing);

static void scale_dialog_layout(HWND hwnd, UINT dpi, const RECT* suggested) {
    scale_calls++;
    scale_dpi = dpi;
    RECT fitted_bounds;
    if (!fit_dialog_layout_to_work_area(480, 470, dpi, 0, 0,
            &simulated_work_area, &simulated_window_bounds, suggested,
            &fitted_layout_dpi, &fitted_bounds)) {
        scale_failed = TRUE;
        return;
    }
    simulated_window_bounds = fitted_bounds;

    HMONITOR before_nested_move = window_monitor;
    window_monitor = (HMONITOR)3;
    nested_move_refit = settings_dialog_refit_on_move(
        hwnd, active_monitor_state, active_reflow_state);
    window_monitor = before_nested_move;
}

__MOVE_HELPER__

static int fits(UINT layout_dpi, UINT frame_dpi, int available_width, int available_height) {
    RECT bounds;
    if (!dialog_window_bounds_for_dpi(480, 470, layout_dpi, frame_dpi, 0, 0, &bounds)) return 0;
    return bounds.right - bounds.left <= available_width &&
        bounds.bottom - bounds.top <= available_height;
}

static int check_fit(const char* label, UINT requested, int width, int height) {
    UINT fitted = fit_dialog_dpi(requested, 480, 470, width, height, 0, 0);
    if (!fitted || fitted > requested || !fits(fitted, requested, width, height)) {
        fprintf(stderr, "FAIL: %s did not fit the work area (dpi %u)\n", label, fitted);
        return 1;
    }
    if (fitted < requested && fits(fitted + 1, requested, width, height)) {
        fprintf(stderr, "FAIL: %s left avoidable space unused (dpi %u)\n", label, fitted);
        return 1;
    }

    int client_height = MulDiv(470, fitted, 96);
    int buttons_bottom = MulDiv(430 + 28, fitted, 96);
    if (buttons_bottom > client_height) {
        fprintf(stderr, "FAIL: %s clipped the bottom action buttons\n", label);
        return 1;
    }
    printf("PASS: %s uses fitting layout DPI %u\n", label, fitted);
    return 0;
}

static int check_same_dpi_monitor_move(void) {
    const UINT monitor_dpi = 192;
    UINT before_move = fit_dialog_dpi(monitor_dpi, 480, 470,
        1300, 1100, 0, 0);
    if (before_move != monitor_dpi) {
        fprintf(stderr, "FAIL: initial large-monitor layout was unexpectedly reduced\n");
        return 1;
    }

    HWND hwnd = expected_window;
    HMONITOR previous_monitor = (HMONITOR)1;
    BOOL reflowing = FALSE;
    active_monitor_state = &previous_monitor;
    active_reflow_state = &reflowing;
    simulated_work_area = (RECT){1920, 0, 2700, 650};
    simulated_window_bounds = (RECT){2600, 500, 3576, 1487};
    window_monitor = (HMONITOR)1;
    scale_calls = 0;
    scale_dpi = 0;
    fitted_layout_dpi = 0;
    nested_move_refit = TRUE;
    scale_failed = FALSE;

    if (settings_dialog_refit_on_move(hwnd, &previous_monitor, &reflowing)) {
        fprintf(stderr, "FAIL: same-monitor movement triggered a refit\n");
        return 1;
    }

    window_monitor = (HMONITOR)2;
    if (!settings_dialog_refit_on_move(hwnd, &previous_monitor, &reflowing) ||
        scale_failed || scale_calls != 1 || scale_dpi != monitor_dpi ||
        fitted_layout_dpi >= before_move || nested_move_refit || reflowing ||
        previous_monitor != (HMONITOR)2) {
        fprintf(stderr,
                "FAIL: same-DPI move reflow count=%d dpi=%u fitted=%u nested=%d failed=%d\n",
                scale_calls, scale_dpi, fitted_layout_dpi, nested_move_refit,
                scale_failed);
        return 1;
    }

    int width = simulated_window_bounds.right - simulated_window_bounds.left;
    int height = simulated_window_bounds.bottom - simulated_window_bounds.top;
    if (simulated_window_bounds.left < simulated_work_area.left ||
        simulated_window_bounds.top < simulated_work_area.top ||
        simulated_window_bounds.right > simulated_work_area.right ||
        simulated_window_bounds.bottom > simulated_work_area.bottom ||
        !fits(fitted_layout_dpi, monitor_dpi, 780, 650) ||
        width > 780 || height > 650) {
        fprintf(stderr,
                "FAIL: moved Settings bounds (%d,%d,%d,%d) escape the work area\n",
                simulated_window_bounds.left, simulated_window_bounds.top,
                simulated_window_bounds.right, simulated_window_bounds.bottom);
        return 1;
    }

    if (settings_dialog_refit_on_move(hwnd, &previous_monitor, &reflowing) ||
        scale_calls != 1) {
        fprintf(stderr, "FAIL: a move within the new monitor triggered another refit\n");
        return 1;
    }
    printf("PASS: same-DPI monitor move refits once and clamps the smaller layout\n");
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= check_fit("high DPI on a small work area", 192, 780, 650);
    failed |= check_fit("height-constrained monitor", 192, 1200, 620);
    failed |= check_fit("unchanged scale on a large monitor", 192, 1300, 1100);
    failed |= check_same_dpi_monitor_move();
    return failed;
}
""".replace("__GEOMETRY_HELPERS__", geometry_helpers).replace(
    "__MOVE_HELPER__", move_helper
)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "settings_dialog_work_area.c"
    binary = Path(tmp) / "settings_dialog_work_area"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PYTEST
