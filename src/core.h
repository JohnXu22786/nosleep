// Core NoSleep class for preventing Windows sleep using SetThreadExecutionState API
#ifndef CORE_H
#define CORE_H

#include <windows.h>
#include <stdbool.h>

// Maximum consecutive failures before giving up
#define MAX_FAILURES 5
#define RETRY_COUNT 2
#define RETRY_DELAY_MS 500
#define DEFAULT_INTERVAL_SECONDS 20

typedef struct NoSleep {
    bool running;
    SYSTEMTIME start_time;
    DWORD start_tick;
    HANDLE stop_event;
    int refresh_count;
    int failure_count;
} NoSleep;

// Function prototypes
NoSleep* nosleep_create(void);
void nosleep_destroy(NoSleep* ns);

bool nosleep_prevent_sleep(NoSleep* ns, bool prevent_display, bool away_mode, bool silent);
bool nosleep_allow_sleep(NoSleep* ns);

int nosleep_run(NoSleep* ns, int duration_minutes, int interval_seconds, 
                bool prevent_display, bool away_mode, bool verbose);

void nosleep_stop(NoSleep* ns);

// Helper functions
void nosleep_log_info(const char* format, ...);
void nosleep_log_warning(const char* format, ...);
void nosleep_log_error(const char* format, ...);

#endif // CORE_H