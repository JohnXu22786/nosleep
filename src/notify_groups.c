#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

// Notification groups implementation for nosleep
#include "notify_groups.h"
#include <ctype.h>
#include <sddl.h>
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
#define NOTIFY_GROUPS_RENAME_SOURCE_VALUE L"__NoSleep_NotifyGroups_RenameSource"
#define NOTIFY_GROUPS_RENAME_DESTINATION_VALUE L"__NoSleep_NotifyGroups_RenameDestination"
#define NOTIFY_GROUPS_RENAME_PHASE_VALUE L"__NoSleep_NotifyGroups_RenamePhase"

#define NOTIFY_GROUPS_RENAME_PHASE_COPYING 1
#define NOTIFY_GROUPS_RENAME_PHASE_DELETE_SOURCE 2

static bool notify_groups_get_current_user_sid(LPWSTR* sid_string) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD token_user_size = 0;
    if (GetTokenInformation(token, TokenUser, NULL, 0, &token_user_size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_user_size < sizeof(TOKEN_USER)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_USER* token_user = (TOKEN_USER*)malloc(token_user_size);
    if (!token_user) {
        CloseHandle(token);
        return false;
    }
    bool read_token_user = GetTokenInformation(token, TokenUser,
        token_user, token_user_size, &token_user_size) != FALSE;
    CloseHandle(token);
    if (!read_token_user) {
        free(token_user);
        return false;
    }

    bool converted = ConvertSidToStringSidW(token_user->User.Sid, sid_string) != FALSE;
    free(token_user);
    return converted;
}

static HANDLE notify_groups_acquire_registry_mutex(void) {
    const wchar_t mutex_name_prefix[] = L"Global\\NoSleep_NotificationGroups_Registry_";
    wchar_t mutex_name[256];
    LPWSTR user_sid = NULL;
    if (!notify_groups_get_current_user_sid(&user_sid)) return NULL;

    size_t prefix_length = wcslen(mutex_name_prefix);
    size_t sid_length = wcslen(user_sid);
    if (prefix_length + sid_length + 1 > sizeof(mutex_name) / sizeof(mutex_name[0])) {
        LocalFree(user_sid);
        return NULL;
    }
    memcpy(mutex_name, mutex_name_prefix, prefix_length * sizeof(wchar_t));
    memcpy(mutex_name + prefix_length, user_sid, (sid_length + 1) * sizeof(wchar_t));
    LocalFree(user_sid);

    HANDLE mutex = CreateMutexW(NULL, FALSE,
        mutex_name);
    if (!mutex) return NULL;

    DWORD wait_result = WaitForSingleObject(mutex, INFINITE);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return NULL;
    }
    return mutex;
}

