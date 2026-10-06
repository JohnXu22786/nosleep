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
block = tray[tray.index('static wchar_t* get_exe_path_w(void) {'):tray.index('static bool apply_path_preference(bool add_to_path) {')]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef long LONG;
typedef unsigned char *LPBYTE;
typedef void *HKEY;
typedef intptr_t LPARAM;
#define MAX_PATH 260
#define HKEY_CURRENT_USER ((HKEY)1)
#define KEY_READ 1
#define KEY_WRITE 2
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define REG_OPTION_NON_VOLATILE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define HWND_BROADCAST 0
#define WM_SETTINGCHANGE 0
#define SMTO_ABORTIFHUNG 0
static const wchar_t *exe = L"C:\\安装\\😀\\nosleep.exe";
static wchar_t registry[2048];
static DWORD registry_bytes;
static DWORD registry_type = REG_EXPAND_SZ;
static int ansi_calls;
static int writes;
static int grow_on_read;
static int registry_exists = 1;
DWORD GetModuleFileNameW(void *module, wchar_t *out, DWORD size) {
    (void)module;
    if (wcslen(exe) >= size) return size;
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
    (void)root; (void)a; (void)b; assert(wcscmp(name, L"Environment") == 0);
    *key = (HKEY)2; return ERROR_SUCCESS;
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
    (void)key; (void)reserved; assert(wcscmp(name, L"Path") == 0);
    if (type) *type = registry_type;
    if (!registry_exists) return ERROR_FILE_NOT_FOUND;
    if (out && grow_on_read) {
        wcscat(registry, L";C:\\新增");
        registry_bytes = (DWORD)((wcslen(registry) + 1) * sizeof(wchar_t));
        grow_on_read = 0;
    }
    if (!out) { *size = registry_bytes; return ERROR_SUCCESS; }
    if (*size < registry_bytes) { *size = registry_bytes; return ERROR_MORE_DATA; }
    memcpy(out, registry, registry_bytes); *size = registry_bytes; return ERROR_SUCCESS;
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
    puts("PASS: Unicode PATH add/remove preserves existing entries and registry byte sizes");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'path.c'
    binary = Path(tmp) / 'path'
    source.write_text(prelude + block + main)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
PY
