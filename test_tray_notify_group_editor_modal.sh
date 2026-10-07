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


def extract_case(function, case, next_case):
    match = re.search(
        r"\bcase\s+" + re.escape(case) + r"\s*:(.*?)\bcase\s+"
        + re.escape(next_case) + r"\s*:",
        function,
        re.S,
    )
    assert match, f"could not find {case} branch before {next_case}"
    return "case " + case + ":" + match.group(1)


edit_dialog = extract_function("show_notify_group_edit_dialog")
edit_proc = extract_function("notify_group_edit_proc")
destroy_case = extract_case(edit_proc, "WM_DESTROY", "WM_CLOSE")
edit_save_case = extract_case(
    edit_proc, "IDC_NOTIFY_GROUP_EDIT_OK", "IDC_NOTIFY_GROUP_EDIT_CANCEL"
)
settings_proc = extract_function("settings_dialog_proc")
notifications_tab = extract_function("create_notifications_tab")
set_active_case = extract_case(
    settings_proc, "IDC_NOTIFY_SET_ACTIVE", "IDC_NOTIFY_DEL_GROUP"
)

assert "NotifyGroupManager updated = *edit_mgr;" in edit_save_case, (
    "group edits must be saved from a candidate manager so failed saves can be retried"
)
assert re.search(
    r"if\s*\(notify_groups_save\(&updated\)\)\s*\{\s*"
    r"\*edit_mgr\s*=\s*updated;\s*success\s*=\s*true;",
    edit_save_case,
    re.S,
), "the editor must commit and report success only after the group save succeeds"
assert "notify_groups_save(&updated)" in set_active_case and (
    "settings_tray->notify_groups = updated;" in set_active_case
), "Set Active must commit the candidate manager only after a successful save"
# Count only the original Set Active/Delete actions, not the new restore action.
legacy_actions = settings_proc[settings_proc.index("case IDC_NOTIFY_SET_ACTIVE:"):]
assert legacy_actions.count("if (notify_groups_save(&updated))") == 2 and (
    legacy_actions.count("settings_tray->notify_groups = updated;") == 2
), "Set Active and Delete must both gate their in-memory changes on save success"
assert re.search(
    r'"Changes are saved immediately; Settings Cancel does\\n"\s*'
    r'"not undo notification group changes\."',
    notifications_tab,
), "the Notifications tab must explain that Settings Cancel does not undo saved group changes"

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void* HANDLE;
typedef HANDLE (*DialogDpiContextFn)(HANDLE);
typedef struct { int left, top, right, bottom; } RECT;
static HANDLE enter_dialog_dpi_context(DialogDpiContextFn* setter) {
    *setter = NULL;
    return NULL;
}
typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HCURSOR;
typedef void* HBRUSH;
typedef void* HMENU;
typedef void* HFONT;
static int GetWindowRect(HWND hwnd, RECT* rect) {
    (void)hwnd;
    *rect = (RECT){0, 0, 460, 400};
    return 1;
}

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LONG_PTR;
typedef long LRESULT;
typedef int BOOL;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    WNDPROC lpfnWndProc;
    HINSTANCE hInstance;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    const char* lpszClassName;
} WNDCLASS;

typedef struct {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
} MSG;

typedef struct NotifyGroupManager NotifyGroupManager;
static struct {
    NotifyGroupManager* mgr;
    int group_index;
    HWND hwnd_parent;
} g_notify_edit_ctx;

static NotifyGroupManager* edit_mgr;
static int edit_index;
static bool exists[3];
static bool enabled[3];
static HWND active_window;
static MSG queue[8];
static int queue_count;
static int failures;
static bool owner_was_disabled_at_create;
static bool creation_fails;
static int editor_destroy_count;

#define CALLBACK
#define TRUE 1
#define FALSE 0
#define WS_POPUP 0x0001u
#define WS_CAPTION 0x0002u
#define WS_SYSMENU 0x0004u
#define DS_MODALFRAME 0x0008u
#define CW_USEDEFAULT 0
#define IDC_ARROW "arrow"
#define COLOR_BTNFACE 15
#define SW_SHOW 5
#define WM_CLOSE 0x0010u
#define WM_COMMAND 0x0111u
#define WM_DESTROY 0x0002u
#define WM_QUIT 0x0012u
#define GWLP_USERDATA (-21)

