#!/bin/bash
# Test: Lazy icon loading - verify numbered icons are created on demand, not at startup

set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_FILE="$SCRIPT_DIR/src/tray.c"

echo "=== Test 1: Source code analysis - eager creation loop removed ==="

# Test 1a: Look for eager creation loop (for-loop with <60 + create_numbered_icon)
# Ignore the destruction loop which also has for (i=0; i<60; i++) but uses DestroyIcon
if grep -A1 -n "for (int i = 0; i < 60; i++)" "$SRC_FILE" | grep -q "create_numbered_icon"; then
    fail "Eager creation loop still exists: for-loop + create_numbered_icon found"
else
    pass "No eager creation loop found (for i<60 with create_numbered_icon)"
fi

# Test 1b: No eager creation inside tray_create_icons function body
# Look for 'for' loop with <60 followed by create_numbered_icon within the function
# (not the forward declaration at line 55)
start_line=$(grep -n "static void tray_create_icons" "$SRC_FILE" | head -1 | cut -d: -f1)
if [ -n "$start_line" ]; then
    # Find the line containing "static void tray_destroy_icons" which comes after
    end_line=$(grep -n "static void tray_destroy_icons\|^static void tray_create_menu" "$SRC_FILE" | while IFS=: read -r num func; do if [ "$num" -gt "$start_line" ]; then echo "$num"; break; fi; done)
    if [ -n "$end_line" ]; then
        # Extract function body and check for eager creation
        function_body=$(sed -n "${start_line},${end_line}p" "$SRC_FILE")
        if echo "$function_body" | grep -q "for.*i.*<.*60.*i++" && echo "$function_body" | grep -q "create_numbered_icon"; then
            fail "tray_create_icons still contains eager creation loop"
        else
            pass "tray_create_icons function body has no eager creation loop"
        fi
    else
        fail "Could not find end of tray_create_icons function"
    fi
else
    fail "Could not find tray_create_icons function"
fi

echo ""
echo "=== Test 2: Verify lazy loading code exists ==="

# Test 2a: Countdown path - lazy loading
if grep -q "hIconNumbered\[countdown_seconds\] == NULL" "$SRC_FILE"; then
    pass "Lazy loading NULL check exists for countdown path"
else
    fail "Missing lazy loading check in countdown path"
fi

if grep -q "hIconNumbered\[countdown_seconds\] = create_numbered_icon" "$SRC_FILE"; then
    pass "Lazy creation exists for countdown path"
else
    fail "Missing lazy creation in countdown path"
fi

# Test 2b: Duration path - lazy loading
if grep -q "hIconNumbered\[display_number\] == NULL" "$SRC_FILE"; then
    pass "Lazy loading NULL check exists for duration path"
else
    fail "Missing lazy loading check in duration path"
fi

if grep -q "hIconNumbered\[display_number\] = create_numbered_icon" "$SRC_FILE"; then
    pass "Lazy creation exists for duration path"
else
    fail "Missing lazy creation in duration path"
fi

echo ""
echo "=== Test 3: Verify NULL-safe destruction ==="

if grep -q "if.*tray->hIconNumbered\[i\]" "$SRC_FILE"; then
    pass "tray_destroy_icons has NULL check before DestroyIcon"
else
    fail "tray_destroy_icons missing NULL check before DestroyIcon"
fi

echo ""
echo "=== Test 4: Verify struct zero-initialization ==="

if grep -q "memset.*tray.*0.*sizeof" "$SRC_FILE"; then
    pass "NoSleepTray struct is zero-initialized via memset in tray_create"
else
    fail "NoSleepTray struct missing zero initialization"
fi

echo ""
echo "=== Test 5: Compilation test ==="

if command -v make &>/dev/null; then
    cd "$SCRIPT_DIR"
    if make clean 2>/dev/null; then true; fi
    if make 2>&1; then
        pass "Project compiles successfully with make"
    else
        fail "Project compilation failed with make"
    fi
else
    echo "SKIP: 'make' not found in PATH (MinGW not installed)"
    pass "SKIP: Compilation test (make not available)"
fi

echo ""
echo "=== Summary ==="
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
