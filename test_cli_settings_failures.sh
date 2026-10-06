#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\b(?:static\s+)?(?:void|bool)\s+" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        tray,
        re.S,
    )
    assert definition, f"could not find definition for {name}"
    start = definition.start()
    opening = definition.end() - 1

    depth = 0
    state = "code"
    i = opening
    while i < len(tray):
        char = tray[i]
        next_two = tray[i : i + 2]
        if state == "code":
            if next_two == "//":
                state = "line_comment"
                i += 2
                continue
            if next_two == "/*":
                state = "block_comment"
                i += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return tray[start : i + 1]
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if next_two == "*/":
                state = "code"
                i += 2
                continue
        elif state in ("string", "char"):
            if char == "\\":
                i += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function: {name}")


windows_stubs = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef unsigned int DWORD;
typedef long LONG;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef void *HKEY;
typedef void *HWND;
typedef intptr_t LPARAM;
typedef int SessionFinishedAction;

typedef struct {
    bool prevent_display;
    bool away_mode;
    bool verbose;
    bool check_updates_on_startup;
    int auto_check_interval;
    int notification_mode;
    bool add_to_path;
    bool start_on_startup;
    int session_finished_action;
    int notify_groups;
} NoSleepTray;

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_MORE_DATA 234
#define KEY_READ 1
#define KEY_WRITE 2
#define REG_SZ 1
#define REG_DWORD 4
#define REG_EXPAND_SZ 2
#define REG_OPTION_NON_VOLATILE 0
#define HKEY_CURRENT_USER ((HKEY)1)
#define HWND_BROADCAST ((HWND)(uintptr_t)0xffff)
#define WM_SETTINGCHANGE 0x001a
#define SMTO_ABORTIFHUNG 0x0002
#define SETTINGS_REG_KEY "Software\\nosleep\\settings"
#define SESSION_FINISHED_NONE 0
#define SESSION_FINISHED_SHUTDOWN 1
#define SESSION_FINISHED_SLEEP 2
#define NOTIFY_ALL 0
#define NOTIFY_CRITICAL_ONLY 1
#define NOTIFY_NONE 2
#define MB_OK 0
#define MB_ICONWARNING 1

static LONG settings_create_result;
static bool saved_update_settings;
static int settings_query_count;
static LONG startup_create_result;
static LONG startup_open_result;
static LONG environment_open_result;
static LONG path_query_result;
static LONG path_data_query_result;
static LONG registry_write_result;
static LONG registry_delete_result;
static const char *failed_value_name;
static bool add_path_result;
static const BYTE *path_data;
static DWORD path_data_size;
static wchar_t written_path[1024];
static DWORD written_path_type;
static DWORD stored_add_to_path;
static int registry_write_count;
static int registry_close_count;
static int notify_groups_save_count;
static bool notify_groups_save_result;
static int settings_warning_count;
static int dialog_destroy_count;
static bool requested_startup;
static int startup_create_count;
static int add_path_count;
static int environment_open_count;
static int environment_create_count;

static bool notify_groups_save(int *groups) {
    (void)groups;
    ++notify_groups_save_count;
    return notify_groups_save_result;
}

static int MessageBox(HWND hwnd, const char *text, const char *title, DWORD flags) {
    (void)hwnd; (void)text; (void)title; (void)flags;
    ++settings_warning_count;
    return 0;
}

static void reset_mocks(void) {
    settings_create_result = ERROR_SUCCESS;
    saved_update_settings = false;
    settings_query_count = 0;
    startup_create_result = ERROR_SUCCESS;
    startup_open_result = ERROR_SUCCESS;
    environment_open_result = ERROR_SUCCESS;
    path_query_result = ERROR_FILE_NOT_FOUND;
    path_data_query_result = ERROR_SUCCESS;
    registry_write_result = ERROR_SUCCESS;
    registry_delete_result = ERROR_SUCCESS;
    failed_value_name = NULL;
    path_data = NULL;
    path_data_size = 0;
    written_path[0] = '\0';
    written_path_type = 0;
    stored_add_to_path = 1;
    registry_write_count = 0;
    registry_close_count = 0;
    notify_groups_save_count = 0;
    notify_groups_save_result = true;
    settings_warning_count = 0;
    startup_create_count = 0;
    add_path_result = true;
    add_path_count = 0;
    environment_open_count = 0;
    environment_create_count = 0;
}

