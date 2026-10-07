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
        r"\bstatic\s+(?:void|bool|int)\s+" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
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


read_dword = extract_function("settings_read_dword")
load_duration = extract_function("settings_load_custom_duration")
save_duration = extract_function("settings_save_custom_duration")
dialog = extract_function("tray_show_custom_dialog")

assert re.search(
    r"if\s*\(!custom_duration_loaded\)\s*\{\s*"
    r"last_custom_duration\s*=\s*settings_load_custom_duration\(\);\s*"
    r"custom_duration_loaded\s*=\s*true;",
    dialog,
), "the custom dialog must load the saved duration once per process"
assert re.search(
    r"if\s*\(result\s*>\s*0\)\s*\{\s*"
    r"if\s*\(settings_save_custom_duration\(result\)\)\s*\{\s*"
    r"last_custom_duration\s*=\s*result;\s*"
    r"\}\s*else\s*\{\s*"
    r"MessageBox\(tray->hwnd,\s*"
    r'"The custom duration could not be saved\. It will be used now but will not be remembered after NoSleep exits\.",\s*'
    r'"nosleep - Custom duration not saved",\s*MB_OK\s*\|\s*MB_ICONWARNING\);',
    dialog,
), "the dialog must remember a duration only after saving it and warn when saving fails"
assert re.search(
    r"minutes\s*>=\s*1\s*&&\s*minutes\s*<=\s*1440", tray
), "custom duration input must retain the 1-1440 validation"

windows_stubs = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int DWORD;
typedef long LONG;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef void *HKEY;

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_MORE_DATA 234
#define KEY_READ 1
#define KEY_WRITE 2
#define REG_DWORD 4
#define REG_SZ 1
#define REG_OPTION_NON_VOLATILE 0
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)1)
#define SETTINGS_REG_KEY "Software\\nosleep\\settings"

static LONG key_create_result;
static LONG value_query_result;
static LONG value_write_result;
static DWORD stored_value;
static DWORD stored_type;
static bool value_present;
static int registry_write_count;
static int registry_close_count;
static char last_key_path[128];
static char last_value_name[64];

LONG RegCreateKeyEx(HKEY root, const char *path, DWORD reserved,
                    const char *class_name, DWORD options, DWORD access,
                    void *security, HKEY *key, DWORD *disposition);
LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved,
                     DWORD *type, LPBYTE value, DWORD *size);
LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   LPBYTE value, DWORD size);
LONG RegCloseKey(HKEY key);

LONG RegCreateKeyEx(HKEY root, const char *path, DWORD reserved,
                    const char *class_name, DWORD options, DWORD access,
                    void *security, HKEY *key, DWORD *disposition) {
    (void)root; (void)reserved; (void)class_name; (void)options;
    (void)security; (void)disposition; (void)access;
    snprintf(last_key_path, sizeof(last_key_path), "%s", path);
    if (key_create_result == ERROR_SUCCESS) *key = (HKEY)(uintptr_t)2;
    return key_create_result;
}

LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved,
                     DWORD *type, LPBYTE value, DWORD *size) {
    (void)key; (void)reserved;
    snprintf(last_value_name, sizeof(last_value_name), "%s", name);
    if (value_query_result != ERROR_SUCCESS) return value_query_result;
    if (!value_present) return ERROR_FILE_NOT_FOUND;
    if (type) *type = stored_type;
    if (!value) {
        *size = sizeof(stored_value);
        return ERROR_SUCCESS;
    }
    if (*size < sizeof(stored_value)) {
        *size = sizeof(stored_value);
        return ERROR_MORE_DATA;
    }
    memcpy(value, &stored_value, sizeof(stored_value));
    *size = sizeof(stored_value);
    return ERROR_SUCCESS;
}

LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   LPBYTE value, DWORD size) {
    (void)key; (void)reserved;
    ++registry_write_count;
    snprintf(last_value_name, sizeof(last_value_name), "%s", name);
    if (value_write_result != ERROR_SUCCESS) return value_write_result;
    if (type != REG_DWORD || size != sizeof(stored_value)) return ERROR_ACCESS_DENIED;
    memcpy(&stored_value, value, sizeof(stored_value));
    stored_type = type;
    value_present = true;
    return ERROR_SUCCESS;
}

LONG RegCloseKey(HKEY key) {
    (void)key;
    ++registry_close_count;
    return ERROR_SUCCESS;
}
"""

behavior_main = r"""
static int failures;

static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main(void) {
    expect(settings_load_custom_duration() == 30,
           "a missing custom duration must retain the existing 30-minute default");
    expect(registry_close_count == 1,
           "loading a missing duration must close its settings key");

    expect(settings_save_custom_duration(75),
           "a successful registry write must report persistence success");
    expect(value_present && stored_value == 75 && stored_type == REG_DWORD,
           "an accepted custom duration must be saved as a DWORD");
    expect(strcmp(last_key_path, SETTINGS_REG_KEY) == 0 &&
           strcmp(last_value_name, "custom_duration_minutes") == 0,
           "the duration must use the existing settings registry key and value name");
    expect(settings_load_custom_duration() == 75,
           "a saved custom duration must be restored by a fresh settings load");

    int writes_before_key_failure = registry_write_count;
    int closes_before_key_failure = registry_close_count;
    key_create_result = ERROR_ACCESS_DENIED;
    expect(!settings_save_custom_duration(90),
           "failure to open the settings key must report persistence failure");
    expect(registry_write_count == writes_before_key_failure,
           "a failed key creation must not attempt to write the duration");
    expect(registry_close_count == closes_before_key_failure,
           "a failed key creation must not close an unopened key");
    key_create_result = ERROR_SUCCESS;

    int closes_before_write_failure = registry_close_count;
    value_write_result = ERROR_ACCESS_DENIED;
    expect(!settings_save_custom_duration(90),
           "failure to write the custom duration must report persistence failure");
    expect(registry_close_count == closes_before_write_failure + 1,
           "the settings key must close after a failed value write");
    expect(stored_value == 75,
           "a failed value write must not replace the previously saved duration");
    value_write_result = ERROR_SUCCESS;

    settings_save_custom_duration(1);
    expect(settings_load_custom_duration() == 1,
           "the smallest valid custom duration must round-trip");
    settings_save_custom_duration(1440);
    expect(settings_load_custom_duration() == 1440,
           "the largest valid custom duration must round-trip");

    int writes_before_invalid = registry_write_count;
    expect(!settings_save_custom_duration(0) &&
           !settings_save_custom_duration(1441),
           "out-of-range durations must report that nothing was saved");
    expect(registry_write_count == writes_before_invalid,
           "out-of-range durations must not be persisted");

    stored_value = 0;
    expect(settings_load_custom_duration() == 30,
           "an invalid saved duration must fall back to 30 minutes");
    stored_value = 1441;
    expect(settings_load_custom_duration() == 30,
           "an out-of-range saved duration must fall back to 30 minutes");
    stored_value = 75;
    stored_type = REG_SZ;
    expect(settings_load_custom_duration() == 30,
           "a non-DWORD saved duration must fall back to 30 minutes");

    key_create_result = ERROR_ACCESS_DENIED;
    expect(settings_load_custom_duration() == 30,
           "an unavailable settings key must retain the default duration");
    expect(failures == 0, "all custom duration persistence checks must pass");
    return failures ? 1 : 0;
}
"""

with tempfile.TemporaryDirectory(prefix="nosleep-custom-duration-") as temp_dir:
    harness = Path(temp_dir) / "test_custom_duration.c"
    executable = Path(temp_dir) / "test_custom_duration"
    harness.write_text(
        windows_stubs + "\n" + read_dword + "\n" + load_duration + "\n" +
        save_duration + "\n" + behavior_main
    )
    subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("PASS: custom duration settings persist and restore valid registry values")
PY