static LRESULT CALLBACK notify_group_edit_proc(HWND hwnd, UINT msg,
                                                WPARAM wParam, LPARAM lParam) {
    (void)hwnd; (void)msg; (void)wParam; (void)lParam;
    return 0;
}

static void expect(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int index_of(HWND hwnd) {
    return (int)(uintptr_t)hwnd;
}

static void enqueue(MSG msg) {
    expect(queue_count < (int)(sizeof(queue) / sizeof(queue[0])), "message queue has capacity");
    if (queue_count < (int)(sizeof(queue) / sizeof(queue[0]))) queue[queue_count++] = msg;
}

static void PostQuitMessage(int exit_code) {
    enqueue((MSG){ NULL, WM_QUIT, (WPARAM)exit_code, 0 });
}

static HINSTANCE GetModuleHandle(const char* name) {
    (void)name;
    return (HINSTANCE)(uintptr_t)1;
}

static HCURSOR LoadCursor(HINSTANCE instance, const char* name) {
    (void)instance;
    (void)name;
    return (HCURSOR)(uintptr_t)1;
}

static unsigned short RegisterClass(const WNDCLASS* wc) {
    (void)wc;
    return 1;
}

static BOOL UnregisterClass(const char* name, HINSTANCE instance) {
    (void)name;
    (void)instance;
    return TRUE;
}

static BOOL IsWindow(HWND hwnd) {
    int index = index_of(hwnd);
    return index > 0 && index < (int)(sizeof(exists) / sizeof(exists[0])) && exists[index];
}

static BOOL IsWindowEnabled(HWND hwnd) {
    int index = index_of(hwnd);
    return index > 0 && index < (int)(sizeof(enabled) / sizeof(enabled[0])) && enabled[index];
}

static BOOL EnableWindow(HWND hwnd, BOOL enable) {
    int index = index_of(hwnd);
    BOOL was_enabled = IsWindowEnabled(hwnd);
    if (index > 0 && index < (int)(sizeof(enabled) / sizeof(enabled[0]))) enabled[index] = enable != FALSE;
    return was_enabled;
}

static HWND SetActiveWindow(HWND hwnd) {
    HWND previous = active_window;
    active_window = hwnd;
    return previous;
}

static HWND CreateWindowEx(DWORD ex_style, const char* class_name, const char* title,
                           DWORD style, int x, int y, int width, int height,
                           HWND parent, HMENU menu, HINSTANCE instance, void* param) {
    (void)ex_style; (void)title; (void)style; (void)x; (void)y;
    (void)width; (void)height; (void)menu; (void)instance; (void)param;
    if (creation_fails) return NULL;
    expect(strcmp(class_name, "NoSleepNotifyGroupEditDialog") == 0,
           "the editor window class is created");
    owner_was_disabled_at_create = !IsWindowEnabled(parent);
    exists[2] = true;
    return (HWND)(uintptr_t)2;
}

static BOOL ShowWindow(HWND hwnd, int command) {
    (void)hwnd; (void)command;
    return TRUE;
}

static int GetMessage(MSG* msg, HWND hwnd, UINT min_filter, UINT max_filter) {
    (void)min_filter; (void)max_filter;
    for (int i = 0; i < queue_count; ++i) {
        if (hwnd && queue[i].hwnd != hwnd) continue;
        *msg = queue[i];
        for (int j = i; j + 1 < queue_count; ++j) queue[j] = queue[j + 1];
        --queue_count;
        return msg->message == WM_QUIT ? 0 : 1;
    }
    fprintf(stderr, "FAIL: nested pump waited without a queued test message\n");
    ++failures;
    return -1;
}

static BOOL IsDialogMessage(HWND hwnd, MSG* msg) {
    (void)hwnd; (void)msg;
    return FALSE;
}

static BOOL TranslateMessage(const MSG* msg) {
    (void)msg;
    return TRUE;
}

static BOOL DestroyWindow(HWND hwnd);
static LRESULT DispatchMessage(const MSG* msg) {
    if (msg->hwnd == (HWND)(uintptr_t)1 && msg->message == WM_COMMAND) {
        // A queued Settings Cancel is already in flight when the nested pump starts.
        DestroyWindow(msg->hwnd);
        PostQuitMessage(17);
    } else if (msg->hwnd == (HWND)(uintptr_t)2 && msg->message == WM_CLOSE) {
        DestroyWindow(msg->hwnd);
    }
    return 0;
}

static LONG_PTR GetWindowLongPtr(HWND hwnd, int index) {
    (void)hwnd; (void)index;
    return 0;
}

static BOOL DeleteObject(HFONT font) {
    (void)font;
    return TRUE;
}

static BOOL DestroyWindow(HWND hwnd) {
    int index = index_of(hwnd);
    if (index <= 0 || index >= (int)(sizeof(exists) / sizeof(exists[0])) || !exists[index]) {
        return FALSE;
    }
    exists[index] = false;
    if (index == 2) {
        ++editor_destroy_count;
        switch (WM_DESTROY) {
            __EDIT_DESTROY_CASE__
        }
    }
    return TRUE;
}

__SHOW_EDITOR_FUNCTION__

static void reset(void) {
    memset(exists, 0, sizeof(exists));
    memset(enabled, 0, sizeof(enabled));
    memset(queue, 0, sizeof(queue));
    queue_count = 0;
    failures = 0;
    owner_was_disabled_at_create = false;
    creation_fails = false;
    editor_destroy_count = 0;
    active_window = (HWND)(uintptr_t)1;
    exists[1] = true;
    enabled[1] = true;
}

int main(void) {
    reset();
    enqueue((MSG){ (HWND)(uintptr_t)1, WM_COMMAND, 0, 0 });
    show_notify_group_edit_dialog((HWND)(uintptr_t)1,
                                  (NotifyGroupManager*)(uintptr_t)1, -1);
    expect(owner_was_disabled_at_create,
           "Settings is disabled before the nested editor is created");
    expect(!IsWindow((HWND)(uintptr_t)2),
           "Settings destruction during the nested pump closes the editor");
    expect(editor_destroy_count == 1,
           "owner cancellation destroys the editor exactly once");
    expect(queue_count == 1 && queue[0].message == WM_QUIT && queue[0].wParam == 17,
           "the owner quit code remains queued for the outer Settings loop");
    expect(!IsWindow((HWND)(uintptr_t)1),
           "the Settings owner remains destroyed after its queued Cancel is dispatched");
    if (failures != 0) return 1;

    reset();
    enqueue((MSG){ (HWND)(uintptr_t)2, WM_CLOSE, 0, 0 });
    show_notify_group_edit_dialog((HWND)(uintptr_t)1,
                                  (NotifyGroupManager*)(uintptr_t)1, -1);
    expect(owner_was_disabled_at_create,
           "Settings is disabled before a normal editor close");
    expect(!IsWindow((HWND)(uintptr_t)2), "a normal editor close destroys its window");
    expect(editor_destroy_count == 1, "a normal editor close destroys the window once");
    expect(IsWindowEnabled((HWND)(uintptr_t)1),
           "the Settings owner is restored after the editor closes");
    expect(queue_count == 0,
           "closing the editor does not post WM_QUIT to the Settings loop");
    if (failures != 0) return 1;

    reset();
    creation_fails = true;
    show_notify_group_edit_dialog((HWND)(uintptr_t)1,
                                  (NotifyGroupManager*)(uintptr_t)1, -1);
    expect(IsWindowEnabled((HWND)(uintptr_t)1),
           "a failed editor creation restores the Settings owner");
    if (failures != 0) return 1;

    puts("PASS: notification editor and Settings owner close coherently");
    return 0;
}
'''

harness = harness.replace("__EDIT_DESTROY_CASE__", destroy_case).replace(
    "__SHOW_EDITOR_FUNCTION__", edit_dialog
)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_notify_group_editor_modal.c"
    binary = Path(tmp) / "test_tray_notify_group_editor_modal"
    source.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
