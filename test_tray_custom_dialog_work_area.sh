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


helpers = "\n\n".join(
    extract_function(signature)
    for signature in (
        "static void layout_custom_dialog(",
        "static void size_custom_dialog(",
        "static BOOL dialog_window_bounds_for_dpi(",
        "static UINT fit_dialog_dpi(",
        "static void fit_custom_dialog_to_work_area(",
    )
)

harness = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* HWND;
typedef void* HFONT;
typedef void* HMODULE;
typedef void* FARPROC;
typedef long LPARAM;
typedef struct { int left, top, right, bottom; } RECT;
typedef RECT* LPRECT;

#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define WM_APP 0x8000
#define GWL_STYLE -16
#define GWL_EXSTYLE -20
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010

static RECT window_bounds;
static RECT control_bounds[5];
static UINT current_layout_dpi;
static HFONT dialog_font;

static int MulDiv(int value, int numerator, int denominator) {
    long long product = (long long)value * numerator;
    return (int)((product + denominator / 2) / denominator);
}

static BOOL adjust_frame_for_dpi(RECT* rect, DWORD style, BOOL menu,
                                 DWORD ex_style, UINT dpi) {
    (void)style;
    (void)menu;
    (void)ex_style;
    rect->left -= MulDiv(8, dpi, 96);
    rect->right += MulDiv(8, dpi, 96);
    rect->top -= MulDiv(31, dpi, 96);
    rect->bottom += MulDiv(8, dpi, 96);
    return TRUE;
}

static BOOL AdjustWindowRectEx(RECT* rect, DWORD style, BOOL menu, DWORD ex_style) {
    return adjust_frame_for_dpi(rect, style, menu, ex_style, 96);
}

static HMODULE GetModuleHandle(const char* name) {
    (void)name;
    return (HMODULE)1;
}

static FARPROC GetProcAddress(HMODULE module, const char* name) {
    if (!module || strcmp(name, "AdjustWindowRectExForDpi") != 0) return NULL;
    return (FARPROC)adjust_frame_for_dpi;
}

static long GetWindowLongPtr(HWND hwnd, int index) {
    (void)hwnd;
    (void)index;
    return 0;
}

static int SetWindowPos(HWND hwnd, HWND insert_after, int x, int y,
                        int width, int height, UINT flags) {
    (void)hwnd;
    (void)insert_after;
    if (!(flags & SWP_NOMOVE)) {
        window_bounds.left = x;
        window_bounds.top = y;
    }
    if (!(flags & SWP_NOSIZE)) {
        window_bounds.right = window_bounds.left + width;
        window_bounds.bottom = window_bounds.top + height;
    }
    return TRUE;
}

static int GetWindowRect(HWND hwnd, RECT* bounds) {
    (void)hwnd;
    *bounds = window_bounds;
    return TRUE;
}

static HWND GetDlgItem(HWND hwnd, int id) {
    (void)hwnd;
    return (HWND)(intptr_t)id;
}

static BOOL MoveWindow(HWND hwnd, int x, int y, int width, int height, BOOL repaint) {
    (void)repaint;
    int id = (int)(intptr_t)hwnd;
    control_bounds[id] = (RECT){x, y, x + width, y + height};
    return TRUE;
}

static HFONT create_dialog_font(int size) {
    (void)size;
    return NULL;
}

static BOOL set_child_font_proc(HWND child, LPARAM font) {
    (void)child;
    (void)font;
    return TRUE;
}

static BOOL EnumChildWindows(HWND hwnd, BOOL (CALLBACK *callback)(HWND, LPARAM),
                             LPARAM parameter) {
    (void)hwnd;
    (void)callback;
    (void)parameter;
    return TRUE;
}

static int DeleteObject(HFONT font) {
    (void)font;
    return TRUE;
}

static UINT custom_dialog_dpi(HWND hwnd) {
    (void)hwnd;
    return 192;
}

static void layout_custom_dialog(HWND hwnd, UINT dpi, HFONT* font);

static int SendMessage(HWND hwnd, UINT message, unsigned long wparam, long lparam) {
    (void)lparam;
    if (message == WM_APP) {
        current_layout_dpi = (UINT)wparam;
        layout_custom_dialog(hwnd, current_layout_dpi, &dialog_font);
    }
    return 0;
}

static BOOL dialog_window_bounds_for_dpi(int width, int height, UINT layout_dpi,
                                         UINT window_dpi, DWORD style, DWORD ex_style,
                                         RECT* bounds);

__HELPERS__

static int check_case(const char* label, RECT work_area, const RECT* suggested,
                      BOOL should_fit) {
    window_bounds = (RECT){work_area.left, work_area.top,
                           work_area.left + 260, work_area.top + 150};
    memset(control_bounds, 0, sizeof(control_bounds));
    current_layout_dpi = 0;

    fit_custom_dialog_to_work_area((HWND)1, 192, &work_area, suggested);

    int width = window_bounds.right - window_bounds.left;
    int height = window_bounds.bottom - window_bounds.top;
    if (window_bounds.left < work_area.left || window_bounds.top < work_area.top ||
        window_bounds.right > work_area.right || window_bounds.bottom > work_area.bottom) {
        fprintf(stderr, "FAIL: %s window (%d,%d)-(%d,%d) exceeds work area\n",
                label, window_bounds.left, window_bounds.top,
                window_bounds.right, window_bounds.bottom);
        return 1;
    }
    if ((current_layout_dpi < 192) != should_fit) {
        fprintf(stderr, "FAIL: %s selected layout DPI %u\n", label, current_layout_dpi);
        return 1;
    }

    int client_width = MulDiv(260, current_layout_dpi, 96);
    int client_height = MulDiv(125, current_layout_dpi, 96);
    for (int id = 1; id <= 4; ++id) {
        RECT control = control_bounds[id];
        if (control.right <= control.left || control.bottom <= control.top ||
            control.left < 0 || control.top < 0 ||
            control.right > client_width || control.bottom > client_height) {
            fprintf(stderr, "FAIL: %s control %d is outside the dialog client area\n",
                    label, id);
            return 1;
        }
    }
    if (!suggested &&
        (window_bounds.left != work_area.left + ((work_area.right - work_area.left) - width) / 2 ||
         window_bounds.top != work_area.top + ((work_area.bottom - work_area.top) - height) / 2)) {
        fprintf(stderr, "FAIL: %s initial placement is not centered\n", label);
        return 1;
    }
    printf("PASS: %s fits the window and all controls\n", label);
    return 0;
}

int main(void) {
    RECT suggested = {700, -450, 1220, -200};
    int failed = 0;
    failed |= check_case("high DPI on a narrow work area",
                         (RECT){-1600, 100, -1100, 350}, NULL, TRUE);
    failed |= check_case("DPI change on a height-constrained monitor",
                         (RECT){300, -500, 1100, -280}, &suggested, TRUE);
    failed |= check_case("large work area keeps the requested DPI",
                         (RECT){0, 0, 1920, 1040}, NULL, FALSE);
    return failed;
}
""".replace("__HELPERS__", helpers)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "tray_custom_dialog_work_area.c"
    binary = Path(tmp) / "tray_custom_dialog_work_area"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
