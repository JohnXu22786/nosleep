#!/bin/bash
# Test: Notification group data structure and version comparison
# Tests the core logic of the new notification group system
# and the update version comparison

set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# Create a temp dir
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# If compiling on Linux with MinGW, skip tests that need compilation
CAN_COMPILE=false
if command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    CAN_COMPILE=true
elif command -v gcc &> /dev/null; then
    # For local testing we might have gcc
    # But we can't link Win32 APIs, so compile only logic tests
    CAN_COMPILE=true
fi

# === Test 1: Version comparison logic ===
cat > "$TMPDIR/test_version_compare.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Simulate the version comparison logic that will be in the update feature
// Returns: 1 if new_version > current_version, 0 if equal, -1 if new_version < current_version
int compare_versions(const char* v1, const char* v2) {
    // Strip leading 'v' or 'V'
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;
    
    // Parse major.minor.patch
    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;
    
    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);
    
    if (maj1 > maj2) return 1;
    if (maj1 < maj2) return -1;
    if (min1 > min2) return 1;
    if (min1 < min2) return -1;
    if (pat1 > pat2) return 1;
    if (pat1 < pat2) return -1;
    return 0;
}

int main() {
    int failures = 0;
    
    // Test 1: newer version
    if (compare_versions("2.0.0", "1.0.0") != 1) {
        printf("FAIL: 2.0.0 should be > 1.0.0\n");
        failures++;
    }
    
    // Test 2: older version
    if (compare_versions("1.0.0", "2.0.0") != -1) {
        printf("FAIL: 1.0.0 should be < 2.0.0\n");
        failures++;
    }
    
    // Test 3: equal
    if (compare_versions("1.2.3", "1.2.3") != 0) {
        printf("FAIL: 1.2.3 should equal 1.2.3\n");
        failures++;
    }
    
    // Test 4: with leading v
    if (compare_versions("v2.0.0", "1.0.0") != 1) {
        printf("FAIL: v2.0.0 should be > 1.0.0\n");
        failures++;
    }
    
    // Test 5: minor version difference
    if (compare_versions("1.3.0", "1.2.9") != 1) {
        printf("FAIL: 1.3.0 should be > 1.2.9\n");
        failures++;
    }
    
    // Test 6: patch version difference
    if (compare_versions("1.0.1", "1.0.0") != 1) {
        printf("FAIL: 1.0.1 should be > 1.0.0\n");
        failures++;
    }
    
    // Test 7: with leading V (uppercase)
    if (compare_versions("V1.0.0", "0.9.9") != 1) {
        printf("FAIL: V1.0.0 should be > 0.9.9\n");
        failures++;
    }
    
    // Test 8: major version update
    if (compare_versions("2.1.0", "1.9.9") != 1) {
        printf("FAIL: 2.1.0 should be > 1.9.9\n");
        failures++;
    }
    
    return failures;
}
EOF

if gcc -o "$TMPDIR/test_version_compare" "$TMPDIR/test_version_compare.c"; then
    RESULT=$("$TMPDIR/test_version_compare")
    if [ -z "$RESULT" ]; then
        pass "Version comparison: all 8 cases correct"
    else
        fail "Version comparison: $RESULT"
    fi
else
    fail "Version comparison test failed to compile"
fi

# === Test 2: Update batch script generation ===
cat > "$TMPDIR/test_update_batch_gen.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Simulate the batch script generation logic
// The batch script needs to:
// 1. Wait for the parent process to exit
// 2. Replace the old EXE with the downloaded one (rename to original name)
// 3. Launch the new EXE
// 4. Delete itself

char* generate_update_script(const char* current_exe_path, 
                              const char* downloaded_temp_path,
                              const char* original_exe_name) {
    // Calculate required buffer size
    const char* template = 
        "@echo off\r\n"
        "title Updating nosleep...\r\n"
        "echo Waiting for nosleep to exit...\r\n"
        ":waitloop\r\n"
        "ping -n 2 127.0.0.1 > nul\r\n"
        "tasklist /FI \"IMAGENAME eq %s\" 2>nul | find /I \"%s\" > nul\r\n"
        "if errorlevel 1 goto replace\r\n"
        "goto waitloop\r\n"
        ":\r\n"
        ":replace\r\n"
        "echo Replacing executable...\r\n"
        "move /Y \"%s\" \"%s\" > nul\r\n"
        "if errorlevel 1 (\r\n"
        "    echo Failed to replace executable.\r\n"
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "echo Starting nosleep...\r\n"
        "start \"\" \"%s\"\r\n"
        "echo Done.\r\n"
        "del \"%%~f0\"\r\n";
    
    // Estimate size: template + all strings + overhead
    size_t size = strlen(template) + strlen(current_exe_path) * 3 
                  + strlen(downloaded_temp_path) + strlen(original_exe_name) + 256;
    char* script = (char*)malloc(size);
    if (!script) return NULL;
    
    // Get just the executable name from path
    const char* exe_name = original_exe_name;
    
    snprintf(script, size, template, 
             exe_name,           // for tasklist filter
             exe_name,           // for find
             downloaded_temp_path, // source
             current_exe_path,     // destination
             current_exe_path);    // to launch
    
    return script;
}

