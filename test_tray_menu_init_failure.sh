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
tray_source = Path(os.environ.get("TRAY_SOURCE", root / "src/tray.c"))
tray = tray_source.read_text()


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", tray)
    assert definition, f"could not find definition for {signature}"
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
    raise AssertionError(f"unterminated function: {signature}")


prelude = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long long UINT_PTR;
typedef long LRESULT;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HMENU;
typedef void *HICON;
typedef void *HINSTANCE;
typedef void *HCURSOR;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef struct { int unused; } NotifyGroupManager;
typedef enum {
    SESSION_FINISHED_NONE,
    SESSION_FINISHED_SHUTDOWN,
    SESSION_FINISHED_SLEEP,
    SESSION_FINISHED_SHUTDOWN_GRACEFUL
} SessionFinishedAction;
enum { NOTIFY_EVENT_APP_START = 1 };
typedef struct {
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    HICON hIcon;
    char szTip[128];
    UINT uVersion;
} NOTIFYICONDATA;
typedef struct {
    LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM);
    HINSTANCE hInstance;
    const char *lpszClassName;
    HCURSOR hCursor;
} WNDCLASS;
typedef struct NoSleepTray {
    HWND hwnd;
    HMENU hmenu;
    HICON hIconDefault;
    HICON hIconCountdownBlank;
    NOTIFYICONDATA nid;
    SessionFinishedAction session_finished_action;
    int notification_mode;
    bool startup_cli_overrides_set;
    int notification_mode_cli_override;
    int auto_check_interval_cli_override;
    int check_updates_startup_cli_override;
    int auto_check_interval;
    bool start_on_startup;
    bool add_to_path;
    bool check_updates_on_startup;
    UINT uTrayMessage;
    NotifyGroupManager notify_groups;
} NoSleepTray;

#define CALLBACK
#define DEBUG_PRINT(...) ((void)0)
#define DEBUG_LOG(...) ((void)0)
#define IDC_ARROW ((const char *)1)
#define TRAY_WINDOW_CLASS "nosleep_tray_window"
#define WS_OVERLAPPEDWINDOW 0x00cf0000u
#define CW_USEDEFAULT ((int)0x80000000u)
#define TRAY_ICON_MESSAGE_ID 1000
#define NIF_ICON 1u
#define NIF_MESSAGE 2u
#define NIF_TIP 4u
#define NIM_ADD 1u
#define NIM_SETVERSION 4u
#define MF_STRING 0x0000u
#define MF_GRAYED 0x0001u
#define MF_POPUP 0x0010u
#define MF_SEPARATOR 0x0800u
#define MF_CHECKED 0x0008u
#define MF_UNCHECKED 0x0000u
#define IDM_START_30MIN 1001u
#define IDM_START_1HOUR 1002u
#define IDM_START_2HOURS 1003u
#define IDM_START_CUSTOM 1004u
#define IDM_START_INDEFINITE 1005u
#define IDM_STOP 1006u
#define IDM_EXIT 1007u
#define IDM_SESSION_FINISHED_NONE 1009u
#define IDM_SESSION_FINISHED_SHUTDOWN 1010u
#define IDM_SESSION_FINISHED_SLEEP 1011u
#define IDM_SESSION_FINISHED_SHUTDOWN_GRACEFUL 1016u
#define IDM_SETTINGS 1014u
#define IDM_CHECK_UPDATES 1015u
#define IDM_REVIEW_UPDATE 1017u
#define IDM_ABOUT 1012u

static unsigned int menu_create_calls;
static unsigned int menu_destroy_calls;
static unsigned int window_destroy_calls;
static unsigned int class_unregister_calls;
static unsigned int icon_cleanup_calls;
static unsigned int shell_notify_calls;
static unsigned int settings_load_calls;
static unsigned int update_timer_calls;
static unsigned int fail_menu_call;
static int menu_handles[3];
static unsigned int menu_destroyed[3];
static int window_handle;
static int module_handle;
static int cursor_handle;

static void tray_create_menu(NoSleepTray *tray);

