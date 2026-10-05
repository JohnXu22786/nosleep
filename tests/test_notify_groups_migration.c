#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "notify_groups.h"

enum {
    REGISTRY_ROOT_HANDLE = 1,
    REGISTRY_STAGING_HANDLE = 2,
    REGISTRY_PARENT_HANDLE = 3,
    REGISTRY_GROUP_HANDLE_BASE = 10,
    REGISTRY_GROUP_LIMIT = 4
};

#ifndef ERROR_ACCESS_DENIED
#define ERROR_ACCESS_DENIED 5
#endif

#define NOTIFY_GROUPS_PARENT_REG_KEY "Software\\nosleep\\settings"
#define NOTIFY_GROUPS_STAGING_REG_KEY NOTIFY_GROUPS_REG_KEY "_Staging"
#define NOTIFY_GROUPS_BACKUP_REG_KEY NOTIFY_GROUPS_REG_KEY "_Backup"

typedef struct {
    bool key_available;
    bool name_available;
    bool event_mask_available;
    bool is_default_available;
    const char *name;
    DWORD event_mask;
    DWORD is_default;
} StoredGroup;

static bool stored_groups_available;
static bool staging_groups_available;
static int stored_group_count;
static DWORD stored_active_index;
static DWORD staged_active_index;
static StoredGroup stored_groups[REGISTRY_GROUP_LIMIT];
static int inaccessible_group_index;
static int saved_active_index;
static int active_index_writes;

static void reset_registry(void) {
    stored_groups_available = false;
    staging_groups_available = false;
    stored_group_count = 0;
    stored_active_index = 0;
    staged_active_index = 0;
    memset(stored_groups, 0, sizeof(stored_groups));
    inaccessible_group_index = -1;
    saved_active_index = -1;
    active_index_writes = 0;
}

static void set_stored_group(int index, const char *name,
                             DWORD event_mask, DWORD is_default) {
    stored_groups[index] = (StoredGroup){
        true, true, true, true, name, event_mask, is_default
    };
}

static bool copy_registry_value(const void *source, DWORD source_size,
                                DWORD *type, DWORD value_type,
                                BYTE *value, DWORD *size) {
    if (*size < source_size) return false;
    memcpy(value, source, source_size);
    *size = source_size;
    if (type) *type = value_type;
    return true;
}

