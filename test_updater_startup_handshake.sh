#!/bin/bash
# Verify that the updater relay waits for the relaunched app to report readiness.
set -euo pipefail

python3 - "${1:-src/main.c}" <<'PY'
import re
import subprocess
import sys
import tempfile
from pathlib import Path

source = Path(sys.argv[1]).read_text()
match = re.search(
    r"static int relaunch_from_command_line_file\(const wchar_t\* arguments_path\) \{.*?^\}",
    source,
    re.S | re.M,
)
assert match, "could not find the updater relay"

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "updater_command_line.h"

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef unsigned char BYTE;
typedef long long LONGLONG;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { DWORD cb; } STARTUPINFOW;
typedef struct { HANDLE hProcess; HANDLE hThread; } PROCESS_INFORMATION;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define GENERIC_READ 1
#define OPEN_EXISTING 2
#define FILE_ATTRIBUTE_NORMAL 0
#define MB_OK 1
#define MB_ICONERROR 2
#define MB_TOPMOST 4
#define EVENT_MODIFY_STATE 2
#define ERROR_ALREADY_EXISTS 183
#define WAIT_OBJECT_0 0
#define WAIT_FAILED ((DWORD)-1)
#define INFINITE ((DWORD)-1)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

static const wchar_t saved_command_line[] = L"nosleep.exe --duration 20";
static const HANDLE arguments_handle = (HANDLE)(uintptr_t)0x11;
static const HANDLE ready_event_handle = (HANDLE)(uintptr_t)0x22;
static const HANDLE child_process_handle = (HANDLE)(uintptr_t)0x33;
static const HANDLE child_thread_handle = (HANDLE)(uintptr_t)0x44;
static wchar_t created_event_name[128];
static wchar_t launched_command_line[UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS];
static DWORD requested_wait_result;
static unsigned int wait_calls;
static unsigned int create_process_calls;
static unsigned int error_dialog_calls;
static BOOL launch_succeeds;
static DWORD last_error;

HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD sharing,
                   void *security, DWORD creation, DWORD attributes,
                   HANDLE template_file) {
    (void)path; (void)access; (void)sharing; (void)security;
    (void)creation; (void)attributes; (void)template_file;
    return arguments_handle;
}

BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size) {
    assert(file == arguments_handle);
    size->QuadPart = (LONGLONG)(sizeof(saved_command_line) - sizeof(wchar_t));
    return TRUE;
}

BOOL ReadFile(HANDLE file, void *buffer, DWORD bytes, DWORD *read_bytes,
              void *overlapped) {
    (void)overlapped;
    assert(file == arguments_handle);
    assert(bytes == sizeof(saved_command_line) - sizeof(wchar_t));
    memcpy(buffer, saved_command_line, bytes);
    *read_bytes = bytes;
    return TRUE;
}

DWORD GetModuleFileNameW(HANDLE module, wchar_t *path, DWORD capacity) {
    const wchar_t expected[] = L"C:\\nosleep.exe";
    (void)module;
    assert(capacity >= sizeof(expected) / sizeof(expected[0]));
    wcscpy(path, expected);
    return (DWORD)(sizeof(expected) / sizeof(expected[0]) - 1);
}

DWORD GetCurrentProcessId(void) { return 4242; }

HANDLE CreateEventW(void *security, BOOL manual_reset, BOOL initial_state,
                    const wchar_t *name) {
    (void)security;
    assert(manual_reset == TRUE);
    assert(initial_state == FALSE);
    wcsncpy(created_event_name, name,
            sizeof(created_event_name) / sizeof(created_event_name[0]) - 1);
    created_event_name[sizeof(created_event_name) / sizeof(created_event_name[0]) - 1] = L'\0';
    last_error = 0;
    return ready_event_handle;
}

DWORD GetLastError(void) { return last_error; }

BOOL CreateProcessW(const wchar_t *application, wchar_t *command_line,
                    void *process_attributes, void *thread_attributes,
                    BOOL inherit_handles, DWORD creation_flags, void *environment,
                    const wchar_t *current_directory, STARTUPINFOW *startup_info,
                    PROCESS_INFORMATION *process_info) {
    (void)process_attributes; (void)thread_attributes; (void)inherit_handles;
    (void)creation_flags; (void)environment; (void)current_directory;
    assert(wcscmp(application, L"C:\\nosleep.exe") == 0);
    assert(startup_info->cb == sizeof(*startup_info));
    ++create_process_calls;
    wcsncpy(launched_command_line, command_line,
            sizeof(launched_command_line) / sizeof(launched_command_line[0]) - 1);
    launched_command_line[sizeof(launched_command_line) / sizeof(launched_command_line[0]) - 1] = L'\0';
    if (!launch_succeeds) return FALSE;
    process_info->hProcess = child_process_handle;
    process_info->hThread = child_thread_handle;
    return TRUE;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL wait_all,
                             DWORD timeout) {
    assert(count == 2);
    assert(handles[0] == ready_event_handle);
    assert(handles[1] == child_process_handle);
    assert(wait_all == FALSE);
    assert(timeout == INFINITE);
    ++wait_calls;
    return requested_wait_result;
}

BOOL CloseHandle(HANDLE handle) {
    assert(handle == arguments_handle || handle == ready_event_handle ||
           handle == child_process_handle || handle == child_thread_handle);
    return TRUE;
}

int MessageBoxW(void *window, const wchar_t *message, const wchar_t *title,
                UINT flags) {
    (void)window; (void)message; (void)title; (void)flags;
    ++error_dialog_calls;
    return 0;
}

''' + match.group(0) + r'''

static void reset(void) {
    created_event_name[0] = L'\0';
    launched_command_line[0] = L'\0';
    requested_wait_result = WAIT_OBJECT_0;
    wait_calls = 0;
    create_process_calls = 0;
    error_dialog_calls = 0;
    launch_succeeds = TRUE;
    last_error = 0;
}

int main(void) {
    reset();
    assert(relaunch_from_command_line_file(L"arguments.dat") == 0);
    assert(create_process_calls == 1);
    assert(wait_calls == 1);
    assert(error_dialog_calls == 0);
    assert(wcscmp(created_event_name, L"Local\\NoSleepUpdaterReady-4242") == 0);
    assert(wcsstr(launched_command_line,
                  L"--nosleep-internal-update-ready-event") != NULL);
    assert(wcsstr(launched_command_line, created_event_name) != NULL);

    reset();
    requested_wait_result = WAIT_OBJECT_0 + 1;
    assert(relaunch_from_command_line_file(L"arguments.dat") == 1);
    assert(wait_calls == 1);
    assert(error_dialog_calls == 1);

    reset();
    launch_succeeds = FALSE;
    assert(relaunch_from_command_line_file(L"arguments.dat") == 1);
    assert(wait_calls == 0);
    assert(error_dialog_calls == 1);

    puts("PASS: updater relay exits successfully only after startup readiness");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / "startup_handshake.c"
    test.write_text(harness)
    binary = Path(directory) / "startup_handshake"
    subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", str(test), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
