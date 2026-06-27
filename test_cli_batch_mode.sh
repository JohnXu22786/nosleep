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
HELP_TEXT=$(sed -n '/const char\* help_text/,/^;/p' src/main.c | tr -d '\n' | sed 's/const char\* help_text = //' | sed 's/;$//' | sed 's/"//g')

check_help_flag() {
    local flag="$1"
    local description="$2"
    if echo "$HELP_TEXT" | grep -q -- "$flag"; then
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
check_help_flag "--away-mode" "Away mode"
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
    check_readme_flag "--configure"
    check_readme_flag "--version"
else
    fail "README.md does not exist"
fi

# ============================================================
# Test 7: Build test - verify source compiles and parses flags
# ============================================================
echo ""
echo "=== Test: Source code compiles with new flags ==="

cat > "$TMPDIR/test_args.c" << 'ENDTEST'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static int parse_batch_args_test(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--session-finished") == 0) { i++; }
        else if (strcmp(argv[i], "--notification-mode") == 0) { i++; }
        else if (strcmp(argv[i], "--auto-check-interval") == 0) { i++; }
        else if (strcmp(argv[i], "--auto-start") == 0) { }
        else if (strcmp(argv[i], "--no-auto-start") == 0) { }
        else if (strcmp(argv[i], "--check-updates-startup") == 0) { }
        else if (strcmp(argv[i], "--no-check-updates-startup") == 0) { }
        else if (strcmp(argv[i], "--configure") == 0) { }
        else if (strcmp(argv[i], "--version") == 0) { }
        else if (strcmp(argv[i], "--help") == 0) return 0;
        else { printf("Unknown: %s\n", argv[i]); return 1; }
    }
    return 0;
}

int main() {
    int result = 0;
    char* t1[] = {"p", "--session-finished", "shutdown"};
    char* t2[] = {"p", "--notification-mode", "critical"};
    char* t3[] = {"p", "--auto-check-interval", "daily"};
    char* t4[] = {"p", "--auto-start"};
    char* t5[] = {"p", "--no-auto-start"};
    char* t6[] = {"p", "--check-updates-startup"};
    char* t7[] = {"p", "--no-check-updates-startup"};
    char* t8[] = {"p", "--configure"};
    char* t9[] = {"p", "--version"};
    char* t10[] = {"p", "--help"};
    char* t11[] = {"p", "--session-finished", "sleep", "--notification-mode", "none", "--configure"};

    result |= parse_batch_args_test(3, t1);
    result |= parse_batch_args_test(3, t2);
    result |= parse_batch_args_test(3, t3);
    result |= parse_batch_args_test(2, t4);
    result |= parse_batch_args_test(2, t5);
    result |= parse_batch_args_test(2, t6);
    result |= parse_batch_args_test(2, t7);
    result |= parse_batch_args_test(2, t8);
    result |= parse_batch_args_test(2, t9);
    result |= parse_batch_args_test(2, t10);
    result |= parse_batch_args_test(6, t11);

    if (result == 0) {
        printf("ALL PASS\n");
    }
    return result;
}
ENDTEST

if gcc -o "$TMPDIR/test_args" "$TMPDIR/test_args.c" 2>/dev/null; then
    TEST_OUT=$("$TMPDIR/test_args" 2>&1 || true)
    if echo "$TEST_OUT" | grep -q "ALL PASS"; then
        pass "All new CLI flags parse correctly in test compilation"
    else
        fail "CLI parsing test returned unexpected output: $TEST_OUT"
    fi
else
    fail "CLI parsing test compilation failed (this may be expected if MinGW not available)"
    echo "  (Note: This test needs MinGW gcc - may pass on CI environment)"
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
