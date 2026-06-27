#!/bin/bash
# Test: Version replacement works correctly during CD build
# This tests that CURRENT_VERSION uses VERSION_STR from compiler flags

set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# Create a temp dir
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Copy constants.h (with our fix) to temp dir and test compilation
cp src/constants.h "$TMPDIR/constants.h"

# === Test 1: With VERSION_STR defined, CURRENT_VERSION should match it ===
cat > "$TMPDIR/test_version_define.c" << 'EOF'
#include "constants.h"
#include <stdio.h>
int main() {
    printf("%s", CURRENT_VERSION);
    return 0;
}
EOF

if gcc -DVERSION_STR='"2.0.0"' -I"$TMPDIR" -o "$TMPDIR/test_define" "$TMPDIR/test_version_define.c"; then
    RESULT=$("$TMPDIR/test_define")
    if [ "$RESULT" = "2.0.0" ]; then
        pass "With VERSION_STR defined, CURRENT_VERSION equals VERSION_STR"
    else
        fail "Expected '2.0.0' but got '$RESULT'"
    fi
else
    fail "Test program with VERSION_STR defined failed to compile"
fi

# === Test 2: Without VERSION_STR, CURRENT_VERSION should use default fallback ===
cat > "$TMPDIR/test_version_nodef.c" << 'EOF'
#include "constants.h"
#include <stdio.h>
int main() {
    printf("%s", CURRENT_VERSION);
    return 0;
}
EOF

if gcc -I"$TMPDIR" -o "$TMPDIR/test_nodef" "$TMPDIR/test_version_nodef.c"; then
    RESULT=$("$TMPDIR/test_nodef")
    if [ "$RESULT" = "1.0.0" ]; then
        pass "Without VERSION_STR, CURRENT_VERSION uses default fallback '1.0.0'"
    else
        fail "Expected '1.0.0' but got '$RESULT'"
    fi
else
    fail "Test program without VERSION_STR failed to compile"
fi

# === Summary ===
echo "---"
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
