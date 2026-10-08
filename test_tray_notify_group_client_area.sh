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
proc = tray[tray.index("static LRESULT CALLBACK notify_group_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {"):]
start = proc.index("// Calculate dialog size based on number of events")
end = proc.index("return 0;", start)
layout = proc[start:end]
size_start = tray.index("static BOOL dialog_window_bounds_for_dpi(")
size_brace = tray.index("{", size_start)
size_depth = 0
for size_end in range(size_brace, len(tray)):
    if tray[size_end] == "{":
        size_depth += 1
    elif tray[size_end] == "}":
        size_depth -= 1
        if size_depth == 0:
            size_end += 1
            break
sizing = tray[size_start:size_end]

count = int(re.search(r"NOTIFY_EVENT_COUNT\s*=\s*(\d+)", (root / "src/notify_groups.h").read_text()).group(1))
harness = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef int HWND;
typedef int HINSTANCE;
typedef intptr_t HMENU;
typedef struct { int left, top, right, bottom; } RECT;
typedef RECT* LPRECT;
typedef unsigned int UINT;
typedef int BOOL;
#define WS_CHILD 1
#define WS_VISIBLE 2
#define WS_TABSTOP 4
#define BS_DEFPUSHBUTTON 8
#define IDC_NOTIFY_GROUP_EDIT_OK 101
#define IDC_NOTIFY_GROUP_EDIT_CANCEL 102
#define WINAPI
#define TRUE 1
#define FALSE 0
typedef unsigned long DWORD;
static int MulDiv(int value, int numerator, int denominator) { return value * numerator / denominator; }
#define NOTIFY_EVENT_COUNT __COUNT__
static int frame_x, frame_y, client_width, client_height, buttons;
static int button_right[2], button_bottom[2];
static void* GetModuleHandle(const char* name) { (void)name; return NULL; }
static void* GetProcAddress(void* module, const char* name) {
    (void)module; (void)name; return NULL;
}
static int AdjustWindowRectEx(RECT* rc, DWORD style, BOOL menu, DWORD exstyle) {
    if (style != 0x123 || exstyle != 0x456 || menu != 0) exit(2);
    rc->left -= frame_x; rc->right += frame_x;
    rc->top -= frame_y; rc->bottom += frame_x;
    return 1;
}
__SIZING__
static int CreateWindowEx(int exstyle, const char* cls, const char* text, int style,
                         int x, int y, int w, int h, HWND hwnd, HMENU id, HINSTANCE inst, void* param) {
    (void)exstyle; (void)cls; (void)text; (void)style; (void)hwnd; (void)inst; (void)param;
    if (id != IDC_NOTIFY_GROUP_EDIT_OK && id != IDC_NOTIFY_GROUP_EDIT_CANCEL) exit(3);
    button_right[buttons] = x + w; button_bottom[buttons++] = y + h;
    return 1;
}
static int initialize_dialog_layout(HWND hwnd, int width, int height) {
    (void)hwnd;
    RECT bounds;
    if (!dialog_window_bounds_for_dpi(width, height, 96, 96, 0x123, 0x456, &bounds)) return 0;
    client_width = bounds.right - bounds.left - 2 * frame_x;
    client_height = bounds.bottom - bounds.top - frame_y - frame_x;
    return 1;
}
static int layout(void) {
    HWND hwnd = 1; HINSTANCE hInst = 1;
    int y = 122;
    __LAYOUT__
    return 0;
}
int main(void) {
    const int frames[][2] = {{4,27},{8,31},{12,45}};
    for (unsigned i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
        frame_x = frames[i][0]; frame_y = frames[i][1]; buttons = 0;
        layout();
        for (int j = 0; j < buttons; ++j) {
            if (button_right[j] > client_width || button_bottom[j] + 12 > client_height) {
                fprintf(stderr, "FAIL: action button clipped or missing bottom margin\n"); return 1;
            }
        }
        if (buttons != 2 || client_width < 445) return 1;
    }
    puts("PASS: notification editor action buttons fit the client area across frame sizes");
}
"""
harness = harness.replace("__COUNT__", str(count)).replace("__LAYOUT__", layout).replace("__SIZING__", sizing)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "layout.c"
    binary = Path(tmp) / "layout"
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
PYTEST
