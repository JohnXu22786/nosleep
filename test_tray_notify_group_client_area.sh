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
end = proc.index("// Apply font", start)
layout = proc[start:end]
count = int(re.search(r"NOTIFY_EVENT_COUNT\s*=\s*(\d+)", (root / "src/notify_groups.h").read_text()).group(1))
harness = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef int HWND;
typedef int HINSTANCE;
typedef intptr_t HMENU;
typedef struct { int left, top, right, bottom; } RECT;
#define WS_CHILD 1
#define WS_VISIBLE 2
#define WS_TABSTOP 4
#define BS_DEFPUSHBUTTON 8
#define IDC_NOTIFY_GROUP_EDIT_OK 101
#define IDC_NOTIFY_GROUP_EDIT_CANCEL 102
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define SWP_NOMOVE 1
#define SWP_NOZORDER 2
#define FALSE 0
#define NOTIFY_EVENT_COUNT __COUNT__
static int frame_x, frame_y, client_width, client_height, buttons;
static int button_right[2], button_bottom[2];
static int GetWindowLongPtr(HWND hwnd, int index) { (void)hwnd; return index == GWL_STYLE ? 0x123 : 0x456; }
static void* GetMenu(HWND hwnd) { (void)hwnd; return 0; }
static int AdjustWindowRectEx(RECT* rc, unsigned long style, int menu, unsigned long exstyle) {
    if (style != 0x123 || exstyle != 0x456 || menu != 0) exit(2);
    rc->left -= frame_x; rc->right += frame_x;
    rc->top -= frame_y; rc->bottom += frame_x;
    return 1;
}
static int GetWindowRect(HWND hwnd, RECT* rc) { (void)hwnd; *rc = (RECT){0,0,460,326}; return 1; }
static int CreateWindowEx(int exstyle, const char* cls, const char* text, int style,
                         int x, int y, int w, int h, HWND hwnd, HMENU id, HINSTANCE inst, void* param) {
    (void)exstyle; (void)cls; (void)text; (void)style; (void)hwnd; (void)inst; (void)param;
    if (id != IDC_NOTIFY_GROUP_EDIT_OK && id != IDC_NOTIFY_GROUP_EDIT_CANCEL) exit(3);
    button_right[buttons] = x + w; button_bottom[buttons++] = y + h;
    return 1;
}
static int SetWindowPos(HWND hwnd, void* after, int x, int y, int w, int h, int flags) {
    (void)hwnd; (void)after; (void)x; (void)y; (void)flags;
    client_width = w - 2 * frame_x; client_height = h - frame_y - frame_x;
    return 1;
}
static void layout(void) {
    HWND hwnd = 1; HINSTANCE hInst = 1;
    int y = 122;
    __LAYOUT__
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
harness = harness.replace("__COUNT__", str(count)).replace("__LAYOUT__", layout)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "layout.c"
    binary = Path(tmp) / "layout"
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
PYTEST
