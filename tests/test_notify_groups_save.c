#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "notify_groups.h"

enum {
    REGISTRY_ROOT_HANDLE = 1,
    REGISTRY_GROUP_HANDLE_BASE = 100
};

#ifndef ERROR_ACCESS_DENIED
#define ERROR_ACCESS_DENIED 5
#endif

#ifndef ERROR_MORE_DATA
#define ERROR_MORE_DATA 234
#endif

typedef struct {
    bool exists;
    bool has_name;
    bool has_event_mask;
    bool has_is_default;
    char name[MAX_GROUP_NAME];
    DWORD event_mask;
    DWORD is_default;
} StoredGroup;

static bool stored_root_exists;
static bool has_active_index;
static DWORD stored_active_index;
static StoredGroup stored_groups[MAX_NOTIFY_GROUPS + 5];
static bool fail_root_create;
static int fail_group_create_index;
static int fail_group_delete_index;
static int fail_write_group_index;
static char fail_write_name[32];

static void reset_registry(void) {
    stored_root_exists = false;
    has_active_index = false;
    stored_active_index = 0;
    memset(stored_groups, 0, sizeof(stored_groups));
    fail_root_create = false;
    fail_group_create_index = -1;
    fail_group_delete_index = -1;
    fail_write_group_index = -2;
    fail_write_name[0] = '\0';
}

static bool get_group_index(const char *path, int *index) {
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s\\Group_", NOTIFY_GROUPS_REG_KEY);
    size_t prefix_length = strlen(prefix);
    if (strncmp(path, prefix, prefix_length) != 0) return false;

    char *end = NULL;
    long parsed = strtol(path + prefix_length, &end, 10);
    if (end == path + prefix_length || *end != '\0' || parsed < 0 ||
        parsed >= (long)(sizeof(stored_groups) / sizeof(stored_groups[0]))) {
        return false;
    }

    *index = (int)parsed;
    return true;
}

static int group_index_from_handle(HKEY key) {
    intptr_t handle = (intptr_t)key;
    int index = (int)(handle - REGISTRY_GROUP_HANDLE_BASE);
    if (index < 0 || index >= (int)(sizeof(stored_groups) / sizeof(stored_groups[0]))) {
        return -1;
    }
    return index;
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
        if (fail_root_create) return ERROR_ACCESS_DENIED;
        stored_root_exists = true;
        *key = (HKEY)(intptr_t)REGISTRY_ROOT_HANDLE;
        return ERROR_SUCCESS;
    }

    int index;
    if (!get_group_index(path, &index)) return ERROR_FILE_NOT_FOUND;
    if (index == fail_group_create_index) return ERROR_ACCESS_DENIED;
    stored_groups[index].exists = true;
    *key = (HKEY)(intptr_t)(REGISTRY_GROUP_HANDLE_BASE + index);
    return ERROR_SUCCESS;
}

LONG RegOpenKeyEx(HKEY root, const char *path, DWORD reserved, DWORD access,
                  HKEY *key) {
    (void)root;
    (void)reserved;
    (void)access;

    if (strcmp(path, NOTIFY_GROUPS_REG_KEY) == 0) {
        if (!stored_root_exists) return ERROR_FILE_NOT_FOUND;
        *key = (HKEY)(intptr_t)REGISTRY_ROOT_HANDLE;
        return ERROR_SUCCESS;
    }

    int index;
    if (!get_group_index(path, &index) || !stored_groups[index].exists) {
        return ERROR_FILE_NOT_FOUND;
    }
    *key = (HKEY)(intptr_t)(REGISTRY_GROUP_HANDLE_BASE + index);
    return ERROR_SUCCESS;
}

LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size) {
    (void)reserved;

    int group_index = (intptr_t)key == REGISTRY_ROOT_HANDLE
        ? -1 : group_index_from_handle(key);
    if (group_index == fail_write_group_index && strcmp(name, fail_write_name) == 0) {
        return ERROR_ACCESS_DENIED;
    }

    if (group_index == -1 && strcmp(name, "active_index") == 0 &&
        type == REG_DWORD && size == sizeof(DWORD)) {
        memcpy(&stored_active_index, value, sizeof(DWORD));
        has_active_index = true;
        return ERROR_SUCCESS;
    }
    if (group_index < 0 || !stored_groups[group_index].exists) {
        return ERROR_FILE_NOT_FOUND;
    }

    StoredGroup *group = &stored_groups[group_index];
    if (strcmp(name, "name") == 0 && type == REG_SZ && size <= sizeof(group->name)) {
        memcpy(group->name, value, size);
        group->has_name = true;
        return ERROR_SUCCESS;
    }
    if (strcmp(name, "event_mask") == 0 && type == REG_DWORD && size == sizeof(DWORD)) {
        memcpy(&group->event_mask, value, sizeof(DWORD));
        group->has_event_mask = true;
        return ERROR_SUCCESS;
    }
    if (strcmp(name, "is_default") == 0 && type == REG_DWORD && size == sizeof(DWORD)) {
        memcpy(&group->is_default, value, sizeof(DWORD));
        group->has_is_default = true;
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

static LONG copy_registry_value(const void *source, DWORD source_size,
                                DWORD source_type, DWORD *type,
                                BYTE *value, DWORD *size) {
    if (!size || *size < source_size) return ERROR_MORE_DATA;
    memcpy(value, source, source_size);
    *size = source_size;
    if (type) *type = source_type;
    return ERROR_SUCCESS;
}

LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved, DWORD *type,
                     BYTE *value, DWORD *size) {
    (void)reserved;

    if ((intptr_t)key == REGISTRY_ROOT_HANDLE && strcmp(name, "active_index") == 0) {
        if (!has_active_index) return ERROR_FILE_NOT_FOUND;
        return copy_registry_value(&stored_active_index, sizeof(stored_active_index),
                                   REG_DWORD, type, value, size);
    }

    int index = group_index_from_handle(key);
    if (index < 0 || !stored_groups[index].exists) return ERROR_FILE_NOT_FOUND;
    StoredGroup *group = &stored_groups[index];
    if (strcmp(name, "name") == 0 && group->has_name) {
        return copy_registry_value(group->name, (DWORD)strlen(group->name) + 1,
                                   REG_SZ, type, value, size);
    }
    if (strcmp(name, "event_mask") == 0 && group->has_event_mask) {
        return copy_registry_value(&group->event_mask, sizeof(group->event_mask),
                                   REG_DWORD, type, value, size);
    }
    if (strcmp(name, "is_default") == 0 && group->has_is_default) {
        return copy_registry_value(&group->is_default, sizeof(group->is_default),
                                   REG_DWORD, type, value, size);
    }
    return ERROR_FILE_NOT_FOUND;
}

LONG RegDeleteKey(HKEY root, const char *path) {
    (void)root;
    int index;
    if (!get_group_index(path, &index)) return ERROR_FILE_NOT_FOUND;
    if (index == fail_group_delete_index) return ERROR_ACCESS_DENIED;
    if (!stored_groups[index].exists) return ERROR_FILE_NOT_FOUND;
    memset(&stored_groups[index], 0, sizeof(stored_groups[index]));
    return ERROR_SUCCESS;
}

LONG RegCloseKey(HKEY key) {
    (void)key;
    return ERROR_SUCCESS;
}

static void make_manager(NotifyGroupManager *manager) {
    memset(manager, 0, sizeof(*manager));
    manager->count = 2;
    manager->active_index = 1;
    strcpy(manager->groups[0].name, "All notifications");
    manager->groups[0].event_mask = 0xFFFFFFFFu;
    manager->groups[0].is_default = true;
    strcpy(manager->groups[1].name, "Work events");
    manager->groups[1].event_mask = 0x155u;
}

static int test_root_key_creation_failure(void) {
    reset_registry();
    fail_root_create = true;
    NotifyGroupManager manager;
    make_manager(&manager);

    if (notify_groups_save(&manager)) {
        fprintf(stderr, "FAIL: root key creation failure was reported as a successful save\n");
        return 1;
    }
    return 0;
}

static int test_group_key_creation_failure(void) {
    reset_registry();
    fail_group_create_index = 0;
    NotifyGroupManager manager;
    make_manager(&manager);

    if (notify_groups_save(&manager)) {
        fprintf(stderr, "FAIL: group key creation failure was reported as a successful save\n");
        return 1;
    }
    return 0;
}

static int test_value_write_failure(const char *name, int group_index) {
    reset_registry();
    strcpy(fail_write_name, name);
    fail_write_group_index = group_index;
    NotifyGroupManager manager;
    make_manager(&manager);

    if (notify_groups_save(&manager)) {
        fprintf(stderr, "FAIL: write failure for %s was reported as a successful save\n", name);
        return 1;
    }
    return 0;
}

static int test_old_group_delete_failure(void) {
    reset_registry();
    stored_root_exists = true;
    stored_groups[0].exists = true;
    fail_group_delete_index = 0;
    NotifyGroupManager manager;
    make_manager(&manager);

    if (notify_groups_save(&manager)) {
        fprintf(stderr, "FAIL: old group deletion failure was reported as a successful save\n");
        return 1;
    }
    return 0;
}

static int test_successful_save_round_trips_groups(void) {
    reset_registry();
    NotifyGroupManager manager;
    make_manager(&manager);

    if (!notify_groups_save(&manager)) {
        fprintf(stderr, "FAIL: successful registry writes were reported as a failed save\n");
        return 1;
    }

    NotifyGroupManager loaded;
    memset(&loaded, 0, sizeof(loaded));
    notify_groups_load(&loaded);
    if (loaded.count != manager.count || loaded.active_index != manager.active_index ||
        strcmp(loaded.groups[0].name, manager.groups[0].name) != 0 ||
        loaded.groups[0].event_mask != manager.groups[0].event_mask ||
        loaded.groups[0].is_default != manager.groups[0].is_default ||
        strcmp(loaded.groups[1].name, manager.groups[1].name) != 0 ||
        loaded.groups[1].event_mask != manager.groups[1].event_mask ||
        loaded.groups[1].is_default != manager.groups[1].is_default) {
        fprintf(stderr, "FAIL: saved notification groups did not round-trip through the registry\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_root_key_creation_failure();
    failures += test_group_key_creation_failure();
    failures += test_value_write_failure("active_index", -1);
    failures += test_value_write_failure("name", 0);
    failures += test_value_write_failure("event_mask", 0);
    failures += test_value_write_failure("is_default", 0);
    failures += test_old_group_delete_failure();
    failures += test_successful_save_round_trips_groups();

    if (failures != 0) return 1;
    puts("PASS: notification-group saves report registry failures and round-trip successfully");
    return 0;
}