LONG RegCreateKeyEx(HKEY root, const char *path, DWORD reserved,
                    const char *class_name, DWORD options, DWORD access,
                    void *security, HKEY *key, DWORD *disposition) {
    (void)root; (void)reserved; (void)class_name; (void)options;
    (void)access; (void)security; (void)disposition;
    if (strcmp(path, "Software\\Microsoft\\Windows\\CurrentVersion\\Run") == 0) {
        ++startup_create_count;
        if (startup_create_result == ERROR_SUCCESS) *key = (HKEY)5;
        return startup_create_result;
    }
    if (strcmp(path, "Environment") == 0) ++environment_create_count;
    if (settings_create_result == ERROR_SUCCESS) *key = (HKEY)2;
    return settings_create_result;
}

LONG RegOpenKeyEx(HKEY root, const char *path, DWORD reserved, DWORD access, HKEY *key) {
    (void)root; (void)reserved; (void)access;
    if (strcmp(path, "Environment") == 0) {
        ++environment_open_count;
        if (environment_open_result == ERROR_SUCCESS) *key = (HKEY)4;
        return environment_open_result;
    }
    if (startup_open_result == ERROR_SUCCESS) *key = (HKEY)3;
    return startup_open_result;
}

LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size) {
    (void)reserved;
    ++registry_write_count;
    if (failed_value_name && strcmp(name, failed_value_name) == 0) {
        return registry_write_result;
    }
    if (strcmp(name, "Path") == 0 && value && size <= sizeof(written_path)) {
        memcpy(written_path, value, size);
        written_path_type = type;
    }
    if (key == (HKEY)2 && strcmp(name, "add_to_path") == 0 &&
        value && size == sizeof(stored_add_to_path)) {
        memcpy(&stored_add_to_path, value, sizeof(stored_add_to_path));
    }
    return ERROR_SUCCESS;
}

LONG RegDeleteValue(HKEY key, const char *name) {
    (void)key; (void)name;
    return registry_delete_result;
}

LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved, DWORD *type,
                     BYTE *value, DWORD *size) {
    (void)reserved;
    if (key == (HKEY)2) {
        ++settings_query_count;
        if (saved_update_settings &&
            (strcmp(name, "check_updates_on_startup") == 0 ||
             strcmp(name, "auto_check_interval") == 0)) {
            DWORD data = strcmp(name, "auto_check_interval") == 0 ? 2 : 0;
            memcpy(value, &data, sizeof(data));
            *size = sizeof(data);
            if (type) *type = REG_DWORD;
            return ERROR_SUCCESS;
        }
        return ERROR_FILE_NOT_FOUND;
    }
    if (strcmp(name, "Path") != 0) return ERROR_FILE_NOT_FOUND;
    if (!value) {
        if (path_query_result != ERROR_SUCCESS) return path_query_result;
        *size = path_data_size;
        return ERROR_SUCCESS;
    }
    if (path_data_query_result != ERROR_SUCCESS) return path_data_query_result;
    if (*size < path_data_size) {
        *size = path_data_size;
        return ERROR_MORE_DATA;
    }
    if (path_data_size > 0) memcpy(value, path_data, path_data_size);
    *size = path_data_size;
    return ERROR_SUCCESS;
}

LONG RegCloseKey(HKEY key) {
    (void)key;
    ++registry_close_count;
    return ERROR_SUCCESS;
}


LONG RegOpenKeyExW(HKEY root, const wchar_t *path, DWORD reserved, DWORD access, HKEY *key) {
    const char *key_path = wcscmp(path, L"Environment") == 0 ? "Environment" :
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    return RegOpenKeyEx(root, key_path, reserved, access, key);
}
LONG RegCreateKeyExW(HKEY root, const wchar_t *path, DWORD reserved, void *class_name,
                     DWORD options, DWORD access, void *security, HKEY *key, DWORD *disposition) {
    const char *key_path = wcscmp(path, L"Environment") == 0 ? "Environment" :
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    return RegCreateKeyEx(root, key_path, reserved, class_name, options, access,
                          security, key, disposition);
}
LONG RegQueryValueExW(HKEY key, const wchar_t *name, DWORD *reserved, DWORD *type,
                      BYTE *value, DWORD *size) {
    (void)name;
    return RegQueryValueEx(key, "Path", reserved, type, value, size);
}
LONG RegSetValueExW(HKEY key, const wchar_t *name, DWORD reserved, DWORD type,
                    const BYTE *value, DWORD size) {
    const char *value_name = wcscmp(name, L"Path") == 0 ? "Path" : "nosleep";
    return RegSetValueEx(key, value_name, reserved, type, value, size);
}
LONG RegDeleteValueW(HKEY key, const wchar_t *name) {
    (void)name;
    return RegDeleteValue(key, "nosleep");
}
#define SendMessageTimeoutW SendMessageTimeout

