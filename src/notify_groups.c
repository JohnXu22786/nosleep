#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

// Notification groups implementation for nosleep
#include "notify_groups.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Human-readable names for each notification event
const char* NOTIFY_EVENT_NAMES[NOTIFY_EVENT_COUNT] = {
    "Application started",
    "Session started",
    "Session stopped",
    "Timer expired",
    "Error occurred",
    "Countdown cancelled",
    "Update available",
    "Update check failed",
    "Sleep detected",
    "Action failed",
    "Update check completed"
};

// Default event masks (bit N = 1 means event N is enabled)
// All events: all bits set
static const unsigned int DEFAULT_MASK_ALL = 0xFFFFFFFF;

// Critical only: only errors and failures
static const unsigned int DEFAULT_MASK_CRITICAL = 
    (1 << NOTIFY_EVENT_ERROR) | 
    (1 << NOTIFY_EVENT_UPDATE_CHECK_FAILED) | 
    (1 << NOTIFY_EVENT_ACTION_FAILED);

// None: no events
static const unsigned int DEFAULT_MASK_NONE = 0;

#define NOTIFY_GROUPS_PARENT_REG_KEY "Software\\nosleep\\settings"
#define NOTIFY_GROUPS_STAGING_REG_KEY NOTIFY_GROUPS_REG_KEY "_Staging"
#define NOTIFY_GROUPS_BACKUP_REG_KEY NOTIFY_GROUPS_REG_KEY "_Backup"

static bool notify_groups_delete_registry_tree(const char* root_key) {
    char subkey[512];
    for (int i = 0; i < MAX_NOTIFY_GROUPS + 5; i++) {
        snprintf(subkey, sizeof(subkey), "%s\\Group_%d", root_key, i);
        LONG result = RegDeleteKey(HKEY_CURRENT_USER, subkey);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
            return false;
        }
    }

    LONG result = RegDeleteKey(HKEY_CURRENT_USER, root_key);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

static bool notify_groups_rename_registry_key(HKEY parent,
                                               const wchar_t* old_name,
                                               const wchar_t* new_name) {
    return RegRenameKey(parent, old_name, new_name) == ERROR_SUCCESS;
}

// Initialize default groups
static void init_default_groups(NotifyGroupManager* mgr) {
    mgr->count = 3;
    mgr->active_index = 0;
    
    // Default group 0: All notifications
    strncpy(mgr->groups[0].name, "All notifications", MAX_GROUP_NAME - 1);
    mgr->groups[0].name[MAX_GROUP_NAME - 1] = '\0';
    mgr->groups[0].event_mask = DEFAULT_MASK_ALL;
    mgr->groups[0].is_default = true;
    
    // Default group 1: Critical only
    strncpy(mgr->groups[1].name, "Critical only", MAX_GROUP_NAME - 1);
    mgr->groups[1].name[MAX_GROUP_NAME - 1] = '\0';
    mgr->groups[1].event_mask = DEFAULT_MASK_CRITICAL;
    mgr->groups[1].is_default = true;
    
    // Default group 2: None
    strncpy(mgr->groups[2].name, "None", MAX_GROUP_NAME - 1);
    mgr->groups[2].name[MAX_GROUP_NAME - 1] = '\0';
    mgr->groups[2].event_mask = DEFAULT_MASK_NONE;
    mgr->groups[2].is_default = true;
}

void notify_groups_init(NotifyGroupManager* mgr, int old_notification_mode) {
    if (!mgr) return;
    
    memset(mgr, 0, sizeof(NotifyGroupManager));
    
    // Try to load from registry first
    notify_groups_load(mgr);
    
    // If no groups found in registry, initialize defaults
    if (mgr->count == 0) {
        init_default_groups(mgr);
        notify_groups_migrate_old_settings(mgr, old_notification_mode);
    }
}

NotifyGroup* notify_groups_get_active(NotifyGroupManager* mgr) {
    if (!mgr || mgr->active_index < 0 || mgr->active_index >= mgr->count) {
        return NULL;
    }
    return &mgr->groups[mgr->active_index];
}

