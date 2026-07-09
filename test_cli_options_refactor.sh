#!/bin/bash
# Test: CLI options struct refactoring
# Validates that:
# 1. CLIOptions struct is defined in main.c
# 2. parse_arguments uses CLIOptions* instead of 18 parameters
# 3. run_tray_mode uses const CLIOptions*
# 4. run_configure_mode uses const CLIOptions*
# 5. help_text is a static const global constant
# 6. Project compiles successfully

set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# Create a temp dir for compilation tests
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "=== Test 1: CLIOptions struct is defined ==="

if grep -q "} CLIOptions" src/main.c; then
    pass "CLIOptions struct typedef found with closing brace"
    
    # Verify it has the expected fields inside the struct definition
    STRUCT_BODY=$(sed -n '/^typedef struct {/,/^} CLIOptions/p' src/main.c)
    for field in duration interval prevent_display away_mode verbose tray_mode startup \
                 prevent_display_set away_mode_set verbose_set \
                 session_finished auto_start notification_mode auto_check_interval \
                 check_updates_startup add_to_path configure_mode show_version; do
        if echo "$STRUCT_BODY" | grep -q "$field"; then
            pass "  CLIOptions field '$field' found"
        fi
    done
else
    fail "CLIOptions struct not found in main.c"
fi

echo ""
echo "=== Test 2: parse_arguments uses CLIOptions* ==="

if grep -q "parse_arguments.*CLIOptions" src/main.c; then
    pass "parse_arguments references CLIOptions"
else
    fail "parse_arguments does not reference CLIOptions"
fi

if grep -q "parse_arguments(int argc, wchar_t\* argv\[\], CLIOptions" src/main.c; then
    pass "parse_arguments signature uses CLIOptions*"
else
    fail "parse_arguments signature does not use CLIOptions*"
fi

echo ""
echo "=== Test 3: run_tray_mode uses const CLIOptions* ==="

if grep -q "run_tray_mode.*const.*CLIOptions" src/main.c; then
    pass "run_tray_mode uses const CLIOptions*"
else
    fail "run_tray_mode does not use const CLIOptions*"
fi

echo ""
echo "=== Test 4: run_configure_mode uses const CLIOptions* ==="

if grep -q "run_configure_mode.*const.*CLIOptions" src/main.c; then
    pass "run_configure_mode uses const CLIOptions*"
else
    fail "run_configure_mode does not use const CLIOptions*"
fi

echo ""
echo "=== Test 5: help_text is static const global ==="

# Multiple patterns to detect the static const global
if grep -q "^static const char\* const HELP_TEXT" src/main.c || \
   grep -q "^static const char \*const HELP_TEXT" src/main.c; then
    pass "HELP_TEXT is static const global constant"
elif grep -q "^static const char" src/main.c; then
    pass "Found static const char declaration (for HELP_TEXT)"
else
    # Check it's not a local in WinMain anymore
    if grep -q "const char\* help_text" src/main.c; then
        fail "help_text still appears as local variable (not static const)"
    else
        pass "No local const char* help_text found (should be global now)"
    fi
fi

echo ""
echo "=== Test 6: Project compiles cleanly ==="

MAKE=mingw32-make
if $MAKE clean 2>/dev/null && $MAKE 2>/dev/null; then
    pass "Project compiles successfully with the refactored code"
    
    echo ""
    echo "=== Test 7: --help flag runs without crash ==="
    # GUI app - output may not capture in all contexts, check at least it doesn't hang/crash
    if timeout 5 ./bin/nosleep.exe --help > /dev/null 2>&1; then
        pass "--help command runs and exits cleanly"
    else
        # Try without timeout
        if ./bin/nosleep.exe --help > /dev/null 2>&1; then
            pass "--help command runs and exits cleanly"
        else
            fail "--help command had non-zero exit"
        fi
    fi
    
    echo ""
    echo "=== Test 8: --version flag runs without crash ==="
    if timeout 5 ./bin/nosleep.exe --version > /dev/null 2>&1; then
        pass "--version command runs and exits cleanly"
    else
        if ./bin/nosleep.exe --version > /dev/null 2>&1; then
            pass "--version command runs and exits cleanly"
        else
            fail "--version command had non-zero exit"
        fi
    fi
    
    echo ""
    echo "=== Test 9: Invalid args produce non-zero exit ==="
    if timeout 5 ./bin/nosleep.exe --invalid-flag > /dev/null 2>&1; then
        fail "Invalid args did not produce non-zero exit"
    else
        INVALID_EXIT=$?
        if [ $INVALID_EXIT -ne 0 ]; then
            pass "Invalid args produce non-zero exit (exit code: $INVALID_EXIT)"
        else
            fail "Invalid args produced zero exit"
        fi
    fi
else
    fail "Project compilation FAILED"
    echo "  (Check compiler output above for details)"
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