static LRESULT CALLBACK tray_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)hwnd; (void)msg; (void)wParam; (void)lParam;
    return 0;
}
static HINSTANCE GetModuleHandle(const char *name) { (void)name; return &module_handle; }
static HCURSOR LoadCursor(HINSTANCE instance, const char *name) {
    (void)instance; (void)name; return &cursor_handle;
}
static UINT RegisterClass(const WNDCLASS *wc) { (void)wc; return 1; }
static HWND CreateWindowEx(DWORD ex_style, const char *class_name,
                           const char *window_name, DWORD style,
                           int x, int y, int width, int height,
                           HWND parent, void *menu, HINSTANCE instance,
                           void *parameter) {
    (void)ex_style; (void)class_name; (void)window_name; (void)style;
    (void)x; (void)y; (void)width; (void)height; (void)parent;
    (void)menu; (void)instance; (void)parameter;
    return &window_handle;
}
static int DestroyWindow(HWND hwnd) {
    if (hwnd == &window_handle) ++window_destroy_calls;
    return 1;
}
static int UnregisterClass(const char *name, HINSTANCE instance) {
    (void)name; (void)instance; ++class_unregister_calls; return 1;
}
static HMENU CreatePopupMenu(void) {
    unsigned int call = ++menu_create_calls;
    if (call == fail_menu_call) return NULL;
    return &menu_handles[call - 1];
}
static int DestroyMenu(HMENU menu) {
    for (unsigned int i = 0; i < 3; ++i) {
        if (menu == &menu_handles[i]) ++menu_destroyed[i];
    }
    ++menu_destroy_calls; return 1;
}
static int AppendMenu(HMENU menu, UINT flags, UINT_PTR item, const char *text) {
    (void)menu; (void)flags; (void)item; (void)text; return 1;
}
static UINT RegisterWindowMessage(const char *name) { (void)name; return 123; }
static int Shell_NotifyIcon(DWORD operation, NOTIFYICONDATA *nid) {
    (void)operation; (void)nid; ++shell_notify_calls; return 1;
}
static bool is_startup_enabled(void) { return false; }
static void tray_load_settings(NoSleepTray *tray) { (void)tray; ++settings_load_calls; }
static void tray_update_session_finished_menu(NoSleepTray *tray) { (void)tray; }
static bool apply_path_preference(bool add_to_path) { (void)add_to_path; return true; }
static void notify_groups_init(NotifyGroupManager *groups, int mode) {
    (void)groups; (void)mode;
}
static bool notify_groups_set_active(NotifyGroupManager *groups, int mode) {
    (void)groups; (void)mode; return true;
}
static void tray_show_notification(NoSleepTray *tray, int event_type,
                                   const char *title, const char *message,
                                   bool error) {
    (void)tray; (void)event_type; (void)title; (void)message; (void)error;
}
static bool should_check_for_updates(void) { return false; }
static void tray_check_for_updates(NoSleepTray *tray, bool startup) {
    (void)tray; (void)startup;
}
static void tray_setup_update_timer(NoSleepTray *tray) {
    (void)tray; ++update_timer_calls;
}
static void tray_create_icons(NoSleepTray *tray) {
    tray->hIconDefault = (HICON)1;
    tray->hIconCountdownBlank = (HICON)2;
}
static void tray_destroy_icons(NoSleepTray *tray) {
    ++icon_cleanup_calls;
    tray->hIconDefault = NULL;
    tray->hIconCountdownBlank = NULL;
}
"""

main = r"""
static int check_menu_failure(unsigned int fail_at, unsigned int expected_menu_destroys) {
    NoSleepTray tray;
    memset(&tray, 0, sizeof(tray));
    menu_create_calls = 0;
    menu_destroy_calls = 0;
    memset(menu_destroyed, 0, sizeof(menu_destroyed));
    window_destroy_calls = 0;
    class_unregister_calls = 0;
    icon_cleanup_calls = 0;
    shell_notify_calls = 0;
    settings_load_calls = 0;
    update_timer_calls = 0;
    fail_menu_call = fail_at;

    if (tray_init(&tray)) {
        fprintf(stderr, "FAIL: menu creation failure at call %u reported initialized\n", fail_at);
        return 1;
    }
    if (menu_destroy_calls != expected_menu_destroys || tray.hmenu != NULL) {
        fprintf(stderr, "FAIL: menu creation failure at call %u left menu cleanup inconsistent\n", fail_at);
        return 1;
    }
    for (unsigned int i = 0; i < fail_at - 1; ++i) {
        if (menu_destroyed[i] != 1) {
            fprintf(stderr, "FAIL: menu %u was not destroyed exactly once after failure %u\n", i + 1, fail_at);
            return 1;
        }
    }
    if (window_destroy_calls != 1 || tray.hwnd != NULL || class_unregister_calls != 1) {
        fprintf(stderr, "FAIL: menu creation failure at call %u left its window class resources alive\n", fail_at);
        return 1;
    }
    if (icon_cleanup_calls != 1 || tray.hIconDefault != NULL || tray.hIconCountdownBlank != NULL) {
        fprintf(stderr, "FAIL: menu creation failure at call %u did not clean up created icons\n", fail_at);
        return 1;
    }
    if (shell_notify_calls != 0 || settings_load_calls != 0 || update_timer_calls != 0) {
        fprintf(stderr, "FAIL: menu creation failure at call %u continued tray initialization\n", fail_at);
        return 1;
    }
    return 0;
}

int main(void) {
    if (check_menu_failure(1, 0) != 0) return 1;
    if (check_menu_failure(2, 1) != 0) return 1;
    if (check_menu_failure(3, 2) != 0) return 1;
    puts("PASS: tray initialization fails and releases resources when required menu creation fails");
    return 0;
}
"""

functions = "\n\n".join(
    extract_function(signature)
    for signature in (
        "bool tray_init(NoSleepTray* tray)",
        "static void tray_create_menu(NoSleepTray* tray)",
    )
)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_tray_menu_init_failure.c"
    binary = Path(tmp) / "test_tray_menu_init_failure"
    source.write_text(prelude + "\n" + functions + "\n" + main)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
