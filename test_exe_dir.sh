#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", tray)
    assert definition, f"could not find definition for {signature}"
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
    raise AssertionError(f"unterminated function: {signature}")


prelude = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const wchar_t *test_exe_path;

static wchar_t *get_exe_path_w(void) {
    size_t length = wcslen(test_exe_path);
    wchar_t *path = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (path) memcpy(path, test_exe_path, (length + 1) * sizeof(wchar_t));
    return path;
}
"""

main = r"""
int main(void) {
    static const struct {
        const wchar_t *exe_path;
        const wchar_t *expected_dir;
    } cases[] = {
        {L"C:\\nosleep.exe", L"C:\\"},
        {L"\\\\?\\C:\\nosleep.exe", L"\\\\?\\C:\\"},
        {L"\\\\?\\C:\\NoSleep\\nosleep.exe", L"\\\\?\\C:\\NoSleep"},
        {L"C:\\Program Files\\NoSleep\\nosleep.exe", L"C:\\Program Files\\NoSleep"},
        {L"\\\\server\\share\\nosleep.exe", L"\\\\server\\share"},
        {L"\\\\server\\share\\NoSleep\\nosleep.exe", L"\\\\server\\share\\NoSleep"},
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_exe_path = cases[i].exe_path;
        wchar_t *actual_dir = get_exe_dir();
        if (!actual_dir || wcscmp(actual_dir, cases[i].expected_dir) != 0) {
            fprintf(stderr, "FAIL: directory for '%ls' was '%ls', expected '%ls'\n",
                    cases[i].exe_path, actual_dir ? actual_dir : L"(null)",
                    cases[i].expected_dir);
            ++failures;
        }
        free(actual_dir);
    }

    if (failures != 0) return 1;
    puts("PASS: executable directory extraction preserves drive roots and UNC paths");
    return 0;
}
"""

function = extract_function("static wchar_t* get_exe_dir(void)")
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test_exe_dir.c"
    binary = Path(tmp) / "test_exe_dir"
    source.write_text(prelude + "\n" + function + "\n" + main)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
PY
