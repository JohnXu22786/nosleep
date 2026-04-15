// System tray integration for nosleep Windows utility
// Provides taskbar icon with duration settings and notifications
#ifndef TRAY_H
#define TRAY_H

#include <windows.h>
#include <stdbool.h>

// Atomic operation macros for thread-safe flag access
#ifdef __GNUC__
// GCC/Clang atomic builtins
#define ATOMIC_STORE_BOOL(dest, value) __atomic_store_n((dest), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD_BOOL(src) __atomic_load_n((src), __ATOMIC_SEQ_CST)
#define ATOMIC_STORE_INT(dest, value) __atomic_store_n((dest), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD_INT(src) __atomic_load_n((src), __ATOMIC_SEQ_CST)
#define ATOMIC_EXCHANGE_BOOL(dest, value) __atomic_exchange_n((dest), (value), __ATOMIC_SEQ_CST)
#define ATOMIC_EXCHANGE_INT(dest, value) __atomic_exchange_n((dest), (value), __ATOMIC_SEQ_CST)
#define MEMORY_BARRIER() __sync_synchronize()
#else
// Windows Interlocked APIs
#define ATOMIC_STORE_BOOL(dest, value) InterlockedExchange8((volatile char*)(dest), (char)(value))
#define ATOMIC_LOAD_BOOL(src) (*(src))
#define ATOMIC_STORE_INT(dest, value) InterlockedExchange((volatile LONG*)(dest), (LONG)(value))
#define ATOMIC_LOAD_INT(src) (*(src))
#define ATOMIC_EXCHANGE_BOOL(dest, value) InterlockedExchange8((volatile char*)(dest), (char)(value))
#define ATOMIC_EXCHANGE_INT(dest, value) InterlockedExchange((volatile LONG*)(dest), (LONG)(value))
#define MEMORY_BARRIER() MemoryBarrier()
#endif

// Debug output macro - compiles out when not in debug mode
#ifdef _DEBUG
#include <stdio.h>
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, "[nosleep] " fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...) ((void)0)
#endif

// Forward declaration
struct NoSleep;

// Tray icon message ID
#define TRAY_ICON_MESSAGE_ID 1000

// Menu command IDs
#define IDM_START_30MIN     1001
#define IDM_START_1HOUR     1002
#define IDM_START_2HOURS    1003
#define IDM_START_CUSTOM    1004
#define IDM_START_INDEFINITE 1005
#define IDM_STOP            1006
#define IDM_EXIT            1007
#define IDM_TOGGLE_SLEEP_AFTER_TIMEOUT 1008

typedef struct NoSleepTray {
    HWND hwnd;                  // Window handle for tray icon
    HMENU hmenu;                // Right-click menu
    NOTIFYICONDATA nid;         // Tray icon data
    bool is_running;            // Whether nosleep is active - accessed atomically
    bool duration_expired;      // Whether the timer duration has expired (to show correct notification) - accessed atomically
    bool stopping;              // Prevent re-entrant calls to tray_stop_nosleep - accessed atomically
    int duration_minutes;       // Current duration (0 = indefinite, -1 = not set)
    SYSTEMTIME start_time;      // When nosleep started
    HANDLE stop_event;          // Event to signal stop
    HANDLE timer_thread;        // Thread for duration timer
    HANDLE nosleep_thread;      // Thread for nosleep execution
    DWORD timer_thread_id;      // Thread ID for duration timer
    DWORD nosleep_thread_id;    // Thread ID for nosleep execution
    HICON hIconDefault;         // Default gray icon
    HICON hIconActive;          // Green active icon
    HICON hIconNumbered[60];    // Numbered icons for countdown (0-59 minutes)
    HICON hIconCurrentNumbered; // Currently displayed numbered icon
    int current_number;         // Currently displayed number (-1 if none)
    bool prevent_display;       // Also prevent display from sleeping
    bool away_mode;             // Enable away mode
    bool verbose;               // Print verbose status
    bool sleep_after_timeout;   // Whether to sleep after timeout expires
    HANDLE sleep_timer;         // Timer handle for delayed sleep
    HANDLE sleep_stop_event;    // Event to signal stop delayed sleep
    bool delayed_sleep_countdown_active; // Whether delayed sleep countdown is active (60s countdown) - accessed atomically
    int countdown_seconds;               // Remaining seconds (60 to 0) - accessed atomically
    HANDLE countdown_timer_thread; // Countdown update thread handle
    bool countdown_blink_state; // Current blink state (true=show number, false=show default icon) - accessed atomically
    bool countdown_stopping;    // Whether countdown is being stopped (prevents race conditions) - accessed atomically
    HANDLE countdown_stop_event; // Event to signal stop countdown thread
    HICON hIconCountdownBlank;  // Blank icon for blinking (optional, can use hIconDefault)
    UINT uTrayMessage;          // Registered tray message ID
} NoSleepTray;

// Function prototypes
NoSleepTray* tray_create(void);
void tray_destroy(NoSleepTray* tray);

bool tray_init(NoSleepTray* tray);
void tray_run(NoSleepTray* tray);

void tray_start_nosleep(NoSleepTray* tray, int duration_minutes);
void tray_stop_nosleep(NoSleepTray* tray, bool timer_expired, bool suppress_notification);
void tray_set_duration(NoSleepTray* tray, int minutes);

void tray_update_icon(NoSleepTray* tray);
void tray_show_notification(NoSleepTray* tray, const char* title, const char* message);
void tray_update_stop_menu_item(NoSleepTray* tray);

LRESULT CALLBACK tray_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Countdown display functions
void tray_start_countdown(NoSleepTray* tray);  // Start 60-second countdown display
void tray_stop_countdown(NoSleepTray* tray);   // Stop countdown display
DWORD WINAPI countdown_thread(LPVOID lpParam); // Countdown thread function

#endif // TRAY_H