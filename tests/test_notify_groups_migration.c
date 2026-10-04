#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "notify_groups.h"

enum {
    REGISTRY_ROOT_HANDLE = 1,
    REGISTRY_GROUP_HANDLE_BASE = 2,
    REGISTRY_GROUP_LIMIT = 4
};

typedef struct {
    const char *name;
    DWORD event_mask;
    DWORD is_default;
} StoredGroup;

static bool stored_groups_available;
static int stored_group_count;
static DWORD stored_active_index;
static StoredGroup stored_groups[REGISTRY_GROUP_LIMIT];
static int saved_active_index;
static int active_index_writes;

static void reset_registry(void) {
    stored_groups_available = false;
    stored_group_count = 0;
    stored_active_index = 0;
    memset(stored_groups, 0, sizeof(stored_groups));
    saved_active_index = -1;
    active_index_writes = 0;
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

    if (strcmp(path, NOTIFY_GROUPS_REG_KEY) == 0) {
        stored_groups_available = true;
        *key = (HKEY)(uintptr_t)REGISTRY_ROOT_HANDLE;
        return ERROR_SUCCESS;
    }

    int group_index;
    if (sscanf(path, NOTIFY_GROUPS_REG_KEY "\\Group_%d", &group_index) == 1) {
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

    int group_index;
    if (sscanf(path, NOTIFY_GROUPS_REG_KEY "\\Group_%d", &group_index) == 1 &&
        group_index >= 0 && group_index < stored_group_count) {
        *key = (HKEY)(uintptr_t)(REGISTRY_GROUP_HANDLE_BASE + group_index);
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size) {
    (void)reserved;
    (void)type;

    if ((uintptr_t)key == REGISTRY_ROOT_HANDLE &&
        strcmp(name, "active_index") == 0 && size == sizeof(DWORD)) {
        memcpy(&saved_active_index, value, sizeof(DWORD));
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
    if (group_index < 0 || group_index >= stored_group_count) {
        return ERROR_FILE_NOT_FOUND;
    }

    if (strcmp(name, "name") == 0) {
        const char *group_name = stored_groups[group_index].name;
        return copy_registry_value(group_name, (DWORD)strlen(group_name) + 1,
                                   type, REG_SZ, value, size)
            ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }
    if (strcmp(name, "event_mask") == 0) {
        return copy_registry_value(&stored_groups[group_index].event_mask,
                                   sizeof(stored_groups[group_index].event_mask),
                                   type, REG_DWORD, value, size)
            ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }
    if (strcmp(name, "is_default") == 0) {
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

static int test_saved_custom_group_is_not_replaced(void) {
    reset_registry();
    stored_groups_available = true;
    stored_group_count = REGISTRY_GROUP_LIMIT;
    stored_active_index = 3;
    stored_groups[0] = (StoredGroup){"All notifications", 0xFFFFFFFFu, 1};
    stored_groups[1] = (StoredGroup){"Critical only", 0x290u, 1};
    stored_groups[2] = (StoredGroup){"None", 0, 1};
    stored_groups[3] = (StoredGroup){"Custom Work", 0x55u, 0};

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
    stored_groups[0] = (StoredGroup){"All notifications", 0xFFFFFFFFu, 1};
    stored_groups[1] = (StoredGroup){"", 0, 0};
    stored_groups[2] = (StoredGroup){"Custom Work", 0x55u, 0};

    NotifyGroupManager manager;
    notify_groups_init(&manager, 0);

    if (manager.count != 2 || manager.active_index != 1 ||
        strcmp(manager.groups[manager.active_index].name, "Custom Work") != 0) {
        fprintf(stderr, "FAIL: saved active group was not remapped past a corrupt registry slot\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_saved_custom_group_is_not_replaced();
    failures += test_legacy_setting_migrates_when_groups_are_missing();
    failures += test_saved_active_group_survives_corrupt_prior_slot();
    if (failures != 0) return 1;
    puts("PASS: notification-group migration preserves saved groups and migrates legacy settings");
    return 0;
}
