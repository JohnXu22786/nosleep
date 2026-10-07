#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>

#include "notify_groups.h"

enum {
    REGISTRY_PARENT_HANDLE = 1,
    REGISTRY_ROOT_HANDLE = 2,
    REGISTRY_STAGING_HANDLE = 3,
    REGISTRY_BACKUP_HANDLE = 4,
    REGISTRY_GROUP_HANDLE_BASE = 100,
    REGISTRY_GROUP_HANDLE_STRIDE = 100,
    REGISTRY_TREE_PRIMARY = 0,
    REGISTRY_TREE_STAGING = 1,
    REGISTRY_TREE_BACKUP = 2,
    REGISTRY_TREE_COUNT = 3
};

#define NOTIFY_GROUPS_PARENT_REG_KEY "Software\\nosleep\\settings"
#define NOTIFY_GROUPS_STAGING_REG_KEY NOTIFY_GROUPS_REG_KEY "_Staging"
#define NOTIFY_GROUPS_BACKUP_REG_KEY NOTIFY_GROUPS_REG_KEY "_Backup"

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

typedef struct {
    bool exists;
    bool has_active_index_value;
    DWORD active_index;
    StoredGroup groups[MAX_NOTIFY_GROUPS + 5];
} StoredRegistryTree;

static StoredRegistryTree stored_trees[REGISTRY_TREE_COUNT];
static bool fail_root_create;
static int fail_group_create_index;
static int fail_group_open_index;
static int fail_group_delete_index;
static int fail_write_group_index;
static char fail_write_name[32];

#define stored_root_exists (stored_trees[REGISTRY_TREE_PRIMARY].exists)
#define has_active_index (stored_trees[REGISTRY_TREE_PRIMARY].has_active_index_value)
#define stored_active_index (stored_trees[REGISTRY_TREE_PRIMARY].active_index)
#define stored_groups (stored_trees[REGISTRY_TREE_PRIMARY].groups)

static void reset_registry(void) {
    memset(stored_trees, 0, sizeof(stored_trees));
    fail_root_create = false;
    fail_group_create_index = -1;
    fail_group_open_index = -1;
    fail_group_delete_index = -1;
    fail_write_group_index = -2;
    fail_write_name[0] = '\0';
}

static const char *registry_tree_path(int tree) {
    switch (tree) {
        case REGISTRY_TREE_PRIMARY: return NOTIFY_GROUPS_REG_KEY;
        case REGISTRY_TREE_STAGING: return NOTIFY_GROUPS_STAGING_REG_KEY;
        case REGISTRY_TREE_BACKUP: return NOTIFY_GROUPS_BACKUP_REG_KEY;
        default: return NULL;
    }
}

static int registry_tree_handle(int tree) {
    switch (tree) {
        case REGISTRY_TREE_PRIMARY: return REGISTRY_ROOT_HANDLE;
        case REGISTRY_TREE_STAGING: return REGISTRY_STAGING_HANDLE;
        case REGISTRY_TREE_BACKUP: return REGISTRY_BACKUP_HANDLE;
        default: return -1;
    }
}

static int registry_tree_from_handle(HKEY key) {
    intptr_t handle = (intptr_t)key;
    for (int tree = 0; tree < REGISTRY_TREE_COUNT; tree++) {
        if (handle == registry_tree_handle(tree)) return tree;
    }
    return -1;
}

static bool get_root_tree(const char *path, int *tree) {
    for (int candidate = 0; candidate < REGISTRY_TREE_COUNT; candidate++) {
        if (strcmp(path, registry_tree_path(candidate)) == 0) {
            *tree = candidate;
            return true;
        }
    }
    return false;
}

static bool get_group_location(const char *path, int *tree, int *index) {
    for (int candidate = 0; candidate < REGISTRY_TREE_COUNT; candidate++) {
        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s\\Group_", registry_tree_path(candidate));
        size_t prefix_length = strlen(prefix);
        if (strncmp(path, prefix, prefix_length) != 0) continue;

        char *end = NULL;
        long parsed = strtol(path + prefix_length, &end, 10);
        if (end == path + prefix_length || *end != '\0' || parsed < 0 ||
            parsed >= MAX_NOTIFY_GROUPS + 5) {
            return false;
        }
        *tree = candidate;
        *index = (int)parsed;
        return true;
    }
    return false;
}

