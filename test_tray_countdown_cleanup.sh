#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
python3 - "$SCRIPT_DIR/src/tray.c" "$TMP_DIR/test.c" <<'PYCODE'
from pathlib import Path
import re
import sys
source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])
def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", source)
    assert definition, f"could not find definition for {signature}"
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
    raise AssertionError(f"unterminated function: {signature}")



prelude = r"""
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef void *HANDLE;
typedef unsigned long DWORD;
#define INFINITE 0xffffffffu
#define DEBUG_LOG(...) do {} while (0)
#define MEMORY_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
typedef struct {
    bool countdown_stopping;
    bool delayed_sleep_countdown_active;
    HANDLE countdown_timer_thread;
    HANDLE countdown_stop_event;
} NoSleepTray;
static NoSleepTray *active_tray;
static pthread_barrier_t entry_barrier;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int returned, waits, closes, resets, icons, menus;
static bool release_wait;
static _Thread_local bool first_guard = true;
/* Force both old load/store callers to observe the same false guard value.
 * The replacement exchange instead claims ownership at this same rendezvous. */
static bool load_bool(bool *value) {
    bool snapshot = __atomic_load_n(value, __ATOMIC_SEQ_CST);
    if (value == &active_tray->countdown_stopping && first_guard) {
        first_guard = false;
        pthread_barrier_wait(&entry_barrier);
    }
    return snapshot;
}
static bool exchange_bool(bool *value, bool desired) {
    if (first_guard) {
        first_guard = false;
        pthread_barrier_wait(&entry_barrier);
    }
    return __atomic_exchange_n(value, desired, __ATOMIC_SEQ_CST);
}
#define ATOMIC_LOAD_BOOL(value) load_bool(value)
#define ATOMIC_STORE_BOOL(value, desired) __atomic_store_n(value, desired, __ATOMIC_SEQ_CST)
#define ATOMIC_EXCHANGE_BOOL(value, desired) exchange_bool(value, desired)
static bool tray_countdown_has_work(bool active, bool thread_present) {
    return active || thread_present;
}
static void SetEvent(HANDLE event) { assert(event == (HANDLE)(uintptr_t)2); }
static void ResetEvent(HANDLE event) {
    assert(event == (HANDLE)(uintptr_t)2);
    pthread_mutex_lock(&lock); resets++; pthread_mutex_unlock(&lock);
}
static DWORD WaitForSingleObject(HANDLE handle, DWORD duration) {
    assert(handle == (HANDLE)(uintptr_t)1 && duration == INFINITE);
    pthread_mutex_lock(&lock);
    waits++;
    pthread_cond_broadcast(&changed);
    while (!release_wait) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    return 0;
}
static void CloseHandle(HANDLE handle) {
    assert(handle == (HANDLE)(uintptr_t)1);
    pthread_mutex_lock(&lock); closes++; pthread_mutex_unlock(&lock);
}
static void tray_update_icon(NoSleepTray *tray) {
    (void)tray; pthread_mutex_lock(&lock); icons++; pthread_mutex_unlock(&lock);
}
static void tray_update_stop_menu_item(NoSleepTray *tray) {
    (void)tray; pthread_mutex_lock(&lock); menus++; pthread_mutex_unlock(&lock);
}
"""
tail = r"""
static void *stop(void *argument) {
    tray_stop_countdown(argument);
    pthread_mutex_lock(&lock);
    returned++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return NULL;
}
int main(void) {
    NoSleepTray tray = {false, true, (HANDLE)(uintptr_t)1, (HANDLE)(uintptr_t)2};
    active_tray = &tray;
    pthread_barrier_init(&entry_barrier, NULL, 2);
    pthread_t callers[2];
    assert(pthread_create(&callers[0], NULL, stop, &tray) == 0);
    assert(pthread_create(&callers[1], NULL, stop, &tray) == 0);
    pthread_mutex_lock(&lock);
    while (waits + returned < 2) pthread_cond_wait(&changed, &lock);
    /* The loser must return without touching the owner's live handle/event. */
    bool single_owner = waits == 1 && returned == 1 && closes == 0 && resets == 0;
    release_wait = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    pthread_join(callers[0], NULL);
    pthread_join(callers[1], NULL);
    if (!single_owner || waits != 1 || closes != 1 || resets != 1 ||
        icons != 1 || menus != 1 || tray.countdown_timer_thread != NULL ||
        tray.countdown_stopping || tray.delayed_sleep_countdown_active) {
        fprintf(stderr, "FAIL: overlapping countdown stops shared cleanup ownership\n");
        return 1;
    }
    /* An idle stop must release ownership so a later completed thread is reaped. */
    first_guard = false;
    tray_stop_countdown(&tray);
    assert(!tray.countdown_stopping && closes == 1);
    tray.countdown_timer_thread = (HANDLE)(uintptr_t)1;
    tray_stop_countdown(&tray);
    assert(waits == 2 && closes == 2 && tray.countdown_timer_thread == NULL);
    assert(!tray.countdown_stopping);
    puts("PASS: overlapping countdown stops have one cleanup owner and guard is reusable");
    return 0;
}
"""
output.write_text(prelude + extract_function("void tray_stop_countdown(NoSleepTray* tray)") + tail)
PYCODE
"${CC:-cc}" -std=c11 -Wall -Wextra -pthread "$TMP_DIR/test.c" -o "$TMP_DIR/test"
timeout 10 "$TMP_DIR/test"
