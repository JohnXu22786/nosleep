#!/bin/bash
# Execute the updater's post-download launch flow with injected preparation failures.
set -euo pipefail
python3 - "${1:-src/updater.c}" <<'PY'
import subprocess
import sys
import tempfile
from pathlib import Path

source = Path(sys.argv[1]).read_text()
start = source.index('    // Get the EXE name from current path')
end = source.index('// --- Helper functions ---', start)
flow = source[start:end]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 260
#define MB_OK 0
#define MB_ICONERROR 0
#define MB_TOPMOST 0
#define MB_ICONINFORMATION 0
#define SEE_MASK_NOCLOSEPROCESS 1
#define SEE_MASK_NOASYNC 2
#define SW_HIDE 0
typedef struct { size_t cbSize; int fMask; const char *lpFile; int nShow; } SHELLEXECUTEINFO;
static int failure;
static char error[512];
static bool DeleteFileA(const char *path) { return remove(path) == 0; }
static int MessageBox(void *parent, const char *message, const char *title, int flags) {
    (void)parent; (void)flags;
    if (strcmp(title, "Update Failed") == 0) snprintf(error, sizeof(error), "%s", message);
    return 0;
}
static char *get_exe_name_from_path(const char *path) {
    (void)path;
    if (failure == 1) return NULL;
    char *name = malloc(12); assert(name); strcpy(name, "nosleep.exe"); return name;
}
static void touch(const char *path) { FILE *f = fopen(path, "wb"); assert(f); fclose(f); }
static bool create_update_batch_script(const char *exe, const char *download, const char *name,
        char *script, size_t script_size, char *arguments, size_t arguments_size) {
    (void)exe; (void)download; (void)name;
    if (failure == 2) return false;
    snprintf(script, script_size, "update.bat");
    snprintf(arguments, arguments_size, "args.txt");
    touch(script); touch(arguments); return true;
}
static bool ShellExecuteEx(SHELLEXECUTEINFO *info) { (void)info; return failure != 3; }
static bool run(void) {
    void *hwnd_parent = NULL;
    const char *current_exe_path = "nosleep.exe";
    char *temp_path = malloc(13); assert(temp_path); strcpy(temp_path, "download.exe");
'''
checks = r'''
static bool exists(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return false; fclose(f); return true; }
int main(void) {
    for (failure = 1; failure <= 3; ++failure) {
        touch("download.exe"); error[0] = '\0';
        assert(!run());
        assert(!exists("download.exe"));
        assert(!exists("update.bat")); assert(!exists("args.txt"));
        assert(strstr(error, "manually") == NULL);
    }
    failure = 0; touch("download.exe");
    assert(run());
    assert(exists("download.exe")); assert(exists("update.bat")); assert(exists("args.txt"));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'launch.c'
    test.write_text(harness + flow + checks)
    binary = Path(directory) / 'launch'
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], cwd=directory, check=True)
print('PASS: update preparation failures remove downloads; successful launch retains handoff files')
PY
