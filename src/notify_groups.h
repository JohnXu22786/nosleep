// Notification groups for nosleep
// Provides customizable notification event filtering via named groups
#ifndef NOTIFY_GROUPS_H
#define NOTIFY_GROUPS_H

#include <windows.h>
#include <stdbool.h>

// Registry key for storing notification groups
#define NOTIFY_GROUPS_REG_KEY "Software\\nosleep\\settings\\NotificationGroups"

// Maximum number of notification groups
#define MAX_NOTIFY_GROUPS 20

// Maximum group name length
#define MAX_GROUP_NAME 128

// Maximum number of notification events
#define MAX_NOTIFY_EVENTS 32

// Notification event IDs
// Each event represents a specific notification type that can be individually toggled
typedef enum {
    NOTIFY_EVENT_APP_START = 0,         // "Application started" - shown on system tray startup
    NOTIFY_EVENT_SESSION_START = 1,     // "Session started" - shown when nosleep session begins
    NOTIFY_EVENT_SESSION_STOP = 2,      // "Session stopped" - shown when session is manually stopped
    NOTIFY_EVENT_TIMER_EXPIRED = 3,     // "Timer expired" - shown when duration finishes
    NOTIFY_EVENT_ERROR = 4,             // "Error occurred" - shown on errors (thread failures, API errors)
    NOTIFY_EVENT_COUNTDOWN_CANCEL = 5,  // "Countdown cancelled" - shown when sleep/shutdown cancelled
    NOTIFY_EVENT_UPDATE_AVAILABLE = 6,  // "Update available" - shown when new version found
    NOTIFY_EVENT_UPDATE_CHECK_FAILED = 7, // "Update check failed" - shown when network fails
    NOTIFY_EVENT_SLEEP_DETECTED = 8,    // "Sleep detected" - shown when system goes to sleep
    NOTIFY_EVENT_ACTION_FAILED = 9,     // "Action failed" - shown when sleep/shutdown fails
    NOTIFY_EVENT_COUNT = 10             // Total number of defined events
} NotifyEventId;

// Human-readable names for each event
extern const char* NOTIFY_EVENT_NAMES[NOTIFY_EVENT_COUNT];

// Notification group structure
typedef struct {
    char name[MAX_GROUP_NAME];      // User-visible group name
    unsigned int event_mask;        // Bitmask: bit N = 1 means event N is enabled
    bool is_default;                // True for built-in default groups (cannot be deleted)
} NotifyGroup;

// Notification group collection
typedef struct {
    NotifyGroup groups[MAX_NOTIFY_GROUPS];
    int count;                      // Number of groups
    int active_index;               // Index of the currently active group
} NotifyGroupManager;

// Initialize the notification group manager
// Reads groups from registry, migrates old settings if needed
void notify_groups_init(NotifyGroupManager* mgr);

// Get the currently active group
NotifyGroup* notify_groups_get_active(NotifyGroupManager* mgr);

// Set the active group by index
bool notify_groups_set_active(NotifyGroupManager* mgr, int index);

// Check if a notification event should be shown based on the active group
bool notify_groups_should_show(NotifyGroupManager* mgr, NotifyEventId event_id);

// Add a new custom group
int notify_groups_add(NotifyGroupManager* mgr, const char* name, unsigned int event_mask);

// Remove a custom group (cannot remove default groups)
bool notify_groups_remove(NotifyGroupManager* mgr, int index);

// Update a group's name and event mask
bool notify_groups_update(NotifyGroupManager* mgr, int index, const char* name, unsigned int event_mask);

// Save all groups to registry
void notify_groups_save(NotifyGroupManager* mgr);

// Load groups from registry
void notify_groups_load(NotifyGroupManager* mgr);

// Migrate old single notification_mode to group system
// Returns the index of the migrated group (-1 if no migration needed)
int notify_groups_migrate_old_settings(NotifyGroupManager* mgr, int old_notification_mode);

// Get the index of a default group by its event mask pattern
int notify_groups_find_default_by_mask(NotifyGroupManager* mgr, unsigned int mask);

#endif // NOTIFY_GROUPS_H
