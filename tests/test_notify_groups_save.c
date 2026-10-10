#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <pthread.h>

#include "notify_groups.h"
#include <sddl.h>

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
static bool deny_parent_mutation;
static DWORD registry_parent_access;
static bool journal_phase_exists;
static DWORD journal_phase;
static bool journal_source_exists;
static bool journal_destination_exists;
static wchar_t journal_source[64];
static wchar_t journal_destination[64];
static int fail_group_create_index;
static int fail_group_open_index;
static int fail_group_delete_index;
static int fail_write_group_index;
static bool rename_api_available;
static int rename_api_call_count;
static char fail_write_name[32];
static bool mutex_held;
static bool global_mutex_name_requested;
static int mutex_wait_count;
static int fallback_copy_count;
static bool inject_interleaved_load;
static bool interleaved_load_attempted;
static bool interleaved_load_waiting;
static bool interleaved_load_lock_was_busy;
static bool interleaved_load_thread_entered;
static bool interleaved_load_finished;
static bool interleaved_load_thread_started;
static pthread_t interleaved_load_thread;
static pthread_t interleaved_load_thread_id;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t test_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_state_changed = PTHREAD_COND_INITIALIZER;
static NotifyGroupManager interleaved_load;

static void make_manager(NotifyGroupManager *manager);
static void seed_stored_group(int index, const char *name, DWORD event_mask,
                              DWORD is_default);

#define stored_root_exists (stored_trees[REGISTRY_TREE_PRIMARY].exists)
#define has_active_index (stored_trees[REGISTRY_TREE_PRIMARY].has_active_index_value)
#define stored_active_index (stored_trees[REGISTRY_TREE_PRIMARY].active_index)
#define stored_groups (stored_trees[REGISTRY_TREE_PRIMARY].groups)

static void reset_registry(void) {
    memset(stored_trees, 0, sizeof(stored_trees));
    fail_root_create = false;
    deny_parent_mutation = false;
    registry_parent_access = 0;
    journal_phase_exists = false;
    journal_phase = 0;
    journal_source_exists = false;
    journal_destination_exists = false;
    journal_source[0] = L'\0';
    journal_destination[0] = L'\0';
    fail_group_create_index = -1;
    fail_group_open_index = -1;
    fail_group_delete_index = -1;
    fail_write_group_index = -2;
    rename_api_available = false;
    rename_api_call_count = 0;
    fail_write_name[0] = '\0';
    mutex_held = false;
    global_mutex_name_requested = false;
    mutex_wait_count = 0;
    fallback_copy_count = 0;
    inject_interleaved_load = false;
    interleaved_load_attempted = false;
    interleaved_load_waiting = false;
    interleaved_load_lock_was_busy = false;
    interleaved_load_thread_entered = false;
    interleaved_load_finished = false;
    interleaved_load_thread_started = false;
    memset(&interleaved_load, 0, sizeof(interleaved_load));
}

HANDLE CreateMutexW(void *security, BOOL initial_owner, LPCWSTR name) {
    (void)security;
    (void)initial_owner;
    global_mutex_name_requested = name && wcscmp(name,
        L"Global\\NoSleep_NotificationGroups_Registry_S-1-5-21-1000") == 0;
    return (HANDLE)(uintptr_t)1;
}

HANDLE GetCurrentProcess(void) {
    return (HANDLE)(uintptr_t)1;
}

BOOL OpenProcessToken(HANDLE process, DWORD access, HANDLE *token) {
    (void)process;
    (void)access;
    *token = (HANDLE)(uintptr_t)2;
    return TRUE;
}

static DWORD mock_last_error;

BOOL GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS info_class,
                         LPVOID info, DWORD info_size, DWORD *return_length) {
    if ((intptr_t)token != 2 || info_class != TokenUser) return FALSE;
    if (!info || info_size < sizeof(TOKEN_USER)) {
        if (return_length) *return_length = sizeof(TOKEN_USER);
        mock_last_error = ERROR_INSUFFICIENT_BUFFER;
        return FALSE;
    }
    ((TOKEN_USER *)info)->User.Sid = (PSID)(uintptr_t)3;
    ((TOKEN_USER *)info)->User.Attributes = 0;
    if (return_length) *return_length = sizeof(TOKEN_USER);
    return TRUE;
}

