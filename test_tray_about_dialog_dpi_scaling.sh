#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PYTEST'
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()
signature = "static LRESULT CALLBACK about_dialog_proc("
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
            about_proc = tray[start:index + 1]
            break
else:
    raise AssertionError("unterminated function: about_dialog_proc")

harness = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define WINAPI
#define CALLBACK

typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HMENU;
typedef void* HFONT;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef intptr_t LONG_PTR;
typedef int BOOL;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { void* lpCreateParams; } CREATESTRUCT;
typedef struct {
    HWND hwnd;
    struct { UINT uID; } nid;
    BOOL menu_invoked_by_keyboard;
} NoSleepTray;

#define TRUE 1
#define FALSE 0
#define WM_CREATE 1
#define WM_COMMAND 2
#define WM_DESTROY 3
#define WM_CLOSE 4
#define WM_NCDESTROY 5
#define WM_DPICHANGED 6
#define GWLP_USERDATA (-21)
#define IDCANCEL 2
#define WS_CHILD 0x40000000
#define WS_VISIBLE 0x10000000
#define WS_TABSTOP 0x00010000
#define WS_DISABLED 0x08000000
#define BS_PUSHBUTTON 0x00000000
#define BS_DEFPUSHBUTTON 0x00000001
#define IDC_ABOUT_CHECK_UPDATES 3001
#define IDC_ABOUT_OK 3002
#define IDC_ABOUT_UPDATE_STATUS 3003
#define CURRENT_VERSION "test"
#define LOWORD(value) ((UINT)((uintptr_t)(value) & 0xffff))
#define HIWORD(value) ((UINT)(((uintptr_t)(value) >> 16) & 0xffff))

static HWND about_dialog_hwnd;
static char manual_update_status[128];
static BOOL update_check_visible;
static int initialized_width;
static int initialized_height;
static int initialize_calls;
static int scale_calls;
static UINT scaled_dpi;
static const RECT* scaled_suggested;
static int freed_layout_calls;

static HINSTANCE GetModuleHandle(const char* name) {
    (void)name;
    return (HINSTANCE)1;
}

static HWND CreateWindowEx(DWORD ex_style, const char* class_name,
                           const char* text, DWORD style, int x, int y,
                           int width, int height, HWND parent, HMENU menu,
                           HINSTANCE instance, void* param) {
    (void)ex_style; (void)class_name; (void)text; (void)style;
    (void)x; (void)y; (void)width; (void)height; (void)parent;
    (void)menu; (void)instance; (void)param;
    return (HWND)3;
}

static HFONT create_dialog_font(int size) {
    (void)size;
    return (HFONT)4;
}

static BOOL CALLBACK set_child_font_proc(HWND child, LPARAM param) {
    (void)child; (void)param;
    return TRUE;
}

static BOOL EnumChildWindows(HWND parent, BOOL (CALLBACK *callback)(HWND, LPARAM),
                             LPARAM param) {
    (void)parent; (void)callback; (void)param;
    return TRUE;
}

static LONG_PTR SetWindowLongPtr(HWND hwnd, int index, LONG_PTR value) {
    (void)hwnd; (void)index;
    return value;
}

static LONG_PTR GetWindowLongPtr(HWND hwnd, int index) {
    (void)hwnd; (void)index;
    return (LONG_PTR)4;
}

static BOOL initialize_dialog_layout(HWND hwnd, int width, int height) {
    (void)hwnd;
    initialize_calls++;
    initialized_width = width;
    initialized_height = height;
    return TRUE;
}

static void scale_dialog_layout(HWND hwnd, UINT dpi, const RECT* suggested) {
    (void)hwnd;
    scale_calls++;
    scaled_dpi = dpi;
    scaled_suggested = suggested;
}

static void free_dialog_layout(HWND hwnd) {
    (void)hwnd;
    freed_layout_calls++;
}

static void center_about_dialog_on_invoking_monitor(HWND hwnd, HWND owner,
                                                    UINT icon_id, BOOL keyboard) {
    (void)hwnd; (void)owner; (void)icon_id; (void)keyboard;
}

static void tray_check_for_updates(NoSleepTray* tray, BOOL silent) {
    (void)tray; (void)silent;
}

static BOOL DestroyWindow(HWND hwnd) { (void)hwnd; return TRUE; }
static BOOL DeleteObject(HFONT font) { (void)font; return TRUE; }
static LRESULT DefWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)hwnd; (void)msg; (void)wParam; (void)lParam;
    return -1;
}

__ABOUT_PROC__

int main(void) {
    NoSleepTray tray = { (HWND)11, {42}, TRUE };
    CREATESTRUCT create = { &tray };
    about_dialog_proc((HWND)10, WM_CREATE, 0, (LPARAM)&create);

    if (initialize_calls != 1 || initialized_width != 380 ||
        initialized_height != 240) {
        fprintf(stderr,
                "FAIL: About dialog did not initialize its shared 380x240 baseline layout\n");
        return 1;
    }

    RECT suggested = { 100, 200, 860, 680 };
    about_dialog_proc((HWND)10, WM_DPICHANGED, (WPARAM)(192UL << 16),
                      (LPARAM)&suggested);
    if (scale_calls != 1 || scaled_dpi != 192 ||
        scaled_suggested != &suggested) {
        fprintf(stderr,
                "FAIL: About dialog did not apply the suggested 192-DPI layout\n");
        return 1;
    }

    about_dialog_proc((HWND)10, WM_NCDESTROY, 0, 0);
    if (freed_layout_calls != 1) {
        fprintf(stderr, "FAIL: About dialog did not release its shared layout\n");
        return 1;
    }
    printf("PASS: About dialog initializes and reapplies shared DPI layout\n");
    return 0;
}
"""

source_text = harness.replace("__ABOUT_PROC__", about_proc)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "about_dialog_dpi_scaling.c"
    binary = Path(tmp) / "about_dialog_dpi_scaling"
    source.write_text(source_text)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PYTEST