LONG RegCreateKeyEx(HKEY root, const char *path, DWORD reserved,
                    const char *class_name, DWORD options, DWORD access,
                    void *security, HKEY *key, DWORD *disposition) {
    (void)root;
    (void)reserved;
    (void)class_name;
    (void)options;
    (void)access;
    (void)security;
    (void)disposition;

    if (strcmp(path, NOTIFY_GROUPS_PARENT_REG_KEY) == 0) {
        *key = (HKEY)(uintptr_t)REGISTRY_PARENT_HANDLE;
        return ERROR_SUCCESS;
    }
    if (strcmp(path, NOTIFY_GROUPS_STAGING_REG_KEY) == 0) {
        staging_groups_available = true;
        *key = (HKEY)(uintptr_t)REGISTRY_STAGING_HANDLE;
        return ERROR_SUCCESS;
    }

    int group_index;
    if (sscanf(path, NOTIFY_GROUPS_STAGING_REG_KEY "\\Group_%d", &group_index) == 1) {
        *key = (HKEY)(uintptr_t)(REGISTRY_GROUP_HANDLE_BASE + group_index);
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

LONG RegOpenKeyEx(HKEY root, const char *path, DWORD reserved, DWORD access,
                  HKEY *key) {
    (void)root;
    (void)reserved;
    (void)access;

    if (strcmp(path, NOTIFY_GROUPS_REG_KEY) == 0) {
        if (!stored_groups_available) return ERROR_FILE_NOT_FOUND;
        *key = (HKEY)(uintptr_t)REGISTRY_ROOT_HANDLE;
        return ERROR_SUCCESS;
    }
    if (strcmp(path, NOTIFY_GROUPS_BACKUP_REG_KEY) == 0) return ERROR_FILE_NOT_FOUND;

    int group_index;
    if (sscanf(path, NOTIFY_GROUPS_REG_KEY "\\Group_%d", &group_index) == 1 &&
        group_index >= 0 && group_index < stored_group_count) {
        if (group_index == inaccessible_group_index) return ERROR_ACCESS_DENIED;
        if (!stored_groups[group_index].key_available) return ERROR_FILE_NOT_FOUND;
        *key = (HKEY)(uintptr_t)(REGISTRY_GROUP_HANDLE_BASE + group_index);
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size) {
    (void)reserved;
    (void)type;

    if ((uintptr_t)key == REGISTRY_STAGING_HANDLE &&
        strcmp(name, "active_index") == 0 && size == sizeof(DWORD)) {
        memcpy(&staged_active_index, value, sizeof(DWORD));
        saved_active_index = (int)staged_active_index;
        ++active_index_writes;
    }
    return ERROR_SUCCESS;
}

LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved, DWORD *type,
                     BYTE *value, DWORD *size) {
    (void)reserved;

    if ((uintptr_t)key == REGISTRY_ROOT_HANDLE &&
        strcmp(name, "active_index") == 0) {
        return copy_registry_value(&stored_active_index, sizeof(stored_active_index),
                                   type, REG_DWORD, value, size)
            ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }

    int group_index = (int)(uintptr_t)key - REGISTRY_GROUP_HANDLE_BASE;
    if (group_index < 0 || group_index >= stored_group_count ||
        !stored_groups[group_index].key_available) {
        return ERROR_FILE_NOT_FOUND;
    }

    if (strcmp(name, "name") == 0) {
        if (!stored_groups[group_index].name_available) return ERROR_FILE_NOT_FOUND;
        const char *group_name = stored_groups[group_index].name;
        return copy_registry_value(group_name, (DWORD)strlen(group_name) + 1,
                                   type, REG_SZ, value, size)
            ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }
    if (strcmp(name, "event_mask") == 0) {
        if (!stored_groups[group_index].event_mask_available) return ERROR_FILE_NOT_FOUND;
        return copy_registry_value(&stored_groups[group_index].event_mask,
                                   sizeof(stored_groups[group_index].event_mask),
                                   type, REG_DWORD, value, size)
            ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }
    if (strcmp(name, "is_default") == 0) {
        if (!stored_groups[group_index].is_default_available) return ERROR_FILE_NOT_FOUND;
        return copy_registry_value(&stored_groups[group_index].is_default,
                                   sizeof(stored_groups[group_index].is_default),
                                   type, REG_DWORD, value, size)
            ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }
    return ERROR_FILE_NOT_FOUND;
}

LONG RegDeleteKey(HKEY root, const char *path) {
    (void)root;
    (void)path;
    return ERROR_SUCCESS;
}

LONG RegCloseKey(HKEY key) {
    (void)key;
    return ERROR_SUCCESS;
}

LONG RegRenameKey(HKEY key, const wchar_t *subkey_name, const wchar_t *new_name) {
    if ((uintptr_t)key != REGISTRY_PARENT_HANDLE ||
        wcscmp(subkey_name, L"NotificationGroups_Staging") != 0 ||
        wcscmp(new_name, L"NotificationGroups") != 0 || !staging_groups_available) {
        return ERROR_FILE_NOT_FOUND;
    }
    stored_groups_available = true;
    stored_active_index = staged_active_index;
    staging_groups_available = false;
    return ERROR_SUCCESS;
}

static int test_saved_custom_group_is_not_replaced(void) {
    reset_registry();
    stored_groups_available = true;
    stored_group_count = REGISTRY_GROUP_LIMIT;
    stored_active_index = 3;
    set_stored_group(0, "All notifications", 0xFFFFFFFFu, 1);
    set_stored_group(1, "Critical only", 0x290u, 1);
    set_stored_group(2, "None", 0, 1);
    set_stored_group(3, "Custom Work", 0x55u, 0);

    NotifyGroupManager manager;
    notify_groups_init(&manager, 0);

    if (manager.active_index != 3 ||
        strcmp(manager.groups[manager.active_index].name, "Custom Work") != 0) {
        fprintf(stderr, "FAIL: saved custom active group was replaced by stale legacy mode\n");
        return 1;
    }
    if (active_index_writes != 0) {
        fprintf(stderr, "FAIL: startup rewrote the saved active group during migration\n");
        return 1;
    }
    return 0;
}

static int test_legacy_setting_migrates_when_groups_are_missing(void) {
    reset_registry();

    NotifyGroupManager manager;
    notify_groups_init(&manager, 1);

    if (manager.active_index != 1) {
        fprintf(stderr, "FAIL: legacy critical-only setting did not select its default group\n");
        return 1;
    }
    if (active_index_writes != 1 || saved_active_index != 1) {
        fprintf(stderr, "FAIL: legacy migration did not persist the selected default group\n");
        return 1;
    }
    return 0;
}

static int test_saved_active_group_survives_corrupt_prior_slot(void) {
    reset_registry();
    stored_groups_available = true;
    stored_group_count = 3;
    stored_active_index = 2;
    set_stored_group(0, "All notifications", 0xFFFFFFFFu, 1);
    set_stored_group(1, "", 0, 0);
    set_stored_group(2, "Custom Work", 0x55u, 0);

    NotifyGroupManager manager;
    notify_groups_init(&manager, 0);

    if (manager.count != 2 || manager.active_index != 1 ||
        strcmp(manager.groups[manager.active_index].name, "Custom Work") != 0) {
        fprintf(stderr, "FAIL: saved active group was not remapped past a corrupt registry slot\n");
        return 1;
    }
    return 0;
}

static int test_missing_group_key_is_skipped_and_active_index_is_remapped(void) {
    reset_registry();
    stored_groups_available = true;
    stored_group_count = 3;
    stored_active_index = 2;
    set_stored_group(0, "Group A", 0x1u, 0);
    set_stored_group(2, "Group C", 0x4u, 1);
    stored_groups[2].event_mask_available = false;
    stored_groups[2].is_default_available = false;

    NotifyGroupManager manager = {0};
    notify_groups_load(&manager);

    if (manager.count != 2 || manager.active_index != 1 ||
        strcmp(manager.groups[0].name, "Group A") != 0 ||
        strcmp(manager.groups[1].name, "Group C") != 0) {
        fprintf(stderr, "FAIL: loader did not compact groups around a missing key and remap the active index\n");
        return 1;
    }
    if (manager.groups[1].event_mask != 0 || manager.groups[1].is_default) {
        fprintf(stderr, "FAIL: missing optional group values did not use their default values\n");
        return 1;
    }
    return 0;
}

static int test_missing_group_name_is_skipped_and_loading_continues(void) {
    reset_registry();
    stored_groups_available = true;
    stored_group_count = 2;
    stored_active_index = 1;
    set_stored_group(0, "Corrupt", 0x1u, 0);
    stored_groups[0].name_available = false;
    set_stored_group(1, "Group B", 0x2u, 0);

    NotifyGroupManager manager = {0};
    notify_groups_load(&manager);

    if (manager.count != 1 || manager.active_index != 0 ||
        strcmp(manager.groups[0].name, "Group B") != 0) {
        fprintf(stderr, "FAIL: loader did not skip a missing required name and continue loading\n");
        return 1;
    }
    return 0;
}

static int test_group_open_failure_stops_after_loaded_groups(void) {
    reset_registry();
    stored_groups_available = true;
    stored_group_count = 3;
    stored_active_index = 2;
    set_stored_group(0, "Group A", 0x1u, 0);
    set_stored_group(1, "Group B", 0x2u, 0);
    set_stored_group(2, "Group C", 0x4u, 0);
    inaccessible_group_index = 1;

    NotifyGroupManager manager = {0};
    notify_groups_load(&manager);

    if (manager.count != 1 || manager.active_index != 0 ||
        strcmp(manager.groups[0].name, "Group A") != 0) {
        fprintf(stderr, "FAIL: loader crossed a group-open failure or left an invalid active index\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_saved_custom_group_is_not_replaced();
    failures += test_legacy_setting_migrates_when_groups_are_missing();
    failures += test_saved_active_group_survives_corrupt_prior_slot();
    failures += test_missing_group_key_is_skipped_and_active_index_is_remapped();
    failures += test_missing_group_name_is_skipped_and_loading_continues();
    failures += test_group_open_failure_stops_after_loaded_groups();
    if (failures != 0) return 1;
    puts("PASS: notification-group migration preserves saved groups and migrates legacy settings");
    return 0;
}
