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
    r"static int parse_arguments\(int argc, wchar_t\* argv\[\], CLIOptions\* opts,\s*"
    r"CLIParseError\* error\) \{.*?^\}",
    source,
    re.S | re.M,
)
assert parser_match, "could not find parse_arguments definition"
error_types_match = re.search(
    r"typedef enum\s*\{.*?\}\s*CLIParseErrorKind\s*;\s*"
    r"typedef struct\s*\{\s*CLIParseErrorKind\s+kind;.*?\}\s*CLIParseError\s*;",
    source,
    re.S,
)
assert error_types_match, "could not find CLI parse error types"
failure_match = re.search(r"static int fail_cli_parse\(.*?^\}", source, re.S | re.M)
assert failure_match, "could not find fail_cli_parse definition"

overrides_match = re.search(
    r"// Apply CLI overrides AFTER tray_load_settings.*?^\s*tray->refresh_interval_seconds = opts->interval;",
    source,
    re.S | re.M,
)
assert overrides_match, "could not find the run-mode CLI override block"
run_mode_match = re.search(
    r"static int run_tray_mode\(const CLIOptions\* opts\) \{.*?^\}",
    source,
    re.S | re.M,
)
assert run_mode_match, "could not find run_tray_mode"
configure_mode_match = re.search(
    r"static int run_configure_mode\(const CLIOptions\* opts\) \{.*?^\}",
    source,
    re.S | re.M,
)
assert configure_mode_match, "could not find run_configure_mode"

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
typedef void *HWND;
typedef unsigned char *LPBYTE;
typedef struct { int unused; } NotifyGroupManager;
typedef int SessionFinishedAction;
#define REG_DWORD 4
#define ERROR_SUCCESS 0
#define MB_OK 1
#define MB_ICONERROR 2
#define MB_ICONWARNING 4
#define ATTACH_PARENT_PROCESS ((DWORD)-1)

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
    int notification_mode;
    bool startup_cli_overrides_set;
    int notification_mode_cli_override;
    int auto_check_interval_cli_override;
    int check_updates_startup_cli_override;
    NotifyGroupManager notify_groups;
    int auto_check_interval;
    bool check_updates_on_startup;
    bool start_on_startup;
    bool add_to_path;
    HWND hwnd;
} NoSleepTray;

static int startup_set_calls;
static int path_set_calls;
static int tray_run_calls;
static int settings_save_calls;
static int saved_session_finished;
static int saved_auto_start;
static int saved_notification_mode;
static int saved_auto_check_interval;
static int saved_check_updates_startup;
static int saved_add_to_path;
static NoSleepTray test_tray;

static NoSleepTray *tray_create(void) { return &test_tray; }
static bool tray_init(NoSleepTray *tray) { (void)tray; return true; }
static void tray_destroy(NoSleepTray *tray) { (void)tray; }
static void tray_run(NoSleepTray *tray) { (void)tray; ++tray_run_calls; }
static void tray_start_nosleep(NoSleepTray *tray, int duration) {
    (void)tray; (void)duration;
}
void tray_set_startup_enabled(NoSleepTray *tray, bool enable) {
    ++startup_set_calls;
    tray->start_on_startup = enable;
}
bool tray_set_add_to_path(NoSleepTray *tray, bool enable) {
    ++path_set_calls;
    tray->add_to_path = enable;
    return true;
}
static int tray_save_settings_cli(int session_finished, int auto_start,
                                  int notification_mode, int auto_check_interval,
                                  int check_updates_startup, int add_to_path) {
    ++settings_save_calls;
    saved_session_finished = session_finished;
    saved_auto_start = auto_start;
    saved_notification_mode = notification_mode;
    saved_auto_check_interval = auto_check_interval;
    saved_check_updates_startup = check_updates_startup;
    saved_add_to_path = add_to_path;
    return 1;
}
static void prepare_cli_output(void) {}
static void OutputDebugString(const char *message) { (void)message; }
static int MessageBox(HWND hwnd, const char *text, const char *title,
                      unsigned int flags) {
    (void)hwnd; (void)text; (void)title; (void)flags; return 0;
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
    CLIParseError error = {0};
    assert(parse_arguments(argc, argv, &options, &error) == 0);
    assert(error.kind == CLI_PARSE_ERROR_NONE);
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

static void test_persistent_flags_require_configure_mode(void) {
    wchar_t *run_argv[] = {L"nosleep", L"--auto-start", L"--add-to-path"};
    CLIOptions run_options = default_options();
    CLIParseError error = {0};
    assert(parse_arguments(3, run_argv, &run_options, &error) == 1);
    assert(error.kind == CLI_PARSE_ERROR_REQUIRES_CONFIGURE);
    assert(wcscmp(error.option, L"--auto-start") == 0);

    assert(!run_options.configure_mode);
    assert(tray_run_calls == 0);
    assert(startup_set_calls == 0);
    assert(path_set_calls == 0);
    assert(settings_save_calls == 0);

    wchar_t *configure_argv[] = {
        L"nosleep", L"--auto-start", L"--add-to-path", L"--configure"
    };
    CLIOptions configure_options = parse(4, configure_argv);

    assert(configure_options.configure_mode);
    assert(run_configure_mode(&configure_options) == 0);
    assert(settings_save_calls == 1);
    assert(saved_auto_start == CLI_ENABLE);
    assert(saved_add_to_path == CLI_ENABLE);
}

int main(void) {
    test_absent_flags_preserve_saved_values();
    test_negative_flags_disable_saved_values_for_this_run();
    test_existing_positive_flags_still_enable_modes();
    test_last_explicit_flag_wins();
    test_persistent_flags_require_configure_mode();
    puts("PASS: CLI mode overrides preserve saved values by default and apply per run");
    return 0;
}
"""

output.write_text(
    prefix
    + options_match.group(0)
    + error_types_match.group(0)
    + "\n"
    + failure_match.group(0)
    + "\n"
    + parser_match.group(0)
    + "\n"
    + mode_match.group(0)
    + middle
    + overrides_match.group(0)
    + "\n}\n"
    + run_mode_match.group(0)
    + "\n"
    + configure_mode_match.group(0)
    + "\n"
    + save_function
    + suffix
)
PY

gcc -std=c99 -Wall -Wextra -Werror -Isrc "$TMPDIR/test_cli_run_mode_overrides.c" \
    -o "$TMPDIR/test_cli_run_mode_overrides"
"$TMPDIR/test_cli_run_mode_overrides"
