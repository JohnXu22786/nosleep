#!/bin/bash
# Regression test: failed UTF-8 conversions must be rejected before comparing
# their destination buffers as CLI options or enum values.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMPDIR/test_cli_utf8_arguments.c" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "src/main.c").read_text()
match = re.search(
    r"static int parse_arguments\(int argc, wchar_t\* argv\[\], CLIOptions\* opts\) \{.*?^\}",
    source,
    re.S | re.M,
)
assert match, "could not find parse_arguments definition"
parser = match.group(0)

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
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

typedef struct {
    int duration;
    int interval;
    int prevent_display;
    int away_mode;
    bool verbose;
    bool tray_mode;
    bool startup;
    bool verbose_set;
    int session_finished;
    int auto_start;
    int notification_mode;
    int auto_check_interval;
    int check_updates_startup;
    int add_to_path;
    bool configure_mode;
    bool show_version;
} CLIOptions;

static const wchar_t *failed_input;
static const char *failed_output;

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
    if (wide == failed_input || length + 1 > (size_t)output_capacity) {
        /* Win32 leaves the destination undefined on failure; poison it with
           a valid-looking value so accidental use is deterministic here. */
        if (output_capacity > 0) {
            snprintf(output, (size_t)output_capacity, "%s",
                     failed_output ? failed_output : "--help");
        }
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        if ((unsigned int)wide[i] > 0x7f) return 0;
        output[i] = (char)wide[i];
    }
    output[length] = '\0';
    return (int)length + 1;
}

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

static void make_oversized(wchar_t value[257]) {
    for (size_t i = 0; i < 256; ++i) value[i] = L'x';
    value[256] = L'\0';
}

"""

suffix = r"""
static void expect_oversized_option_rejected(void) {
    wchar_t oversized[257];
    make_oversized(oversized);
    wchar_t *argv[] = {L"nosleep", oversized};
    CLIOptions options = default_options();
    failed_input = oversized;
    failed_output = "--help";

    assert(parse_arguments(2, argv, &options) == 1);
    assert(!options.show_version);
}

static void expect_oversized_enum_rejected(const wchar_t *option,
                                           const char *fallback) {
    wchar_t oversized[257];
    make_oversized(oversized);
    wchar_t *argv[] = {L"nosleep", (wchar_t *)option, oversized};
    CLIOptions options = default_options();
    failed_input = oversized;
    failed_output = fallback;

    assert(parse_arguments(3, argv, &options) == 1);
}

int main(void) {
    expect_oversized_option_rejected();
    expect_oversized_enum_rejected(L"--session-finished", "sleep");
    expect_oversized_enum_rejected(L"--notification-mode", "none");
    expect_oversized_enum_rejected(L"--auto-check-interval", "weekly");

    CLIOptions options = default_options();
    wchar_t *valid_argv[] = {
        L"nosleep", L"--session-finished", L"shutdown",
        L"--notification-mode", L"critical", L"--auto-check-interval", L"daily"
    };
    failed_input = NULL;
    failed_output = NULL;
    assert(parse_arguments(7, valid_argv, &options) == 0);
    assert(options.session_finished == SESSION_FINISHED_SHUTDOWN);
    assert(options.notification_mode == NOTIFY_CRITICAL_ONLY);
    assert(options.auto_check_interval == 1);

    puts("PASS: failed or oversized UTF-8 argument conversions are rejected safely");
    return 0;
}
"""

output.write_text(prefix + parser + suffix)
PY

gcc -std=c99 -Wall -Wextra -Werror -Isrc "$TMPDIR/test_cli_utf8_arguments.c" \
    -o "$TMPDIR/test_cli_utf8_arguments"
"$TMPDIR/test_cli_utf8_arguments"