bool notify_groups_set_active(NotifyGroupManager* mgr, int index) {
    if (!mgr || index < 0 || index >= mgr->count) {
        return false;
    }
    mgr->active_index = index;
    return true;
}

bool notify_groups_should_show(NotifyGroupManager* mgr, NotifyEventId event_id) {
    if (!mgr) return true; // Default: show everything
    
    NotifyGroup* active = notify_groups_get_active(mgr);
    if (!active) return true;
    
    if (event_id < 0 || event_id >= NOTIFY_EVENT_COUNT) {
        return true; // Unknown events are shown
    }
    
    return (active->event_mask & (1 << event_id)) != 0;
}

bool notify_groups_name_is_blank(const char* name) {
    if (!name) return true;

    for (size_t i = 0; i < MAX_GROUP_NAME - 1 && name[i] != '\0'; i++) {
        if (!isspace((unsigned char)name[i])) return false;
    }

    return true;
}

int notify_groups_add(NotifyGroupManager* mgr, const char* name, unsigned int event_mask) {
    if (!mgr || !name || mgr->count >= MAX_NOTIFY_GROUPS) {
        return -1;
    }
    
    // Reject names that contain no non-whitespace characters
    if (notify_groups_name_is_blank(name)) {
        return -1;
    }
    
    int index = mgr->count;
    strncpy(mgr->groups[index].name, name, MAX_GROUP_NAME - 1);
    mgr->groups[index].name[MAX_GROUP_NAME - 1] = '\0';
    mgr->groups[index].event_mask = event_mask;
    mgr->groups[index].is_default = false;
    mgr->count++;
    
    return index;
}

bool notify_groups_remove(NotifyGroupManager* mgr, int index) {
    if (!mgr || index < 0 || index >= mgr->count) return false;
    if (mgr->groups[index].is_default) return false; // Cannot delete defaults
    
    // Shift remaining groups
    for (int i = index; i < mgr->count - 1; i++) {
        memcpy(&mgr->groups[i], &mgr->groups[i + 1], sizeof(NotifyGroup));
    }
    mgr->count--;
    
    // Adjust active index if needed
    if (mgr->active_index >= mgr->count) {
        mgr->active_index = mgr->count - 1;
    } else if (mgr->active_index > index) {
        mgr->active_index--;
    }
    
    return true;
}

bool notify_groups_update(NotifyGroupManager* mgr, int index, const char* name, unsigned int event_mask) {
    if (!mgr || !name || index < 0 || index >= mgr->count) return false;
    if (notify_groups_name_is_blank(name)) return false;
    
    strncpy(mgr->groups[index].name, name, MAX_GROUP_NAME - 1);
    mgr->groups[index].name[MAX_GROUP_NAME - 1] = '\0';
    mgr->groups[index].event_mask = event_mask;
    
    return true;
}

