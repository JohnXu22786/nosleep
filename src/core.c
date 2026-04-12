// Core NoSleep implementation
#include "core.h"
#include "constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

NoSleep* nosleep_create() {
    NoSleep* ns = (NoSleep*)malloc(sizeof(NoSleep));
    if (!ns) return NULL;
    
    memset(ns, 0, sizeof(NoSleep));
    ns->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ns->stop_event) {
        free(ns);
        return NULL;
    }
    
    return ns;
}

void nosleep_destroy(NoSleep* ns) {
    if (!ns) return;
    
    if (ns->stop_event) {
        CloseHandle(ns->stop_event);
    }
    
    free(ns);
}

bool nosleep_prevent_sleep(NoSleep* ns, bool prevent_display, bool away_mode, bool silent) {
    (void)ns; // Unused parameter for now
    
    DWORD flags = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;
    
    if (prevent_display) {
        flags |= ES_DISPLAY_REQUIRED;
        if (!silent) {
            nosleep_log_info("Also preventing display sleep");
        }
    }
    
    if (away_mode) {
        flags |= ES_AWAYMODE_REQUIRED;
        if (!silent) {
            nosleep_log_info("Away mode enabled (may require specific hardware)");
        }
    }
    
    // Try up to RETRY_COUNT times with a short delay between attempts
    for (int attempt = 0; attempt < RETRY_COUNT; attempt++) {
        DWORD result = SetThreadExecutionState(flags);
        if (result != 0) {
            return true;
        }
        
        // If result is 0 but no exception, API call failed
        if (attempt == 0 && !silent) {
            nosleep_log_warning("Sleep prevention failed, retrying...");
        }
        
        // Wait before retry (only if first attempt failed)
        if (attempt == 0) {
            Sleep(RETRY_DELAY_MS);
        }
    }
    
    // Both attempts failed
    nosleep_log_error("Sleep prevention failed after retry");
    return false;
}

bool nosleep_allow_sleep(NoSleep* ns) {
    (void)ns; // Unused parameter for now
    
    DWORD result = SetThreadExecutionState(ES_CONTINUOUS);
    return result != 0;
}

static double get_elapsed_seconds(DWORD start_tick) {
    DWORD current_tick = GetTickCount();
    // Handle tick counter wrap-around (every 49.7 days)
    if (current_tick < start_tick) {
        return (double)((0xFFFFFFFF - start_tick) + current_tick) / 1000.0;
    }
    return (double)(current_tick - start_tick) / 1000.0;
}

