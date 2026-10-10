// System tray integration for nosleep Windows utility
// Provides taskbar icon with duration settings and notifications
#ifndef TRAY_H
#define TRAY_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <stdbool.h>
#include "notify_groups.h"
#include "tray_session_finished_action.h"
#include "tray_countdown_icon.h"
#include "updater_logic.h"

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
#define ATOMIC_LOAD_BOOL(src) ((bool)InterlockedCompareExchange8((volatile CHAR*)(src), 0, 0))
#define ATOMIC_STORE_INT(dest, value) InterlockedExchange((volatile LONG*)(dest), (LONG)(value))
#define ATOMIC_LOAD_INT(src) ((int)InterlockedCompareExchange((volatile LONG*)(src), 0, 0))
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

// Runtime debug log macro - controlled by NOSLEEP_DEBUG=1 environment variable
// Outputs to stderr with [nosleep] prefix and newline. Caches env var lookup.
#define DEBUG_LOG(fmt, ...) do { \
    static const char* _nosleep_d = NULL; \
    if (!_nosleep_d) _nosleep_d = getenv("NOSLEEP_DEBUG"); \
    if (_nosleep_d && _nosleep_d[0] == '1') \
        fprintf(stderr, "[nosleep] " fmt "\n", ##__VA_ARGS__); \
} while(0)

// Forward declaration
struct NoSleep;
typedef struct TrayUpdateCheckTask TrayUpdateCheckTask;

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
#define IDM_SESSION_FINISHED_NONE 1009
#define IDM_SESSION_FINISHED_SHUTDOWN 1010
#define IDM_SESSION_FINISHED_SLEEP 1011
#define IDM_SESSION_FINISHED_SHUTDOWN_GRACEFUL 1016
#define IDM_ABOUT                 1012
#define IDM_SETTINGS             1014
#define IDM_CHECK_UPDATES        1015
#define IDM_REVIEW_UPDATE        1017

// Notification mode
#define NOTIFY_ALL 0
#define NOTIFY_CRITICAL_ONLY 1
#define NOTIFY_NONE 2

