#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / 'src/tray.c').read_text()
# Compile the production PATH block, not a copied implementation.
block = tray[tray.index('static bool is_supported_path_registry_type(DWORD type) {'):tray.index('static bool apply_path_preference(bool add_to_path) {')]
startup_state = tray[tray.index('static bool is_startup_enabled(void) {'):tray.index('static bool set_startup_registry(bool enable) {')]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
typedef uint32_t DWORD;
typedef long LONG;
typedef unsigned char *LPBYTE;
typedef void *HKEY;
typedef intptr_t LPARAM;
#define MAX_PATH 260
#define MAXDWORD ((DWORD)~0U)
#define HKEY_CURRENT_USER ((HKEY)1)
#define KEY_READ 1
#define KEY_WRITE 2
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define REG_OPTION_NON_VOLATILE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_MULTI_SZ 7
#define HWND_BROADCAST 0
#define WM_SETTINGCHANGE 0
#define SMTO_ABORTIFHUNG 0
#define TRUE 1
#define CSTR_EQUAL 2
// Model the Windows API for the fixture's exact Unicode and ASCII-case inputs.
int CompareStringOrdinal(const wchar_t *a, int a_len, const wchar_t *b,
                         int b_len, int ignore_case) {
    if (a_len != b_len) return 1;
    for (int i = 0; i < a_len; ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ignore_case) {
            if (ca >= L'A' && ca <= L'Z') ca += L'a' - L'A';
            if (cb >= L'A' && cb <= L'Z') cb += L'a' - L'A';
        }
        if (ca != cb) return ca < cb ? 1 : 3;
    }
    return CSTR_EQUAL;
}
static const wchar_t *exe = L"C:\\安装\\😀\\nosleep.exe";
static wchar_t registry[2048];
static wchar_t startup_registry[2048];
static DWORD registry_bytes;
static DWORD startup_registry_bytes;
static DWORD registry_type = REG_EXPAND_SZ;
static int change_type_on_data_read;
static DWORD data_read_type;
static DWORD startup_registry_type = REG_SZ;
static int ansi_calls;
static int writes;
static int grow_on_read;
static int registry_exists = 1;
static int fail_module_filename;
static int fail_malloc;
static int fail_realloc;
static int module_filename_calls;
static void *test_malloc(size_t size) {
    return fail_malloc ? NULL : malloc(size);
}
static void *test_realloc(void *ptr, size_t size) {
    return fail_realloc ? NULL : realloc(ptr, size);
}
#define malloc test_malloc
#define realloc test_realloc
DWORD GetModuleFileNameW(void *module, wchar_t *out, DWORD size) {
    (void)module;
    ++module_filename_calls;
    if (fail_module_filename) return 0;
    if (wcslen(exe) >= size) {
        wmemcpy(out, exe, size);
        return size;
    }
    wcscpy(out, exe);
    return (DWORD)wcslen(out);
}
DWORD GetModuleFileName(void *module, char *out, DWORD size) {
    (void)module; (void)size;
    ++ansi_calls;
    strcpy(out, "C:\\??\\?\\nosleep.exe");
    return (DWORD)strlen(out);
}
LONG RegOpenKeyExW(HKEY root, const wchar_t *name, DWORD a, DWORD b, HKEY *key) {
    (void)root; (void)a; (void)b;
    if (wcscmp(name, L"Environment") == 0) {
        *key = (HKEY)2;
        return ERROR_SUCCESS;
    }
    if (wcscmp(name, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") == 0) {
        *key = (HKEY)3;
        return ERROR_SUCCESS;
    }
    assert(0);
    return ERROR_FILE_NOT_FOUND;
}
LONG RegOpenKeyEx(HKEY root, const char *name, DWORD a, DWORD b, HKEY *key) {
    (void)name; ++ansi_calls; return RegOpenKeyExW(root, L"Environment", a, b, key);
}
LONG RegCreateKeyExW(HKEY root, const wchar_t *name, DWORD a, void *c, DWORD d,
                    DWORD b, void *e, HKEY *key, void *f) {
    (void)c; (void)d; (void)e; (void)f; return RegOpenKeyExW(root, name, a, b, key);
}
LONG RegCreateKeyEx(HKEY root, const char *name, DWORD a, void *c, DWORD d,
                   DWORD b, void *e, HKEY *key, void *f) {
    (void)name; ++ansi_calls; return RegCreateKeyExW(root, L"Environment", a, c, d, b, e, key, f);
}
LONG RegQueryValueExW(HKEY key, const wchar_t *name, void *reserved, DWORD *type,
                     LPBYTE out, DWORD *size) {
    (void)reserved;
    if (key == (HKEY)2 && out && change_type_on_data_read) {
        registry_type = data_read_type;
        change_type_on_data_read = 0;
    }
    wchar_t *value;
    DWORD value_bytes;
    DWORD value_type;
    if (key == (HKEY)3) {
        assert(wcscmp(name, L"nosleep") == 0);
        value = startup_registry;
        value_bytes = startup_registry_bytes;
        value_type = startup_registry_type;
    } else {
        assert(key == (HKEY)2 && wcscmp(name, L"Path") == 0);
        value = registry;
        value_bytes = registry_bytes;
        value_type = registry_type;
    }
    if (type) *type = value_type;
    if (key == (HKEY)2 && !registry_exists) return ERROR_FILE_NOT_FOUND;
    if (key == (HKEY)2 && out && grow_on_read) {
        wcscat(registry, L";C:\\新增");
        registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
        value_bytes = registry_bytes;
        grow_on_read = 0;
    }
    if (!out) { *size = value_bytes; return ERROR_SUCCESS; }
    if (*size < value_bytes) { *size = value_bytes; return ERROR_MORE_DATA; }
    memcpy(out, value, value_bytes); *size = value_bytes; return ERROR_SUCCESS;
}
LONG RegQueryValueEx(HKEY key, const char *name, void *reserved, DWORD *type,
                    LPBYTE out, DWORD *size) {
    (void)name; ++ansi_calls;
    const char *lossy = "C:\\??";
    if (!out) { *size = (DWORD)strlen(lossy) + 1; return ERROR_SUCCESS; }
    (void)key; (void)reserved; (void)type;
    strcpy((char *)out, lossy); *size = (DWORD)strlen(lossy) + 1; return ERROR_SUCCESS;
}
LONG RegSetValueExW(HKEY key, const wchar_t *name, DWORD a, DWORD type,
                   LPBYTE data, DWORD size) {
    (void)key; (void)a; assert(wcscmp(name, L"Path") == 0); assert(type == registry_type);
    assert(size <= sizeof(registry)); assert(size % sizeof(wchar_t) == 0);
    assert(((wchar_t *)data)[size / sizeof(wchar_t) - 1] == 0);
    memcpy(registry, data, size); registry_bytes = size; registry_exists = 1; ++writes; return ERROR_SUCCESS;
}
LONG RegSetValueEx(HKEY key, const char *name, DWORD a, DWORD type, LPBYTE data, DWORD size) {
    (void)key; (void)name; (void)a; (void)type; (void)data; (void)size;
    ++ansi_calls; ++writes; return ERROR_SUCCESS;
}
LONG RegCloseKey(HKEY key) { (void)key; return ERROR_SUCCESS; }
#define SendMessageTimeout(...) ((void)0)
#define SendMessageTimeoutW(...) ((void)0)
'''
main = r'''
int main(void) {
    (void)get_exe_path_w;
    wcscpy(registry, L"%USERPROFILE%\\工具; C:\\既存 ");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    assert(add_app_to_path());
    assert(ansi_calls == 0);
    assert(wcscmp(registry, L"%USERPROFILE%\\工具; C:\\既存 ;C:\\安装\\😀") == 0);
    int previous_writes = writes;
    assert(add_app_to_path());
    assert(writes == previous_writes);
    assert(remove_app_from_path());
    assert(wcscmp(registry, L"%USERPROFILE%\\工具; C:\\既存 ") == 0);
    assert(remove_app_from_path());
    assert(writes == previous_writes + 1);
    registry_bytes = 0;
    registry_exists = 0;
    assert(add_app_to_path());
    assert(wcscmp(registry, L"C:\\安装\\😀") == 0);
    assert(remove_app_from_path());
    assert(registry[0] == 0);
    wcscpy(registry, L"C:\\既存");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    grow_on_read = 1;
    assert(add_app_to_path());
    assert(wcscmp(registry, L"C:\\既存;C:\\新增;C:\\安装\\😀") == 0);
    grow_on_read = 1;
    assert(remove_app_from_path());
    assert(wcscmp(registry, L"C:\\既存;C:\\新增;C:\\新增") == 0);
    // Registry string values need not include a terminating NUL.
    wcscpy(registry, L"C:\\工具");
    registry_bytes = (DWORD)(wcslen(registry) * sizeof(wchar_t));
    assert(add_app_to_path());
    assert(wcscmp(registry, L"C:\\工具;C:\\安装\\😀") == 0);
    registry_bytes -= sizeof(wchar_t);
    assert(remove_app_from_path());
    assert(wcscmp(registry, L"C:\\工具") == 0);
    // Malformed partial UTF-16 units must not silently replace existing PATH.
    registry_bytes = 1;
    previous_writes = writes;
    assert(!add_app_to_path());
    assert(!remove_app_from_path());
    assert(writes == previous_writes);
    // A literal %NAME% entry in REG_SZ must retain its non-expanding type.
    registry_type = REG_SZ;
    wcscpy(registry, L"%USERPROFILE%\\工具");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    assert(add_app_to_path());
    assert(wcscmp(registry, L"%USERPROFILE%\\工具;C:\\安装\\😀") == 0);
    assert(remove_app_from_path());
    assert(wcscmp(registry, L"%USERPROFILE%\\工具") == 0);
    // An existing zero-byte REG_SZ is still an existing typed value.
    registry_bytes = 0;
    assert(add_app_to_path());
    assert(remove_app_from_path());

    // Unsupported existing registry types must never be parsed or rewritten.
    registry_type = REG_MULTI_SZ;
    wcscpy(registry, L"C:\\Existing");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    previous_writes = writes;
    assert(!add_app_to_path());
    assert(writes == previous_writes);
    wcscpy(registry, L"C:\\安装\\😀;C:\\Existing");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    assert(!remove_app_from_path());
    assert(writes == previous_writes);

    // A zero-byte value keeps its unsupported type and must not be replaced.
    registry_bytes = 0;
    assert(!add_app_to_path());
    assert(!remove_app_from_path());
    assert(writes == previous_writes);

    // Recheck the type returned by the data read in case it changed after sizing.
    registry_type = REG_SZ;
    wcscpy(registry, L"C:\\Existing");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    data_read_type = REG_MULTI_SZ;
    change_type_on_data_read = 1;
    assert(!add_app_to_path());
    assert(writes == previous_writes);

    registry_type = REG_SZ;
    wcscpy(registry, L"C:\\安装\\😀;C:\\Existing");
    registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
    data_read_type = REG_MULTI_SZ;
    change_type_on_data_read = 1;
    assert(!remove_app_from_path());
    assert(writes == previous_writes);

    registry_type = REG_EXPAND_SZ;

    static wchar_t long_exe[512];
    const wchar_t *prefix = L"C:\\NoSleep\\";
    size_t long_exe_length = wcslen(prefix);
    wmemcpy(long_exe, prefix, long_exe_length);
    wmemset(long_exe + long_exe_length, L'x', 300);
    long_exe_length += 300;
    long_exe[long_exe_length++] = L'\\';
    wcscpy(long_exe + long_exe_length, L"nosleep.exe");
    exe = long_exe;

    fail_malloc = 1;
    assert(get_exe_path_w() == NULL);
    fail_malloc = 0;
    fail_module_filename = 1;
    assert(get_exe_path_w() == NULL);
    fail_module_filename = 0;
    fail_realloc = 1;
    assert(get_exe_path_w() == NULL);
    fail_realloc = 0;

    int calls_before_long_path = module_filename_calls;
    wchar_t *long_path = get_exe_path_w();
    assert(long_path && wcscmp(long_path, long_exe) == 0);
    assert(module_filename_calls >= calls_before_long_path + 2);
    free(long_path);

    wchar_t expected_dir[512];
    wcscpy(expected_dir, long_exe);
    *wcsrchr(expected_dir, L'\\') = L'\0';
    registry_type = REG_EXPAND_SZ;
    registry_exists = 0;
    registry_bytes = 0;
    assert(add_app_to_path());
    assert(wcscmp(registry, expected_dir) == 0);

    wcscpy(startup_registry, L"\"");
    wcscat(startup_registry, long_exe);
    wcscat(startup_registry, L"\" --startup");
    startup_registry_bytes = (DWORD)((wcslen(startup_registry) + 1) * sizeof(wchar_t));
    assert(is_startup_enabled());
    puts("PASS: Unicode PATH and long executable paths preserve registry behavior");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'path.c'
    binary = Path(tmp) / 'path'
    source.write_text(prelude + block + startup_state + main)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
PY
