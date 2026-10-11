#!/bin/bash
# Regression test: stderr reopened after AttachConsole must also become the
# Win32 standard error handle used by wide CLI diagnostics.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMPDIR/test_cli_stderr_handle.c" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "src/main.c").read_text(encoding="utf-8")
match = re.search(
    r"\bstatic\s+void\s+prepare_cli_output\s*\([^;]*?\)\s*\{",
    source,
    re.S,
)
assert match, "could not find prepare_cli_output definition"
start = match.start()
opening = match.end() - 1
depth = 0
state = "code"
i = opening
while i < len(source):
    char = source[i]
    next_two = source[i : i + 2]
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
                function = source[start : i + 1]
                break
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
else:
    raise AssertionError("unterminated prepare_cli_output definition")

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef void *HANDLE;
typedef int BOOL;
typedef unsigned long DWORD;

#define ATTACH_PARENT_PROCESS ((DWORD)-1)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define TRUE 1

static FILE *mock_stdout = (FILE *)(uintptr_t)1;
static FILE *mock_stderr = (FILE *)(uintptr_t)2;
static int stdout_fd = -1;
static int stderr_fd = -1;
static int set_std_handle_calls;
static DWORD set_std_handle_id;
static HANDLE set_std_handle_value;

static int mock_fileno(FILE *stream) {
    if (stream == mock_stdout) return stdout_fd;
    if (stream == mock_stderr) return stderr_fd;
    return -1;
}

static intptr_t mock_get_osfhandle(int fd) {
    return (intptr_t)(0x100 + fd);
}

static BOOL mock_attach_console(DWORD process_id) {
    (void)process_id;
    return 1;
}

static HANDLE mock_freopen(const char *path, const char *mode, FILE *stream) {
    assert(path != NULL && mode != NULL);
    if (stream == mock_stdout) stdout_fd = 4;
    else if (stream == mock_stderr) stderr_fd = 5;
    else assert(0 && "unexpected stream");
    return stream;
}

static BOOL mock_set_std_handle(DWORD id, HANDLE handle) {
    ++set_std_handle_calls;
    set_std_handle_id = id;
    set_std_handle_value = handle;
    return 1;
}

static BOOL mock_set_console_ctrl_handler(void *handler, BOOL add) {
    (void)handler;
    (void)add;
    return 1;
}

#undef stdout
#undef stderr
#define stdout mock_stdout
#define stderr mock_stderr
#define _fileno mock_fileno
#define _get_osfhandle mock_get_osfhandle
#define AttachConsole mock_attach_console
#define freopen mock_freopen
#define SetStdHandle mock_set_std_handle
#define SetConsoleCtrlHandler mock_set_console_ctrl_handler
"""

suffix = r"""
int main(void) {
    prepare_cli_output(false);

    assert(stderr_fd == 5);
    assert(set_std_handle_calls == 1);
    assert(set_std_handle_id == STD_ERROR_HANDLE);
    assert(set_std_handle_value == (HANDLE)(intptr_t)0x105);
    return 0;
}
"""

output.write_text(prefix + "\n" + function + "\n" + suffix, encoding="utf-8")
PY

gcc -std=c99 -Wall -Wextra -Werror "$TMPDIR/test_cli_stderr_handle.c" \
    -o "$TMPDIR/test_cli_stderr_handle"
"$TMPDIR/test_cli_stderr_handle"
echo "PASS: reopened stderr is published as STD_ERROR_HANDLE"
