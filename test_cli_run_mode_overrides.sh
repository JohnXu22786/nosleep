#!/bin/bash
# Regression tests for per-run overrides of the saved display and away modes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

python3 - "$SCRIPT_DIR" "$TMPDIR/test_cli_run_mode_overrides.c" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "src/main.c").read_text()
tray_source = (root / "src/tray.c").read_text()

options_match = re.search(
    r"typedef struct \{.*?\} CLIOptions;", source, re.S
)
assert options_match, "could not find CLIOptions definition"

parser_match = re.search(
    r"static int parse_arguments\(int argc, wchar_t\* argv\[\], CLIOptions\* opts\) \{.*?^\}",
    source,
    re.S | re.M,
)
assert parser_match, "could not find parse_arguments definition"

overrides_match = re.search(
    r"// Apply CLI overrides AFTER tray_load_settings.*?^\s*tray->refresh_interval_seconds = opts->interval;",
    source,
    re.S | re.M,
)
assert overrides_match, "could not find the run-mode CLI override block"

mode_match = re.search(
    r"static bool tray_mode_for_run\(bool preference, bool override_set, bool override_value\) \{.*?^\}",
    tray_source,
    re.S | re.M,
)
assert mode_match, "could not find the effective per-run mode helper"

save_start = tray_source.index("    DWORD val = tray->prevent_display ? 1 : 0;")
save_end = tray_source.index("    val = tray->verbose ? 1 : 0;", save_start)
save_modes = tray_source[save_start:save_end].rstrip()

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

typedef unsigned long DWORD;
typedef long LONG;
typedef void *HKEY;
typedef unsigned char *LPBYTE;
#define REG_DWORD 4
#define ERROR_SUCCESS 0

static DWORD saved_prevent_display;
static DWORD saved_away_mode;

static LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved,
                          DWORD type, LPBYTE data, DWORD size) {
    (void)key;
    (void)reserved;
    assert(type == REG_DWORD);
    assert(size == sizeof(DWORD));
    if (strcmp(name, "prevent_display") == 0) {
        memcpy(&saved_prevent_display, data, sizeof(DWORD));
    } else if (strcmp(name, "away_mode") == 0) {
        memcpy(&saved_away_mode, data, sizeof(DWORD));
    } else {
        assert(0 && "unexpected saved mode name");
    }
    return ERROR_SUCCESS;
}

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

middle = r"""

typedef struct {
    bool prevent_display;
    bool away_mode;
    bool prevent_display_cli_override;
    bool prevent_display_cli_override_set;
    bool away_mode_cli_override;
    bool away_mode_cli_override_set;
    bool verbose;
    int refresh_interval_seconds;
    int session_finished_action;
} NoSleepTray;

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

static void apply_cli_run_overrides(const CLIOptions *opts, NoSleepTray *tray) {
"""

save_function = r"""
static void save_mode_preferences(const NoSleepTray *tray) {
    HKEY hKey = NULL;
    bool success = true;
""" + save_modes + r"""
    assert(success);
}
"""

suffix = r"""
static CLIOptions parse(int argc, wchar_t *argv[]) {
    CLIOptions options = default_options();
    assert(parse_arguments(argc, argv, &options) == 0);
    return options;
}

static void test_absent_flags_preserve_saved_values(void) {
    wchar_t *argv[] = {L"nosleep"};
    CLIOptions options = parse(1, argv);
    NoSleepTray tray = {
        .prevent_display = true,
        .away_mode = true,
        .refresh_interval_seconds = 20
    };

    apply_cli_run_overrides(&options, &tray);

    assert(tray_mode_for_run(tray.prevent_display,
                             tray.prevent_display_cli_override_set,
                             tray.prevent_display_cli_override));
    assert(tray_mode_for_run(tray.away_mode,
                             tray.away_mode_cli_override_set,
                             tray.away_mode_cli_override));
}

static void test_negative_flags_disable_saved_values_for_this_run(void) {
    wchar_t *argv[] = {L"nosleep", L"--no-prevent-display", L"--no-away-mode"};
    CLIOptions options = parse(3, argv);
    NoSleepTray tray = {
        .prevent_display = true,
        .away_mode = true,
        .refresh_interval_seconds = 20
    };

    apply_cli_run_overrides(&options, &tray);

    assert(!tray_mode_for_run(tray.prevent_display,
                              tray.prevent_display_cli_override_set,
                              tray.prevent_display_cli_override));
    assert(!tray_mode_for_run(tray.away_mode,
                              tray.away_mode_cli_override_set,
                              tray.away_mode_cli_override));
    assert(tray.prevent_display);
    assert(tray.away_mode);
    tray.session_finished_action = SESSION_FINISHED_SLEEP;
    save_mode_preferences(&tray);
    assert(saved_prevent_display == 1);
    assert(saved_away_mode == 1);
}

static void test_existing_positive_flags_still_enable_modes(void) {
    wchar_t *argv[] = {L"nosleep", L"-p", L"--away-mode"};
    CLIOptions options = parse(3, argv);
    NoSleepTray tray = {
        .prevent_display = false,
        .away_mode = false,
        .refresh_interval_seconds = 20
    };

    apply_cli_run_overrides(&options, &tray);

    assert(tray_mode_for_run(tray.prevent_display,
                             tray.prevent_display_cli_override_set,
                             tray.prevent_display_cli_override));
    assert(tray_mode_for_run(tray.away_mode,
                             tray.away_mode_cli_override_set,
                             tray.away_mode_cli_override));
    assert(!tray.prevent_display);
    assert(!tray.away_mode);
    save_mode_preferences(&tray);
    assert(saved_prevent_display == 0);
    assert(saved_away_mode == 0);
}

static void test_last_explicit_flag_wins(void) {
    wchar_t *argv[] = {
        L"nosleep", L"--prevent-display", L"--no-prevent-display",
        L"--no-away-mode", L"--away-mode"
    };
    CLIOptions options = parse(5, argv);
    NoSleepTray tray = {
        .prevent_display = true,
        .away_mode = false,
        .refresh_interval_seconds = 20
    };

    apply_cli_run_overrides(&options, &tray);

    assert(!tray_mode_for_run(tray.prevent_display,
                              tray.prevent_display_cli_override_set,
                              tray.prevent_display_cli_override));
    assert(tray_mode_for_run(tray.away_mode,
                             tray.away_mode_cli_override_set,
                             tray.away_mode_cli_override));
    assert(tray.prevent_display);
    assert(!tray.away_mode);
    save_mode_preferences(&tray);
    assert(saved_prevent_display == 1);
    assert(saved_away_mode == 0);
}

int main(void) {
    test_absent_flags_preserve_saved_values();
    test_negative_flags_disable_saved_values_for_this_run();
    test_existing_positive_flags_still_enable_modes();
    test_last_explicit_flag_wins();
    puts("PASS: CLI mode overrides preserve saved values by default and apply per run");
    return 0;
}
"""

output.write_text(
    prefix
    + options_match.group(0)
    + "\n"
    + parser_match.group(0)
    + "\n"
    + mode_match.group(0)
    + middle
    + overrides_match.group(0)
    + "\n}\n"
    + save_function
    + suffix
)
PY

gcc -std=c99 -Wall -Wextra -Werror -Isrc "$TMPDIR/test_cli_run_mode_overrides.c" \
    -o "$TMPDIR/test_cli_run_mode_overrides"
"$TMPDIR/test_cli_run_mode_overrides"