static wchar_t *get_exe_path_w(void) {
    const wchar_t *path = L"C:\\NoSleep\\nosleep.exe";
    wchar_t *copy = (wchar_t *)malloc((wcslen(path) + 1) * sizeof(wchar_t));
    if (copy) wcscpy(copy, path);
    return copy;
}

static wchar_t *get_exe_dir(void) {
    const wchar_t *dir = L"C:\\NoSleep";
    wchar_t *copy = (wchar_t *)malloc((wcslen(dir) + 1) * sizeof(wchar_t));
    if (copy) wcscpy(copy, dir);
    return copy;
}

static bool add_app_to_path(void) {
    ++add_path_count;
    return add_path_result;
}

static int str_icmp_n(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (unsigned int)ca - (unsigned int)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

uintptr_t SendMessageTimeout(HWND hwnd, DWORD message, uintptr_t wparam,
                             LPARAM lparam, DWORD flags, DWORD timeout,
                             uintptr_t *result) {
    (void)hwnd; (void)message; (void)wparam; (void)lparam;
    (void)flags; (void)timeout; (void)result;
    return 1;
}


#define BM_GETCHECK 1
#define BST_CHECKED 1
#define CB_GETCURSEL 2
static HWND hGeneralTab;
static HWND hIntervalCombo;
static NoSleepTray *settings_tray;
static intptr_t SendDlgItemMessage(HWND hwnd, int id, int message, int wparam, int lparam) {
    (void)hwnd; (void)message; (void)wparam; (void)lparam;
    return id == 2003 && requested_startup ? BST_CHECKED : 0;
}
static intptr_t SendMessage(HWND hwnd, int message, int wparam, int lparam) {
    (void)hwnd; (void)message; (void)wparam; (void)lparam;
    return 0;
}
static void tray_apply_auto_check_interval(NoSleepTray *tray, int sel) {
    tray->auto_check_interval = sel;
}
static void DestroyWindow(HWND hwnd) { (void)hwnd; ++dialog_destroy_count; }
static int failures;
static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
"""

behavior_main = r"""
static void expect_write_failure(const char *name, int session_finished_action,
                                 int auto_start, int notification_mode,
                                 int auto_check_interval, int check_updates_startup,
                                 int add_to_path) {
    reset_mocks();
    failed_value_name = name;
    registry_write_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(session_finished_action, auto_start,
                                   notification_mode, auto_check_interval,
                                   check_updates_startup, add_to_path),
           "a failed requested settings value write must make CLI save fail");
    expect(registry_close_count == 1,
           "the settings registry handle must close after a failed value write");
}

