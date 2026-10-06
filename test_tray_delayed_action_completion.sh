#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMP_DIR/test_tray_delayed_action_completion.c" <<'PY'
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:void|bool|DWORD\s+WINAPI|ULONGLONG)\s+"
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


sleep_thread = extract_function("delayed_sleep_thread")
shutdown_thread = extract_function("delayed_shutdown_thread")
reap_handle = extract_function("tray_reap_delayed_action_handle")
stop = extract_function("tray_stop_nosleep_for_session")
stop_snapshot = stop[stop.index("    HANDLE sleep_timer = tray->sleep_timer;"):stop.index("    ATOMIC_STORE_BOOL(&tray->is_running, false);")]
stop_snapshot = "static void boundary_stop(NoSleepTray *tray) {\n" + stop_snapshot + "\n    boundary_pending = was_delayed_action_pending;\n}"


prelude = r'''#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include "tray_stop_guard.h"

typedef int SessionFinishedAction;
typedef unsigned int DWORD;
typedef unsigned long long ULONGLONG;
typedef void *HANDLE;
typedef void *LPVOID;
typedef pthread_mutex_t SRWLOCK;

#define WINAPI
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define ATOMIC_LOAD_BOOL(value) __atomic_load_n((value), __ATOMIC_SEQ_CST)
#define DEBUG_LOG(...) ((void)0)

typedef struct NoSleepTray {
    SRWLOCK delayed_action_lock;
    bool stopping;
    bool sleep_action_claimed;
    bool shutdown_action_claimed;
    int countdown_action;
    bool delayed_sleep_countdown_active;
    HANDLE sleep_timer;
    HANDLE sleep_stop_event;
    HANDLE shutdown_timer;
    HANDLE shutdown_stop_event;
} NoSleepTray;

static int sleep_action_calls;
static int shutdown_action_calls;
static bool sleep_action_called_without_lock;
static bool shutdown_action_called_without_lock;
static int sleep_handle_closes;
static int shutdown_handle_closes;
static int sleep_handle_closes_under_lock;
static int shutdown_handle_closes_under_lock;
static int sleep_handle_token;
static int shutdown_handle_token;
static HANDLE signaled_stop_event;
static __thread unsigned int delayed_action_lock_depth;
static bool boundary_pending;
static bool stop_at_dispatch;
static bool stop_before_claim;
static NoSleepTray *boundary_tray;
static void boundary_stop(NoSleepTray *tray);
enum { SESSION_FINISHED_SLEEP = 1, SESSION_FINISHED_SHUTDOWN = 2 };
void SetEvent(HANDLE event) { signaled_stop_event = event; }


void AcquireSRWLockExclusive(SRWLOCK *lock) {
    pthread_mutex_lock(lock);
    ++delayed_action_lock_depth;
}
void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    --delayed_action_lock_depth;
    pthread_mutex_unlock(lock);
    if (stop_at_dispatch && boundary_tray) {
        stop_at_dispatch = false;
        pthread_mutex_lock(lock);
        boundary_stop(boundary_tray);
        pthread_mutex_unlock(lock);
    }
}
ULONGLONG GetTickCount64(void) { return 1; }
ULONGLONG get_elapsed_milliseconds(ULONGLONG start) {
    (void)start;
    return 60 * 1000;
}
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    (void)milliseconds;
    if (handle == signaled_stop_event) return WAIT_OBJECT_0;
    return WAIT_TIMEOUT;
}
bool CloseHandle(HANDLE handle) {
    if (handle == &sleep_handle_token) {
        ++sleep_handle_closes;
        if (delayed_action_lock_depth != 0) ++sleep_handle_closes_under_lock;
    }
    if (handle == &shutdown_handle_token) {
        ++shutdown_handle_closes;
        if (delayed_action_lock_depth != 0) ++shutdown_handle_closes_under_lock;
    }
    return true;
}
void tray_start_countdown(NoSleepTray *tray, int action) {
    (void)tray;
    (void)action;
}
void tray_stop_countdown(NoSleepTray *tray) {
    if (stop_before_claim) {
        stop_before_claim = false;
        pthread_mutex_lock(&tray->delayed_action_lock);
        boundary_stop(tray);
        pthread_mutex_unlock(&tray->delayed_action_lock);
    }
}
void trigger_system_sleep(NoSleepTray *tray) {
    (void)tray;
    ++sleep_action_calls;
    sleep_action_called_without_lock = delayed_action_lock_depth == 0;
}
void trigger_system_shutdown(NoSleepTray *tray) {
    (void)tray;
    ++shutdown_action_calls;
    shutdown_action_called_without_lock = delayed_action_lock_depth == 0;
}

'''