static int group_handle(int tree, int index) {
    return REGISTRY_GROUP_HANDLE_BASE + tree * REGISTRY_GROUP_HANDLE_STRIDE + index;
}

static bool group_location_from_handle(HKEY key, int *tree, int *index) {
    intptr_t offset = (intptr_t)key - REGISTRY_GROUP_HANDLE_BASE;
    if (offset < 0) return false;
    int candidate_tree = (int)(offset / REGISTRY_GROUP_HANDLE_STRIDE);
    int candidate_index = (int)(offset % REGISTRY_GROUP_HANDLE_STRIDE);
    if (candidate_tree >= REGISTRY_TREE_COUNT || candidate_index >= MAX_NOTIFY_GROUPS + 5) {
        return false;
    }
    *tree = candidate_tree;
    *index = candidate_index;
    return true;
}

static int registry_tree_from_name(const wchar_t *name) {
    if (wcscmp(name, L"NotificationGroups") == 0) return REGISTRY_TREE_PRIMARY;
    if (wcscmp(name, L"NotificationGroups_Staging") == 0) return REGISTRY_TREE_STAGING;
    if (wcscmp(name, L"NotificationGroups_Backup") == 0) return REGISTRY_TREE_BACKUP;
    return -1;
}

static bool registry_tree_has_groups(int tree) {
    for (int i = 0; i < MAX_NOTIFY_GROUPS + 5; i++) {
        if (stored_trees[tree].groups[i].exists) return true;
    }
    return false;
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
        *key = (HKEY)(intptr_t)REGISTRY_PARENT_HANDLE;
        return ERROR_SUCCESS;
    }

    int tree;
    if (get_root_tree(path, &tree)) {
        if (tree == REGISTRY_TREE_STAGING && fail_root_create) return ERROR_ACCESS_DENIED;
        stored_trees[tree].exists = true;
        *key = (HKEY)(intptr_t)registry_tree_handle(tree);
        return ERROR_SUCCESS;
    }

    int index;
    if (!get_group_location(path, &tree, &index)) return ERROR_FILE_NOT_FOUND;
    if (index == fail_group_create_index) return ERROR_ACCESS_DENIED;
    stored_trees[tree].groups[index].exists = true;
    *key = (HKEY)(intptr_t)group_handle(tree, index);
    return ERROR_SUCCESS;
}

LONG RegOpenKeyEx(HKEY root, const char *path, DWORD reserved, DWORD access,
                  HKEY *key) {
    (void)root;
    (void)reserved;
    (void)access;

    int tree;
    if (get_root_tree(path, &tree)) {
        if (!stored_trees[tree].exists) return ERROR_FILE_NOT_FOUND;
        *key = (HKEY)(intptr_t)registry_tree_handle(tree);
        return ERROR_SUCCESS;
    }

    int index;
    if (!get_group_location(path, &tree, &index)) return ERROR_FILE_NOT_FOUND;
    if (index == fail_group_open_index) return ERROR_ACCESS_DENIED;
    if (!stored_trees[tree].groups[index].exists) {
        return ERROR_FILE_NOT_FOUND;
    }
    *key = (HKEY)(intptr_t)group_handle(tree, index);
    return ERROR_SUCCESS;
}

LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size) {
    (void)reserved;
    int tree = registry_tree_from_handle(key);
    int group_index = -1;
    if (tree < 0 && !group_location_from_handle(key, &tree, &group_index)) {
        return ERROR_FILE_NOT_FOUND;
    }
    if (group_index == fail_write_group_index && strcmp(name, fail_write_name) == 0) {
        return ERROR_ACCESS_DENIED;
    }

    if (group_index == -1 && strcmp(name, "active_index") == 0 &&
        type == REG_DWORD && size == sizeof(DWORD)) {
        memcpy(&stored_trees[tree].active_index, value, sizeof(DWORD));
        stored_trees[tree].has_active_index_value = true;
        return ERROR_SUCCESS;
    }
    if (group_index < 0 || !stored_trees[tree].groups[group_index].exists) {
        return ERROR_FILE_NOT_FOUND;
    }

    StoredGroup *group = &stored_trees[tree].groups[group_index];
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
    int tree = registry_tree_from_handle(key);
    int group_index = -1;
    if (tree >= 0 && strcmp(name, "active_index") == 0) {
        if (!stored_trees[tree].has_active_index_value) return ERROR_FILE_NOT_FOUND;
        return copy_registry_value(&stored_trees[tree].active_index,
            sizeof(stored_trees[tree].active_index), REG_DWORD, type, value, size);
    }
    if (tree < 0 && !group_location_from_handle(key, &tree, &group_index)) {
        return ERROR_FILE_NOT_FOUND;
    }
    if (group_index < 0 || !stored_trees[tree].groups[group_index].exists) {
        return ERROR_FILE_NOT_FOUND;
    }

    StoredGroup *group = &stored_trees[tree].groups[group_index];
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
    int tree;
    int index;
    if (get_group_location(path, &tree, &index)) {
        if (!stored_trees[tree].groups[index].exists) return ERROR_FILE_NOT_FOUND;
        if (tree == REGISTRY_TREE_STAGING && index == fail_group_delete_index) {
            return ERROR_ACCESS_DENIED;
        }
        memset(&stored_trees[tree].groups[index], 0,
               sizeof(stored_trees[tree].groups[index]));
        return ERROR_SUCCESS;
    }
    if (!get_root_tree(path, &tree) || !stored_trees[tree].exists) {
        return ERROR_FILE_NOT_FOUND;
    }
    if (registry_tree_has_groups(tree)) return ERROR_ACCESS_DENIED;
    memset(&stored_trees[tree], 0, sizeof(stored_trees[tree]));
    return ERROR_SUCCESS;
}

LONG RegCloseKey(HKEY key) {
    (void)key;
    return ERROR_SUCCESS;
}

