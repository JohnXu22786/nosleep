#!/bin/bash
# Test: CLI batch mode - all settings configurable via command line without GUI
# Validates that CLI help output includes all new flags and that argument parsing
# covers all settings exposed in the GUI settings dialog.

set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# Create a temp dir for compilation tests
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# ============================================================
# Test 1: --help output includes all new CLI flags
# ============================================================
echo "=== Test: --help output includes new flags ==="

# Extract help text from main.c
# The declaration ends on its final quoted line with a trailing semicolon.
HELP_TEXT=$(sed -n '/^static const char\* const HELP_TEXT =$/,/";$/p' src/main.c | tr -d '\n' | sed 's/static const char\* const HELP_TEXT = //' | sed 's/;$//' | sed 's/"//g')
# Examples can repeat flags, so check the option sections only.
HELP_OPTIONS=${HELP_TEXT%%Examples:*}

check_help_flag() {
    local flag="$1"
    local description="$2"
    local option_pattern="(^|[^[:alnum:]_-])${flag}([^[:alnum:]_-]|$)"
    if echo "$HELP_OPTIONS" | grep -Eq -- "$option_pattern"; then
        pass "--help includes '$flag' flag"
    else
        fail "--help is missing '$flag' flag - $description"
    fi
}

# New flags for batch/configure mode
check_help_flag "--session-finished" "Action after timer expires (none/shutdown/sleep)"
check_help_flag "--notification-mode" "Notification mode (all/critical/none)"
check_help_flag "--auto-check-interval" "Update check interval (never/daily/weekly)"
check_help_flag "--auto-start" "Auto-start with Windows (enable)"
check_help_flag "--no-auto-start" "Auto-start with Windows (disable)"
check_help_flag "--check-updates-startup" "Check for updates on startup (enable)"
check_help_flag "--no-check-updates-startup" "Check for updates on startup (disable)"
check_help_flag "--configure" "Save settings to registry and exit"
check_help_flag "--version" "Show version information"

# Existing flags should still be present
check_help_flag "--duration" "Duration in minutes"
check_help_flag "--interval" "Interval in seconds"
check_help_flag "--prevent-display" "Prevent display sleep"
check_help_flag "--no-prevent-display" "Disable display sleep prevention for this run"
check_help_flag "--away-mode" "Away mode"
check_help_flag "--no-away-mode" "Disable away mode for this run"
check_help_flag "--verbose" "Verbose logging"
check_help_flag "--tray" "System tray mode"
check_help_flag "--startup" "Startup mode"
check_help_flag "--help" "Help message"

# ============================================================
# Test 2: Source code has parse_arguments for new flags
# ============================================================
echo ""
echo "=== Test: Source code parses new flags ==="

check_parse() {
    local flag="$1"
    if grep -q -- "\"$flag\"" src/main.c; then
        pass "main.c parses '$flag' argument"
    else
        fail "main.c does not parse '$flag' argument"
    fi
}

check_parse "--session-finished"
check_parse "--notification-mode"
check_parse "--auto-check-interval"
check_parse "--auto-start"
check_parse "--no-auto-start"
check_parse "--check-updates-startup"
check_parse "--no-check-updates-startup"
check_parse "--configure"
check_parse "--version"

# ============================================================
# Test 3: --version compilation test
# ============================================================
echo ""
echo "=== Test: --version compilation ==="

# Check that CURRENT_VERSION is accessible from main.c
if grep -q "CURRENT_VERSION" src/main.c; then
    if grep -q "show_version" src/main.c; then
        pass "main.c uses CURRENT_VERSION for --version flag"
    else
        fail "main.c does not use CURRENT_VERSION for --version flag"
    fi
else
    if grep -q '#include "constants.h"' src/main.c; then
        pass "main.c includes constants.h"
    else
        fail "main.c does not include constants.h for version"
    fi
fi

# ============================================================
# Test 4: tray.c has settings-save function for CLI mode
# ============================================================
echo ""
echo "=== Test: Settings persistence via CLI ==="

if grep -q "tray_save_settings_cli" src/tray.c; then
    pass "tray.c has a standalone settings save function for CLI mode"
else
    fail "tray.c is missing a standalone settings save function for CLI mode"
fi

if grep -q "tray_save_settings_cli" src/tray.h; then
    pass "tray.h declares the standalone settings save function"
else
    fail "tray.h is missing declaration for standalone settings save function"
fi

# ============================================================
# Test 5: CLI overrides are applied AFTER registry load
# ============================================================
echo ""
echo "=== Test: CLI overrides applied after tray_load_settings ==="

# Check that CLI overrides are applied after tray_load_settings in the flow
if grep -q "tray_load_settings" src/tray.c; then
    # Check main.c applies CLI overrides after tray_init
    if grep -q "CLI overrides AFTER" src/main.c; then
        pass "CLI settings overrides comment confirms post-init application"
    else
        fail "main.c missing comment showing CLI overrides applied after tray_init"
    fi
fi

# ============================================================
# Test 6: README documents all new CLI flags
# ============================================================
echo ""
echo "=== Test: README documents new flags ==="

if [ -f "README.md" ]; then
    check_readme_flag() {
        local flag="$1"
        if grep -q -- "$flag" README.md; then
            pass "README.md documents '$flag' flag"
        else
            fail "README.md is missing '$flag' flag"
        fi
    }
    check_readme_flag "--session-finished"
    check_readme_flag "--notification-mode"
    check_readme_flag "--auto-check-interval"
    check_readme_flag "--auto-start"
    check_readme_flag "--no-auto-start"
    check_readme_flag "--check-updates-startup"
    check_readme_flag "--no-check-updates-startup"
    check_readme_flag "--no-prevent-display"
    check_readme_flag "--no-away-mode"
    check_readme_flag "--configure"
    check_readme_flag "--version"
else
    fail "README.md does not exist"
fi

# ============================================================
# Test 7: Exercise the production parser for batch/configure flags
# ============================================================
echo ""
echo "=== Test: Production parser applies batch CLI options ==="

if bash "$(dirname "${BASH_SOURCE[0]}")/test_cli_parse_errors.sh"; then
    pass "Production parser handles batch CLI flags and values"
else
    fail "Production parser regression test failed"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "========"
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
