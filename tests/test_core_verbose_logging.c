#include "core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char debug_output[256];
static int debug_output_calls;

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

    assert(debug_output_calls == 1);
    assert(strcmp(debug_output,
                  "[12:34:56] INFO - Status: Active for 2m 5s (refresh #7)\n") == 0);

    debug_output_calls = 0;
    debug_output[0] = '\0';
    nosleep_log_info("Regular status");
    assert(debug_output_calls == 0);

    puts("PASS: verbose logs reach debugger output and ordinary logs do not");
    return 0;
}
