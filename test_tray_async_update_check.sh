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
source = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:bool|void|DWORD\s+WINAPI)\s+" +
        re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        source,
        re.S,
    )
    assert definition, f"could not find definition for {name}"
    start = definition.start()
    opening = definition.end() - 1

    depth = 0
    state = "code"
    i = opening
    while i < len(source):
        char = source[i]
        next_two = source[i : i + 2]
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
                    return source[start : i + 1]
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


task_struct = re.search(
    r"struct\s+TrayUpdateCheckTask\s*\{.*?\};", source, re.S
)
assert task_struct, "could not find TrayUpdateCheckTask definition"

functions = "\n\n".join(
    extract_function(name)
    for name in (
        "tray_update_check_begin",
        "tray_update_check_end",
        "tray_update_check_worker",
        "tray_prompt_available_update",
        "tray_process_update_check_result",
        "tray_check_for_updates",
        "tray_handle_update_check_complete",
        "tray_wait_for_update_check",
    )
)

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *HANDLE;
typedef void *HWND;
typedef void *LPVOID;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef intptr_t LPARAM;
typedef int NotifyEventId;
#define WINAPI
typedef DWORD (WINAPI *ThreadStart)(LPVOID);

#define WM_APP 0x8000
#define WM_TRAY_UPDATE_CHECK_COMPLETE (WM_APP + 2)
#define WM_CLOSE 0x0010
#define INFINITE 0xffffffffUL
#define WAIT_OBJECT_0 0
#define CURRENT_VERSION "1.0.0"
#define MB_OK 0x00000000
#define MB_ICONERROR 0x00000010
#define MB_TOPMOST 0x00040000
#define MEMORY_BARRIER() ((void)0)
#define DEBUG_LOG(...) ((void)0)
#define NOTIFY_EVENT_UPDATE_CHECK_FAILED 1
#define NOTIFY_EVENT_UPDATE_CHECK_COMPLETED 2
#define NOTIFY_EVENT_UPDATE_AVAILABLE 3
#define IDM_REVIEW_UPDATE 1017
#define MF_BYCOMMAND 0x0000
#define MF_GRAYED 0x0001
#define MF_ENABLED 0x0000

typedef struct {
    char latest_version[64];
    char download_url[512];
    char tag_name[64];
    bool update_available;
} UpdateInfo;

typedef struct NoSleepTray NoSleepTray;
typedef struct TrayUpdateCheckTask TrayUpdateCheckTask;

struct NoSleepTray {
    HWND hwnd;
    void *hmenu;
    TrayUpdateCheckTask *update_check_task;
    UpdateInfo available_update;
};

static DWORD current_thread_id = 1;
static ThreadStart pending_worker;
static void *pending_worker_parameter;
static int create_thread_count;
static bool fail_create_thread;
static int fail_post_message_count;
static bool window_exists = true;
static int cancel_count;
static int wait_count;
static int close_count;
static int post_count;
static UINT last_post_message;
static LPARAM last_post_parameter;
static DWORD last_post_thread_id;
static int updater_check_count;
static bool updater_check_result;
static bool updater_update_available;
static HWND updater_parent;
static DWORD updater_thread_id;
static int notification_count;
static NotifyEventId last_notification;
static DWORD notification_thread_id;
static int prompt_count;
static int compare_result;
static bool prompt_answer;
static bool check_again_during_prompt;
static int download_count;
static int save_time_count;
static int message_box_count;
static NoSleepTray *current_tray;
static HANDLE fake_thread_handle = (HANDLE)(uintptr_t)0x1234;

static bool tray_update_check_begin(void);
static void tray_update_check_end(void);
static DWORD WINAPI tray_update_check_worker(LPVOID parameter);
static void tray_process_update_check_result(NoSleepTray *tray, bool silent,
                                              bool check_ok, UpdateInfo *info);
static void tray_check_for_updates(NoSleepTray *tray, bool silent);
static void tray_handle_update_check_complete(NoSleepTray *tray,
                                               TrayUpdateCheckTask *task);
static bool tray_wait_for_update_check(NoSleepTray *tray);
static bool update_check_in_progress;

''' + task_struct.group(0) + r'''

