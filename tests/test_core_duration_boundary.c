#include "core.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_WAIT_COUNT 8

static ULONGLONG fake_time_ms;
static DWORD requested_waits[MAX_WAIT_COUNT];
static int requested_wait_count;
static BOOL signal_external_on_wait;
static int execution_state_call_count;
static int allow_sleep_call_count;
static BOOL fail_refresh_execution_state;

HANDLE CreateEvent(void *attributes, BOOL manual_reset, BOOL initial_state,
                   const char *name) {
    (void)attributes;
    (void)manual_reset;
    (void)name;

    HANDLE event = (HANDLE)malloc(sizeof(*event));
    assert(event != NULL);
    event->signaled = initial_state;
    return event;
}

BOOL CloseHandle(HANDLE event) {
    free(event);
    return TRUE;
}

BOOL ResetEvent(HANDLE event) {
    event->signaled = FALSE;
    return TRUE;
}

BOOL SetEvent(HANDLE event) {
    event->signaled = TRUE;
    return TRUE;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE *events, BOOL wait_all,
                             DWORD timeout) {
    (void)wait_all;

    for (DWORD i = 0; i < count; ++i) {
        if (events[i]->signaled) {
            return WAIT_OBJECT_0 + i;
        }
    }

    if (timeout == 0) {
        return WAIT_TIMEOUT;
    }

    assert(requested_wait_count < MAX_WAIT_COUNT);
    requested_waits[requested_wait_count++] = timeout;

    if (signal_external_on_wait && count == 2) {
        events[1]->signaled = TRUE;
        return WAIT_OBJECT_0 + 1;
    }

    fake_time_ms += timeout;
    return WAIT_TIMEOUT;
}

ULONGLONG GetTickCount64(void) {
    return fake_time_ms;
}

DWORD SetThreadExecutionState(DWORD flags) {
    if (flags == ES_CONTINUOUS) {
        ++allow_sleep_call_count;
        return flags;
    }

    ++execution_state_call_count;
    if (fail_refresh_execution_state && execution_state_call_count > 1) {
        return 0;
    }
    return flags;
}

void Sleep(DWORD milliseconds) {
    fake_time_ms += milliseconds;
}

void GetSystemTime(SYSTEMTIME *time) {
    time->wHour = 0;
    time->wMinute = 0;
    time->wSecond = 0;
}

void OutputDebugStringA(const char *output) {
    (void)output;
}

void AcquireSRWLockExclusive(SRWLOCK *lock) {
    (void)lock;
}

void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    (void)lock;
}

static void reset_fake_clock(void) {
    fake_time_ms = 0;
    requested_wait_count = 0;
    signal_external_on_wait = FALSE;
    execution_state_call_count = 0;
    allow_sleep_call_count = 0;
    fail_refresh_execution_state = FALSE;
}

static void test_short_duration_caps_long_refresh_wait(void) {
    reset_fake_clock();

    NoSleep *ns = nosleep_create();
    assert(ns != NULL);

    int result = nosleep_run(ns, 1, INT_MAX / 1000, FALSE, FALSE, FALSE, NULL);

    assert(result == 0);
    assert(requested_wait_count == 1);
    assert(requested_waits[0] == 60 * 1000);
    assert(fake_time_ms == 60 * 1000);
    assert(ns->refresh_count == 1);
    assert(!ns->running);

    nosleep_destroy(ns);
}

static void test_indefinite_long_wait_remains_interruptible(void) {
    reset_fake_clock();

    NoSleep *ns = nosleep_create();
    assert(ns != NULL);
    HANDLE external_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    signal_external_on_wait = TRUE;

    int result = nosleep_run(ns, 0, INT_MAX / 1000, FALSE, FALSE, FALSE,
                             external_stop_event);

    assert(result == 0);
    assert(requested_wait_count == 1);
    assert(requested_waits[0] == (DWORD)(INT_MAX / 1000) * 1000u);
    assert(fake_time_ms == 0);
    assert(ns->refresh_count == 1);
    assert(!ns->running);

    nosleep_destroy(ns);
    CloseHandle(external_stop_event);
}

static void test_refresh_failures_restore_sleep_and_return_failure(void) {
    reset_fake_clock();
    fail_refresh_execution_state = TRUE;

    NoSleep *ns = nosleep_create();
    assert(ns != NULL);

    int result = nosleep_run(ns, 0, 1, FALSE, FALSE, FALSE, NULL);

    assert(result != 0);
    assert(ns->refresh_count == MAX_FAILURES);
    assert(ns->failure_count == MAX_FAILURES);
    assert(allow_sleep_call_count == 1);
    assert(ns->stop_event->signaled);
    assert(!ns->running);

    nosleep_destroy(ns);
}

int main(void) {
    test_short_duration_caps_long_refresh_wait();
    test_indefinite_long_wait_remains_interruptible();
    test_refresh_failures_restore_sleep_and_return_failure();
    puts("PASS: duration boundaries, external stops, and refresh failure cleanup are handled");
    return 0;
}
