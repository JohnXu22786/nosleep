#include "core.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_LOG_COUNT 8
#define TEST_DURATION_MS 60000u

static ULONGLONG fake_time_ms = (ULONGLONG)UINT32_MAX - 25000u;
static char debug_outputs[STATUS_LOG_COUNT][128];
static int debug_output_count;
static char captured_stdout[4096];
static size_t captured_stdout_length;
static bool jump_elapsed_on_next_wait;
static ULONGLONG elapsed_jump_target_ms;
static bool stop_on_next_wait;

static int capture_output(const char *format, va_list args) {
    if (captured_stdout_length >= sizeof(captured_stdout)) {
        return 0;
    }

    size_t remaining = sizeof(captured_stdout) - captured_stdout_length;
    int written = vsnprintf(captured_stdout + captured_stdout_length,
                            remaining, format, args);
    if (written > 0) {
        if ((size_t)written >= remaining) {
            captured_stdout_length = sizeof(captured_stdout) - 1;
        } else {
            captured_stdout_length += (size_t)written;
        }
    }
    return written;
}

int test_core_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = capture_output(format, args);
    va_end(args);
    return written;
}

int test_core_vprintf(const char *format, va_list args) {
    return capture_output(format, args);
}

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

    if (jump_elapsed_on_next_wait) {
        fake_time_ms = elapsed_jump_target_ms;
        jump_elapsed_on_next_wait = false;
        return WAIT_TIMEOUT;
    }

    if (stop_on_next_wait) {
        events[0]->signaled = TRUE;
        stop_on_next_wait = false;
        return WAIT_OBJECT_0;
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

static void test_elapsed_seconds_remainder_stays_correct_past_int_max(void) {
    const ULONGLONG elapsed_seconds = (ULONGLONG)INT_MAX + 18;
    const ULONGLONG elapsed_milliseconds = elapsed_seconds * 1000;
    NoSleep *ns;

    fake_time_ms = 0;
    debug_output_count = 0;
    captured_stdout_length = 0;
    captured_stdout[0] = '\0';
    elapsed_jump_target_ms = elapsed_milliseconds;
    jump_elapsed_on_next_wait = true;
    stop_on_next_wait = true;

    ns = nosleep_create();
    assert(ns != NULL);

    assert(nosleep_run(ns, 0, 1, FALSE, FALSE, TRUE, NULL) == 0);
    assert(fake_time_ms == elapsed_milliseconds);
    assert(debug_output_count == 2);
    assert(strstr(debug_outputs[1], "Active: 35791394m 25s (#2)") != NULL);
    assert(strstr(captured_stdout, "nosleep ran for 596523h 14m 25s") != NULL);

    nosleep_destroy(ns);
}

int main(void) {
    test_verbose_elapsed_status_crosses_32_bit_tick_boundary();
    test_elapsed_seconds_remainder_stays_correct_past_int_max();
    puts("PASS: production core elapsed logging remains correct across tick and int boundaries");
    return 0;
}