static HANDLE CreateThread(void *attributes, size_t stack_size, ThreadStart start,
                           LPVOID parameter, DWORD flags, DWORD *thread_id) {
    (void)attributes;
    (void)stack_size;
    (void)flags;
    ++create_thread_count;
    if (fail_create_thread) return NULL;
    pending_worker = start;
    pending_worker_parameter = parameter;
    if (thread_id) *thread_id = 2;
    return fake_thread_handle;
}

static BOOL PostMessage(HWND hwnd, UINT message, uintptr_t wparam, LPARAM lparam) {
    (void)hwnd;
    (void)wparam;
    ++post_count;
    last_post_message = message;
    last_post_parameter = lparam;
    last_post_thread_id = current_thread_id;
    if (fail_post_message_count > 0) {
        --fail_post_message_count;
        return 0;
    }
    return window_exists;
}

static BOOL IsWindow(HWND hwnd) { return hwnd && window_exists; }
static void Sleep(DWORD milliseconds) { (void)milliseconds; }

static DWORD GetCurrentThreadId(void) { return current_thread_id; }

static BOOL CancelSynchronousIo(HANDLE thread) {
    (void)thread;
    ++cancel_count;
    return 1;
}

static DWORD WaitForSingleObject(HANDLE handle, DWORD timeout) {
    (void)timeout;
    ++wait_count;
    if (handle == fake_thread_handle && pending_worker) {
        ThreadStart worker = pending_worker;
        void *parameter = pending_worker_parameter;
        pending_worker = NULL;
        pending_worker_parameter = NULL;
        current_thread_id = 2;
        worker(parameter);
        current_thread_id = 1;
    }
    return WAIT_OBJECT_0;
}

static BOOL CloseHandle(HANDLE handle) {
    (void)handle;
    ++close_count;
    return 1;
}

static bool updater_check(UpdateInfo *info, HWND hwnd_parent) {
    ++updater_check_count;
    updater_thread_id = current_thread_id;
    updater_parent = hwnd_parent;
    if (info && updater_update_available) {
        info->update_available = true;
        strcpy(info->latest_version, "2.0.0");
        strcpy(info->download_url, "https://example.test/nosleep.exe");
    }
    return updater_check_result;
}

static void save_last_update_check_time(void) { ++save_time_count; }

static void tray_show_notification(NoSleepTray *tray, NotifyEventId event_type,
                                   const char *title, const char *message, bool critical) {
    (void)tray;
    (void)title;
    (void)message;
    (void)critical;
    ++notification_count;
    last_notification = event_type;
    notification_thread_id = current_thread_id;
}

static int updater_compare_versions(const char *latest, const char *current) {
    (void)latest;
    (void)current;
    return compare_result;
}

static bool updater_show_prompt_dialog(HWND hwnd, UpdateInfo *info) {
    (void)hwnd;
    (void)info;
    ++prompt_count;
    if (check_again_during_prompt) tray_check_for_updates(current_tray, false);
    return prompt_answer;
}

static wchar_t *get_exe_path_w(void) { return NULL; }

static bool updater_download_and_install(UpdateInfo *info, const wchar_t *path, HWND hwnd) {
    (void)info;
    (void)path;
    (void)hwnd;
    ++download_count;
    return false;
}

static UINT EnableMenuItem(void *menu, UINT item, UINT flags) {
    (void)menu; (void)item; (void)flags;
    return 0;
}

static int MessageBox(HWND hwnd, const char *message, const char *title, unsigned int flags) {
    (void)hwnd;
    (void)message;
    (void)title;
    (void)flags;
    ++message_box_count;
    return 0;
}

static void run_pending_worker(void) {
    ThreadStart worker = pending_worker;
    void *parameter = pending_worker_parameter;
    pending_worker = NULL;
    pending_worker_parameter = NULL;
    current_thread_id = 2;
    worker(parameter);
    current_thread_id = 1;
}

static int fail(const char *scenario) {
    fprintf(stderr, "FAIL: %s\n", scenario);
    return 1;
}

''' + functions + r'''

