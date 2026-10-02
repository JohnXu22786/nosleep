#!/bin/bash
# Regression test: a tray stop signal must end the core sleep-prevention worker.

set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

tray = Path("src/tray.c").read_text()
worker = re.search(
    r"static DWORD WINAPI tray_nosleep_thread\(LPVOID lpParam\)\s*\{(.*?)\n\}",
    tray,
    re.S,
)
assert worker, "could not find tray_nosleep_thread"
run_call = re.search(r"nosleep_run\((.*?)\);", worker.group(1), re.S)
assert run_call, "tray worker must call nosleep_run"
assert re.search(r"tray->stop_event\s*(?://[^\n]*)?$", run_call.group(1).rstrip()), (
    "tray worker must pass its session stop event to nosleep_run"
)

core_h = Path("src/core.h").read_text()
run_decl = re.search(r"int nosleep_run\((.*?);", core_h, re.S)
assert run_decl, "could not find nosleep_run declaration"
assert re.search(r"HANDLE\s+external_stop_event\s*\)$", run_decl.group(1).strip()), (
    "nosleep_run must accept an external session stop event"
)
PY

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
mkdir -p "$TMPDIR/mock"

cat > "$TMPDIR/mock/windows.h" <<'WINDOWS_EOF'
#ifndef TEST_WINDOWS_H
#define TEST_WINDOWS_H

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

typedef unsigned int DWORD;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef struct MockEvent* HANDLE;
typedef pthread_mutex_t SRWLOCK;
typedef struct {
    unsigned short wHour;
    unsigned short wMinute;
    unsigned short wSecond;
} SYSTEMTIME;

#define WINAPI
#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffffu
#define INFINITE 0xffffffffu
#define ES_CONTINUOUS 0x80000000u
#define ES_SYSTEM_REQUIRED 0x00000001u
#define ES_DISPLAY_REQUIRED 0x00000002u
#define ES_AWAYMODE_REQUIRED 0x00000040u
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER

HANDLE CreateEvent(void* attributes, BOOL manual_reset, BOOL initial_state, const char* name);
BOOL CloseHandle(HANDLE event);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
DWORD WaitForSingleObject(HANDLE event, DWORD milliseconds);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* events, BOOL wait_all, DWORD milliseconds);
DWORD SetThreadExecutionState(DWORD state);
ULONGLONG GetTickCount64(void);
void GetSystemTime(SYSTEMTIME* system_time);
void Sleep(DWORD milliseconds);
void AcquireSRWLockExclusive(SRWLOCK* lock);
void ReleaseSRWLockExclusive(SRWLOCK* lock);

#endif
WINDOWS_EOF

cat > "$TMPDIR/test_external_stop.c" <<'C_EOF'
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "core.h"

struct MockEvent {
    bool manual_reset;
    bool signaled;
};

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_changed = PTHREAD_COND_INITIALIZER;
static DWORD last_execution_state;
static unsigned int execution_state_calls;
static bool long_wait_entered;

static bool any_signaled(DWORD count, const HANDLE* events, DWORD* index) {
    for (DWORD i = 0; i < count; ++i) {
        if (events[i] && events[i]->signaled) {
            *index = i;
            if (!events[i]->manual_reset) events[i]->signaled = false;
            return true;
        }
    }
    return false;
}

HANDLE CreateEvent(void* attributes, BOOL manual_reset, BOOL initial_state, const char* name) {
    (void)attributes;
    (void)name;
    HANDLE event = (HANDLE)calloc(1, sizeof(*event));
    if (event) {
        event->manual_reset = manual_reset != FALSE;
        event->signaled = initial_state != FALSE;
    }
    return event;
}

BOOL CloseHandle(HANDLE event) {
    free(event);
    return TRUE;
}