harness = r'''
static int test_sleep_completion(void) {
    NoSleepTray tray = {0};
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.sleep_timer = &sleep_handle_token;
    tray.sleep_stop_event = &tray;

    if (delayed_sleep_thread(&tray) != 0) return 1;
    if (sleep_action_calls != 1 || !sleep_action_called_without_lock) {
        fprintf(stderr, "FAIL: sleep action did not return naturally outside the delayed-action lock\n");
        return 1;
    }
    if (tray.sleep_timer != NULL || sleep_handle_closes != 1 ||
        sleep_handle_closes_under_lock != 1) {
        fprintf(stderr, "FAIL: natural sleep completion left its thread handle pending\n");
        return 1;
    }
    if (tray_stop_has_work(false, false, tray.sleep_timer != NULL,
                           tray.shutdown_timer != NULL)) {
        fprintf(stderr, "FAIL: completed sleep action still counts as Stop work\n");
        return 1;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return 0;
}

static int test_shutdown_completion(void) {
    NoSleepTray tray = {0};
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.shutdown_timer = &shutdown_handle_token;
    tray.shutdown_stop_event = &tray;

    if (delayed_shutdown_thread(&tray) != 0) return 1;
    if (shutdown_action_calls != 1 || !shutdown_action_called_without_lock) {
        fprintf(stderr, "FAIL: shutdown action did not return naturally outside the delayed-action lock\n");
        return 1;
    }
    if (tray.shutdown_timer != NULL || shutdown_handle_closes != 1 ||
        shutdown_handle_closes_under_lock != 1) {
        fprintf(stderr, "FAIL: natural shutdown completion left its thread handle pending\n");
        return 1;
    }
    if (tray_stop_has_work(false, false, tray.sleep_timer != NULL,
                           tray.shutdown_timer != NULL)) {
        fprintf(stderr, "FAIL: completed shutdown action still counts as Stop work\n");
        return 1;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return 0;
}

static int test_sleep_cancellation_reaps_handle(void) {
    NoSleepTray tray = {0};
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.sleep_timer = &sleep_handle_token;
    tray.sleep_stop_event = &tray;
    signaled_stop_event = tray.sleep_stop_event;
    int action_calls_before = sleep_action_calls;

    if (delayed_sleep_thread(&tray) != 0) return 1;
    signaled_stop_event = NULL;
    if (sleep_action_calls != action_calls_before || sleep_handle_closes != 2 ||
        sleep_handle_closes_under_lock != 2 || tray.sleep_timer != NULL) {
        fprintf(stderr, "FAIL: cancelled sleep worker did not reap its handle under lock\n");
        return 1;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return 0;
}

static int test_shutdown_cancellation_reaps_handle(void) {
    NoSleepTray tray = {0};
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.shutdown_timer = &shutdown_handle_token;
    tray.shutdown_stop_event = &tray;
    signaled_stop_event = tray.shutdown_stop_event;
    int action_calls_before = shutdown_action_calls;

    if (delayed_shutdown_thread(&tray) != 0) return 1;
    signaled_stop_event = NULL;
    if (shutdown_action_calls != action_calls_before || shutdown_handle_closes != 2 ||
        shutdown_handle_closes_under_lock != 2 || tray.shutdown_timer != NULL) {
        fprintf(stderr, "FAIL: cancelled shutdown worker did not reap its handle under lock\n");
        return 1;
    }
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return 0;
}

static int test_dispatch_boundary(bool shutdown, bool cancel_first) {
    NoSleepTray tray = {0};
    pthread_mutex_init(&tray.delayed_action_lock, NULL);
    tray.sleep_stop_event = &tray.sleep_stop_event;
    tray.shutdown_stop_event = &tray.shutdown_stop_event;
    if (shutdown) tray.shutdown_timer = &shutdown_handle_token;
    else tray.sleep_timer = &sleep_handle_token;
    int calls_before = shutdown ? shutdown_action_calls : sleep_action_calls;
    boundary_tray = &tray;
    stop_before_claim = cancel_first;
    stop_at_dispatch = !cancel_first;
    signaled_stop_event = NULL;
    if (shutdown) delayed_shutdown_thread(&tray);
    else delayed_sleep_thread(&tray);
    boundary_tray = NULL;
    int calls_after = shutdown ? shutdown_action_calls : sleep_action_calls;
    if (boundary_pending != cancel_first ||
        (signaled_stop_event != NULL) != cancel_first ||
        calls_after - calls_before != (cancel_first ? 0 : 1)) {
        fprintf(stderr, "FAIL: cancellation and dispatch disagreed at the boundary (%s, cancel_first=%d)\n",
                shutdown ? "shutdown" : "sleep", cancel_first);
        return 1;
    }
    signaled_stop_event = NULL;
    pthread_mutex_destroy(&tray.delayed_action_lock);
    return 0;
}

int main(void) {
    if (test_sleep_completion() || test_shutdown_completion() ||
        test_sleep_cancellation_reaps_handle() || test_shutdown_cancellation_reaps_handle()) return 1;
    if (test_dispatch_boundary(false, false) || test_dispatch_boundary(true, false) ||
        test_dispatch_boundary(false, true) || test_dispatch_boundary(true, true)) return 1;
    puts("PASS: delayed action completion and cancellation reap their handles");
    return 0;
}
'''

output.write_text(prelude + "\n" + stop_snapshot + "\n" + reap_handle + "\n" + sleep_thread + "\n" + shutdown_thread + "\n" + harness)
command = shlex.split(os.environ.get("CC", "cc")) + [
    "-std=c99", "-Wall", "-Wextra", "-pthread", f"-I{root / 'src'}",
    str(output), "-o", str(output.with_suffix("")),
]
subprocess.run(command, check=True)
subprocess.run([str(output.with_suffix(""))], check=True)
PY
