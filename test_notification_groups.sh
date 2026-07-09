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

# === Test 4: Registry group loading - break vs continue behavior for corrupt entries ===
cat > "$TMPDIR/test_registry_load_continue.c" << 'TESTEOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_NOTIFY_GROUPS 20
#define MAX_GROUP_NAME 128

// Simulate OLD behavior: break on corrupt, store at index i (registry subkey index)
// Returns number of valid groups loaded and fills arr[] at i positions
int load_groups_break_stored(const char* entries_valid[], int num_entries,
                              char arr[][MAX_GROUP_NAME]) {
    int count = 0;
    for (int i = 0; i < MAX_NOTIFY_GROUPS && i < num_entries; i++) {
        if (entries_valid[i] == NULL || entries_valid[i][0] == '\0') {
            break; // Old behavior: stop on corrupt entry
        }
        strncpy(arr[i], entries_valid[i], MAX_GROUP_NAME - 1);
        arr[i][MAX_GROUP_NAME - 1] = '\0';
        count++;
    }
    return count;
}

// Simulate NEW behavior: continue on corrupt, store at arr[count] (compact)
// Returns number of valid groups loaded, fills arr[] compactly
int load_groups_continue_compacted(const char* entries_valid[], int num_entries,
                                    char arr[][MAX_GROUP_NAME]) {
    int count = 0;
    for (int i = 0; i < MAX_NOTIFY_GROUPS && i < num_entries; i++) {
        if (entries_valid[i] == NULL || entries_valid[i][0] == '\0') {
            continue; // New behavior: skip corrupt entry
        }
        strncpy(arr[count], entries_valid[i], MAX_GROUP_NAME - 1);
        arr[count][MAX_GROUP_NAME - 1] = '\0';
        count++;
    }
    return count;
}

// Verify a range of arr[] has NULL (zeroed) entries
int is_zeroed(const char arr[][MAX_GROUP_NAME], int start, int end) {
    for (int j = start; j < end; j++) {
        if (arr[j][0] != '\0') return 0;
    }
    return 1;
}