bool notify_groups_save(NotifyGroupManager* mgr) {
    if (!mgr) return false;
    
    HKEY hParent;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_PARENT_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hParent, NULL);
    if (result != ERROR_SUCCESS) return false;

    // Recover the previous tree if a process ended after moving it aside but
    // before installing the fully written staging tree.
    HKEY hExisting;
    result = RegOpenKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_REG_KEY,
        0, KEY_READ, &hExisting);
    bool had_existing = result == ERROR_SUCCESS;
    if (had_existing) {
        RegCloseKey(hExisting);
    } else if (result == ERROR_FILE_NOT_FOUND) {
        result = RegOpenKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_BACKUP_REG_KEY,
            0, KEY_READ, &hExisting);
        if (result == ERROR_SUCCESS) {
            RegCloseKey(hExisting);
            if (!notify_groups_rename_registry_key(hParent,
                    L"NotificationGroups_Backup", L"NotificationGroups")) {
                RegCloseKey(hParent);
                return false;
            }
            had_existing = true;
        } else if (result != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(hParent);
            return false;
        }
    } else {
        RegCloseKey(hParent);
        return false;
    }

    // A backup is stale only when the primary tree exists. If it is the only
    // remaining copy, the recovery above must succeed before it can be removed.
    if (had_existing && !notify_groups_delete_registry_tree(NOTIFY_GROUPS_BACKUP_REG_KEY)) {
        RegCloseKey(hParent);
        return false;
    }
    if (!notify_groups_delete_registry_tree(NOTIFY_GROUPS_STAGING_REG_KEY)) {
        RegCloseKey(hParent);
        return false;
    }

    HKEY hKeyRoot;
    result = RegCreateKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_STAGING_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKeyRoot, NULL);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hParent);
        return false;
    }

    bool success = true;
    char subkey_buf[512];
    for (int i = 0; success && i < mgr->count; i++) {
        snprintf(subkey_buf, sizeof(subkey_buf), "%s\\Group_%d",
            NOTIFY_GROUPS_STAGING_REG_KEY, i);

        HKEY hKeyGroup;
        result = RegCreateKeyEx(HKEY_CURRENT_USER, subkey_buf,
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKeyGroup, NULL);
        if (result != ERROR_SUCCESS) {
            success = false;
            break;
        }

        result = RegSetValueEx(hKeyGroup, "name", 0, REG_SZ,
            (LPBYTE)mgr->groups[i].name, (DWORD)(strlen(mgr->groups[i].name) + 1));
        if (result == ERROR_SUCCESS) {
            DWORD val = (DWORD)mgr->groups[i].event_mask;
            result = RegSetValueEx(hKeyGroup, "event_mask", 0, REG_DWORD,
                (LPBYTE)&val, sizeof(val));
        }
        if (result == ERROR_SUCCESS) {
            DWORD val = mgr->groups[i].is_default ? 1 : 0;
            result = RegSetValueEx(hKeyGroup, "is_default", 0, REG_DWORD,
                (LPBYTE)&val, sizeof(val));
        }
        if (result != ERROR_SUCCESS) success = false;
        if (RegCloseKey(hKeyGroup) != ERROR_SUCCESS) success = false;
    }

    if (success) {
        DWORD val = (DWORD)mgr->active_index;
        result = RegSetValueEx(hKeyRoot, "active_index", 0, REG_DWORD,
            (LPBYTE)&val, sizeof(val));
        if (result != ERROR_SUCCESS) success = false;
    }
    if (RegCloseKey(hKeyRoot) != ERROR_SUCCESS) success = false;
    if (!success) {
        RegCloseKey(hParent);
        return false;
    }

    if (had_existing && !notify_groups_rename_registry_key(hParent,
            L"NotificationGroups", L"NotificationGroups_Backup")) {
        RegCloseKey(hParent);
        return false;
    }
    if (!notify_groups_rename_registry_key(hParent,
            L"NotificationGroups_Staging", L"NotificationGroups")) {
        if (had_existing) {
            // If recovery also fails, load() can still use the retained backup.
            notify_groups_rename_registry_key(hParent,
                L"NotificationGroups_Backup", L"NotificationGroups");
        }
        RegCloseKey(hParent);
        return false;
    }

    // The replacement is committed. Failure to remove the old backup does not
    // make the new saved state uncertain, and a later save can clean it up.
    if (had_existing) {
        notify_groups_delete_registry_tree(NOTIFY_GROUPS_BACKUP_REG_KEY);
    }
    RegCloseKey(hParent);
    return true;
}