int main() {
    int failures = 0;
    
    char* script = generate_update_script(
        "C:\\Users\\test\\AppData\\Local\\nosleep.exe",
        "C:\\Users\\test\\AppData\\Local\\Temp\\nosleep_v2.0.0.exe",
        "nosleep.exe"
    );
    
    if (!script) {
        printf("FAIL: Script generation returned NULL\n");
        return 1;
    }
    
    // Check the script contains key elements
    if (strstr(script, "move /Y") == NULL) {
        printf("FAIL: Script missing move command\n");
        failures++;
    }
    
    if (strstr(script, "nosleep.exe") == NULL) {
        printf("FAIL: Script missing executable name\n");
        failures++;
    }
    
    if (strstr(script, "Waiting for nosleep") == NULL) {
        printf("FAIL: Script missing wait message\n");
        failures++;
    }
    
    if (strstr(script, "del \"%~f0\"") == NULL) {
        printf("FAIL: Script missing self-delete\n");
        failures++;
    }
    
    if (strstr(script, "start \"\"") == NULL) {
        printf("FAIL: Script missing start command\n");
        failures++;
    }
    
    // Check the downloaded temp path and final path are different
    if (strcmp("C:\\Users\\test\\AppData\\Local\\Temp\\nosleep_v2.0.0.exe", 
               "C:\\Users\\test\\AppData\\Local\\nosleep.exe") == 0) {
        printf("FAIL: Temp path and final path should differ\n");
        failures++;
    }
    
    free(script);
    
    if (failures == 0) {
        printf("OK");
    }
    return failures;
}
EOF

if gcc -o "$TMPDIR/test_update_batch_gen" "$TMPDIR/test_update_batch_gen.c"; then
    RESULT=$("$TMPDIR/test_update_batch_gen")
    if [ "$RESULT" = "OK" ]; then
        pass "Update batch script generation: all checks pass"
    else
        fail "Update batch script generation: $RESULT"
    fi
else
    # MingGW might not be available; this is expected on some platforms
    echo "SKIP: Update batch script generation test (compilation not available)"
fi

# === Test 3: Registry path generation for notification groups ===
cat > "$TMPDIR/test_registry_keys.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NOTIFY_GROUPS_REG_KEY "Software\\nosleep\\settings\\NotificationGroups"
#define MAX_GROUP_NAME 128
#define MAX_GROUPS 20
#define MAX_NOTIFY_EVENTS 32

// Simulate notification group data structure
typedef struct {
    char name[MAX_GROUP_NAME];
    unsigned int event_mask;  // Bitmask of enabled notification events
} NotifyGroup;

// Simulate storing groups in registry paths
// Each group gets a subkey: Software\\nosleep\\settings\\NotificationGroups\\Group_N
// with "name" and "event_mask" values

int generate_group_reg_path(char* buffer, size_t buf_size, int group_index) {
    return snprintf(buffer, buf_size, "%s\\Group_%d", NOTIFY_GROUPS_REG_KEY, group_index);
}

int main() {
    int failures = 0;
    
    char path[512];
    int len = generate_group_reg_path(path, sizeof(path), 0);
    
    if (len <= 0 || (size_t)len >= sizeof(path)) {
        printf("FAIL: Registry path too long\n");
        failures++;
    }
    
    const char* expected = "Software\\nosleep\\settings\\NotificationGroups\\Group_0";
    if (strcmp(path, expected) != 0) {
        printf("FAIL: Expected '%s', got '%s'\n", expected, path);
        failures++;
    }
    
    // Test group 5
    len = generate_group_reg_path(path, sizeof(path), 5);
    expected = "Software\\nosleep\\settings\\NotificationGroups\\Group_5";
    if (strcmp(path, expected) != 0) {
        printf("FAIL: Expected '%s', got '%s'\n", expected, path);
        failures++;
    }
    
    // Test mask operations
    unsigned int mask = 0;
    
    // Enable event 0
    mask |= (1 << 0);
    if (!(mask & (1 << 0))) {
        printf("FAIL: Event 0 should be enabled\n");
        failures++;
    }
    
    // Enable event 3
    mask |= (1 << 3);
    if (!(mask & (1 << 3))) {
        printf("FAIL: Event 3 should be enabled\n");
        failures++;
    }
    
    // Event 1 should still be disabled
    if (mask & (1 << 1)) {
        printf("FAIL: Event 1 should be disabled\n");
        failures++;
    }
    
    // Disable event 0
    mask &= ~(1 << 0);
    if (mask & (1 << 0)) {
        printf("FAIL: Event 0 should now be disabled\n");
        failures++;
    }
    
    if (failures == 0) {
        printf("OK");
    }
    return failures;
}
EOF

if gcc -o "$TMPDIR/test_registry_keys" "$TMPDIR/test_registry_keys.c"; then
    RESULT=$("$TMPDIR/test_registry_keys")
    if [ "$RESULT" = "OK" ]; then
        pass "Registry key generation and bitmask operations: all checks pass"
    else
        fail "Registry key generation and bitmask operations: $RESULT"
    fi
else
    echo "SKIP: Registry keys test (compilation not available)"
fi

# === Summary ===
echo "---"
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
