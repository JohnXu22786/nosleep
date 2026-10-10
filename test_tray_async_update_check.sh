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

shutdown_check = extract_function("tray_update_check_shutdown_requested").replace(
    "tray_update_check_shutdown_requested",
    "tray_update_check_shutdown_requested_source",
    1,
)
worker = extract_function("tray_update_check_worker").replace(
    "tray_update_check_shutdown_requested(task)",
    "tray_update_check_shutdown_requested_for_test(task)",
)
functions = "\n\n".join(
    [
        shutdown_check,
        r'''static bool use_cached_shutdown_read;
static bool cached_shutdown_read;

static bool tray_update_check_shutdown_requested_for_test(TrayUpdateCheckTask *task) {
    if (use_cached_shutdown_read) {
        use_cached_shutdown_read = false;
        return cached_shutdown_read;
    }
    return tray_update_check_shutdown_requested_source(task);
}''',
        worker,
    ]
    + [
        extract_function(name)
        for name in (
            "tray_update_check_begin",
            "tray_update_check_end",
            "tray_set_manual_update_status",
            "tray_prompt_available_update",
            "tray_review_available_update",
            "tray_process_update_check_result",
            "tray_check_for_updates",
            "tray_handle_update_check_complete",
            "tray_wait_for_update_check",
        )
    ]
)

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *HANDLE;
typedef void *HWND;
typedef void *LPVOID;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int32_t LONG;
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
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffffUL
#define CURRENT_VERSION "1.0.0"
#define MB_OK 0x00000000
#define MB_ICONERROR 0x00000010
#define MB_TOPMOST 0x00040000
#define MEMORY_BARRIER() ((void)0)
#define ATOMIC_STORE_BOOL(destination, value) \
    __atomic_store_n((destination), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_STORE_INT(destination, value) \
    __atomic_store_n((destination), (value), __ATOMIC_SEQ_CST)
#define DEBUG_LOG(...) ((void)0)
#define NOTIFY_EVENT_UPDATE_CHECK_FAILED 1
#define NOTIFY_EVENT_UPDATE_CHECK_COMPLETED 2
#define NOTIFY_EVENT_UPDATE_AVAILABLE 3
#define IDM_REVIEW_UPDATE 1017
#define IDC_ABOUT_UPDATE_STATUS 3003
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

static __thread DWORD current_thread_id = 1;
static ThreadStart pending_worker;
static void *pending_worker_parameter;
static TrayUpdateCheckTask *shutdown_task_after_sleep;
static int shutdown_after_sleep_count;
static int retry_sleep_count;
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
static bool simulate_late_request;
static int cancel_count_before_race;
static bool late_request_started_after_cancel;
static bool late_request_cancelled;
static bool late_request_cancelled_while_active;
static bool late_request_active;
static pthread_mutex_t race_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_changed = PTHREAD_COND_INITIALIZER;
static pthread_t race_worker_thread;
static bool race_worker_started;
static bool race_worker_finished;
static ThreadStart race_worker_start;
static void *race_worker_parameter;
static bool updater_release_found;
static bool updater_update_available;
static HWND updater_parent;
static DWORD updater_thread_id;
static int notification_count;
static NotifyEventId last_notification;
static char last_notification_title[128];
static char last_notification_message[512];
static DWORD notification_thread_id;
static int prompt_count;
static int release_page_prompt_count;
static bool prompt_answer;
static bool check_again_during_prompt;
static int download_count;
static int save_time_count;
static int message_box_count;
static char updater_latest_version[64] = "2.0.0";
static char updater_download_url[512] = "https://example.test/nosleep.exe";
static UINT last_menu_item;
static UINT last_menu_flags;
static NoSleepTray *current_tray;
static HANDLE fake_thread_handle = (HANDLE)(uintptr_t)0x1234;
static HWND about_dialog_hwnd;
static char manual_update_status[128];
static char last_status_text[128];
static int last_status_control;
static int status_update_count;

static void *run_race_worker(void *unused) {
    (void)unused;
    current_thread_id = 2;
    race_worker_start(race_worker_parameter);
    current_thread_id = 1;
    pthread_mutex_lock(&race_lock);
    race_worker_finished = true;
    pthread_cond_broadcast(&race_changed);
    pthread_mutex_unlock(&race_lock);
    return NULL;
}

static struct timespec race_deadline_after_ms(unsigned long milliseconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static bool tray_update_check_begin(void);
static void tray_update_check_end(void);
static void tray_set_manual_update_status(const char *status);
static DWORD WINAPI tray_update_check_worker(LPVOID parameter);
static void tray_process_update_check_result(NoSleepTray *tray, bool silent,
                                              bool check_ok, UpdateInfo *info);
static void tray_check_for_updates(NoSleepTray *tray, bool silent);
static void tray_handle_update_check_complete(NoSleepTray *tray,
                                               TrayUpdateCheckTask *task);
static bool tray_wait_for_update_check(NoSleepTray *tray);
static bool update_check_in_progress;
static void updater_prompt_release_page(HWND hwnd_parent, const UpdateInfo *info);
static int update_timer_setup_count;
static bool update_timer_use_remaining_interval;

// Button presentation is outside this fixture's asynchronous worker scope.
static void tray_set_update_check_visible(NoSleepTray *tray, bool checking) {
    (void)tray;
    (void)checking;
}

static void tray_setup_update_timer(NoSleepTray *tray, bool use_remaining_interval) {
    (void)tray;
    ++update_timer_setup_count;
    update_timer_use_remaining_interval = use_remaining_interval;
}

static BOOL SetDlgItemText(HWND hwnd, int control, const char *text) {
    (void)hwnd;
    if (control != IDC_ABOUT_UPDATE_STATUS) return 0;
    last_status_control = control;
    snprintf(last_status_text, sizeof(last_status_text), "%s", text ? text : "");
    ++status_update_count;
    return 1;
}

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
static void Sleep(DWORD milliseconds) {
    (void)milliseconds;
    ++retry_sleep_count;
    if (shutdown_task_after_sleep &&
        retry_sleep_count == shutdown_after_sleep_count) {
        ATOMIC_STORE_INT(&shutdown_task_after_sleep->shutdown_requested, true);
    }
}

static DWORD GetCurrentThreadId(void) { return current_thread_id; }

static BOOL CancelSynchronousIo(HANDLE thread) {
    (void)thread;
    if (simulate_late_request) pthread_mutex_lock(&race_lock);
    ++cancel_count;
    if (simulate_late_request && late_request_active &&
        late_request_started_after_cancel) {
        late_request_cancelled = true;
        late_request_cancelled_while_active = true;
        pthread_cond_broadcast(&race_changed);
    }
    if (simulate_late_request) pthread_mutex_unlock(&race_lock);
    return 1;
}

static DWORD WaitForSingleObject(HANDLE handle, DWORD timeout) {
    ++wait_count;
    if (handle == fake_thread_handle && simulate_late_request) {
        if (pending_worker) {
            race_worker_start = pending_worker;
            race_worker_parameter = pending_worker_parameter;
            pending_worker = NULL;
            pending_worker_parameter = NULL;
            race_worker_finished = false;
            if (pthread_create(&race_worker_thread, NULL, run_race_worker, NULL) != 0) {
                return WAIT_FAILED;
            }
            race_worker_started = true;
        }

        pthread_mutex_lock(&race_lock);
        int wait_result = 0;
        struct timespec startup_deadline = race_deadline_after_ms(5000);
        while (!race_worker_finished && !late_request_active && wait_result == 0) {
            wait_result = pthread_cond_timedwait(&race_changed, &race_lock,
                                                 &startup_deadline);
        }
        bool worker_finished = race_worker_finished;
        bool request_active = late_request_active;
        if (!worker_finished && !request_active) {
            pthread_mutex_unlock(&race_lock);
            return WAIT_FAILED;
        }

        DWORD effective_timeout = timeout == INFINITE ? 100 : timeout;
        struct timespec deadline = race_deadline_after_ms(effective_timeout);
        wait_result = 0;
        while (!worker_finished && wait_result == 0) {
            wait_result = pthread_cond_timedwait(&race_changed, &race_lock,
                                                 &deadline);
            worker_finished = race_worker_finished;
        }
        pthread_mutex_unlock(&race_lock);
        if (!worker_finished && wait_result != ETIMEDOUT) return WAIT_FAILED;
        if (!worker_finished) return WAIT_TIMEOUT;
        if (race_worker_started) {
            pthread_join(race_worker_thread, NULL);
            race_worker_started = false;
        }
        return WAIT_OBJECT_0;
    }

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
    if (simulate_late_request) {
        pthread_mutex_lock(&race_lock);
        late_request_started_after_cancel =
            cancel_count == cancel_count_before_race + 1;
        late_request_active = true;
        pthread_cond_broadcast(&race_changed);
        while (!late_request_cancelled) {
            pthread_cond_wait(&race_changed, &race_lock);
        }
        late_request_active = false;
        pthread_mutex_unlock(&race_lock);
    }
    ++updater_check_count;
    updater_thread_id = current_thread_id;
    updater_parent = hwnd_parent;
    if (info && updater_release_found) {
        strcpy(info->latest_version, updater_latest_version);
        if (updater_update_available) {
            info->update_available = true;
            strcpy(info->download_url, updater_download_url);
        }
    }
    return updater_check_result;
}

static void save_last_update_check_time(void) { ++save_time_count; }

static void tray_show_notification(NoSleepTray *tray, NotifyEventId event_type,
                                   const char *title, const char *message, bool critical) {
    (void)tray;
    (void)critical;
    ++notification_count;
    last_notification = event_type;
    snprintf(last_notification_title, sizeof(last_notification_title), "%s",
             title ? title : "");
    snprintf(last_notification_message, sizeof(last_notification_message), "%s",
             message ? message : "");
    notification_thread_id = current_thread_id;
}

static void updater_prompt_release_page(HWND hwnd_parent, const UpdateInfo *info) {
    (void)hwnd_parent;
    (void)info;
    ++release_page_prompt_count;
}

static int updater_compare_versions(const char *latest, const char *current) {
    int result = strcmp(latest, current);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
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
    (void)menu;
    last_menu_item = item;
    last_menu_flags = flags;
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
    about_dialog_hwnd = (HWND)(uintptr_t)0x77;

    updater_check_result = true;
    tray_check_for_updates(&tray, false);
    if (create_thread_count != 1 || updater_check_count != 0 || post_count != 0 ||
        notification_count != 0 || !update_check_in_progress || !tray.update_check_task ||
        status_update_count != 0) {
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
    if (update_timer_setup_count != 1 || !update_timer_use_remaining_interval) {
        return fail("a completed check must reschedule the automatic timer from its saved timestamp");
    }
    if (tray.update_check_task || update_check_in_progress || save_time_count != 1 ||
        notification_count != 1 || last_notification != NOTIFY_EVENT_UPDATE_CHECK_COMPLETED ||
        notification_thread_id != 1 || prompt_count != 0 || close_count != 1 ||
        tray.available_update.update_available || last_menu_item != IDM_REVIEW_UPDATE ||
        (last_menu_flags & MF_GRAYED) == 0 ||
        strcmp(manual_update_status, "Latest version: v1.0.0") != 0 ||
        last_status_control != IDC_ABOUT_UPDATE_STATUS ||
        strcmp(last_status_text, "Latest version: v1.0.0") != 0 ||
        status_update_count != 1) {
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
    if (update_check_in_progress || notification_count != 1 || save_time_count != 2 ||
        strcmp(manual_update_status, "Latest version: v1.0.0") != 0 ||
        strcmp(last_status_text, "Latest version: v1.0.0") != 0 ||
        status_update_count != 1) {
        return fail("silent failures must save the check time without notifying the user");
    }

    fail_post_message_count = 1000;
    window_exists = true;
    int posts_before_persistent_shutdown = post_count;
    tray_check_for_updates(&tray, true);
    if (!tray.update_check_task) return fail("a running check must be retained for shutdown cleanup");
    int waits_before_persistent_shutdown = wait_count;
    int cancels_before_persistent_shutdown = cancel_count;
    int checks_before_persistent_shutdown = updater_check_count;
    int notifications_before_persistent_shutdown = notification_count;
    if (!tray_wait_for_update_check(&tray) || tray.update_check_task ||
        wait_count != waits_before_persistent_shutdown + 1 ||
        cancel_count != cancels_before_persistent_shutdown + 1 ||
        updater_check_count != checks_before_persistent_shutdown ||
        post_count != posts_before_persistent_shutdown ||
        notification_count != notifications_before_persistent_shutdown ||
        update_check_in_progress) {
        return fail("shutdown must skip a not-yet-started update request before joining and releasing the task");
    }

    NoSleepTray racing_tray = {0};
    racing_tray.hwnd = (HWND)(uintptr_t)0x97;
    tray_check_for_updates(&racing_tray, true);
    TrayUpdateCheckTask *racing_task = racing_tray.update_check_task;
    cached_shutdown_read = tray_update_check_shutdown_requested_source(racing_task);
    if (cached_shutdown_read) return fail("the race fixture must cache a pre-shutdown read");
    use_cached_shutdown_read = true;
    simulate_late_request = true;
    cancel_count_before_race = cancel_count;
    int waits_before_racing_shutdown = wait_count;
    int cancels_before_racing_shutdown = cancel_count;
    int checks_before_racing_shutdown = updater_check_count;
    if (!tray_wait_for_update_check(&racing_tray) || racing_tray.update_check_task ||
        wait_count < waits_before_racing_shutdown + 2 ||
        cancel_count < cancels_before_racing_shutdown + 2 ||
        wait_count - waits_before_racing_shutdown !=
            cancel_count - cancels_before_racing_shutdown ||
        updater_check_count != checks_before_racing_shutdown + 1 ||
        !late_request_started_after_cancel || !late_request_cancelled ||
        !late_request_cancelled_while_active ||
        update_check_in_progress) {
        return fail("shutdown must cancel a request that starts after its first cancellation before joining");
    }
    simulate_late_request = false;

    fail_post_message_count = 1000;
    int posts_before_inflight_shutdown = post_count;
    tray_check_for_updates(&tray, true);
    TrayUpdateCheckTask *retrying_task = tray.update_check_task;
    shutdown_task_after_sleep = retrying_task;
    shutdown_after_sleep_count = 3;
    retry_sleep_count = 0;
    run_pending_worker();
    shutdown_task_after_sleep = NULL;
    if (post_count != posts_before_inflight_shutdown + 3 ||
        tray.update_check_task != retrying_task) {
        return fail("the worker must stop persistent retries when shutdown begins during delivery");
    }
    fail_post_message_count = 0;
    int waits_before_inflight_shutdown = wait_count;
    int notifications_before_inflight_shutdown = notification_count;
    if (!tray_wait_for_update_check(&tray) || tray.update_check_task ||
        wait_count != waits_before_inflight_shutdown + 1 ||
        notification_count != notifications_before_inflight_shutdown ||
        update_check_in_progress) {
        return fail("shutdown must join and release a worker after an in-flight retry stops");
    }

    updater_check_result = true;
    updater_release_found = true;
    updater_update_available = true;
    prompt_answer = false;
    check_again_during_prompt = true;
    int creates_before_update_prompt = create_thread_count;
    tray_check_for_updates(&tray, false);
    run_pending_worker();
    tray_handle_update_check_complete(&tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != 2 || last_notification != NOTIFY_EVENT_UPDATE_AVAILABLE ||
        notification_thread_id != 1 || prompt_count != 1 ||
        create_thread_count != creates_before_update_prompt + 1 || download_count != 0 ||
        update_check_in_progress ||
        strcmp(manual_update_status, "Version 2.0.0 is available") != 0 ||
        strcmp(last_status_text, "Version 2.0.0 is available") != 0 ||
        status_update_count != 2) {
        return fail("an update prompt must remain on the tray thread and suppress nested checks");
    }

    int notifications_before_repeat_manual = notification_count;
    tray_check_for_updates(&tray, false);
    run_pending_worker();
    tray_handle_update_check_complete(&tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != notifications_before_repeat_manual + 1 ||
        last_notification != NOTIFY_EVENT_UPDATE_AVAILABLE || prompt_count != 2 ||
        strcmp(manual_update_status, "Version 2.0.0 is available") != 0 ||
        strcmp(last_status_text, "Version 2.0.0 is available") != 0 ||
        status_update_count != 3) {
        return fail("an explicit check must still notify and prompt for the same available version");
    }

    NoSleepTray automatic_tray = {0};
    automatic_tray.hwnd = (HWND)(uintptr_t)0x98;
    automatic_tray.hmenu = (void *)(uintptr_t)0x42;
    strcpy(updater_latest_version, "2.0.0");
    strcpy(updater_download_url, "https://example.test/nosleep-2.0.0.exe");
    int notifications_before_automatic = notification_count;
    tray_check_for_updates(&automatic_tray, true);
    run_pending_worker();
    tray_handle_update_check_complete(&automatic_tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != notifications_before_automatic + 1 ||
        last_notification != NOTIFY_EVENT_UPDATE_AVAILABLE ||
        !automatic_tray.available_update.update_available ||
        strcmp(automatic_tray.available_update.latest_version, "2.0.0") != 0 ||
        strcmp(automatic_tray.available_update.download_url, updater_download_url) != 0 ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) != 0 ||
        strcmp(manual_update_status, "Version 2.0.0 is available") != 0 ||
        strcmp(last_status_text, "Version 2.0.0 is available") != 0 ||
        status_update_count != 3) {
        return fail("the first automatic discovery must notify and keep the update cached and reviewable");
    }

    strcpy(updater_download_url, "https://example.test/nosleep-2.0.0-refreshed.exe");
    tray_check_for_updates(&automatic_tray, true);
    run_pending_worker();
    tray_handle_update_check_complete(&automatic_tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != notifications_before_automatic + 1 ||
        strcmp(automatic_tray.available_update.download_url, updater_download_url) != 0 ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) != 0) {
        return fail("a repeated automatic result must refresh the review cache without notifying again");
    }

    strcpy(updater_latest_version, "3.0.0");
    strcpy(updater_download_url, "https://example.test/nosleep-3.0.0.exe");
    tray_check_for_updates(&automatic_tray, true);
    run_pending_worker();
    tray_handle_update_check_complete(&automatic_tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != notifications_before_automatic + 2 ||
        last_notification != NOTIFY_EVENT_UPDATE_AVAILABLE ||
        strcmp(automatic_tray.available_update.latest_version, "3.0.0") != 0 ||
        strcmp(automatic_tray.available_update.download_url, updater_download_url) != 0 ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) != 0) {
        return fail("a newer automatic release must notify and replace the cached review version");
    }

    strcpy(updater_download_url, "https://example.test/nosleep-3.0.0-refreshed.exe");
    tray_check_for_updates(&automatic_tray, true);
    run_pending_worker();
    tray_handle_update_check_complete(&automatic_tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != notifications_before_automatic + 2 ||
        strcmp(automatic_tray.available_update.download_url, updater_download_url) != 0 ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) != 0) {
        return fail("a repeated check of the newer release must refresh the cache without another notification");
    }

    updater_check_result = false;
    tray_check_for_updates(&automatic_tray, true);
    run_pending_worker();
    tray_handle_update_check_complete(&automatic_tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (automatic_tray.update_check_task || !automatic_tray.available_update.update_available ||
        strcmp(automatic_tray.available_update.latest_version, "3.0.0") != 0 ||
        notification_count != notifications_before_automatic + 2 ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) != 0) {
        return fail("a failed automatic check must keep the last known review update");
    }

    updater_check_result = true;
    updater_release_found = false;
    updater_update_available = false;
    tray_check_for_updates(&automatic_tray, true);
    run_pending_worker();
    tray_handle_update_check_complete(&automatic_tray, (TrayUpdateCheckTask *)last_post_parameter);
    if (automatic_tray.update_check_task || automatic_tray.available_update.update_available ||
        notification_count != notifications_before_automatic + 2 ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) == 0 ||
        strcmp(manual_update_status, "Version 2.0.0 is available") != 0 ||
        strcmp(last_status_text, "Version 2.0.0 is available") != 0 ||
        status_update_count != 3) {
        return fail("a successful no-update result must clear and disable the cached review update");
    }

    NoSleepTray no_asset_tray = {0};
    no_asset_tray.hwnd = (HWND)(uintptr_t)0x97;
    no_asset_tray.hmenu = (void *)(uintptr_t)0x43;
    updater_release_found = true;
    strcpy(updater_latest_version, "4.0.0");
    int notifications_before_no_asset = notification_count;
    int release_prompts_before_no_asset = release_page_prompt_count;
    int statuses_before_no_asset = status_update_count;
    tray_check_for_updates(&no_asset_tray, false);
    run_pending_worker();
    tray_handle_update_check_complete(&no_asset_tray,
                                     (TrayUpdateCheckTask *)last_post_parameter);
    if (notification_count != notifications_before_no_asset + 1 ||
        last_notification != NOTIFY_EVENT_UPDATE_AVAILABLE ||
        strcmp(no_asset_tray.available_update.latest_version, "4.0.0") != 0 ||
        no_asset_tray.available_update.update_available ||
        last_menu_item != IDM_REVIEW_UPDATE || (last_menu_flags & MF_GRAYED) != 0 ||
        !strstr(last_notification_message, "no Windows installer") ||
        !strstr(last_notification_message, "Review Available Update") ||
        strcmp(last_notification_title, "Installer Unavailable") != 0 ||
        release_page_prompt_count != release_prompts_before_no_asset + 1 ||
        strcmp(manual_update_status, "Installer unavailable for v4.0.0") != 0 ||
        strcmp(last_status_text, "Installer unavailable for v4.0.0") != 0 ||
        status_update_count != statuses_before_no_asset + 1) {
        return fail("a newer release without an EXE must be successful, cached, and offer its release page");
    }

    int download_prompts_before_review = prompt_count;
    int release_prompts_before_review = release_page_prompt_count;
    tray_review_available_update(&no_asset_tray);
    if (release_page_prompt_count != release_prompts_before_review + 1 ||
        prompt_count != download_prompts_before_review || update_check_in_progress) {
        return fail("reviewing a cached no-installer release must offer its page, not the EXE download");
    }

    fail_create_thread = true;
    updater_update_available = false;
    updater_release_found = false;
    about_dialog_hwnd = (HWND)(uintptr_t)0x78;
    int notifications_before_create_failure = notification_count;
    int checks_before_create_failure = save_time_count;
    tray_check_for_updates(&tray, false);
    if (tray.update_check_task || update_check_in_progress || save_time_count != checks_before_create_failure + 1 ||
        notification_count != notifications_before_create_failure + 1 ||
        last_notification != NOTIFY_EVENT_UPDATE_CHECK_FAILED ||
        strcmp(manual_update_status, "Check failed") != 0 ||
        last_status_control != IDC_ABOUT_UPDATE_STATUS ||
        strcmp(last_status_text, "Check failed") != 0 ||
        status_update_count != 5) {
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
        post_count != posts_before_shutdown ||
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
        "-std=c99", "-Wall", "-Wextra", "-pthread", str(test_source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
