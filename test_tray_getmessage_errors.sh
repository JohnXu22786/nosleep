#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()
match = re.search(r"void tray_run\(NoSleepTray\* tray\)\s*\{", tray)
assert match, "could not find definition for tray_run"
start = match.start()
opening = match.end() - 1
depth = 0
state = "code"
for index in range(opening, len(tray)):
    char = tray[index]
    following = tray[index:index + 2]
    if state == "code":
        if following == "//":
            state = "line_comment"
        elif following == "/*":
            state = "block_comment"
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                function = tray[start:index + 1]
                break
    elif state == "line_comment" and char == "\n":
        state = "code"
    elif state == "block_comment" and following == "*/":
        state = "code"
    elif state in ("string", "char"):
        if char == "\\":
            continue
        if (state == "string" and char == '"') or (state == "char" and char == "'"):
            state = "code"
else:
    raise AssertionError("unterminated tray_run function")

prelude = r"""
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HWND;
typedef struct { UINT message; HWND hwnd; } MSG;
typedef struct { HWND hwnd; } NoSleepTray;

static int message_results[4];
static UINT message_ids[4];
static size_t message_count;
static size_t message_index;
static unsigned int get_message_calls;
static unsigned int translate_calls;
static unsigned int dispatch_calls;
static unsigned int last_error_calls;
static unsigned int debug_log_calls;
static char debug_log_message[128];
static UINT translated_messages[4];
static UINT dispatched_messages[4];

#define DEBUG_PRINT(...) ((void)0)

static void test_debug_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(debug_log_message, sizeof(debug_log_message), format, args);
    va_end(args);
    ++debug_log_calls;
}

#define DEBUG_LOG(...) test_debug_log(__VA_ARGS__)

static int GetMessage(MSG *message, HWND hwnd, UINT first, UINT last) {
    (void)hwnd;
    (void)first;
    (void)last;
    ++get_message_calls;
    if (message_index >= message_count) return 0;
    size_t index = message_index++;
    message->message = message_ids[index];
    return message_results[index];
}

static int TranslateMessage(const MSG *message) {
    translated_messages[translate_calls++] = message->message;
    return 1;
}

static long DispatchMessage(const MSG *message) {
    dispatched_messages[dispatch_calls++] = message->message;
    return 0;
}

static DWORD GetLastError(void) {
    ++last_error_calls;
    return 1984;
}
"""

main = r"""
static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static void reset_message_pump(void) {
    message_count = 0;
    message_index = 0;
    get_message_calls = 0;
    translate_calls = 0;
    dispatch_calls = 0;
    last_error_calls = 0;
    debug_log_calls = 0;
    debug_log_message[0] = '\0';
}

int main(void) {
    int failures = 0;
    NoSleepTray tray = { (HWND)1 };

    reset_message_pump();
    message_results[0] = 1;
    message_ids[0] = 0x0401;
    message_results[1] = 1;
    message_ids[1] = 0x0402;
    message_results[2] = 0;
    message_count = 3;
    tray_run(&tray);
    failures += check(get_message_calls == 3,
                      "positive messages and WM_QUIT should be read once each");
    failures += check(translate_calls == 2 && dispatch_calls == 2,
                      "only positive GetMessage results should be dispatched");
    failures += check(translated_messages[0] == 0x0401 &&
                      translated_messages[1] == 0x0402 &&
                      dispatched_messages[0] == 0x0401 &&
                      dispatched_messages[1] == 0x0402,
                      "positive message contents should be preserved");
    failures += check(last_error_calls == 0 && debug_log_calls == 0,
                      "WM_QUIT should not be reported as an error");

    reset_message_pump();
    message_results[0] = -1;
    message_ids[0] = 0xDEAD;
    message_results[1] = 0;
    message_count = 2;
    tray_run(&tray);
    failures += check(get_message_calls == 1,
                      "GetMessage failure should stop the message pump immediately");
    failures += check(translate_calls == 0 && dispatch_calls == 0,
                      "a failed GetMessage result must not dispatch an invalid message");
    failures += check(last_error_calls == 1 && debug_log_calls == 1 &&
                      strcmp(debug_log_message, "tray_run: GetMessage failed, error=1984") == 0,
                      "GetMessage failure should be recorded with its Windows error");

    if (failures != 0) return 1;
    puts("PASS: tray message pump handles GetMessage errors and WM_QUIT");
    return 0;
}
"""

with tempfile.TemporaryDirectory() as temporary_directory:
    source = Path(temporary_directory) / "test_tray_getmessage_errors.c"
    binary = Path(temporary_directory) / "test_tray_getmessage_errors"
    source.write_text(prelude + "\n" + function + "\n" + main)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
