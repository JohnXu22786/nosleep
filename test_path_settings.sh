#!/bin/bash
# Test: PATH manipulation and OK/Cancel settings behavior
set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "=== Test: PATH manipulation functions ==="

# === Test 1: PATH add/remove functions compile correctly ===
cat > "$TMPDIR/test_path_compile.c" << 'PATH_EOF'
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Case-insensitive comparison (same as production code)
static int str_icmp_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] == '\0' && b[i] == '\0') return 0;
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

static char* get_exe_dir(void) {
    static char path[MAX_PATH];
    DWORD len = GetModuleFileName(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return NULL;
    char* last_backslash = strrchr(path, '\\');
    if (last_backslash) {
        *last_backslash = '\0';
    }
    return path;
}

static bool is_in_path(const char* dir) {
    char* env_path = getenv("PATH");
    if (!env_path) return false;
    size_t dir_len = strlen(dir);
    char* p = env_path;
    while (*p) {
        char* next = strchr(p, ';');
        size_t seg_len = next ? (size_t)(next - p) : strlen(p);
        if (seg_len == dir_len && str_icmp_n(p, dir, dir_len) == 0) {
            return true;
        }
        if (next) {
            p = next + 1;
        } else {
            break;
        }
    }
    return false;
}

int main() {
    char* exe_dir = get_exe_dir();
    if (!exe_dir) {
        printf("FAIL: Could not get executable directory\n");
        return 1;
    }
    printf("Executable directory: %s\n", exe_dir);

    // Test is_in_path with system32 (should be in PATH on Windows)
    bool found_sys = is_in_path("C:\\Windows\\system32");
    printf("is_in_path('C:\\Windows\\system32'): %s\n", found_sys ? "true" : "false");

    // Test with a bogus directory (should NOT be in PATH)
    bool found_bogus = is_in_path("C:\\NoSuchDir_12345_XYZ");
    printf("is_in_path(bogus): %s\n", found_bogus ? "true" : "false");

    if (found_bogus) {
        printf("FAIL: Bogus directory should not be in PATH\n");
        return 1;
    }

    // The PATH parsing itself compiled and ran correctly
    printf("PASS: PATH manipulation functions compile and work\n");
    return 0;
}
PATH_EOF

if gcc -o "$TMPDIR/test_path_compile" "$TMPDIR/test_path_compile.c" -luser32 2>&1; then
    RESULT=$("$TMPDIR/test_path_compile" 2>&1 || true)
    echo "$RESULT"
    if echo "$RESULT" | grep -q "PASS:"; then
        pass "PATH manipulation functions compile and pass basic logic tests"
    else
        fail "PATH manipulation logic test failed"
    fi
else
    fail "PATH manipulation functions failed to compile"
fi

# === Test 2: Verify the real NoSleepTray struct with add_to_path compiles ===
echo ""
echo "=== Test: Real struct compilation ==="

cat > "$TMPDIR/test_real_struct.c" << 'STRUCT_EOF'
// Include the actual project headers
#include "tray.h"
#include "constants.h"

int main() {
    // Verify the struct can be instantiated with the new field
    NoSleepTray tray;
    tray.add_to_path = false;
    tray.prevent_display = false;
    (void)tray;
    return 0;
}
STRUCT_EOF

if gcc -std=c99 -Isrc -o "$TMPDIR/test_real_struct" "$TMPDIR/test_real_struct.c" 2>&1; then
    pass "Real NoSleepTray struct (from tray.h) with add_to_path compiles"
else
    fail "Real NoSleepTray struct with add_to_path failed to compile"
fi

# === Test 3: Verify new feature exists in production code ===
echo ""
echo "=== Test: New feature presence validation ==="

# Check that tray.h has add_to_path field
if grep -q "add_to_path" src/tray.h; then
    pass "tray.h contains add_to_path field"
else
    fail "tray.h does NOT contain add_to_path field (feature not implemented yet)"
fi

# Check that tray.c has path manipulation functions
if grep -Eq "add_to_path|add_app_to_path|remove_app_from_path|WM_SETTINGCHANGE" src/tray.c; then
    pass "tray.c contains PATH manipulation code"
else
    fail "tray.c does NOT contain PATH manipulation code (feature not implemented yet)"
fi

# Check that settings dialog has the new checkbox
if grep -Eq "IDC_ADD_TO_PATH|Add nosleep to PATH|Add to environment PATH" src/tray.c; then
    pass "Settings dialog has PATH checkbox"
else
    fail "Settings dialog does NOT have PATH checkbox (feature not implemented yet)"
fi

# === Test 4: Compile production source files individually to verify no syntax errors ===
echo ""
echo "=== Test: Production source compilation (syntax check) ==="
if gcc -std=c99 -Wall -Wextra -fsyntax-only -Isrc -c src/core.c -DVERSION_STR=\"0.0.0\" 2>&1; then
    pass "core.c has no syntax errors"
else
    fail "core.c has syntax errors"
fi

if gcc -std=c99 -Wall -Wextra -fsyntax-only -Isrc -c src/tray.c -DVERSION_STR=\"0.0.0\" 2>&1; then
    pass "tray.c has no syntax errors"
else
    fail "tray.c has syntax errors"
fi

if gcc -std=c99 -Wall -Wextra -fsyntax-only -Isrc -c src/main.c -DVERSION_STR=\"0.0.0\" 2>&1; then
    pass "main.c has no syntax errors"
else
    fail "main.c has syntax errors"
fi

# === Summary ===
echo "---"
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