DWORD GetLastError(void) {
    return mock_last_error;
}

BOOL ConvertSidToStringSidW(PSID sid, LPWSTR *string_sid) {
    static wchar_t current_user_sid[] = L"S-1-5-21-1000";
    if ((intptr_t)sid != 3) return FALSE;
    *string_sid = current_user_sid;
    return TRUE;
}

HLOCAL LocalFree(HLOCAL memory) {
    (void)memory;
    return NULL;
}

DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds) {
    (void)object;
    (void)milliseconds;
    pthread_mutex_lock(&test_state_mutex);
    mutex_wait_count++;
    bool is_interleaved_load = interleaved_load_thread_entered &&
        pthread_equal(pthread_self(), interleaved_load_thread_id);
    pthread_mutex_unlock(&test_state_mutex);

    bool acquired = false;
    if (is_interleaved_load) {
        int result = pthread_mutex_trylock(&registry_mutex);
        if (result == 0) {
            acquired = true;
        } else if (result != EBUSY) {
            return WAIT_FAILED;
        }
        pthread_mutex_lock(&test_state_mutex);
        interleaved_load_lock_was_busy = !acquired;
        interleaved_load_waiting = true;
        pthread_cond_broadcast(&test_state_changed);
        pthread_mutex_unlock(&test_state_mutex);
    }

    if (!acquired && pthread_mutex_lock(&registry_mutex) != 0) return WAIT_FAILED;
    pthread_mutex_lock(&test_state_mutex);
    mutex_held = true;
    pthread_mutex_unlock(&test_state_mutex);
    return WAIT_OBJECT_0;
}

BOOL ReleaseMutex(HANDLE object) {
    (void)object;
    pthread_mutex_lock(&test_state_mutex);
    if (!mutex_held) {
        pthread_mutex_unlock(&test_state_mutex);
        return FALSE;
    }
    mutex_held = false;
    pthread_mutex_unlock(&test_state_mutex);
    return pthread_mutex_unlock(&registry_mutex) == 0 ? TRUE : FALSE;
}

BOOL CloseHandle(HANDLE object) {
    (void)object;
    return TRUE;
}

static void *run_interleaved_load(void *unused) {
    (void)unused;
    pthread_mutex_lock(&test_state_mutex);
    interleaved_load_thread_id = pthread_self();
    interleaved_load_thread_entered = true;
    pthread_cond_broadcast(&test_state_changed);
    pthread_mutex_unlock(&test_state_mutex);
    notify_groups_load(&interleaved_load);
    pthread_mutex_lock(&test_state_mutex);
    interleaved_load_finished = true;
    pthread_cond_broadcast(&test_state_changed);
    pthread_mutex_unlock(&test_state_mutex);
    return NULL;
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

static LONG mock_reg_rename_key(HKEY key, LPCWSTR subkey_name, LPCWSTR new_name);

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
    (void)security;
    (void)disposition;

    if (strcmp(path, NOTIFY_GROUPS_PARENT_REG_KEY) == 0) {
        registry_parent_access = access;
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

    if (strcmp(path, NOTIFY_GROUPS_PARENT_REG_KEY) == 0) {
        if (deny_parent_mutation &&
            (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE)) != 0) {
            return ERROR_ACCESS_DENIED;
        }
        for (int tree = 0; tree < REGISTRY_TREE_COUNT; tree++) {
            if (stored_trees[tree].exists) {
                if ((access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE)) != 0) {
                    registry_parent_access = access;
                }
                *key = (HKEY)(intptr_t)REGISTRY_PARENT_HANDLE;
                return ERROR_SUCCESS;
            }
        }
        return ERROR_FILE_NOT_FOUND;
    }

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

LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                    const BYTE *value, DWORD size) {
    (void)reserved;
    if ((intptr_t)key != REGISTRY_PARENT_HANDLE) return ERROR_FILE_NOT_FOUND;
    if (wcscmp(name, L"__NoSleep_NotifyGroups_RenameSource") == 0 &&
        type == REG_SZ && size <= sizeof(journal_source)) {
        memcpy(journal_source, value, size);
        journal_source_exists = true;
        return ERROR_SUCCESS;
    }
    if (wcscmp(name, L"__NoSleep_NotifyGroups_RenameDestination") == 0 &&
        type == REG_SZ && size <= sizeof(journal_destination)) {
        memcpy(journal_destination, value, size);
        journal_destination_exists = true;
        return ERROR_SUCCESS;
    }
    if (wcscmp(name, L"__NoSleep_NotifyGroups_RenamePhase") == 0 &&
        type == REG_DWORD && size == sizeof(journal_phase)) {
        memcpy(&journal_phase, value, size);
        journal_phase_exists = true;
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

LONG RegQueryValueExW(HKEY key, LPCWSTR name, DWORD *reserved, DWORD *type,
                      BYTE *value, DWORD *size) {
    (void)reserved;
    if ((intptr_t)key != REGISTRY_PARENT_HANDLE) return ERROR_FILE_NOT_FOUND;
    if (wcscmp(name, L"__NoSleep_NotifyGroups_RenameSource") == 0 &&
        journal_source_exists) {
        return copy_registry_value(journal_source,
            (DWORD)(wcslen(journal_source) + 1) * sizeof(wchar_t),
            REG_SZ, type, value, size);
    }
    if (wcscmp(name, L"__NoSleep_NotifyGroups_RenameDestination") == 0 &&
        journal_destination_exists) {
        return copy_registry_value(journal_destination,
            (DWORD)(wcslen(journal_destination) + 1) * sizeof(wchar_t),
            REG_SZ, type, value, size);
    }
    if (wcscmp(name, L"__NoSleep_NotifyGroups_RenamePhase") == 0 &&
        journal_phase_exists) {
        return copy_registry_value(&journal_phase, sizeof(journal_phase),
            REG_DWORD, type, value, size);
    }
    return ERROR_FILE_NOT_FOUND;
}

LONG RegDeleteValueW(HKEY key, LPCWSTR name) {
    if ((intptr_t)key != REGISTRY_PARENT_HANDLE ||
        wcscmp(name, L"__NoSleep_NotifyGroups_RenamePhase") != 0 ||
        !journal_phase_exists) return ERROR_FILE_NOT_FOUND;
    journal_phase_exists = false;
    return ERROR_SUCCESS;
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

LONG RegCreateKeyExW(HKEY root, LPCWSTR path, DWORD reserved,
                     wchar_t *class_name, DWORD options, DWORD access,
                     void *security, HKEY *key, DWORD *disposition) {
    (void)reserved;
    (void)class_name;
    (void)options;
    (void)access;
    (void)security;
    if ((intptr_t)root != REGISTRY_PARENT_HANDLE) return ERROR_ACCESS_DENIED;
    int tree = registry_tree_from_name(path);
    if (tree < 0 || stored_trees[tree].exists) return ERROR_ACCESS_DENIED;
    stored_trees[tree].exists = true;
    *key = (HKEY)(intptr_t)registry_tree_handle(tree);
    if (disposition) *disposition = REG_CREATED_NEW_KEY;
    return ERROR_SUCCESS;
}

LONG RegOpenKeyExW(HKEY root, LPCWSTR path, DWORD reserved, DWORD access,
                   HKEY *key) {
    (void)reserved;
    (void)access;
    if ((intptr_t)root != REGISTRY_PARENT_HANDLE) return ERROR_ACCESS_DENIED;
    int tree = registry_tree_from_name(path);
    if (tree < 0 || !stored_trees[tree].exists) return ERROR_FILE_NOT_FOUND;
    *key = (HKEY)(intptr_t)registry_tree_handle(tree);
    return ERROR_SUCCESS;
}

LONG RegCopyTreeW(HKEY source, LPCWSTR subkey, HKEY destination) {
    int source_tree = subkey ? registry_tree_from_name(subkey) : registry_tree_from_handle(source);
    int destination_tree = registry_tree_from_handle(destination);
    if (source_tree < 0 || destination_tree < 0 ||
        !stored_trees[source_tree].exists || !stored_trees[destination_tree].exists) {
        return ERROR_FILE_NOT_FOUND;
    }
    stored_trees[destination_tree] = stored_trees[source_tree];
    fallback_copy_count++;
    if (inject_interleaved_load && fallback_copy_count == 2) {
        inject_interleaved_load = false;
        interleaved_load_attempted = true;
        if (pthread_create(&interleaved_load_thread, NULL,
                run_interleaved_load, NULL) == 0) {
            interleaved_load_thread_started = true;
            pthread_mutex_lock(&test_state_mutex);
            while (!interleaved_load_thread_entered) {
                pthread_cond_wait(&test_state_changed, &test_state_mutex);
            }
            while (!interleaved_load_waiting && !interleaved_load_finished) {
                pthread_cond_wait(&test_state_changed, &test_state_mutex);
            }
            while (interleaved_load_waiting && !interleaved_load_lock_was_busy &&
                !interleaved_load_finished) {
                pthread_cond_wait(&test_state_changed, &test_state_mutex);
            }
            pthread_mutex_unlock(&test_state_mutex);
        }
    }
    return ERROR_SUCCESS;
}

LONG RegDeleteTreeW(HKEY root, LPCWSTR path) {
    if ((intptr_t)root != REGISTRY_PARENT_HANDLE) return ERROR_ACCESS_DENIED;
    if ((registry_parent_access & DELETE) == 0) return ERROR_ACCESS_DENIED;
    int tree = registry_tree_from_name(path);
    if (tree < 0 || !stored_trees[tree].exists) return ERROR_FILE_NOT_FOUND;
    stored_trees[tree].has_active_index_value = false;
    stored_trees[tree].active_index = 0;
    memset(stored_trees[tree].groups, 0, sizeof(stored_trees[tree].groups));
    return ERROR_SUCCESS;
}

LONG RegDeleteKeyW(HKEY root, LPCWSTR path) {
    if ((intptr_t)root != REGISTRY_PARENT_HANDLE) return ERROR_ACCESS_DENIED;
    int tree = registry_tree_from_name(path);
    if (tree < 0 || !stored_trees[tree].exists) return ERROR_FILE_NOT_FOUND;
    if (registry_tree_has_groups(tree) || stored_trees[tree].has_active_index_value) {
        return ERROR_ACCESS_DENIED;
    }
    memset(&stored_trees[tree], 0, sizeof(stored_trees[tree]));
    return ERROR_SUCCESS;
}

HMODULE GetModuleHandleW(LPCWSTR module_name) {
    return wcscmp(module_name, L"advapi32.dll") == 0 ? (HMODULE)(intptr_t)1 : NULL;
}

FARPROC GetProcAddress(HMODULE module, const char *name) {
    if ((intptr_t)module == 1 && rename_api_available &&
        strcmp(name, "RegRenameKey") == 0) {
        return (FARPROC)mock_reg_rename_key;
    }
    return NULL;
}

static LONG mock_reg_rename_key(HKEY key, LPCWSTR subkey_name, LPCWSTR new_name) {
    rename_api_call_count++;
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

static int test_legacy_rename_fallback_replaces_existing_tree(void) {
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

    if (!notify_groups_save(&updated) || rename_api_call_count != 0) {
        fprintf(stderr, "FAIL: registry tree replacement failed without RegRenameKey\n");
        return 1;
    }

    NotifyGroupManager loaded;
    memset(&loaded, 0, sizeof(loaded));
    notify_groups_load(&loaded);
    if (loaded.count != 2 || loaded.active_index != 1 ||
        strcmp(loaded.groups[0].name, "Replacement all") != 0 ||
        strcmp(loaded.groups[1].name, "Replacement work") != 0 ||
        loaded.groups[1].event_mask != 0x2u ||
        stored_trees[REGISTRY_TREE_STAGING].exists ||
        stored_trees[REGISTRY_TREE_BACKUP].exists) {
        fprintf(stderr, "FAIL: legacy rename fallback did not preserve the replacement tree\n");
        return 1;
    }
    return 0;
}

static int test_dynamic_rename_api_is_used_when_available(void) {
    reset_registry();
    rename_api_available = true;
    NotifyGroupManager manager;
    make_manager(&manager);

    if (!notify_groups_save(&manager) || rename_api_call_count != 1 ||
        (registry_parent_access & DELETE) != 0) {
        fprintf(stderr, "FAIL: dynamic rename export was unavailable or required unnecessary parent DELETE access\n");
        return 1;
    }
    return 0;
}

static int test_interrupted_copy_rename_restores_complete_backup_before_load(void) {
    reset_registry();
    stored_trees[REGISTRY_TREE_PRIMARY].exists = true;
    stored_trees[REGISTRY_TREE_PRIMARY].has_active_index_value = true;
    stored_trees[REGISTRY_TREE_PRIMARY].active_index = 0;
    seed_stored_group(0, "Partial active", 0x1u, 0);

    stored_trees[REGISTRY_TREE_BACKUP].exists = true;
    stored_trees[REGISTRY_TREE_BACKUP].has_active_index_value = true;
    stored_trees[REGISTRY_TREE_BACKUP].active_index = 1;
    stored_trees[REGISTRY_TREE_BACKUP].groups[0] = (StoredGroup){
        true, true, true, true, "Persisted all", 0xFFFFFFFFu, 1
    };
    stored_trees[REGISTRY_TREE_BACKUP].groups[1] = (StoredGroup){
        true, true, true, true, "Persisted work", 0x155u, 0
    };

    journal_phase_exists = true;
    journal_phase = 2;
    journal_source_exists = true;
    journal_destination_exists = true;
    wcscpy(journal_source, L"NotificationGroups");
    wcscpy(journal_destination, L"NotificationGroups_Backup");

    NotifyGroupManager loaded = {0};
    notify_groups_load(&loaded);
    if (loaded.count != 2 || loaded.active_index != 1 ||
        strcmp(loaded.groups[0].name, "Persisted all") != 0 ||
        strcmp(loaded.groups[1].name, "Persisted work") != 0 ||
        stored_trees[REGISTRY_TREE_PRIMARY].exists ||
        !stored_trees[REGISTRY_TREE_BACKUP].exists || journal_phase_exists) {
        fprintf(stderr, "FAIL: interrupted fallback rename did not recover its complete backup before load\n");
        return 1;
    }
    return 0;
}

static int test_load_during_fallback_rename_cannot_delete_live_destination(void) {
    reset_registry();
    stored_root_exists = true;
    has_active_index = true;
    stored_active_index = 0;
    seed_stored_group(0, "Persisted before save", 0x1u, 1);

    NotifyGroupManager updated;
    make_manager(&updated);
    strcpy(updated.groups[0].name, "Replacement all");
    strcpy(updated.groups[1].name, "Replacement work");
    updated.groups[1].event_mask = 0x2u;
    inject_interleaved_load = true;

    bool saved = notify_groups_save(&updated);
    if (interleaved_load_thread_started) {
        pthread_join(interleaved_load_thread, NULL);
    }
    if (!saved || !interleaved_load_attempted || !interleaved_load_thread_started ||
        !interleaved_load_thread_entered || !interleaved_load_finished ||
        !interleaved_load_lock_was_busy ||
        !global_mutex_name_requested ||
        interleaved_load.load_incomplete ||
        interleaved_load.count != 2 ||
        strcmp(interleaved_load.groups[0].name, "Replacement all") != 0 ||
        strcmp(interleaved_load.groups[1].name, "Replacement work") != 0 ||
        !stored_root_exists ||
        !stored_groups[0].exists || strcmp(stored_groups[0].name, "Replacement all") != 0 ||
        !stored_groups[1].exists || strcmp(stored_groups[1].name, "Replacement work") != 0 ||
        mutex_wait_count != 2 ||
        stored_trees[REGISTRY_TREE_STAGING].exists ||
        stored_trees[REGISTRY_TREE_BACKUP].exists || journal_phase_exists || mutex_held) {
        fprintf(stderr, "FAIL: a concurrent load disrupted the active fallback rename\n");
        return 1;
    }
    return 0;
}

static int test_load_without_write_access_when_no_rename_journal(void) {
    reset_registry();
    stored_root_exists = true;
    has_active_index = true;
    stored_active_index = 0;
    seed_stored_group(0, "Persisted all", 0xFFFFFFFFu, 1);
    deny_parent_mutation = true;

    NotifyGroupManager loaded = {0};
    notify_groups_load(&loaded);
    if (loaded.load_incomplete || loaded.count != 1 ||
        strcmp(loaded.groups[0].name, "Persisted all") != 0) {
        fprintf(stderr, "FAIL: loading groups without a pending rename required parent write access\n");
        return 1;
    }
    return 0;
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

    const char *valid_name = "  Personal events  ";
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

static int test_reload_without_registry_trees_clears_stale_groups(void) {
    reset_registry();
    stored_root_exists = true;
    has_active_index = true;
    stored_active_index = 1;
    seed_stored_group(0, "Persisted all", 0xFFFFFFFFu, 1);
    seed_stored_group(1, "Persisted work", 0x155u, 0);

    NotifyGroupManager loaded = {0};
    notify_groups_load(&loaded);
    if (loaded.count != 2 || loaded.active_index != 1) {
        fprintf(stderr, "FAIL: initial registry load did not populate the manager\n");
        return 1;
    }

    memset(&stored_trees[REGISTRY_TREE_PRIMARY], 0,
           sizeof(stored_trees[REGISTRY_TREE_PRIMARY]));
    memset(&stored_trees[REGISTRY_TREE_BACKUP], 0,
           sizeof(stored_trees[REGISTRY_TREE_BACKUP]));
    notify_groups_load(&loaded);
    if (loaded.load_incomplete || loaded.count != 0 || loaded.active_index != 0 ||
        notify_groups_get_active(&loaded) != NULL) {
        fprintf(stderr, "FAIL: reload without registry trees retained stale groups or selection\n");
        return 1;
    }

    if (!notify_groups_save(&loaded) || !stored_root_exists ||
        !has_active_index || stored_active_index != 0 ||
        registry_tree_has_groups(REGISTRY_TREE_PRIMARY) ||
        stored_trees[REGISTRY_TREE_BACKUP].exists) {
        fprintf(stderr, "FAIL: saving after an empty reload recreated deleted groups or selection\n");
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
    failures += test_legacy_rename_fallback_replaces_existing_tree();
    failures += test_dynamic_rename_api_is_used_when_available();
    failures += test_interrupted_copy_rename_restores_complete_backup_before_load();
    failures += test_load_during_fallback_rename_cannot_delete_live_destination();
    failures += test_load_without_write_access_when_no_rename_journal();
    failures += test_reload_without_registry_trees_clears_stale_groups();
    failures += test_load_skips_missing_group_slots_and_preserves_persistence();
    failures += test_load_skips_whitespace_only_group_and_maps_active_selection();
    failures += test_partial_load_cannot_overwrite_persisted_groups();

    if (failures != 0) return 1;
    puts("PASS: notification-group saves report registry failures and round-trip successfully");
    return 0;
}