int main(void) {
    for (int enable = 0; enable <= 1; ++enable) {
        reset_mocks();
        NoSleepTray dialog_tray = { .start_on_startup = !enable };
        settings_tray = &dialog_tray;
        requested_startup = enable;
        dialog_destroy_count = 0;
        failed_value_name = "nosleep";
        registry_write_result = ERROR_ACCESS_DENIED;
        registry_delete_result = ERROR_ACCESS_DENIED;
        settings_ok(NULL);
        expect(settings_warning_count == 1, "failed startup changes must show a warning");
        expect(dialog_destroy_count == 0, "failed startup changes must keep settings open");
        expect(dialog_tray.start_on_startup == !enable, "failed startup changes preserve actual state");
        reset_mocks();
        settings_ok(NULL);
        expect(dialog_tray.start_on_startup == !!enable, "retry must apply the startup change");
        expect(dialog_destroy_count == 1 && settings_warning_count == 0,
               "successful retry must save and close without a warning");
    }

    reset_mocks();
    settings_create_result = ERROR_ACCESS_DENIED;
    NoSleepTray loaded_tray = {0};
    tray_load_settings(&loaded_tray);
    expect(loaded_tray.check_updates_on_startup && loaded_tray.auto_check_interval == 1,
           "an unavailable settings key must retain startup and daily update checks");
    expect(!loaded_tray.prevent_display && !loaded_tray.away_mode && !loaded_tray.verbose &&
           loaded_tray.notification_mode == NOTIFY_ALL && !loaded_tray.add_to_path &&
           loaded_tray.session_finished_action == SESSION_FINISHED_NONE,
           "an unavailable settings key must apply all remaining settings defaults");
    expect(settings_query_count == 0 && registry_close_count == 0,
           "load failure must not query or close an invalid settings handle");

    reset_mocks();
    tray_load_settings(&loaded_tray);
    expect(loaded_tray.check_updates_on_startup && loaded_tray.auto_check_interval == 1 &&
           settings_query_count == 8 && registry_close_count == 1,
           "missing settings values must retain defaults and close the opened key");

    reset_mocks();
    saved_update_settings = true;
    tray_load_settings(&loaded_tray);
    expect(!loaded_tray.check_updates_on_startup && loaded_tray.auto_check_interval == 2 &&
           registry_close_count == 1,
           "successfully loaded update preferences must override defaults");

    reset_mocks();
    settings_create_result = ERROR_ACCESS_DENIED;
    NoSleepTray settings_tray = {0};
    expect(!tray_save_settings(&settings_tray),
           "failure to create the settings key must be reported by the tray save");
    expect(registry_write_count == 0,
           "a failed settings-key creation must not attempt value writes");
    expect(registry_close_count == 0,
           "a failed settings-key creation must not close an invalid handle");
    expect(notify_groups_save_count == 1,
           "notification groups must still be saved independently when the settings key fails");
    reset_mocks();
    settings_create_result = ERROR_ACCESS_DENIED;
    tray_save_settings_with_warning(NULL, &settings_tray);
    expect(settings_warning_count == 1,
           "the settings UI warning must be shown when the settings key cannot be created");

    reset_mocks();
    failed_value_name = "auto_check_interval";
    registry_write_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings(&settings_tray),
           "a failed individual tray settings write must be reported");
    expect(registry_write_count == 8,
           "a failed tray setting must not prevent attempts to save the remaining values");
    expect(registry_close_count == 1,
           "the settings registry handle must close after a failed tray setting write");
    expect(notify_groups_save_count == 1,
           "notification groups must retain their existing save behavior after value failure");
    reset_mocks();
    failed_value_name = "auto_check_interval";
    registry_write_result = ERROR_ACCESS_DENIED;
    tray_save_settings_with_warning(NULL, &settings_tray);
    expect(settings_warning_count == 1,
           "the settings UI warning must be shown when a settings value write fails");

    reset_mocks();
    expect(tray_save_settings(&settings_tray),
           "successful tray settings persistence must report success");
    expect(registry_write_count == 8,
           "successful tray settings persistence must write all eight values");
    expect(registry_close_count == 1,
           "successful tray settings persistence must close its registry handle");
    expect(notify_groups_save_count == 1,
           "successful tray settings persistence must continue saving notification groups");
    reset_mocks();
    tray_save_settings_with_warning(NULL, &settings_tray);
    expect(settings_warning_count == 0,
           "the settings UI warning must not be shown after a successful save");

    reset_mocks();
    notify_groups_save_result = false;
    expect(!tray_save_settings(&settings_tray),
           "a notification-group save failure must make the overall tray save fail");
    expect(notify_groups_save_count == 1,
           "the overall tray save must attempt notification groups exactly once");
    reset_mocks();
    notify_groups_save_result = false;
    expect(!tray_save_settings_with_warning(NULL, &settings_tray),
           "the settings UI save helper must report a notification-group save failure");
    expect(settings_warning_count == 1,
           "the settings UI warning must be shown when notification groups fail to save");

    reset_mocks();
    settings_create_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(SESSION_FINISHED_SLEEP, -1, -1, -1, -1, -1),
           "failure to open the settings key must be reported");

    reset_mocks();
    environment_open_result = ERROR_FILE_NOT_FOUND;
    expect(add_app_to_path_regression(),
           "adding PATH must succeed when the Environment key is absent");
    expect(environment_create_count > 0,
           "adding PATH must create the absent Environment key");
    expect(wcscmp(written_path, L"C:\\NoSleep") == 0,
           "adding PATH must write the executable directory to the new key");
    expect(written_path_type == REG_EXPAND_SZ,
           "the new Path value must use REG_EXPAND_SZ");

    reset_mocks();
    static const wchar_t existing_path_for_add[] = L"C:\\Windows\\System32;C:\\Tools";
    path_query_result = ERROR_SUCCESS;
    path_data = (const BYTE *)existing_path_for_add;
    path_data_size = sizeof(existing_path_for_add);
    expect(add_app_to_path_regression(),
           "adding PATH must succeed when existing entries are present");
    expect(wcscmp(written_path,
                  L"C:\\Windows\\System32;C:\\Tools;C:\\NoSleep") == 0,
           "adding PATH must preserve existing entries and append the executable directory");

    expect_write_failure("session_finished_action", SESSION_FINISHED_SLEEP, -1, -1, -1, -1, -1);
    expect_write_failure("notification_mode", -1, -1, NOTIFY_NONE, -1, -1, -1);
    expect_write_failure("auto_check_interval", -1, -1, -1, 2, -1, -1);
    expect_write_failure("check_updates_on_startup", -1, -1, -1, -1, 0, -1);
    expect_write_failure("add_to_path", -1, -1, -1, -1, -1, 1);

    reset_mocks();
    startup_open_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(-1, 1, -1, -1, -1, -1),
           "failure to open the startup registry key must be reported");

    reset_mocks();
    failed_value_name = "nosleep";
    registry_write_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(-1, 1, -1, -1, -1, -1),
           "failure to write the startup registry value must be reported");

    reset_mocks();
    registry_delete_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(-1, 0, -1, -1, -1, -1),
           "failure to remove the startup registry value must be reported");

    reset_mocks();
    startup_open_result = ERROR_FILE_NOT_FOUND;
    expect(tray_save_settings_cli(-1, 0, -1, -1, -1, -1),
           "removing startup when the Run key is absent must succeed");

    reset_mocks();
    registry_delete_result = ERROR_FILE_NOT_FOUND;
    expect(tray_save_settings_cli(-1, 0, -1, -1, -1, -1),
           "removing startup when its value is absent must succeed");

    reset_mocks();
    startup_open_result = ERROR_FILE_NOT_FOUND;
    NoSleepTray startup_tray = { .start_on_startup = false };
    tray_set_startup_enabled(&startup_tray, true);
    expect(startup_create_count == 1,
           "enabling startup must create the missing Run registry key");
    expect(registry_write_count == 1,
           "enabling startup after creating the Run key must write its registry value");
    expect(startup_tray.start_on_startup,
           "successful startup registration must update the in-memory state");

    reset_mocks();
    failed_value_name = "nosleep";
    registry_write_result = ERROR_ACCESS_DENIED;
    startup_tray.start_on_startup = false;
    tray_set_startup_enabled(&startup_tray, true);
    expect(!startup_tray.start_on_startup,
           "a failed startup registry write must not leave startup enabled in memory");

    reset_mocks();
    add_path_result = false;
    expect(!tray_save_settings_cli(-1, -1, -1, -1, -1, 1),
           "failure to add the application to PATH must be reported");
    expect(add_path_count == 1, "the requested PATH add helper must run");

    reset_mocks();
    environment_open_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(-1, -1, -1, -1, -1, 0),
           "failure to remove the application from PATH must be reported");
    expect(environment_open_count == 1, "the requested PATH remove helper must run");
    expect(stored_add_to_path == 0,
           "the failed removal must retain the user's disabled preference for retry");

    NoSleepTray saved_disabled_preference = { .add_to_path = stored_add_to_path != 0 };
    expect(!apply_path_preference(saved_disabled_preference.add_to_path),
           "startup must retry removal when the persisted preference is disabled");
    expect(environment_open_count == 2,
           "a failed CLI removal must not suppress the startup removal retry");
    environment_open_result = ERROR_SUCCESS;
    expect(apply_path_preference(saved_disabled_preference.add_to_path),
           "a later startup must retry the disabled PATH preference again");
    expect(environment_open_count == 3,
           "each startup must attempt removal while the saved preference is disabled");

    reset_mocks();
    NoSleepTray tray = { .add_to_path = true };
    environment_open_result = ERROR_ACCESS_DENIED;
    expect(!tray_set_add_to_path(&tray, false),
           "the tray setter must report a failed PATH removal");
    expect(!tray.add_to_path,
           "the in-memory preference must retain the requested disabled state for retry");
    environment_open_result = ERROR_SUCCESS;
    expect(apply_path_preference(tray.add_to_path),
           "the in-memory disabled preference must retry PATH removal after failure");

    reset_mocks();
    static const wchar_t existing_path[] = L"C:\\Windows\\System32;C:\\NoSleep;C:\\Temp";
    path_query_result = ERROR_SUCCESS;
    path_data = (const BYTE *)existing_path;
    path_data_size = sizeof(existing_path);
    failed_value_name = "Path";
    registry_write_result = ERROR_ACCESS_DENIED;
    expect(!tray_save_settings_cli(-1, -1, -1, -1, -1, 0),
           "failure to write the updated PATH must be reported");

    reset_mocks();
    static const wchar_t path_removed_during_read[] = L"C:\\Windows\\System32;C:\\NoSleep;C:\\Temp";
    path_query_result = ERROR_SUCCESS;
    path_data_query_result = ERROR_FILE_NOT_FOUND;
    path_data = (const BYTE *)path_removed_during_read;
    path_data_size = sizeof(path_removed_during_read);
    expect(remove_app_from_path(),
           "PATH removal must succeed when the value disappears after its size query");

    reset_mocks();
    path_query_result = ERROR_SUCCESS;
    path_data_query_result = ERROR_FILE_NOT_FOUND;
    path_data = (const BYTE *)path_removed_during_read;
    path_data_size = sizeof(path_removed_during_read);
    expect(tray_save_settings_cli(-1, -1, -1, -1, -1, 0),
           "CLI PATH removal must succeed when the value disappears during the read");

    reset_mocks();
    environment_open_result = ERROR_FILE_NOT_FOUND;
    expect(tray_save_settings_cli(-1, -1, -1, -1, -1, 0),
           "removing PATH when the Environment key is absent must succeed");

    reset_mocks();
    expect(tray_save_settings_cli(-1, -1, -1, -1, -1, 0),
           "removing PATH when the Path value is absent must succeed");

    reset_mocks();
    expect(tray_save_settings_cli(SESSION_FINISHED_SLEEP, 1, NOTIFY_ALL, 2, 1, 1),
           "successful registry, startup, and PATH writes must still report success");

    if (failures) return 1;
    puts("PASS: tray and CLI settings persistence failures are reported");
    return 0;
}
"""

add_app_to_path_test_function = extract_function("add_app_to_path").replace(
    "add_app_to_path", "add_app_to_path_regression", 1
)

ok_start = tray.index("case IDC_SETTINGS_OK:")
ok_end = tray.index("case IDC_SETTINGS_CANCEL:", ok_start)
ok_body = tray[ok_start:ok_end].split("{", 1)[1].rsplit("}", 1)[0]
control_ids = "\n".join(re.findall(r"^#define IDC_\w+\s+\d+.*$", tray, re.M))
ok_function = control_ids + "\nstatic void settings_ok(HWND hwnd) { do {\n" + ok_body + "\n} while (0); }\n"

source = (
    windows_stubs
    + "\n"
    + add_app_to_path_test_function
    + "\n"
    + extract_function("remove_app_from_path")
    + "\n"
    + extract_function("apply_path_preference")
    + "\n"
    + extract_function("set_startup_registry")
    + "\n"
    + extract_function("tray_set_startup_enabled")
    + "\n"
    + extract_function("tray_set_add_to_path")
    + "\n"
    + extract_function("settings_read_dword")
    + "\n"
    + extract_function("tray_load_settings")
    + "\n"
    + extract_function("tray_save_settings")
    + "\n"
    + extract_function("tray_save_settings_with_warning")
    + "\n"
    + extract_function("tray_save_settings_cli")
    + "\n"
    + ok_function
    + behavior_main
)

with tempfile.TemporaryDirectory() as temp_dir:
    source_path = Path(temp_dir) / "test_cli_settings_failures.c"
    binary_path = Path(temp_dir) / "test_cli_settings_failures"
    source_path.write_text(source)
    subprocess.run(
        ["gcc", "-std=c99", "-Wall", "-Wextra", "-o", str(binary_path), str(source_path)],
        check=True,
    )
    subprocess.run([str(binary_path)], check=True)
PY