typedef struct NoSleepTray {
    HWND hwnd;                  // Window handle for tray icon
    HMENU hmenu;                // Right-click menu
    bool menu_invoked_by_keyboard; // Avoid stale cursor placement for keyboard tray activation
    NOTIFYICONDATA nid;         // Tray icon data
    bool is_running;            // Whether nosleep is active - accessed atomically
    bool duration_expired;      // Whether the timer duration has expired (to show correct notification) - accessed atomically
    bool stopping;              // Prevent re-entrant calls to tray_stop_nosleep - accessed atomically
    DWORD stopping_thread_id;   // Thread completing stop cleanup - protected by delayed_action_lock
    bool stopping_expiry_notification_suppressed; // Whether the active cleanup suppressed an expiry notice - protected by delayed_action_lock
    CONDITION_VARIABLE stop_condition; // Wakes concurrent stops after cleanup completes
    bool core_init_failed;      // Suppress follow-up actions if NoSleep initialization fails - accessed atomically
    bool core_init_succeeded;   // Allow follow-up actions only after NoSleep initialization succeeds - accessed atomically
    bool nosleep_run_failed;    // Whether the core worker ended after refresh failures - accessed atomically
    bool session_action_cancelled; // Manual stop blocks expiry action publication - protected by delayed_action_lock
    bool starting_nosleep;      // Prevent expiry actions while a replacement session starts - protected by delayed_action_lock
    bool delayed_countdown_starting; // Coordinates delayed-action notices and countdown startup with cancellation - protected by delayed_action_lock
    SRWLOCK delayed_action_lock; // Serializes session/action startup against stop and failure
    SRWLOCK tray_icon_lock; // Serializes NOTIFYICONDATA updates and shell calls
    SRWLOCK countdown_icon_cache_lock; // Serializes lazy countdown icon creation and cleanup
    SRWLOCK notify_groups_lock; // Protects notification group snapshots and replacements
    int duration_minutes;       // Current duration (0 = indefinite, -1 = not set)
    ULONGLONG start_tick64;     // Monotonic tick count when nosleep started
    HANDLE stop_event;          // Event to signal stop
    HANDLE timer_thread;        // Thread for duration timer
    HANDLE nosleep_thread;      // Thread for nosleep execution
    DWORD timer_thread_id;      // Thread ID for duration timer
    DWORD nosleep_thread_id;    // Thread ID for nosleep execution
    HICON hIconDefault;         // Default gray icon
    HICON hIconActive;          // Green active icon
    HICON hIconNumbered[TRAY_COUNTDOWN_NUMBERED_ICON_COUNT]; // Cached numbered icons (0-60)
    HICON hIconCurrentNumbered; // Currently displayed numbered icon
    int current_number;         // Currently displayed number (-1 if none)
    bool prevent_display;       // Also prevent display from sleeping
    bool away_mode;             // Enable away mode
    bool prevent_display_cli_override;
    bool prevent_display_cli_override_set;
    bool away_mode_cli_override;
    bool away_mode_cli_override_set;
    bool verbose;               // Saved verbose logging preference
    bool verbose_cli_override;
    bool verbose_cli_override_set;
    int refresh_interval_seconds; // Interval between sleep-prevention refreshes
    SessionFinishedAction session_finished_action; // Action to take when session finishes
    bool sleep_after_timeout;   // Whether to sleep after timeout expires (deprecated, use session_finished_action)
    bool sleep_action_claimed;  // Dispatch won cancellation - protected by delayed_action_lock
    SessionFinishedAction shutdown_action; // Captured action for pending shutdown
    bool shutdown_action_claimed; // Dispatch won cancellation - protected by delayed_action_lock
    HANDLE sleep_timer;         // Timer handle for delayed sleep
    HANDLE sleep_stop_event;    // Event to signal stop delayed sleep
    HANDLE shutdown_timer;      // Timer handle for delayed shutdown
    HANDLE shutdown_stop_event; // Event to signal stop delayed shutdown
    bool delayed_sleep_countdown_active; // Whether delayed sleep countdown is active (60s countdown) - accessed atomically
    SessionFinishedAction countdown_action; // Action scheduled for the active countdown
    ULONGLONG countdown_start_tick64; // Shared dispatch/countdown timing origin; set before worker creation
    int countdown_seconds;               // Remaining seconds (60 to 0) - accessed atomically
    HANDLE countdown_timer_thread; // Countdown update thread handle
    bool countdown_blink_state; // Current blink state (true=show number, false=show default icon) - accessed atomically
    bool countdown_stopping;    // Whether countdown is being stopped (prevents race conditions) - accessed atomically
    HANDLE countdown_stop_event; // Event to signal stop countdown thread
    HICON hIconCountdownBlank;  // Blank icon for blinking (optional, can use hIconDefault)
    UINT uTrayMessage;          // Registered tray message ID
    UINT uTaskbarCreatedMessage; // Registered shell notification-area restart message
    UINT_PTR taskbar_restore_timer_id; // Retry timer for failed Explorer icon restoration
    bool start_on_startup;      // Whether to auto-start at Windows logon
    bool check_updates_on_startup; // Whether to check for updates on startup
    int auto_check_interval;    // 0=Never, 1=Daily, 2=Weekly
    UINT_PTR update_timer_id;   // Timer ID for periodic update checks
    TrayUpdateCheckTask* update_check_task; // Active updater request, if any
    UpdateInfo available_update; // Owned value snapshot; accessed only on the tray thread
    int notification_mode;      // 0=all, 1=critical only, 2=none (legacy, use notify_groups instead)
    // Startup CLI values use -1 for an unspecified option; ignored unless set.
    bool startup_cli_overrides_set;
    int notification_mode_cli_override;
    int auto_check_interval_cli_override;
    int check_updates_startup_cli_override;
    bool add_to_path;           // Whether to add nosleep directory to environment PATH
    bool add_to_path_preference_set; // Whether the PATH checkbox has an explicit saved preference
    NotifyGroupManager notify_groups; // Notification group manager for per-event filtering
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
// Notification event type is required so the active group can filter every notification.
void tray_show_notification(NoSleepTray* tray, NotifyEventId event_type,
                            const char* title, const char* message, bool critical);
void tray_update_stop_menu_item(NoSleepTray* tray);
void tray_update_session_finished_menu(NoSleepTray* tray);
void tray_set_startup_enabled(NoSleepTray* tray, bool enable);
bool tray_set_add_to_path(NoSleepTray* tray, bool enable);

// Settings
void tray_load_settings(NoSleepTray* tray);
bool tray_save_settings(NoSleepTray* tray);
bool tray_save_settings_cli(int session_finished_action,
                            int auto_start,
                            int notification_mode,
                            int auto_check_interval,
                            int check_updates_startup,
                            int add_to_path);
void tray_show_settings_dialog(NoSleepTray* tray);

// About dialog
void tray_show_about_dialog(NoSleepTray* tray);

// Update checking
void tray_check_for_updates(NoSleepTray* tray, bool silent);

// Countdown display functions
void tray_start_countdown(NoSleepTray* tray, SessionFinishedAction action, ULONGLONG start_tick64); // Start 60-second countdown display
void tray_stop_countdown(NoSleepTray* tray);   // Stop countdown display
DWORD WINAPI countdown_thread(LPVOID lpParam); // Countdown thread function

#endif // TRAY_H