BOOL SetEvent(HANDLE event) {
    pthread_mutex_lock(&event_lock);
    event->signaled = true;
    pthread_cond_broadcast(&event_changed);
    pthread_mutex_unlock(&event_lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE event) {
    pthread_mutex_lock(&event_lock);
    event->signaled = false;
    pthread_mutex_unlock(&event_lock);
    return TRUE;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* events, BOOL wait_all, DWORD milliseconds) {
    (void)wait_all;
    pthread_mutex_lock(&event_lock);
    DWORD index = 0;
    if (any_signaled(count, events, &index)) {
        pthread_mutex_unlock(&event_lock);
        return WAIT_OBJECT_0 + index;
    }
    if (milliseconds == 0) {
        pthread_mutex_unlock(&event_lock);
        return WAIT_TIMEOUT;
    }
    long_wait_entered = true;
    pthread_cond_broadcast(&event_changed);

    struct timespec deadline;
    if (milliseconds != INFINITE) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += milliseconds / 1000;
        deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    int result = 0;
    while (!any_signaled(count, events, &index) && result == 0) {
        if (milliseconds == INFINITE) {
            result = pthread_cond_wait(&event_changed, &event_lock);
        } else {
            result = pthread_cond_timedwait(&event_changed, &event_lock, &deadline);
        }
    }
    bool signaled = any_signaled(count, events, &index);
    pthread_mutex_unlock(&event_lock);
    if (signaled) return WAIT_OBJECT_0 + index;
    return result == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_FAILED;
}

DWORD WaitForSingleObject(HANDLE event, DWORD milliseconds) {
    return WaitForMultipleObjects(1, &event, FALSE, milliseconds);
}

DWORD SetThreadExecutionState(DWORD state) {
    last_execution_state = state;
    ++execution_state_calls;
    return 1;
}

ULONGLONG GetTickCount64(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (ULONGLONG)now.tv_sec * 1000 + (ULONGLONG)now.tv_nsec / 1000000;
}

void GetSystemTime(SYSTEMTIME* system_time) {
    time_t raw_time = time(NULL);
    struct tm* utc = gmtime(&raw_time);
    system_time->wHour = (unsigned short)utc->tm_hour;
    system_time->wMinute = (unsigned short)utc->tm_min;
    system_time->wSecond = (unsigned short)utc->tm_sec;
}

void Sleep(DWORD milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

void AcquireSRWLockExclusive(SRWLOCK* lock) {
    pthread_mutex_lock(lock);
}

void ReleaseSRWLockExclusive(SRWLOCK* lock) {
    pthread_mutex_unlock(lock);
}

static void* signal_session_stop(void* context) {
    HANDLE stop_event = (HANDLE)context;
    pthread_mutex_lock(&event_lock);
    while (!long_wait_entered) {
        pthread_cond_wait(&event_changed, &event_lock);
    }
    pthread_mutex_unlock(&event_lock);
    SetEvent(stop_event);
    return NULL;
}

int main(void) {
    NoSleep* ns = nosleep_create();
    HANDLE session_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(ns && session_stop_event);

    pthread_t signal_thread;
    assert(pthread_create(&signal_thread, NULL, signal_session_stop, session_stop_event) == 0);
    ULONGLONG started = GetTickCount64();
    int result = nosleep_run(ns, 0, 3600, false, false, false, session_stop_event);
    ULONGLONG elapsed = GetTickCount64() - started;
    assert(pthread_join(signal_thread, NULL) == 0);

    assert(result == 0);
    assert(elapsed < 1000);
    assert(execution_state_calls >= 2);
    assert(last_execution_state == ES_CONTINUOUS);

    nosleep_destroy(ns);
    CloseHandle(session_stop_event);
    puts("PASS: tray session stop wakes the core worker and restores sleep behavior");
    return 0;
}
C_EOF

gcc -std=c99 -Wall -Wextra -Werror -pthread -I"$TMPDIR/mock" -Isrc \
    "$TMPDIR/test_external_stop.c" src/core.c -o "$TMPDIR/test_external_stop"
"$TMPDIR/test_external_stop"
