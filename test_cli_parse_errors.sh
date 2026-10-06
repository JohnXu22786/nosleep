#!/bin/bash
# Regression test: CLI parse failures should identify the rejected input and
# explain the expected value without losing Unicode arguments.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMPDIR/test_cli_parse_errors.c" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "src/main.c").read_text(encoding="utf-8")


def extract_function(name):
    match = re.search(
        r"\bstatic\s+[^;{}]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        source,
        re.S,
    )
    assert match, f"could not find {name} definition"
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
                    return source[start : i + 1]
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
    raise AssertionError(f"unterminated {name} definition")


options_match = re.search(r"typedef struct\s*\{.*?\}\s*CLIOptions\s*;", source, re.S)
assert options_match, "could not find CLIOptions definition"
error_kind_match = re.search(
    r"typedef enum\s*\{.*?\}\s*CLIParseErrorKind\s*;", source, re.S
)
assert error_kind_match, "CLI parse error kinds are not defined"
error_struct_match = re.search(
    r"typedef struct\s*\{\s*CLIParseErrorKind\s+kind;.*?\}\s*CLIParseError\s*;",
    source,
    re.S,
)
assert error_struct_match, "CLIParseError is not defined"

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cli_duration.h"
#include "cli_interval.h"

#define CP_UTF8 65001
#define CLI_UNSET -1
#define CLI_DISABLE 0
#define CLI_ENABLE 1
#define SESSION_FINISHED_NONE 0
#define SESSION_FINISHED_SHUTDOWN 1
#define SESSION_FINISHED_SLEEP 2
#define NOTIFY_ALL 0
#define NOTIFY_CRITICAL_ONLY 1
#define NOTIFY_NONE 2

int WideCharToMultiByte(unsigned int code_page, unsigned long flags,
                        const wchar_t *wide, int char_count, char *output,
                        int output_capacity, const char *default_char,
                        int *used_default_char) {
    (void)code_page;
    (void)flags;
    (void)char_count;
    (void)default_char;
    (void)used_default_char;
    size_t length = wcslen(wide);
    if (length + 1 > (size_t)output_capacity) return 0;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned int)wide[i] > 0x7f) return 0;
        output[i] = (char)wide[i];
    }
    output[length] = '\0';
    return (int)length + 1;
}

"""

suffix = r"""
static CLIOptions default_options(void) {
    CLIOptions options = {0};
    options.duration = -1;
    options.interval = 20;
    options.prevent_display = CLI_UNSET;
    options.away_mode = CLI_UNSET;
    options.session_finished = CLI_UNSET;
    options.auto_start = CLI_UNSET;
    options.notification_mode = CLI_UNSET;
    options.auto_check_interval = CLI_UNSET;
    options.check_updates_startup = CLI_UNSET;
    options.add_to_path = CLI_UNSET;
    return options;
}

static void expect_error(int argc, wchar_t *argv[], CLIParseErrorKind kind,
                         const wchar_t *option, const wchar_t *value,
                         const wchar_t *expected, const wchar_t *message) {
    CLIOptions options = default_options();
    CLIParseError error = {0};
    assert(parse_arguments(argc, argv, &options, &error) == 1);
    assert(error.kind == kind);
    assert(error.option != NULL && wcscmp(error.option, option) == 0);
    if (value == NULL) assert(error.value == NULL);
    else assert(error.value != NULL && wcscmp(error.value, value) == 0);
    if (expected == NULL) assert(error.expected == NULL);
    else assert(error.expected != NULL && wcscmp(error.expected, expected) == 0);

    wchar_t *formatted = format_cli_parse_error(&error);
    assert(formatted != NULL);
    assert(wcscmp(formatted, message) == 0);
    free(formatted);
}

static void test_missing_values(void) {
    wchar_t *duration_argv[] = {L"nosleep", L"--duration"};
    expect_error(2, duration_argv, CLI_PARSE_ERROR_MISSING_VALUE,
                 L"--duration", NULL, L"an integer number of minutes",
                 L"Option \"--duration\" requires a value. Expected an integer number of minutes.\n");

    wchar_t *enum_argv[] = {L"nosleep", L"--session-finished"};
    expect_error(2, enum_argv, CLI_PARSE_ERROR_MISSING_VALUE,
                 L"--session-finished", NULL, L"one of: none, shutdown, sleep",
                 L"Option \"--session-finished\" requires a value. Expected one of: none, shutdown, sleep.\n");
}

static void test_unknown_option(void) {
    wchar_t *argv[] = {L"nosleep", L"--mystery-option"};
    expect_error(2, argv, CLI_PARSE_ERROR_UNKNOWN_OPTION,
                 L"--mystery-option", NULL, NULL,
                 L"Unknown option \"--mystery-option\". Use --help for usage information.\n");
}