void notify_groups_load(NotifyGroupManager* mgr) {
    if (!mgr) return;
    
    const char* registry_root = NOTIFY_GROUPS_REG_KEY;
    HKEY hKeyRoot;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, registry_root,
        0, KEY_READ, &hKeyRoot);
    if (result == ERROR_FILE_NOT_FOUND) {
        registry_root = NOTIFY_GROUPS_BACKUP_REG_KEY;
        result = RegOpenKeyEx(HKEY_CURRENT_USER, registry_root, 0, KEY_READ, &hKeyRoot);
    }
    if (result != ERROR_SUCCESS) return;
    
    // Read active index
    DWORD val = 0;
    DWORD size = sizeof(DWORD);
    RegQueryValueEx(hKeyRoot, "active_index", NULL, NULL, (LPBYTE)&val, &size);
    DWORD persisted_active_index = val;
    mgr->active_index = 0;
    bool active_index_mapped = false;
    
    RegCloseKey(hKeyRoot);
    
    // Enumerate group subkeys
    mgr->count = 0;
    for (int i = 0; i < MAX_NOTIFY_GROUPS; i++) {
        char subkey[256];
        snprintf(subkey, sizeof(subkey), "%s\\Group_%d", registry_root, i);
        
        HKEY hKeyGroup;
        result = RegOpenKeyEx(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKeyGroup);
        if (result != ERROR_SUCCESS) break;
        
        // Read name with proper error checking
        char name[MAX_GROUP_NAME] = "";
        DWORD name_size = sizeof(name);
        DWORD name_type = 0;
        result = RegQueryValueEx(hKeyGroup, "name", NULL, &name_type, (LPBYTE)name, &name_size);
        if (result != ERROR_SUCCESS || name_type != REG_SZ || name[0] == '\0') {
            RegCloseKey(hKeyGroup);
            continue; // Corrupt entry, skip and continue enumeration
        }
        if ((DWORD)i == persisted_active_index) {
            mgr->active_index = mgr->count;
            active_index_mapped = true;
        }
        strncpy(mgr->groups[mgr->count].name, name, MAX_GROUP_NAME - 1);
        mgr->groups[mgr->count].name[MAX_GROUP_NAME - 1] = '\0';
        
        // Read event mask
        val = 0;
        size = sizeof(DWORD);
        DWORD mask_type = 0;
        result = RegQueryValueEx(hKeyGroup, "event_mask", NULL, &mask_type, (LPBYTE)&val, &size);
        if (result != ERROR_SUCCESS || mask_type != REG_DWORD) {
            val = 0;
        }
        mgr->groups[mgr->count].event_mask = (unsigned int)val;
        
        // Read is_default
        val = 0;
        size = sizeof(DWORD);
        DWORD def_type = 0;
        result = RegQueryValueEx(hKeyGroup, "is_default", NULL, &def_type, (LPBYTE)&val, &size);
        mgr->groups[mgr->count].is_default = (result == ERROR_SUCCESS && def_type == REG_DWORD && val != 0);
        
        RegCloseKey(hKeyGroup);
        mgr->count++;
    }
    
    // A corrupt slot may have shifted the active group to a compacted index.
    if (!active_index_mapped || mgr->active_index < 0 || mgr->active_index >= mgr->count) {
        mgr->active_index = 0;
    }
}

int notify_groups_migrate_old_settings(NotifyGroupManager* mgr, int old_notification_mode) {
    if (!mgr) return -1;
    
    // Map old notification_mode to default group
    // 0 = NOTIFY_ALL -> "All notifications" (index 0)
    // 1 = NOTIFY_CRITICAL_ONLY -> "Critical only" (index 1)
    // 2 = NOTIFY_NONE -> "None" (index 2)
    
    int target_index;
    switch (old_notification_mode) {
        case 0: target_index = 0; break; // ALL
        case 1: target_index = 1; break; // CRITICAL
        case 2: target_index = 2; break; // NONE
        default: target_index = 0; break;
    }
    
    // Ensure the target group exists
    if (target_index < mgr->count) {
        mgr->active_index = target_index;
        notify_groups_save(mgr);
        return target_index;
    }
    
    return -1;
}

int notify_groups_find_default_by_mask(NotifyGroupManager* mgr, unsigned int mask) {
    if (!mgr) return -1;
    
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->groups[i].is_default && mgr->groups[i].event_mask == mask) {
            return i;
        }
    }
    
    return -1;
}