LONG RegRenameKey(HKEY key, const wchar_t *subkey_name, const wchar_t *new_name) {
    if ((intptr_t)key != REGISTRY_PARENT_HANDLE) return ERROR_ACCESS_DENIED;
    int old_tree = registry_tree_from_name(subkey_name);
    int new_tree = registry_tree_from_name(new_name);
    if (old_tree < 0 || new_tree < 0 || !stored_trees[old_tree].exists) {
        return ERROR_FILE_NOT_FOUND;
    }
    if (stored_trees[new_tree].exists) return ERROR_ACCESS_DENIED;
    stored_trees[new_tree] = stored_trees[old_tree];
    memset(&stored_trees[old_tree], 0, sizeof(stored_trees[old_tree]));
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

static void seed_stored_group(int index, const char *name, DWORD event_mask,
                              DWORD is_default) {
    StoredGroup *group = &stored_groups[index];
    group->exists = true;
    group->has_name = true;
    strcpy(group->name, name);
    group->has_event_mask = true;
    group->event_mask = event_mask;
    group->has_is_default = true;
    group->is_default = is_default;
}

static int test_group_names_reject_whitespace_only(void) {
    NotifyGroupManager manager;
    make_manager(&manager);

    int original_count = manager.count;
    if (notify_groups_add(&manager, " \t\r\n", 0x1u) != -1 ||
        manager.count != original_count) {
        fprintf(stderr, "FAIL: whitespace-only group name was added\n");
        return 1;
    }

    char truncated_blank_name[MAX_GROUP_NAME + 1];
    memset(truncated_blank_name, ' ', MAX_GROUP_NAME - 1);
    truncated_blank_name[MAX_GROUP_NAME - 1] = 'x';
    truncated_blank_name[MAX_GROUP_NAME] = '\0';
    if (notify_groups_add(&manager, truncated_blank_name, 0x1u) != -1 ||
        manager.count != original_count) {
        fprintf(stderr, "FAIL: name truncated to whitespace was added\n");
        return 1;
    }

    char original_name[MAX_GROUP_NAME];
    strcpy(original_name, manager.groups[1].name);
    unsigned int original_mask = manager.groups[1].event_mask;
    if (notify_groups_update(&manager, 1, "\t \n", 0x2u) ||
        notify_groups_update(&manager, 1, truncated_blank_name, 0x2u) ||
        strcmp(manager.groups[1].name, original_name) != 0 ||
        manager.groups[1].event_mask != original_mask) {
        fprintf(stderr, "FAIL: blank group name updated an existing group\n");
        return 1;
    }

    const char *valid_name = "  Work events  ";
    int added_index = notify_groups_add(&manager, valid_name, 0x4u);
    if (added_index != original_count ||
        strcmp(manager.groups[added_index].name, valid_name) != 0) {
        fprintf(stderr, "FAIL: valid nonblank group name was not preserved\n");
        return 1;
    }

    if (!notify_groups_update(&manager, added_index, valid_name, 0x8u) ||
        strcmp(manager.groups[added_index].name, valid_name) != 0 ||
        manager.groups[added_index].event_mask != 0x8u) {
        fprintf(stderr, "FAIL: valid nonblank group name could not update a group\n");
        return 1;
    }

    return 0;
}

static int test_group_names_reject_boundary_whitespace_duplicates(void) {
    NotifyGroupManager manager;
    make_manager(&manager);

    int work_index = notify_groups_add(&manager, "Work", 0x1u);
    int personal_index = notify_groups_add(&manager, "Personal", 0x2u);
    int original_count = manager.count;
    if (work_index < 0 || personal_index < 0 ||
        notify_groups_add(&manager, " \tWork\r\n", 0x4u) != -1 ||
        manager.count != original_count ||
        strcmp(manager.groups[work_index].name, "Work") != 0) {
        fprintf(stderr, "FAIL: adding a boundary-whitespace duplicate was not rejected\n");
        return 1;
    }

    if (notify_groups_update(&manager, personal_index, "\tWork ", 0x8u) ||
        strcmp(manager.groups[personal_index].name, "Personal") != 0 ||
        manager.groups[personal_index].event_mask != 0x2u) {
        fprintf(stderr, "FAIL: renaming to a boundary-whitespace duplicate was not rejected\n");
        return 1;
    }

    int lowercase_index = notify_groups_add(&manager, "work", 0x10u);
    if (lowercase_index < 0 || strcmp(manager.groups[lowercase_index].name, "work") != 0) {
        fprintf(stderr, "FAIL: group-name comparison became case-insensitive\n");
        return 1;
    }
    return 0;
}

static int test_unchanged_legacy_name_can_update_with_normalized_collision(void) {
    NotifyGroupManager manager;
    memset(&manager, 0, sizeof(manager));
    manager.count = 2;
    manager.active_index = 1;
    strcpy(manager.groups[0].name, "Work");
    manager.groups[0].event_mask = 0x1u;
    strcpy(manager.groups[1].name, " Work ");
    manager.groups[1].event_mask = 0x2u;

    if (!notify_groups_update(&manager, 1, " Work ", 0x4u) ||
        strcmp(manager.groups[1].name, " Work ") != 0 ||
        manager.groups[1].event_mask != 0x4u) {
        fprintf(stderr, "FAIL: unchanged legacy name could not update its events or was normalized\n");
        return 1;
    }
    return 0;
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

static int test_staging_cleanup_failure(void) {
    reset_registry();
    stored_root_exists = true;
    has_active_index = true;
    stored_active_index = 1;
    seed_stored_group(0, "Persisted all", 0xFFFFFFFFu, 1);
    stored_trees[REGISTRY_TREE_STAGING].exists = true;
    stored_trees[REGISTRY_TREE_STAGING].groups[0].exists = true;
    fail_group_delete_index = 0;
    NotifyGroupManager manager;
    make_manager(&manager);

    if (notify_groups_save(&manager)) {
        fprintf(stderr, "FAIL: staging cleanup failure was reported as a successful save\n");
        return 1;
    }

    NotifyGroupManager loaded;
    memset(&loaded, 0, sizeof(loaded));
    notify_groups_load(&loaded);
    if (loaded.count != 1 || strcmp(loaded.groups[0].name, "Persisted all") != 0) {
        fprintf(stderr, "FAIL: staging cleanup failure changed the active registry tree\n");
        return 1;
    }
    return 0;
}

static int test_failed_save_preserves_previously_persisted_groups(void) {
    int failures = 0;
    for (int fail_create = 0; fail_create < 2; fail_create++) {
        reset_registry();
        stored_root_exists = true;
        has_active_index = true;
        stored_active_index = 1;
        seed_stored_group(0, "Persisted all", 0xFFFFFFFFu, 1);
        seed_stored_group(1, "Persisted work", 0x155u, 0);

        NotifyGroupManager updated;
        make_manager(&updated);
        strcpy(updated.groups[0].name, "Replacement all");
        strcpy(updated.groups[1].name, "Replacement work");
        updated.groups[1].event_mask = 0x2u;

        if (fail_create) {
            fail_group_create_index = 1;
        } else {
            fail_write_group_index = 1;
            strcpy(fail_write_name, "name");
        }
        if (notify_groups_save(&updated)) {
            fprintf(stderr, "FAIL: injected mid-save %s failure was reported as success\n",
                fail_create ? "key creation" : "value write");
            failures++;
            continue;
        }

        NotifyGroupManager loaded;
        memset(&loaded, 0, sizeof(loaded));
        notify_groups_load(&loaded);
        if (loaded.count != 2 || loaded.active_index != 1 ||
            strcmp(loaded.groups[0].name, "Persisted all") != 0 ||
            loaded.groups[0].event_mask != 0xFFFFFFFFu ||
            strcmp(loaded.groups[1].name, "Persisted work") != 0 ||
            loaded.groups[1].event_mask != 0x155u) {
            fprintf(stderr, "FAIL: failed %s changed previously persisted notification groups\n",
                fail_create ? "key creation" : "value write");
            failures++;
        }
    }
    return failures;
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

static int test_load_skips_missing_group_slots_and_preserves_persistence(void) {
    reset_registry();
    stored_root_exists = true;
    has_active_index = true;
    stored_active_index = 2;
    seed_stored_group(0, "All notifications", 0xFFFFFFFFu, 1);
    seed_stored_group(2, "Later group", 0x155u, 0);

    NotifyGroupManager loaded;
    memset(&loaded, 0, sizeof(loaded));
    notify_groups_load(&loaded);
    if (loaded.count != 2 || loaded.active_index != 1 ||
        strcmp(loaded.groups[0].name, "All notifications") != 0 ||
        strcmp(loaded.groups[1].name, "Later group") != 0 ||
        loaded.groups[1].event_mask != 0x155u) {
        fprintf(stderr, "FAIL: loading stopped at a missing notification-group slot\n");
        return 1;
    }

    if (!notify_groups_save(&loaded)) {
        fprintf(stderr, "FAIL: loaded groups could not be saved after a missing slot\n");
        return 1;
    }

    NotifyGroupManager reloaded;
    memset(&reloaded, 0, sizeof(reloaded));
    notify_groups_load(&reloaded);
    if (reloaded.count != 2 || reloaded.active_index != 1 ||
        strcmp(reloaded.groups[0].name, "All notifications") != 0 ||
        strcmp(reloaded.groups[1].name, "Later group") != 0 ||
        reloaded.groups[1].event_mask != 0x155u) {
        fprintf(stderr, "FAIL: saving after a missing slot lost a later notification group\n");
        return 1;
    }
    return 0;
}

static int test_load_skips_whitespace_only_group_and_maps_active_selection(void) {
    reset_registry();
    stored_root_exists = true;
    has_active_index = true;
    stored_active_index = 2;
    seed_stored_group(0, " \t\r\n", 0x1u, 0);
    seed_stored_group(1, "Earlier group", 0x2u, 0);
    seed_stored_group(2, "Active group", 0x4u, 0);

    NotifyGroupManager loaded;
    memset(&loaded, 0, sizeof(loaded));
    notify_groups_load(&loaded);
    NotifyGroup *active = notify_groups_get_active(&loaded);
    if (loaded.count != 2 || loaded.active_index != 1 || !active ||
        strcmp(loaded.groups[0].name, "Earlier group") != 0 ||
        strcmp(loaded.groups[1].name, "Active group") != 0 ||
        strcmp(active->name, "Active group") != 0) {
        fprintf(stderr, "FAIL: loading retained a whitespace-only group or mis-mapped the active group\n");
        return 1;
    }
    return 0;
}

static int test_partial_load_cannot_overwrite_persisted_groups(void) {
    reset_registry();
    stored_root_exists = true;
    seed_stored_group(0, "All notifications", 0xFFFFFFFFu, 1);
    seed_stored_group(1, "Unread group", 0x233u, 0);
    seed_stored_group(2, "Later group", 0x155u, 0);
    has_active_index = true;
    stored_active_index = 2;
    StoredRegistryTree original = stored_trees[REGISTRY_TREE_PRIMARY];
    fail_group_open_index = 1;

    NotifyGroupManager loaded;
    memset(&loaded, 0, sizeof(loaded));
    notify_groups_load(&loaded);
    if (notify_groups_save(&loaded) ||
        memcmp(&original, &stored_trees[REGISTRY_TREE_PRIMARY], sizeof(original)) != 0 ||
        stored_trees[REGISTRY_TREE_STAGING].exists ||
        stored_trees[REGISTRY_TREE_BACKUP].exists) {
        fprintf(stderr, "FAIL: saving an incomplete load changed the original registry tree\n");
        return 1;
    }
    if (loaded.count != 2 || loaded.active_index != 1 ||
        strcmp(loaded.groups[1].name, "Later group") != 0) {
        fprintf(stderr, "FAIL: an inaccessible group prevented loading a later readable group\n");
        return 1;
    }

    // Access may recover before a subsequent load; only that complete load can save.
    fail_group_open_index = -1;
    if (notify_groups_save(&loaded)) {
        fprintf(stderr, "FAIL: an incomplete manager became saveable without reloading\n");
        return 1;
    }
    notify_groups_load(&loaded);
    if (loaded.count != 3 || loaded.active_index != 2 || !notify_groups_save(&loaded)) {
        fprintf(stderr, "FAIL: a complete reload did not restore safe saving\n");
        return 1;
    }
    NotifyGroupManager reloaded;
    memset(&reloaded, 0, sizeof(reloaded));
    notify_groups_load(&reloaded);
    if (reloaded.count != 3 || reloaded.active_index != 2 ||
        strcmp(reloaded.groups[1].name, "Unread group") != 0 ||
        reloaded.groups[1].event_mask != 0x233u ||
        strcmp(reloaded.groups[2].name, "Later group") != 0 ||
        reloaded.groups[2].event_mask != 0x155u) {
        fprintf(stderr, "FAIL: recovered load-then-save lost previously unread groups\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_group_names_reject_whitespace_only();
    failures += test_group_names_reject_boundary_whitespace_duplicates();
    failures += test_unchanged_legacy_name_can_update_with_normalized_collision();
    failures += test_root_key_creation_failure();
    failures += test_group_key_creation_failure();
    failures += test_value_write_failure("active_index", -1);
    failures += test_value_write_failure("name", 0);
    failures += test_value_write_failure("event_mask", 0);
    failures += test_value_write_failure("is_default", 0);
    failures += test_staging_cleanup_failure();
    failures += test_failed_save_preserves_previously_persisted_groups();
    failures += test_successful_save_round_trips_groups();
    failures += test_load_skips_missing_group_slots_and_preserves_persistence();
    failures += test_load_skips_whitespace_only_group_and_maps_active_selection();
    failures += test_partial_load_cannot_overwrite_persisted_groups();

    if (failures != 0) return 1;
    puts("PASS: notification-group saves report registry failures and round-trip successfully");
    return 0;
}
