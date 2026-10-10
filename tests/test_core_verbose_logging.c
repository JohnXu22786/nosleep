#include "core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char debug_output[256];
static int debug_output_calls;
static int stdout_flush_calls;
static FILE *last_flushed_stream;

int test_core_fflush(FILE *stream) {
    stdout_flush_calls++;
    last_flushed_stream = stream;
    return 0;
}

void GetSystemTime(SYSTEMTIME *time) {
    time->wHour = 12;
    time->wMinute = 34;
    time->wSecond = 56;
}

void OutputDebugStringA(const char *output) {
    debug_output_calls++;
    strncpy(debug_output, output, sizeof(debug_output) - 1);
    debug_output[sizeof(debug_output) - 1] = '\0';
}

void AcquireSRWLockExclusive(SRWLOCK *lock) {
    (void)lock;
}

void ReleaseSRWLockExclusive(SRWLOCK *lock) {
    (void)lock;
}

int main(void) {
    nosleep_log_verbose("Status: Active for %dm %ds (refresh #%d)", 2, 5, 7);

    assert(stdout_flush_calls == 1);
    assert(last_flushed_stream == stdout);
    assert(debug_output_calls == 1);
    assert(strcmp(debug_output,
                  "[12:34:56 UTC] INFO - Status: Active for 2m 5s (refresh #7)\n") == 0);

    debug_output_calls = 0;
    debug_output[0] = '\0';
    nosleep_log_info("Regular status");
    assert(stdout_flush_calls == 2);
    assert(last_flushed_stream == stdout);
    assert(debug_output_calls == 0);

    nosleep_log_warning("Warning status");
    assert(stdout_flush_calls == 3);
    assert(last_flushed_stream == stdout);

    nosleep_log_error("Error status");
    assert(stdout_flush_calls == 4);
    assert(last_flushed_stream == stdout);

    puts("PASS: logs flush stdout and verbose logs also reach debugger output");
    return 0;
}