static void notify_groups_release_registry_mutex(HANDLE mutex) {
    if (!mutex) return;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

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

typedef LONG (WINAPI *NotifyGroupsRegRenameKeyProc)(HKEY, LPCWSTR, LPCWSTR);

static bool notify_groups_remove_registry_key(HKEY parent, const wchar_t* name) {
    LONG result = RegDeleteTreeW(parent, name);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return false;

    result = RegDeleteKeyW(parent, name);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

static bool notify_groups_registry_key_name_is_known(const wchar_t* name) {
    return wcscmp(name, L"NotificationGroups") == 0 ||
        wcscmp(name, L"NotificationGroups_Staging") == 0 ||
        wcscmp(name, L"NotificationGroups_Backup") == 0;
}

static bool notify_groups_write_copy_rename_journal(HKEY parent,
                                                     const wchar_t* old_name,
                                                     const wchar_t* new_name,
                                                     DWORD phase) {
    LONG result = RegDeleteValueW(parent, NOTIFY_GROUPS_RENAME_PHASE_VALUE);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return false;

    DWORD old_size = (DWORD)((wcslen(old_name) + 1) * sizeof(wchar_t));
    DWORD new_size = (DWORD)((wcslen(new_name) + 1) * sizeof(wchar_t));
    result = RegSetValueExW(parent, NOTIFY_GROUPS_RENAME_SOURCE_VALUE, 0,
        REG_SZ, (const BYTE*)old_name, old_size);
    if (result != ERROR_SUCCESS) return false;

    result = RegSetValueExW(parent, NOTIFY_GROUPS_RENAME_DESTINATION_VALUE, 0,
        REG_SZ, (const BYTE*)new_name, new_size);
    if (result != ERROR_SUCCESS) return false;

    result = RegSetValueExW(parent, NOTIFY_GROUPS_RENAME_PHASE_VALUE, 0,
        REG_DWORD, (const BYTE*)&phase, sizeof(phase));
    return result == ERROR_SUCCESS;
}

static bool notify_groups_set_copy_rename_phase(HKEY parent, DWORD phase) {
    LONG result = RegSetValueExW(parent, NOTIFY_GROUPS_RENAME_PHASE_VALUE, 0,
        REG_DWORD, (const BYTE*)&phase, sizeof(phase));
    return result == ERROR_SUCCESS;
}

static bool notify_groups_read_copy_rename_name(HKEY parent, const wchar_t* value_name,
                                                wchar_t* name, DWORD name_capacity) {
    DWORD type = 0;
    DWORD size = name_capacity * (DWORD)sizeof(wchar_t);
    LONG result = RegQueryValueExW(parent, value_name, NULL, &type, (BYTE*)name, &size);
    if (result != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) ||
        size % sizeof(wchar_t) != 0 || size > name_capacity * sizeof(wchar_t)) {
        return false;
    }
    return name[size / sizeof(wchar_t) - 1] == L'\0';
}

static bool notify_groups_clear_copy_rename_journal(HKEY parent) {
    LONG result = RegDeleteValueW(parent, NOTIFY_GROUPS_RENAME_PHASE_VALUE);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

static bool notify_groups_recover_interrupted_copy_rename(HKEY parent) {
    DWORD phase = 0;
    DWORD type = 0;
    DWORD size = sizeof(phase);
    LONG result = RegQueryValueExW(parent, NOTIFY_GROUPS_RENAME_PHASE_VALUE,
        NULL, &type, (BYTE*)&phase, &size);
    if (result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(phase)) {
        return false;
    }

    wchar_t old_name[64] = L"";
    wchar_t new_name[64] = L"";
    if (!notify_groups_read_copy_rename_name(parent, NOTIFY_GROUPS_RENAME_SOURCE_VALUE,
            old_name, sizeof(old_name) / sizeof(old_name[0])) ||
        !notify_groups_read_copy_rename_name(parent, NOTIFY_GROUPS_RENAME_DESTINATION_VALUE,
            new_name, sizeof(new_name) / sizeof(new_name[0])) ||
        !notify_groups_registry_key_name_is_known(old_name) ||
        !notify_groups_registry_key_name_is_known(new_name) ||
        wcscmp(old_name, new_name) == 0) {
        return false;
    }

    if (phase == NOTIFY_GROUPS_RENAME_PHASE_COPYING) {
        if (!notify_groups_remove_registry_key(parent, new_name)) return false;
    } else if (phase == NOTIFY_GROUPS_RENAME_PHASE_DELETE_SOURCE) {
        if (!notify_groups_remove_registry_key(parent, old_name)) return false;
    } else {
        return false;
    }
    return notify_groups_clear_copy_rename_journal(parent);
}

static bool notify_groups_rename_registry_key_fallback(HKEY parent,
                                                        const wchar_t* old_name,
                                                        const wchar_t* new_name) {
    HKEY hExisting;
    LONG result = RegOpenKeyExW(parent, new_name, 0, KEY_READ, &hExisting);
    if (result == ERROR_SUCCESS) {
        RegCloseKey(hExisting);
        return false;
    }
    if (result != ERROR_FILE_NOT_FOUND) return false;
    if (!notify_groups_write_copy_rename_journal(parent, old_name, new_name,
            NOTIFY_GROUPS_RENAME_PHASE_COPYING)) {
        return false;
    }

    HKEY hSource;
    result = RegOpenKeyExW(parent, old_name, 0, KEY_READ, &hSource);
    if (result != ERROR_SUCCESS) {
        notify_groups_clear_copy_rename_journal(parent);
        return false;
    }

    HKEY hDestination;
    DWORD disposition = 0;
    result = RegCreateKeyExW(parent, new_name, 0, NULL, REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE, NULL, &hDestination, &disposition);
    if (result != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        RegCloseKey(hSource);
        if (result == ERROR_SUCCESS) RegCloseKey(hDestination);
        notify_groups_clear_copy_rename_journal(parent);
        return false;
    }

    result = RegCopyTreeW(hSource, NULL, hDestination);
    RegCloseKey(hDestination);
    RegCloseKey(hSource);
    if (result != ERROR_SUCCESS) {
        if (notify_groups_remove_registry_key(parent, new_name)) {
            notify_groups_clear_copy_rename_journal(parent);
        }
        return false;
    }

    if (!notify_groups_set_copy_rename_phase(parent,
            NOTIFY_GROUPS_RENAME_PHASE_DELETE_SOURCE)) return false;
    if (!notify_groups_remove_registry_key(parent, old_name)) return false;
    return notify_groups_clear_copy_rename_journal(parent);
}

static NotifyGroupsRegRenameKeyProc notify_groups_get_reg_rename_key(void) {
    HMODULE advapi32 = GetModuleHandleW(L"advapi32.dll");
    FARPROC address = advapi32 ? GetProcAddress(advapi32, "RegRenameKey") : NULL;
    return (NotifyGroupsRegRenameKeyProc)address;
}

static bool notify_groups_rename_registry_key(HKEY parent,
                                               const wchar_t* old_name,
                                               const wchar_t* new_name) {
    NotifyGroupsRegRenameKeyProc rename_key = notify_groups_get_reg_rename_key();
    if (rename_key) {
        return rename_key(parent, old_name, new_name) == ERROR_SUCCESS;
    }
    return notify_groups_rename_registry_key_fallback(parent, old_name, new_name);
}

static bool notify_groups_recover_interrupted_registry_rename(void) {
    HKEY hParent;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_PARENT_REG_KEY,
        0, KEY_READ, &hParent);
    if (result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) return false;

    DWORD phase = 0;
    DWORD type = 0;
    DWORD size = sizeof(phase);
    result = RegQueryValueExW(hParent, NOTIFY_GROUPS_RENAME_PHASE_VALUE,
        NULL, &type, (BYTE*)&phase, &size);
    RegCloseKey(hParent);
    if (result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(phase)) {
        return false;
    }

    result = RegOpenKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_PARENT_REG_KEY,
        0, KEY_READ | KEY_WRITE | DELETE, &hParent);
    if (result != ERROR_SUCCESS) return false;

    bool success = notify_groups_recover_interrupted_copy_rename(hParent);
    RegCloseKey(hParent);
    return success;
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

static bool notify_groups_names_equal_ignoring_boundary_whitespace(const char* left,
                                                                   const char* right) {
    while (*left && isspace((unsigned char)*left)) left++;
    while (*right && isspace((unsigned char)*right)) right++;

    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    while (left_length > 0 && isspace((unsigned char)left[left_length - 1])) left_length--;
    while (right_length > 0 && isspace((unsigned char)right[right_length - 1])) right_length--;

    return left_length == right_length && memcmp(left, right, left_length) == 0;
}

bool notify_groups_name_is_duplicate(const NotifyGroupManager* mgr, const char* name, int exclude_index) {
    if (!mgr || !name) return false;

    char stored_name[MAX_GROUP_NAME];
    strncpy(stored_name, name, MAX_GROUP_NAME - 1);
    stored_name[MAX_GROUP_NAME - 1] = '\0';

    // Allow event edits for unchanged names, including legacy duplicates.
    if (exclude_index >= 0 && exclude_index < mgr->count &&
        strcmp(mgr->groups[exclude_index].name, stored_name) == 0) {
        return false;
    }

    for (int i = 0; i < mgr->count; i++) {
        if (i != exclude_index &&
            notify_groups_names_equal_ignoring_boundary_whitespace(mgr->groups[i].name, stored_name)) {
            return true;
        }
    }
    return false;
}

int notify_groups_add(NotifyGroupManager* mgr, const char* name, unsigned int event_mask) {
    if (!mgr || !name || mgr->count >= MAX_NOTIFY_GROUPS) {
        return -1;
    }
    
    // Reject names that contain no non-whitespace characters
    if (notify_groups_name_is_blank(name) || notify_groups_name_is_duplicate(mgr, name, -1)) {
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
    if (!mgr || mgr->count <= 1 || index < 0 || index >= mgr->count) return false;
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
    if (notify_groups_name_is_blank(name) || notify_groups_name_is_duplicate(mgr, name, index)) return false;
    
    strncpy(mgr->groups[index].name, name, MAX_GROUP_NAME - 1);
    mgr->groups[index].name[MAX_GROUP_NAME - 1] = '\0';
    mgr->groups[index].event_mask = event_mask;
    
    return true;
}

bool notify_groups_restore_default(NotifyGroupManager* mgr, int index, int preset_index) {
    if (!mgr || index < 0 || index >= mgr->count ||
        !mgr->groups[index].is_default || preset_index < 0 || preset_index >= 3) {
        return false;
    }

    NotifyGroupManager defaults = {0};
    init_default_groups(&defaults);
    NotifyGroup* preset = &defaults.groups[preset_index];
    return notify_groups_update(mgr, index, preset->name, preset->event_mask);
}

static bool notify_groups_save_locked(NotifyGroupManager* mgr) {
    if (!mgr || mgr->load_incomplete) return false;
    
    DWORD parent_access = KEY_READ | KEY_WRITE;
    if (!notify_groups_get_reg_rename_key()) parent_access |= DELETE;

    HKEY hParent;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, NOTIFY_GROUPS_PARENT_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, parent_access,
        NULL, &hParent, NULL);
    if (result != ERROR_SUCCESS) return false;

    if (!notify_groups_recover_interrupted_registry_rename()) {
        RegCloseKey(hParent);
        return false;
    }

    // Restore the previous tree if a process ended after moving it aside but
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

bool notify_groups_save(NotifyGroupManager* mgr) {
    if (!mgr || mgr->load_incomplete) return false;

    HANDLE mutex = notify_groups_acquire_registry_mutex();
    if (!mutex) return false;
    bool saved = notify_groups_save_locked(mgr);
    notify_groups_release_registry_mutex(mutex);
    return saved;
}

// Missing values and oversized payloads use the existing corrupt-data fallback.
// Other query failures leave persisted data unread and must block replacement.
static bool notify_groups_value_read_failed(LONG result) {
    return result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND &&
        result != ERROR_MORE_DATA;
}

static void notify_groups_load_locked(NotifyGroupManager* mgr) {
    if (!mgr) return;
    mgr->load_incomplete = false;

    if (!notify_groups_recover_interrupted_registry_rename()) {
        mgr->load_incomplete = true;
        return;
    }
    
    const char* registry_root = NOTIFY_GROUPS_REG_KEY;
    HKEY hKeyRoot;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, registry_root,
        0, KEY_READ, &hKeyRoot);
    if (result == ERROR_FILE_NOT_FOUND) {
        registry_root = NOTIFY_GROUPS_BACKUP_REG_KEY;
        result = RegOpenKeyEx(HKEY_CURRENT_USER, registry_root, 0, KEY_READ, &hKeyRoot);
    }
    if (result != ERROR_SUCCESS) {
        if (result == ERROR_FILE_NOT_FOUND) {
            // Neither registry tree exists, so this is a successful empty load.
            mgr->count = 0;
            mgr->active_index = 0;
        } else {
            mgr->load_incomplete = true;
        }
        return;
    }
    
    // Read active index
    DWORD val = 0;
    DWORD size = sizeof(DWORD);
    DWORD active_index_type = 0;
    result = RegQueryValueEx(hKeyRoot, "active_index", NULL, &active_index_type, (LPBYTE)&val, &size);
    if (notify_groups_value_read_failed(result)) mgr->load_incomplete = true;
    if (result != ERROR_SUCCESS || active_index_type != REG_DWORD || size != sizeof(DWORD)) val = 0;
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
        if (result == ERROR_FILE_NOT_FOUND) continue;
        if (result != ERROR_SUCCESS) {
            // Keep readable groups available, but never replace unread data.
            mgr->load_incomplete = true;
            continue;
        }
        
        // Read name with proper error checking
        char name[MAX_GROUP_NAME] = "";
        DWORD name_size = sizeof(name);
        DWORD name_type = 0;
        result = RegQueryValueEx(hKeyGroup, "name", NULL, &name_type, (LPBYTE)name, &name_size);
        if (notify_groups_value_read_failed(result)) mgr->load_incomplete = true;
        if (result != ERROR_SUCCESS || name_type != REG_SZ || notify_groups_name_is_blank(name)) {
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
        if (notify_groups_value_read_failed(result)) mgr->load_incomplete = true;
        if (result != ERROR_SUCCESS || mask_type != REG_DWORD || size != sizeof(DWORD)) {
            val = 0;
        }
        mgr->groups[mgr->count].event_mask = (unsigned int)val;
        
        // Read is_default
        val = 0;
        size = sizeof(DWORD);
        DWORD def_type = 0;
        result = RegQueryValueEx(hKeyGroup, "is_default", NULL, &def_type, (LPBYTE)&val, &size);
        if (notify_groups_value_read_failed(result)) mgr->load_incomplete = true;
        mgr->groups[mgr->count].is_default = (result == ERROR_SUCCESS && def_type == REG_DWORD &&
            size == sizeof(DWORD) && val != 0);
        
        RegCloseKey(hKeyGroup);
        mgr->count++;
    }
    
    // A corrupt slot may have shifted the active group to a compacted index.
    if (!active_index_mapped || mgr->active_index < 0 || mgr->active_index >= mgr->count) {
        mgr->active_index = 0;
    }
}

void notify_groups_load(NotifyGroupManager* mgr) {
    if (!mgr) return;

    HANDLE mutex = notify_groups_acquire_registry_mutex();
    if (!mutex) {
        mgr->load_incomplete = true;
        return;
    }
    notify_groups_load_locked(mgr);
    notify_groups_release_registry_mutex(mutex);
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
