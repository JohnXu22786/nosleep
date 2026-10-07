#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:void|LRESULT\s+CALLBACK)\s+"
        + re.escape(name)
        + r"\s*\([^;]*?\)\s*\{",
        tray,
        re.S,
    )
    assert definition, f"could not find definition for {name}"
    start = definition.start()
    opening = definition.end() - 1

    depth = 0
    state = "code"
    i = opening
    while i < len(tray):
        char = tray[i]
        next_two = tray[i : i + 2]
        if state == "code":
            if next_two == "//":
                state = "line_comment"
                i += 2
                continue
            if next_two == "/*":
                state = "block_comment"
                i += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return tray[start : i + 1]
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if next_two == "*/":
                state = "code"
                i += 2
                continue
        elif state in ("string", "char"):
            if char == "\\":
                i += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function: {name}")


functions = '\n'.join(extract_function(n) for n in ('tray_show_settings_dialog', 'tray_show_about_dialog'))
guard = re.search(r'^static bool tray_dialog_open[^;]*;', tray, re.M)
harness = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void* HANDLE;
typedef HANDLE (*DialogDpiContextFn)(HANDLE);
typedef struct { int left, top, right, bottom; } RECT;
static HANDLE enter_dialog_dpi_context(DialogDpiContextFn* setter) {
    *setter = NULL;
    return NULL;
}
typedef void* HWND;
static int GetWindowRect(HWND hwnd, RECT* rect) {
    (void)hwnd;
    *rect = (RECT){0, 0, 490, 440};
    return 1;
}
typedef void* HINSTANCE;
typedef void* HBRUSH;
typedef void* LPVOID;
typedef struct { HWND hwnd; } NoSleepTray;
typedef struct { unsigned long wParam; } MSG;
typedef struct { void* lpfnWndProc; HINSTANCE hInstance; void* hCursor; HBRUSH hbrBackground; const char* lpszClassName; } WNDCLASS;
#define IDC_ARROW 0
#define COLOR_BTNFACE 0
#define WS_POPUP 0
#define WS_CAPTION 0
#define WS_SYSMENU 0
#define DS_MODALFRAME 0
#define CW_USEDEFAULT 0
#define SW_SHOW 0
static void* settings_dialog_proc;
static void* about_dialog_proc;
void tray_show_settings_dialog(NoSleepTray*);
void tray_show_about_dialog(NoSleepTray*);
static NoSleepTray tray_instance;
static int creates, pumps, update_checks, depth, phase;
static bool fail_create, dialog_alive;
static NoSleepTray* about_context;
static void tray_check_for_updates(NoSleepTray*, bool);
static HINSTANCE GetModuleHandle(void* x) { return x; }
static void* LoadCursor(void* x, int id) { (void)id; return x; }
static int RegisterClass(WNDCLASS* x) { (void)x; return 1; }
static int UnregisterClass(const char* x, HINSTANCE y) { (void)x; (void)y; return 1; }
static HWND CreateWindowEx(int e,const char* c,const char* t,int s,int x,int y,int w,int h,HWND owner,void* menu,HINSTANCE i,LPVOID p) {
 (void)e;(void)t;(void)s;(void)x;(void)y;(void)w;(void)h;(void)owner;(void)menu;(void)i;
 if (++creates > 1) { fputs("FAIL: nested open created another dialog and overwrote state\n",stderr); exit(1); }
 if (fail_create) return NULL;
 dialog_alive = true;
 if (strstr(c,"About")) {
  about_context=p;
  if (about_context != &tray_instance) exit(6);
 }
 // Synchronous WM_CREATE can dispatch another tray action too.
 tray_show_settings_dialog(&tray_instance); tray_show_about_dialog(&tray_instance);
 return (HWND)1;
}
static int ShowWindow(HWND h,int s) { (void)h;(void)s; return 1; }
static int IsWindow(HWND h) { return h != NULL && dialog_alive; }
static int DestroyWindow(HWND h) { (void)h; dialog_alive=false; return 1; }
static void PostQuitMessage(int code) { (void)code; }
static int GetMessage(MSG* m, HWND h,int a,int b) { m->wParam=0;(void)h;(void)a;(void)b; return pumps++ == 0; }
static int IsDialogMessage(HWND h,MSG* m) { (void)h;(void)m;return 0; }
static int TranslateMessage(MSG* m) { (void)m; return 1; }
static int DispatchMessage(MSG* m) {
 (void)m; ++depth;
 if (depth > 1) exit(2);
 tray_show_settings_dialog(&tray_instance); tray_show_about_dialog(&tray_instance);
 // Original About action must still reach its original tray context.
 if (phase == 1 && about_context) tray_check_for_updates(about_context,false);
 DestroyWindow((HWND)1);
 --depth; return 0;
}
static void tray_check_for_updates(NoSleepTray* t,bool s) { (void)t;(void)s; ++update_checks; }
__GUARD__
__FUNCTIONS__
int main(void) {
 tray_show_settings_dialog(NULL); tray_show_about_dialog(NULL);
 tray_show_settings_dialog(&tray_instance);
 if (creates != 1) return 3;
 creates=pumps=0; phase=1;
 tray_show_about_dialog(&tray_instance);
 if (creates != 1 || update_checks != 1) return 4;
 creates=pumps=0; fail_create=true;
 tray_show_about_dialog(&tray_instance);
 creates=pumps=0; fail_create=false; phase=0;
 tray_show_settings_dialog(&tray_instance);
 if (creates != 1) return 5;
 puts("PASS: nested dialog opens preserve original state and release the guard");
 return 0;
}
"""
harness=harness.replace('__GUARD__',guard.group(0) if guard else '').replace('__FUNCTIONS__',functions)
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
PY
