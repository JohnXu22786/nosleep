#include "core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_LOG_COUNT 8
#define TEST_DURATION_MS 60000u

static ULONGLONG fake_time_ms = (ULONGLONG)UINT32_MAX - 25000u;
static char debug_outputs[STATUS_LOG_COUNT][128];
static int debug_output_count;

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

    fake_time_ms += timeout;
    return WAIT_TIMEOUT;
}

ULONGLONG GetTickCount64(void) {
    return fake_time_ms;
}

DWORD SetThreadExecutionState(DWORD flags) {
    return flags;
}

void Sleep(DWORD milliseconds) {
    fake_time_ms += milliseconds;
}

void GetSystemTime(SYSTEMTIME *time) {
    time->wHour = 12;
    time->wMinute = 34;
    time->wSecond = 56;
}

void OutputDebugStringA(const char *output) {
    assert(debug_output_count < STATUS_LOG_COUNT);
    strncpy(debug_outputs[debug_output_count], output,
            sizeof(debug_outputs[debug_output_count]) - 1);
    debug_outputs[debug_output_count][sizeof(debug_outputs[debug_output_count]) - 1] = '\0';
    ++debug_output_count;
}

void AcquireSRWLockExclusive(SRWLOCK *lock) {
    (void)lock;
}

void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    (void)lock;
}

static void test_verbose_elapsed_status_crosses_32_bit_tick_boundary(void) {
    NoSleep *ns = nosleep_create();
    assert(ns != NULL);

    assert(nosleep_run(ns, 1, 10, FALSE, FALSE, TRUE, NULL) == 0);
    assert(ns->refresh_count == 6);
    assert(fake_time_ms - ns->start_tick64 == TEST_DURATION_MS);
    assert(ns->start_tick64 < UINT32_MAX);
    assert(fake_time_ms > UINT32_MAX);
    assert(debug_output_count == 6);

    for (int i = 0; i < debug_output_count; ++i) {
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "[12:34:56 UTC] INFO - [12:34:56 UTC] Active: 0m %ds (#%d)\n",
                 i * 10, i + 1);
        assert(strcmp(debug_outputs[i], expected) == 0);
    }

    nosleep_destroy(ns);
}

int main(void) {
    test_verbose_elapsed_status_crosses_32_bit_tick_boundary();
    puts("PASS: production core elapsed logging remains correct across a 32-bit tick boundary");
    return 0;
}