int nosleep_run(NoSleep* ns, int duration_minutes, int interval_seconds, 
                bool prevent_display, bool away_mode, bool verbose) {
    
    if (!ns) return 1;
    
    ns->running = true;
    ns->start_tick = GetTickCount();
    GetSystemTime(&ns->start_time);
    ns->refresh_count = 0;
    ns->failure_count = 0;
    
    ResetEvent(ns->stop_event);
    
    nosleep_log_info("nosleep started - preventing system sleep");
    printf("Press Ctrl+C to stop and allow sleep\n");
    
    SYSTEMTIME end_time;
    if (duration_minutes > 0) {
        FILETIME ft_start, ft_end;
        SystemTimeToFileTime(&ns->start_time, &ft_start);
        
        ULARGE_INTEGER uli;
        uli.LowPart = ft_start.dwLowDateTime;
        uli.HighPart = ft_start.dwHighDateTime;
        
        // Add duration in minutes (converted to 100-nanosecond intervals)
        uli.QuadPart += (ULONGLONG)duration_minutes * 60 * 10000000LL;
        
        ft_end.dwLowDateTime = uli.LowPart;
        ft_end.dwHighDateTime = uli.HighPart;
        FileTimeToSystemTime(&ft_end, &end_time);
        
        char time_str[64];
        sprintf(time_str, "%02d:%02d:%02d", 
                end_time.wHour, end_time.wMinute, end_time.wSecond);
        nosleep_log_info("Will run for %d minutes (until %s)", duration_minutes, time_str);
    }
    
    // Prevent sleep initially
    if (!nosleep_prevent_sleep(ns, prevent_display, away_mode, false)) {
        nosleep_log_warning("Failed to prevent sleep. Try running as administrator.");
    }
    
    // Display refresh interval info
    nosleep_log_info("Refreshing sleep prevention every %d seconds", interval_seconds);
    
    while (ns->running) {
        // Check for stop event (for external stop signal)
        if (WaitForSingleObject(ns->stop_event, 0) == WAIT_OBJECT_0) {
            ns->running = false;
            break;
        }
        
        // Refresh sleep prevention state
        ns->refresh_count++;
        bool success = nosleep_prevent_sleep(ns, prevent_display, away_mode, true);
        
        if (success) {
            ns->failure_count = 0; // Reset failure count on success
            
            double elapsed = get_elapsed_seconds(ns->start_tick);
            int minutes = (int)(elapsed / 60);
            int seconds = (int)(elapsed) % 60;
            
            if (verbose) {
                SYSTEMTIME now;
                GetSystemTime(&now);
                nosleep_log_info("[%02d:%02d:%02d] Active: %dm %ds (#%d)",
                                now.wHour, now.wMinute, now.wSecond,
                                minutes, seconds, ns->refresh_count);
            } else {
                nosleep_log_info("Status: Active for %dm %ds (refresh #%d)",
                                minutes, seconds, ns->refresh_count);
            }
        } else {
            ns->failure_count++;
            nosleep_log_warning("Failed to refresh sleep prevention (attempt #%d, failure #%d)",
                               ns->refresh_count, ns->failure_count);
            
            // If too many consecutive failures, exit
            if (ns->failure_count >= MAX_FAILURES) {
                nosleep_log_error("%d consecutive failures. Exiting...", MAX_FAILURES);
                ns->running = false;
                break;
            }
        }
        
        // Sleep for the specified interval, but check stop_event periodically
        DWORD sleep_interval = interval_seconds * 1000;
        DWORD wait_result = WaitForSingleObject(ns->stop_event, sleep_interval);
        if (wait_result == WAIT_OBJECT_0) {
            ns->running = false;
            break;
        }
        
        // Check if duration has elapsed
        if (duration_minutes > 0) {
            SYSTEMTIME now;
            GetSystemTime(&now);
            
            FILETIME ft_now, ft_end;
            SystemTimeToFileTime(&now, &ft_now);
            SystemTimeToFileTime(&end_time, &ft_end);
            
            // Compare FILETIMEs
            if (CompareFileTime(&ft_now, &ft_end) >= 0) {
                nosleep_log_info("Duration reached (%d minutes). Stopping...", duration_minutes);
                break;
            }
        }
    }
    
    nosleep_stop(ns);
    return 0;
}

void nosleep_stop(NoSleep* ns) {
    if (!ns) return;
    
    if (ns->running) {
        ns->running = false;
        SetEvent(ns->stop_event);
        
        if (nosleep_allow_sleep(ns)) {
            nosleep_log_info("Sleep behavior restored");
        } else {
            nosleep_log_warning("Failed to restore sleep behavior");
        }
        
        double elapsed = get_elapsed_seconds(ns->start_tick);
        int hours = (int)(elapsed / 3600);
        int minutes = (int)((elapsed - hours * 3600) / 60);
        int seconds = (int)(elapsed) % 60;
        
        if (hours > 0) {
            nosleep_log_info("nosleep ran for %dh %dm %ds", hours, minutes, seconds);
        } else {
            nosleep_log_info("nosleep ran for %dm %ds", minutes, seconds);
        }
    }
}

// Logging functions
void nosleep_log_info(const char* format, ...) {
    SYSTEMTIME now;
    GetSystemTime(&now);
    
    va_list args;
    va_start(args, format);
    
    printf("[%02d:%02d:%02d] INFO - ", now.wHour, now.wMinute, now.wSecond);
    vprintf(format, args);
    printf("\n");
    
    va_end(args);
}

void nosleep_log_warning(const char* format, ...) {
    SYSTEMTIME now;
    GetSystemTime(&now);
    
    va_list args;
    va_start(args, format);
    
    printf("[%02d:%02d:%02d] WARNING - ", now.wHour, now.wMinute, now.wSecond);
    vprintf(format, args);
    printf("\n");
    
    va_end(args);
}

void nosleep_log_error(const char* format, ...) {
    SYSTEMTIME now;
    GetSystemTime(&now);
    
    va_list args;
    va_start(args, format);
    
    printf("[%02d:%02d:%02d] ERROR - ", now.wHour, now.wMinute, now.wSecond);
    vprintf(format, args);
    printf("\n");
    
    va_end(args);
}