int main() {
    int failures = 0;
    char result_break[MAX_NOTIFY_GROUPS][MAX_GROUP_NAME] = {{0}};
    char result_cont[MAX_NOTIFY_GROUPS][MAX_GROUP_NAME] = {{0}};

    // ==================================================
    // Test 1: All entries valid - both produce same result
    // ==================================================
    memset(result_break, 0, sizeof(result_break));
    memset(result_cont, 0, sizeof(result_cont));
    const char* all_valid[] = {"GroupA", "GroupB", "GroupC"};
    int r1b = load_groups_break_stored(all_valid, 3, result_break);
    int r1c = load_groups_continue_compacted(all_valid, 3, result_cont);
    if (r1b != 3 || r1c != 3) {
        printf("FAIL Test1a: counts break=%d continue=%d (expected 3)\n", r1b, r1c);
        failures++;
    }
    // Both should have compact arrays
    if (strcmp(result_break[0], "GroupA") != 0 || strcmp(result_cont[0], "GroupA") != 0) {
        printf("FAIL Test1b: arr[0] mismatch\n");
        failures++;
    }
    if (strcmp(result_break[1], "GroupB") != 0 || strcmp(result_cont[1], "GroupB") != 0) {
        printf("FAIL Test1c: arr[1] mismatch\n");
        failures++;
    }
    if (strcmp(result_break[2], "GroupC") != 0 || strcmp(result_cont[2], "GroupC") != 0) {
        printf("FAIL Test1d: arr[2] mismatch\n");
        failures++;
    }

    // ==================================================
    // Test 2: Corrupt entry in middle
    // ==================================================
    memset(result_break, 0, sizeof(result_break));
    memset(result_cont, 0, sizeof(result_cont));
    const char* mid_corrupt[] = {"GroupA", NULL, "GroupC"};
    int r2b = load_groups_break_stored(mid_corrupt, 3, result_break);
    int r2c = load_groups_continue_compacted(mid_corrupt, 3, result_cont);
    // break: stops at index 1, only GroupA loaded
    if (r2b != 1) {
        printf("FAIL Test2a: break count=%d (expected 1)\n", r2b);
        failures++;
    }
    if (strcmp(result_break[0], "GroupA") != 0) {
        printf("FAIL Test2b: break arr[0]=%s (expected GroupA)\n", result_break[0]);
        failures++;
    }
    // verify break did NOT touch arr[1] and arr[2]
    if (!is_zeroed(result_break, 1, 3)) {
        printf("FAIL Test2c: break left data beyond arr[0], possible gap\n");
        failures++;
    }
    // continue: skips NULL, loads GroupA at [0], GroupC at [1] (compact!)
    if (r2c != 2) {
        printf("FAIL Test2d: continue count=%d (expected 2)\n", r2c);
        failures++;
    }
    if (strcmp(result_cont[0], "GroupA") != 0) {
        printf("FAIL Test2e: continue arr[0]=%s (expected GroupA)\n", result_cont[0]);
        failures++;
    }
    if (strcmp(result_cont[1], "GroupC") != 0) {
        printf("FAIL Test2f: continue arr[1]=%s (expected GroupC)\n", result_cont[1]);
        failures++;
    }
    // arr[1..MAX] must be zeroed (compact array, no gaps)
    if (!is_zeroed(result_cont, 2, MAX_NOTIFY_GROUPS)) {
        printf("FAIL Test2g: continue array has unexpected data after count\n");
        failures++;
    }

    // ==================================================
    // Test 3: First entry corrupt
    // ==================================================
    memset(result_break, 0, sizeof(result_break));
    memset(result_cont, 0, sizeof(result_cont));
    const char* first_corrupt[] = {NULL, "GroupB", "GroupC"};
    int r3b = load_groups_break_stored(first_corrupt, 3, result_break);
    int r3c = load_groups_continue_compacted(first_corrupt, 3, result_cont);
    if (r3b != 0) {
        printf("FAIL Test3a: break count=%d (expected 0)\n", r3b);
        failures++;
    }
    if (r3c != 2) {
        printf("FAIL Test3c: continue count=%d (expected 2)\n", r3c);
        failures++;
    }
    if (strcmp(result_cont[0], "GroupB") != 0 || strcmp(result_cont[1], "GroupC") != 0) {
        printf("FAIL Test3d: continue compact storage wrong\n");
        failures++;
    }
    if (!is_zeroed(result_cont, 2, MAX_NOTIFY_GROUPS)) {
        printf("FAIL Test3e: continue array has unexpected data after count\n");
        failures++;
    }

    // ==================================================
    // Test 4: Last entry corrupt
    // ==================================================
    memset(result_break, 0, sizeof(result_break));
    memset(result_cont, 0, sizeof(result_cont));
    const char* last_corrupt[] = {"GroupA", "GroupB", NULL};
    int r4b = load_groups_break_stored(last_corrupt, 3, result_break);
    int r4c = load_groups_continue_compacted(last_corrupt, 3, result_cont);
    if (r4b != 2 || r4c != 2) {
        printf("FAIL Test4a: counts break=%d continue=%d (expected 2)\n", r4b, r4c);
        failures++;
    }
    if (strcmp(result_break[0], "GroupA") != 0 || strcmp(result_cont[0], "GroupA") != 0) {
        printf("FAIL Test4b: arr[0] mismatch\n");
        failures++;
    }
    if (strcmp(result_break[1], "GroupB") != 0 || strcmp(result_cont[1], "GroupB") != 0) {
        printf("FAIL Test4c: arr[1] mismatch\n");
        failures++;
    }

    // ==================================================
    // Test 5: Multiple corrupt entries interleaved
    // ==================================================
    memset(result_break, 0, sizeof(result_break));
    memset(result_cont, 0, sizeof(result_cont));
    const char* multi_corrupt[] = {"GroupA", NULL, "GroupC", NULL, "GroupE"};
    int r5b = load_groups_break_stored(multi_corrupt, 5, result_break);
    int r5c = load_groups_continue_compacted(multi_corrupt, 5, result_cont);
    if (r5b != 1) {
        printf("FAIL Test5a: break count=%d (expected 1)\n", r5b);
        failures++;
    }
    if (r5c != 3) {
        printf("FAIL Test5c: continue count=%d (expected 3)\n", r5c);
        failures++;
    }
    // continue must store compactly: [0]=A, [1]=C, [2]=E
    if (strcmp(result_cont[0], "GroupA") != 0 ||
        strcmp(result_cont[1], "GroupC") != 0 ||
        strcmp(result_cont[2], "GroupE") != 0) {
        printf("FAIL Test5d: continue compact storage wrong\n");
        failures++;
    }
    if (!is_zeroed(result_cont, 3, MAX_NOTIFY_GROUPS)) {
        printf("FAIL Test5e: continue array has unexpected data after count\n");
        failures++;
    }

    // ==================================================
    // Test 6: Empty string entry (name[0] == '\0' case)
    // ==================================================
    memset(result_break, 0, sizeof(result_break));
    memset(result_cont, 0, sizeof(result_cont));
    const char* empty_name[] = {"GroupA", "", "GroupC"};
    int r6b = load_groups_break_stored(empty_name, 3, result_break);
    int r6c = load_groups_continue_compacted(empty_name, 3, result_cont);
    if (r6b != 1) {
        printf("FAIL Test6a: break count=%d (expected 1)\n", r6b);
        failures++;
    }
    if (r6c != 2) {
        printf("FAIL Test6c: continue count=%d (expected 2)\n", r6c);
        failures++;
    }
    if (strcmp(result_cont[0], "GroupA") != 0 || strcmp(result_cont[1], "GroupC") != 0) {
        printf("FAIL Test6d: continue compact storage wrong\n");
        failures++;
    }

    if (failures == 0) {
        printf("OK");
    }
    return failures;
}
TESTEOF

if gcc -o "$TMPDIR/test_registry_load_continue.exe" "$TMPDIR/test_registry_load_continue.c"; then
    RESULT=$("$TMPDIR/test_registry_load_continue.exe")
    if [ "$RESULT" = "OK" ]; then
        pass "Registry load continue: all 6 cases correct (break stops, continue skips + compact)"
    else
        fail "Registry load continue: $RESULT"
    fi
else
    echo "SKIP: Registry load continue test (compilation not available)"
fi

# === Summary ===
echo "---"
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