static void test_invalid_numeric_value(void) {
    wchar_t *argv[] = {L"nosleep", L"--duration", L"30minutes"};
    expect_error(3, argv, CLI_PARSE_ERROR_INVALID_VALUE,
                 L"--duration", L"30minutes", L"an integer number of minutes",
                 L"Invalid value \"30minutes\" for option \"--duration\"; expected an integer number of minutes.\n");

    wchar_t *interval_argv[] = {L"nosleep", L"--interval", L"0"};
    expect_error(3, interval_argv, CLI_PARSE_ERROR_INVALID_VALUE,
                 L"--interval", L"0", L"an integer number of seconds from 1 to 2147483",
                 L"Invalid value \"0\" for option \"--interval\"; expected an integer number of seconds from 1 to 2147483.\n");
}

static void test_invalid_enum_value(void) {
    wchar_t *argv[] = {L"nosleep", L"--notification-mode", L"loud"};
    expect_error(3, argv, CLI_PARSE_ERROR_INVALID_ENUM,
                 L"--notification-mode", L"loud", L"all, critical, none",
                 L"Invalid value \"loud\" for option \"--notification-mode\"; expected one of: all, critical, none.\n");
}

static void test_unicode_value_is_preserved(void) {
    wchar_t *argv[] = {L"nosleep", L"--session-finished", L"睡眠"};
    expect_error(3, argv, CLI_PARSE_ERROR_INVALID_ENUM,
                 L"--session-finished", L"睡眠", L"none, shutdown, sleep",
                 L"Invalid value \"睡眠\" for option \"--session-finished\"; expected one of: none, shutdown, sleep.\n");
}

static CLIOptions parse_options(int argc, wchar_t *argv[]) {
    CLIOptions options = default_options();
    CLIParseError error = {0};
    assert(parse_arguments(argc, argv, &options, &error) == 0);
    assert(error.kind == CLI_PARSE_ERROR_NONE);
    return options;
}

static void test_batch_options(void) {
    wchar_t *enabled_argv[] = {
        L"nosleep", L"--session-finished", L"shutdown",
        L"--notification-mode", L"critical",
        L"--auto-check-interval", L"daily",
        L"--auto-start", L"--check-updates-startup",
        L"--configure", L"--version"
    };
    CLIOptions options = parse_options(
        (int)(sizeof(enabled_argv) / sizeof(enabled_argv[0])), enabled_argv);
    assert(options.session_finished == SESSION_FINISHED_SHUTDOWN);
    assert(options.notification_mode == NOTIFY_CRITICAL_ONLY);
    assert(options.auto_check_interval == 1);
    assert(options.auto_start == CLI_ENABLE);
    assert(options.check_updates_startup == CLI_ENABLE);
    assert(options.configure_mode);
    assert(options.show_version);

    wchar_t *disabled_argv[] = {
        L"nosleep", L"--session-finished", L"sleep",
        L"--notification-mode", L"none",
        L"--auto-check-interval", L"weekly",
        L"--no-auto-start", L"--no-check-updates-startup", L"--configure"
    };
    options = parse_options(
        (int)(sizeof(disabled_argv) / sizeof(disabled_argv[0])), disabled_argv);
    assert(options.session_finished == SESSION_FINISHED_SLEEP);
    assert(options.notification_mode == NOTIFY_NONE);
    assert(options.auto_check_interval == 2);
    assert(options.auto_start == CLI_DISABLE);
    assert(options.check_updates_startup == CLI_DISABLE);
    assert(options.configure_mode);
}

static void test_help_result(void) {
    wchar_t *argv[] = {L"nosleep", L"--help"};
    CLIOptions options = default_options();
    CLIParseError error = {0};
    assert(parse_arguments(2, argv, &options, &error) == 2);
    assert(error.kind == CLI_PARSE_ERROR_NONE);
}

int main(void) {
    test_missing_values();
    test_unknown_option();
    test_invalid_numeric_value();
    test_invalid_enum_value();
    test_unicode_value_is_preserved();
    test_batch_options();
    test_help_result();
    puts("PASS: production CLI parser handles errors and batch options");
    return 0;
}
"""

output.write_text(
    prefix
    + options_match.group(0)
    + error_kind_match.group(0)
    + error_struct_match.group(0)
    + "\n"
    + extract_function("fail_cli_parse")
    + "\n"
    + extract_function("format_cli_parse_error")
    + "\n"
    + extract_function("parse_arguments")
    + suffix,
    encoding="utf-8",
)
PY

gcc -std=c99 -Wall -Wextra -Werror -Isrc "$TMPDIR/test_cli_parse_errors.c" \
    -o "$TMPDIR/test_cli_parse_errors"
"$TMPDIR/test_cli_parse_errors"