int main(void) {
    NoSleepTray tray = {0};
    tray.hwnd = (HWND)(uintptr_t)0x99;
    current_tray = &tray;

    updater_check_result = true;
    tray_check_for_updates(&tray, false);
    if (create_thread_count != 1 || updater_check_count != 0 || post_count != 0 ||
        notification_count != 0 || !update_check_in_progress || !tray.update_check_task) {
        return fail("manual checks must return before network work or UI handling starts");
    }
    tray_check_for_updates(&tray, false);
    if (create_thread_count != 1) return fail("a duplicate check must be suppressed");

    run_pending_worker();
    if (updater_check_count != 1 || updater_thread_id != 2 || updater_parent != NULL ||
        post_count != 1 || last_post_message != WM_TRAY_UPDATE_CHECK_COMPLETE ||
        last_post_parameter != (LPARAM)tray.update_check_task || last_post_thread_id != 2 ||
        notification_count != 0 || prompt_count != 0 || message_box_count != 0) {
        return fail("the worker must only check and post its result to the tray thread");
    }
    tray_handle_update_check_complete(&tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (tray.update_check_task || update_check_in_progress || save_time_count != 1 ||
        notification_count != 1 || last_notification != NOTIFY_EVENT_UPDATE_CHECK_COMPLETED ||
        notification_thread_id != 1 || prompt_count != 0 || close_count != 1) {
        return fail("a successful no-update result must be handled on the tray thread");
    }

    updater_check_result = false;
    tray_check_for_updates(&tray, true);
    fail_post_message_count = 1;
    int posts_before_retry = post_count;
    run_pending_worker();
    if (post_count != posts_before_retry + 2 ||
        last_post_message != WM_TRAY_UPDATE_CHECK_COMPLETE) {
        return fail("a transient result-post failure must be retried");
    }
    tray_handle_update_check_complete(&tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (update_check_in_progress || notification_count != 1 || save_time_count != 2) {
        return fail("silent failures must save the check time without notifying the user");
    }

    updater_check_result = true;
    updater_update_available = true;
    compare_result = 1;
    prompt_answer = false;
    check_again_during_prompt = true;
    int creates_before_update_prompt = create_thread_count;
    tray_check_for_updates(&tray, false);
    run_pending_worker();
    tray_handle_update_check_complete(&tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != 2 || last_notification != NOTIFY_EVENT_UPDATE_AVAILABLE ||
        notification_thread_id != 1 || prompt_count != 1 ||
        create_thread_count != creates_before_update_prompt + 1 || download_count != 0 ||
        update_check_in_progress) {
        return fail("an update prompt must remain on the tray thread and suppress nested checks");
    }

    fail_create_thread = true;
    updater_update_available = false;
    int notifications_before_create_failure = notification_count;
    tray_check_for_updates(&tray, false);
    if (tray.update_check_task || update_check_in_progress || save_time_count != 4 ||
        notification_count != notifications_before_create_failure + 1 ||
        last_notification != NOTIFY_EVENT_UPDATE_CHECK_FAILED) {
        return fail("thread-creation failure must release suppression and report a manual failure");
    }

    fail_create_thread = false;
    fail_post_message_count = 1;
    window_exists = false;
    tray_check_for_updates(&tray, true);
    if (!tray.update_check_task) return fail("a running check must be retained for shutdown cleanup");
    int notifications_before_shutdown = notification_count;
    int closes_before_shutdown = close_count;
    int cancels_before_shutdown = cancel_count;
    int waits_before_shutdown = wait_count;
    int posts_before_shutdown = post_count;
    if (!tray_wait_for_update_check(&tray) || tray.update_check_task ||
        wait_count != waits_before_shutdown + 1 || close_count != closes_before_shutdown + 1 ||
        cancel_count != cancels_before_shutdown + 1 ||
        post_count != posts_before_shutdown + 1 ||
        notification_count != notifications_before_shutdown) {
        return fail("shutdown must join and release an unreported check without touching UI");
    }

    puts("PASS: update checks run asynchronously, return results to the tray thread, and clean up on shutdown");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    test_source = Path(tmp) / "test_tray_async_update_check.c"
    binary = Path(tmp) / "test_tray_async_update_check"
    test_source.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", str(test_source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
